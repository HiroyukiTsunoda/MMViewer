// Integration tests include the host implementation to exercise real Win32
// notifications and asynchronous completion without exporting test-only APIs.
#define wWinMain IncludedWinMain
#include "../src/native/main.cpp"
#undef wWinMain

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 0.015) throw std::runtime_error(message);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template<typename Predicate>
void Until(Predicate done, const char* message, DWORD limit = 5000) {
    const auto deadline = GetTickCount64() + limit;
    do {
        Pump();
        if (done()) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error(message);
}

void PumpFor(DWORD milliseconds) {
    const auto until = GetTickCount64() + milliseconds;
    do { Pump(); MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE); }
    while (GetTickCount64() < until);
}

void Write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) throw std::runtime_error("Cannot write host fixture");
}

// Each suite keeps only its newest run-* directories. Fixture trees left behind
// by earlier runs otherwise accumulate inside the repository, which a running
// viewer may have registered as a folder.
void PruneOldRuns(const fs::path& suite, size_t keep = 2) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> runs;
    for (const auto& entry : fs::directory_iterator(suite, ec)) {
        if (!entry.is_directory(ec) || !entry.path().filename().wstring().starts_with(L"run-")) continue;
        runs.emplace_back(entry.last_write_time(ec), entry.path());
    }
    std::sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t index = keep; index < runs.size(); ++index) fs::remove_all(runs[index].second, ec);
}

std::string LongDocument(const std::string& title) {
    std::string source = "# " + title + "\n\nlayout-marker\n\n";
    for (int i = 0; i < 90; ++i)
        source += "## Section " + std::to_string(i) + "\n\nParagraph " + std::to_string(i) + " with enough content to scroll and retain a reading position.\n\n";
    return source;
}

struct Host {
    App app;
    std::wstring className = L"MMViewer.Native.HostTests." + std::to_wstring(GetCurrentProcessId());
    explicit Host(const fs::path& session) {
        // Never call App::Restore: it targets the user's real LOCALAPPDATA.
        fs::create_directories(session.parent_path());
        app.ini = session.wstring();
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = App::Proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpszClassName = className.c_str();
        Check(RegisterClassExW(&type) != 0, "Cannot register test host class");
        HWND window = CreateWindowExW(0, className.c_str(), L"MMViewer hidden integration test",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 1100, 800,
            nullptr, nullptr, type.hInstance, &app);
        Check(window != nullptr && app.view.Handle() != nullptr, "Cannot create test host controls");
        Check(!IsWindowVisible(window), "Integration test window must remain hidden");
        app.LoadActive();
    }
    ~Host() {
        if (IsWindow(app.hwnd)) SendMessageW(app.hwnd, WM_CLOSE, 0, 0);
        Pump();
        UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
    }
};

int IndexOf(App& app, const fs::path& path) {
    const auto normalized = mm::NormalizePath(path.wstring());
    for (size_t i = 0; i < app.tabs.size(); ++i)
        if (app.tabs[i].path == normalized) return static_cast<int>(i);
    throw std::runtime_error("Expected tab is absent");
}

void LoadedPath(App& app, const fs::path& path) {
    const auto normalized = mm::NormalizePath(path.wstring());
    Until([&] {
        return !app.loading && app.currentDoc && app.currentDoc->path == normalized
            && app.active >= 0 && app.displayedTabId == app.tabs[app.active].id;
    }, "Timed out waiting for document load");
}

void LayoutDocument(App& app) {
    // Search measures and lays out the native document even with a hidden HWND.
    app.view.Search(L"layout-marker", 0);
    Check(app.view.SearchResult().second > 0, "Native layout/search did not find fixture text");
    app.view.Search(L"", 0);
}

HTREEITEM TreeItem(App& app, const fs::path& path) {
    HTREEITEM found = nullptr;
    const auto normalized = mm::NormalizePath(path.wstring());
    app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) {
        if (node && node->path == normalized) found = item;
    });
    return found;
}

FolderRegistration& Folder(App& app, const fs::path& path) {
    auto* folder = app.FindFolder(mm::NormalizePath(path.wstring()));
    Check(folder != nullptr, "Expected folder registration is absent");
    return *folder;
}

mm::FolderNode* FolderTree(App& app, const fs::path& path) {
    const auto* folder = app.FindFolder(mm::NormalizePath(path.wstring()));
    return folder ? folder->tree.get() : nullptr;
}

void RemoveAllFolders(App& app) {
    for (const auto& path : app.FolderPaths()) app.RemoveFolderFromList(path);
    Check(!app.HasFolders(), "Cannot clear isolated test folder registrations");
}

bool WatchTouchesFolder(const App& app, const fs::path& folder) {
    const auto path = mm::NormalizePath(folder.wstring());
    return std::any_of(app.watches.begin(), app.watches.end(), [&](const auto& watch) {
        return App::IsWithinFolder(watch->path, path)
            || (watch->recursive && App::IsWithinFolder(path, watch->path));
    });
}

bool WatchCoversFolder(const App& app, const fs::path& folder) {
    const auto path = mm::NormalizePath(folder.wstring());
    return std::any_of(app.watches.begin(), app.watches.end(), [&](const auto& watch) {
        return _wcsicmp(watch->path.c_str(), path.c_str()) == 0
            || (watch->recursive && App::IsWithinFolder(path, watch->path));
    });
}

bool Key(App& app, int key, bool control = false, bool shift = false) {
    struct KeyboardState {
        BYTE previous[256]{};
        KeyboardState(bool control, bool shift) {
            Check(GetKeyboardState(previous) != FALSE, "Cannot read thread keyboard state");
            BYTE state[256]{};
            if (control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
            if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
            Check(SetKeyboardState(state) != FALSE, "Cannot set thread keyboard state");
        }
        ~KeyboardState() { SetKeyboardState(previous); }
    } keyboard(control, shift);
    MSG message{};
    message.hwnd = app.hwnd; message.message = WM_KEYDOWN; message.wParam = key;
    return app.Key(message);
}

void TreeAndSidebar(App& app, const fs::path& root) {
    app.OpenRoot(root.wstring());
    Check(!FolderTree(app, root) && TreeView_GetCount(app.treeH) == 1 && TreeItem(app, root) == TreeView_GetRoot(app.treeH),
        "Opening a folder did not show exactly one pending root item before scanning");
    Until([&] { return FolderTree(app, root) && FolderTree(app, root)->path == mm::NormalizePath(root.wstring()); }, "Timed out waiting for folder tree");
    Until([&] { return WatchCoversFolder(app, root); }, "Timed out waiting for initial folder watch");
    int rootItems = 0;
    app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM, mm::FolderNode* node) {
        if (node && node->path == mm::NormalizePath(root.wstring())) ++rootItems;
    });
    Check(rootItems == 1 && TreeItem(app, root) == TreeView_GetRoot(app.treeH)
        && TreeView_GetNextSibling(app.treeH, TreeView_GetRoot(app.treeH)) == nullptr,
        "Completed folder scan duplicated the root item");
    RECT tabsBounds{}, treeBounds{}, documentBounds{};
    Check(GetWindowRect(app.tabsH, &tabsBounds) && GetWindowRect(app.treeH, &treeBounds)
        && GetWindowRect(app.view.Handle(), &documentBounds), "Cannot inspect native control positions");
    for (const auto& [command, button] : app.buttons) {
        if (command >= SearchNext && command <= SearchClose) continue;
        RECT bounds{};
        Check(GetWindowRect(button, &bounds) != FALSE, "Cannot inspect native command button position");
        if (command == NewTab) {
            const auto center = bounds.top + (bounds.bottom - bounds.top) / 2;
            Check(center >= tabsBounds.top && center < tabsBounds.bottom, "New-tab button is outside the tab strip");
        } else {
            Check(bounds.bottom <= tabsBounds.top, "Command menu must appear above the tab strip");
        }
    }
    Check(tabsBounds.bottom <= treeBounds.top && tabsBounds.bottom <= documentBounds.top,
        "Tab strip overlaps the folder tree or document content");
    Check(TreeItem(app, root / L"ancestor") != nullptr, "Missing ancestor of nested Markdown");
    Check(TreeItem(app, root / L"ancestor" / L"deep") != nullptr, "Missing deep Markdown folder");
    Check(TreeItem(app, root / L"ancestor" / L"deep" / L"three.MD") != nullptr, "Uppercase Markdown missing from native tree");
    Check(TreeItem(app, root / L"no-md") == nullptr, "Folder without Markdown appears in tree");
    Check(TreeItem(app, root / L"note.txt") == nullptr, "Non-Markdown file appears in tree");
    Check(TreeItem(app, root / L"open-only.markdown") == nullptr, "Tree must restrict file extension to .md");
    Check((GetWindowLongPtrW(app.treeH, GWL_STYLE) & (TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT)) == 0,
        "Folder tree still shows plus/minus buttons or branch connector lines");
    const auto folderImages = TreeView_GetImageList(app.treeH, TVSIL_NORMAL);
    Check(folderImages != nullptr && ImageList_GetImageCount(folderImages) == 2,
        "Folder tree did not install its closed/open folder icons");
    auto checkPresentation = [&] {
        app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) {
            Check(node != nullptr, "Tree presentation item has no model node");
            wchar_t label[1024]{};
            TVITEMW info{};
            info.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE;
            info.hItem = item;
            info.pszText = label;
            info.cchTextMax = 1024;
            info.stateMask = TVIS_EXPANDED;
            Check(TreeView_GetItem(app.treeH, &info) != FALSE, "Cannot inspect tree label and icons");
            Check(std::wstring(label) == node->name, "Tree filename or folder label contains extra formatting text");
            if (node->directory) {
                const int expectedImage = (info.state & TVIS_EXPANDED) ? 1 : 0;
                Check(info.iImage == expectedImage && info.iSelectedImage == expectedImage,
                    "Folder icon does not match its current open/closed state");
            } else {
                // TreeView returns its missing-image sentinel as an unsigned
                // 16-bit value on this Windows version. Neither state may
                // reference an actual image in the folder-only image list.
                const int count = ImageList_GetImageCount(folderImages);
                Check((info.iImage < 0 || info.iImage >= count)
                    && (info.iSelectedImage < 0 || info.iSelectedImage >= count),
                    "Markdown file must display its plain name without a file icon");
            }
        });
    };
    checkPresentation();

    Check(app.sidebar && (GetWindowLongPtrW(app.treeH, GWL_STYLE) & WS_VISIBLE), "Sidebar should initially be enabled");
    Check(Key(app, 'B', true), "Ctrl+B was not handled");
    Check(!app.sidebar && !(GetWindowLongPtrW(app.treeH, GWL_STYLE) & WS_VISIBLE), "Ctrl+B failed to hide sidebar");
    Check(Key(app, 'B', true), "Second Ctrl+B was not handled");
    Check(app.sidebar && (GetWindowLongPtrW(app.treeH, GWL_STYLE) & WS_VISIBLE), "Sidebar failed to reopen");

    const auto branch = TreeItem(app, root / L"ancestor");
    const auto branchPath = mm::NormalizePath((root / L"ancestor").wstring());
    const auto initialTabCount = app.tabs.size();
    const auto initialLoadGeneration = app.loadGeneration;
    auto checkBranch = [&](bool expanded) {
        Check(((TreeView_GetItemState(app.treeH, branch, TVIS_EXPANDED) & TVIS_EXPANDED) != 0) == expanded,
            "Folder interaction did not toggle the native expansion state");
        Check(app.expanded.contains(branchPath) == expanded, "Folder interaction did not persist its expansion state");
        checkPresentation();
    };
    auto itemPoint = [&](HTREEITEM item, bool icon) {
        TreeView_EnsureVisible(app.treeH, item);
        RECT bounds{};
        Check(TreeView_GetItemRect(app.treeH, item, &bounds, TRUE) != FALSE, "Cannot locate tree item label");
        POINT point{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
        if (icon) {
            // Find the control's actual icon hit region without assuming icon
            // size, row indentation, theme padding, or monitor DPI.
            bool found = false;
            for (LONG x = bounds.left - 1; x >= 0; --x) {
                TVHITTESTINFO hit{};
                hit.pt = {x, point.y};
                if (TreeView_HitTest(app.treeH, &hit) == item && (hit.flags & TVHT_ONITEMICON)) {
                    point.x = x;
                    found = true;
                    break;
                }
            }
            Check(found, "Folder icon has no clickable native hit region");
        }
        TVHITTESTINFO hit{};
        hit.pt = point;
        Check(TreeView_HitTest(app.treeH, &hit) == item && (hit.flags & (icon ? TVHT_ONITEMICON : TVHT_ONITEMLABEL)),
            "Tree click target does not match its native label/icon region");
        return point;
    };
    auto clickItem = [&](HTREEITEM item, bool icon) {
        const auto point = itemPoint(item, icon);
        SendMessageW(app.treeH, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        SendMessageW(app.treeH, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    };
    // TVM_EXPAND stops sending expansion notifications after EXPANDEDONCE.
    // Exercise the control's keyboard interaction to test its user-notification
    // path instead of incorrectly requiring notifications from repeated API calls.
    TreeView_SelectItem(app.treeH, branch);
    SendMessageW(app.treeH, WM_KEYDOWN, VK_RIGHT, 1);
    SendMessageW(app.treeH, WM_KEYUP, VK_RIGHT, 1);
    Check((TreeView_GetItemState(app.treeH, branch, TVIS_EXPANDED) & TVIS_EXPANDED) != 0, "Right arrow did not expand native tree");
    Check(app.expanded.contains(mm::NormalizePath((root / L"ancestor").wstring())), "Tree expansion notification was not persisted");
    checkBranch(true);
    SendMessageW(app.treeH, WM_KEYDOWN, VK_LEFT, 1);
    SendMessageW(app.treeH, WM_KEYUP, VK_LEFT, 1);
    Check((TreeView_GetItemState(app.treeH, branch, TVIS_EXPANDED) & TVIS_EXPANDED) == 0, "Left arrow did not collapse native tree");
    Check(!app.expanded.contains(mm::NormalizePath((root / L"ancestor").wstring())), "Tree collapse notification was not persisted");
    checkBranch(false);
    for (const bool icon : {false, true, false, true}) {
        clickItem(branch, icon);
        checkBranch(true);
        clickItem(branch, icon);
        checkBranch(false);
    }
    clickItem(branch, false);
    checkBranch(true);
    const auto doubleClickPoint = itemPoint(branch, false);
    SendMessageW(app.treeH, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(doubleClickPoint.x, doubleClickPoint.y));
    SendMessageW(app.treeH, WM_LBUTTONUP, 0, MAKELPARAM(doubleClickPoint.x, doubleClickPoint.y));
    checkBranch(true);
    clickItem(branch, true);
    checkBranch(false);
    SendMessageW(app.treeH, WM_KEYDOWN, VK_RIGHT, 1);
    SendMessageW(app.treeH, WM_KEYUP, VK_RIGHT, 1);
    checkBranch(true);
    SendMessageW(app.treeH, WM_KEYDOWN, VK_LEFT, 1);
    SendMessageW(app.treeH, WM_KEYUP, VK_LEFT, 1);
    checkBranch(false);
    Check(app.tabs.size() == initialTabCount && app.loadGeneration == initialLoadGeneration,
        "Folder label/icon interaction opened a document tab or queued a file read");
    app.Command(ExpandAll);
    Check((TreeView_GetItemState(app.treeH, branch, TVIS_EXPANDED) & TVIS_EXPANDED) != 0, "Expand-all did not expand native tree");
    Check(app.expanded.contains(mm::NormalizePath((root / L"ancestor").wstring())), "Expand-all did not persist expanded state");
    checkPresentation();
    app.Command(CollapseAll);
    Check((TreeView_GetItemState(app.treeH, branch, TVIS_EXPANDED) & TVIS_EXPANDED) == 0, "Collapse-all did not collapse native tree");
    Check(!app.expanded.contains(mm::NormalizePath((root / L"ancestor").wstring())), "Collapse-all did not persist collapsed state");
    checkPresentation();
    const auto file = root / L"one.md";
    clickItem(TreeItem(app, file), false);
    LoadedPath(app, file);
    Check(app.tabs.size() == initialTabCount + 1 && app.tabs[app.active].path == mm::NormalizePath(file.wstring()),
        "Plain file-label click did not open exactly its document tab");
    checkPresentation();
    std::cout << "PASS native tree filtering, folder-only icons, label/icon toggles, keyboard expansion, plain file click\n";
}

void SidebarSplitter(App& app, const fs::path& root) {
    Check(app.splitH != nullptr && IsWindow(app.splitH), "Sidebar does not have a dedicated native splitter");
    RECT originalWindow{}, originalClient{};
    Check(GetWindowRect(app.hwnd, &originalWindow) && GetClientRect(app.hwnd, &originalClient),
        "Cannot inspect splitter fixture window geometry");
    const auto originalWidth = app.sideWidth;
    const auto originalTree = FolderTree(app, root);
    const auto originalRootItem = TreeItem(app, root);
    const auto originalSelection = TreeView_GetSelection(app.treeH);
    const auto originalDocument = app.currentDoc;
    struct RestoreCursor { HCURSOR previous; ~RestoreCursor() { SetCursor(previous); } } cursor{GetCursor()};
    auto resizeClient = [&](int width) {
        RECT bounds{0, 0, app.D(width), originalClient.bottom};
        Check(AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_STYLE)),
            FALSE, static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE)), app.dpi) != FALSE,
            "Cannot calculate splitter test window bounds");
        Check(SetWindowPos(app.hwnd, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Cannot resize hidden splitter fixture");
        app.Layout();
    };
    auto checkLayout = [&] {
        RECT tree{}, splitter{}, view{};
        Check(GetWindowRect(app.treeH, &tree) && GetWindowRect(app.splitH, &splitter)
            && GetWindowRect(app.view.Handle(), &view), "Cannot inspect splitter layout");
        Check(tree.right == splitter.left && splitter.right == view.left,
            "Tree, splitter, and document overlap or leave a horizontal gap");
        Check(std::abs((splitter.right - splitter.left) - app.D(8)) <= 1,
            "Dedicated splitter does not provide its full 8-DIP drag target");
    };
    auto beginDrag = [&] {
        RECT bounds{};
        Check(GetClientRect(app.splitH, &bounds) != FALSE, "Cannot locate native splitter drag target");
        const POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        SendMessageW(app.splitH, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        Check(GetCapture() == app.splitH, "Splitter did not capture the mouse when dragging began");
    };
    auto dragPoint = [&](int width) {
        RECT bounds{};
        Check(GetWindowRect(app.splitH, &bounds) != FALSE, "Cannot map current splitter drag coordinates");
        POINT point{bounds.left, (bounds.top + bounds.bottom) / 2};
        Check(ScreenToClient(app.hwnd, &point) != FALSE, "Cannot map splitter point to its parent");
        point.x = app.D(width);
        Check(ClientToScreen(app.hwnd, &point) && ScreenToClient(app.splitH, &point),
            "Cannot map parent width to splitter mouse coordinates");
        return point;
    };
    auto moveTo = [&](int width) {
        const auto point = dragPoint(width);
        SendMessageW(app.splitH, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    };
    auto endDrag = [&] {
        const auto point = dragPoint(app.sideWidth);
        SendMessageW(app.splitH, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        Check(GetCapture() != app.splitH, "Splitter retained mouse capture after button release");
    };

    resizeClient(800);
    app.sideWidth = 300;
    app.Layout();
    checkLayout();
    SendMessageW(app.splitH, WM_SETCURSOR, reinterpret_cast<WPARAM>(app.splitH), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    Check(GetCursor() == LoadCursorW(nullptr, IDC_SIZEWE), "Splitter does not show the horizontal resize cursor");
    beginDrag();
    moveTo(360);
    Check(std::abs(app.sideWidth - 360) <= 1, "Dragging the splitter right did not widen the sidebar");
    checkLayout();
    moveTo(260);
    Check(std::abs(app.sideWidth - 260) <= 1, "Dragging the splitter left did not narrow the sidebar");
    checkLayout();
    endDrag();
    app.Save();
    Check(GetPrivateProfileIntW(L"App", L"SidebarWidth", 0, app.ini.c_str()) == static_cast<UINT>(app.sideWidth),
        "Final dragged sidebar width was not saved in the isolated session");

    beginDrag();
    moveTo(2000);
    RECT limitedDocument{};
    Check(GetWindowRect(app.view.Handle(), &limitedDocument) != FALSE
        && limitedDocument.right - limitedDocument.left >= app.D(300)
        && limitedDocument.right - limitedDocument.left <= app.D(300) + 1,
        "Splitter did not stop at the 300-DIP document width limit in an 800-DIP window");
    moveTo(-100);
    Check(app.sideWidth == 160, "Splitter exceeded the minimum sidebar width");
    endDrag();
    resizeClient(1100);
    beginDrag();
    moveTo(2000);
    Check(app.sideWidth == 600, "Splitter exceeded the maximum sidebar width");
    endDrag();
    checkLayout();

    app.sideWidth = 300;
    app.Layout();
    beginDrag();
    moveTo(350);
    ReleaseCapture();
    const auto captureLostWidth = app.sideWidth;
    moveTo(450);
    Check(GetCapture() != app.splitH && app.sideWidth == captureLostWidth,
        "Splitter continued resizing after capture was lost");
    beginDrag();
    moveTo(320);
    SendMessageW(app.splitH, WM_CANCELMODE, 0, 0);
    const auto cancelledWidth = app.sideWidth;
    moveTo(420);
    Check(GetCapture() != app.splitH && app.sideWidth == cancelledWidth,
        "Splitter continued resizing after cancellation");
    beginDrag();
    moveTo(330);
    app.Command(Sidebar);
    const auto hiddenWidth = app.sideWidth;
    Check(!app.sidebar && GetCapture() != app.splitH
        && !(GetWindowLongPtrW(app.splitH, GWL_STYLE) & WS_VISIBLE), "Hiding the sidebar did not hide and release its splitter");
    moveTo(450);
    Check(app.sideWidth == hiddenWidth, "Hidden splitter continued resizing after sidebar dismissal");
    app.Command(Sidebar);

    SetWindowPos(app.hwnd, nullptr, 0, 0, originalWindow.right - originalWindow.left,
        originalWindow.bottom - originalWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    app.sideWidth = originalWidth;
    app.Layout();
    checkLayout();
    Check(FolderTree(app, root) == originalTree && TreeItem(app, root) == originalRootItem
        && TreeView_GetSelection(app.treeH) == originalSelection && app.currentDoc == originalDocument,
        "Splitter dragging replaced the tree model, selected item, or displayed document");
    Check(!IsWindowVisible(app.hwnd), "Splitter test unexpectedly showed its host window");
    app.Save();
    std::cout << "PASS native sidebar splitter drag target, geometry, width limits, persistence, capture cancellation\n";
}

void TabsAndState(App& app, const fs::path& root) {
    const auto first = root / L"one.md", second = root / L"two.md", third = root / L"ancestor" / L"deep" / L"three.MD";
    app.OpenPaths({first.wstring(), second.wstring(), third.wstring()});
    LoadedPath(app, third);
    Check(app.tabs.size() == 3 && TabCtrl_GetItemCount(app.tabsH) == 3, "Multiple file open did not create three tabs");
    LayoutDocument(app);
    const int a = IndexOf(app, first), b = IndexOf(app, second), c = IndexOf(app, third);
    app.tabs[a].scroll = 0.15; app.tabs[b].scroll = 0.35; app.tabs[c].scroll = 0.75;
    app.view.SetScrollRatio(0.75);

    // Do not pump between these calls: completion messages must remain queued,
    // reproducing the old display/new active-tab interval deterministically.
    app.Switch(a);
    app.Capture();
    app.Switch(b);
    app.Save();
    Near(app.tabs[a].scroll, 0.15, "Rapid switch overwrote first tab's stored scroll");
    Near(app.tabs[b].scroll, 0.35, "Save during loading overwrote second tab's scroll");
    Near(app.tabs[c].scroll, 0.75, "Leaving visible tab lost its scroll");
    LoadedPath(app, second);
    Near(app.view.ScrollRatio(), 0.35, "Accepted document did not restore its own scroll");
    app.Switch(a); LoadedPath(app, first);
    Near(app.view.ScrollRatio(), 0.15, "Switch-back did not restore first tab scroll");
    SendMessageW(app.view.Handle(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 4 * WHEEL_DELTA), 0);
    Check(app.tabs[a].font == 20 && app.view.FontSize() == 20, "Active tab font did not change");
    app.Switch(b); LoadedPath(app, second);
    Check(app.view.FontSize() == 16, "Font change leaked into another existing tab");
    SendMessageW(app.view.Handle(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, static_cast<WORD>(-4 * WHEEL_DELTA)), 0);
    app.Switch(a); LoadedPath(app, first);
    Check(app.view.FontSize() == 20 && app.tabs[b].font == 12, "Per-tab font size was not preserved");

    app.OpenPaths({(root / L"ancestor" / L".." / L"one.md").wstring()});
    LoadedPath(app, first);
    Check(app.tabs.size() == 3 && app.active == a, "Normalized duplicate path created another tab");
    Check(Key(app, VK_TAB, true), "Ctrl+Tab was not handled");
    LoadedPath(app, second);
    Check(Key(app, VK_TAB, true, true), "Ctrl+Shift+Tab was not handled");
    LoadedPath(app, first);

    app.Command(Source);
    Check(app.tabs[app.active].source, "Source command did not retain tab mode");
    app.Command(Source);
    const auto wasDark = app.dark;
    app.Command(Theme); Check(app.dark != wasDark, "Theme toggle failed");
    app.Command(Theme);
    app.Command(Find); SetWindowTextW(app.searchH, L"layout-marker");
    Check(app.searching && app.view.SearchResult().second == 1, "Host search field did not search native text");
    Check(Key(app, VK_ESCAPE), "Escape did not close host search");
    Check(!app.searching, "Search remained open after Escape");

    Check(Key(app, 'W', true), "Ctrl+W was not handled");
    Check(app.tabs.size() == 2 && app.closed.size() == 1, "Close did not preserve reopen history");
    Check(Key(app, 'T', true, true), "Ctrl+Shift+T was not handled");
    LoadedPath(app, first);
    Check(app.tabs.size() == 3 && app.tabs[app.active].font == 20, "Reopen lost the tab's font state");
    Check(Key(app, 'T', true), "Ctrl+T was not handled");
    Check(app.tabs.size() == 4 && app.tabs[app.active].path.empty(), "Ctrl+T did not create an empty tab");
    const auto explicitMarkdown = root / L"open-only.markdown";
    app.OpenPaths({explicitMarkdown.wstring()}); LoadedPath(app, explicitMarkdown);
    Check(app.tabs.size() == 4, "Opening into empty tab unnecessarily added another tab");
    app.Close(app.active);
    LoadedPath(app, first);
    std::cout << "PASS asynchronous tab scroll identity, deduplication, tab shortcuts, font/mode/search state\n";
}

void Session(App& app) {
    app.view.SetScrollRatio(0.42);
    app.Command(Theme);
    app.Save();
    Check(fs::exists(app.ini), "Test session file was not saved");
    Check(GetPrivateProfileIntW(L"App", L"Tabs", 0, app.ini.c_str()) == app.tabs.size(), "Saved tab count does not match host");
    Check(GetPrivateProfileIntW(L"App", L"Active", 9999, app.ini.c_str()) == app.active, "Saved selected tab is incorrect");
    Check(GetPrivateProfileIntW(L"App", L"Dark", 99, app.ini.c_str()) == static_cast<UINT>(app.dark), "Saved theme is incorrect");
    const auto folders = app.FolderPaths();
    Check(GetPrivateProfileIntW(L"App", L"Roots", 9999, app.ini.c_str()) == folders.size(),
        "Saved folder registration count is incorrect");
    for (size_t index = 0; index < folders.size(); ++index)
        Check(app.ReadIni(L"Roots", std::to_wstring(index).c_str()) == folders[index], "Saved Unicode root path is incorrect");
    const auto section = L"Tab" + std::to_wstring(app.active);
    Check(app.ReadIni(section.c_str(), L"Path") == app.tabs[app.active].path, "Saved tab path is incorrect");
    Near(_wtof(app.ReadIni(section.c_str(), L"Scroll").c_str()), 0.42, "Saved reading position is incorrect");
    Check(GetPrivateProfileIntW(section.c_str(), L"Font", 0, app.ini.c_str()) == 20, "Saved per-tab font is incorrect");
    Check(!fs::exists(app.ini + L".tmp"), "Atomic session temp file was not replaced");
    app.Save();
    Check(fs::exists(app.ini + L".bak"), "Previous session backup was not created");
    std::cout << "PASS isolated INI session content, atomic replacement, previous-session backup\n";
}

void CachedTabDocuments(const fs::path& artifacts) {
    const auto root = artifacts / L"cached-tabs";
    const auto first = root / L"first.md", second = root / L"second.md";
    const auto original = LongDocument("Cache alpha");
    auto updated = original;
    updated.replace(updated.find("alpha"), 5, "omega");
    Write(first, original);
    Write(second, LongDocument("Cache second"));
    {
        Host host(root / L"session.ini");
        auto& app = host.app;
        app.OpenPaths({first.wstring()}); LoadedPath(app, first); LayoutDocument(app);
        const auto firstDocument = app.currentDoc;
        app.view.SetScrollRatio(0.25);
        app.OpenPaths({second.wstring()}); LoadedPath(app, second); LayoutDocument(app);
        const auto secondDocument = app.currentDoc;
        const auto a = IndexOf(app, first), b = IndexOf(app, second);

        // Deny a new GENERIC_READ handle while still allowing metadata probes.
        // A tab switch must succeed from its cache without reading its body.
        struct BodyLock {
            HANDLE handle=INVALID_HANDLE_VALUE;
            ~BodyLock(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
        } bodyLock;
        bodyLock.handle=CreateFileW(first.c_str(),GENERIC_WRITE,FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(bodyLock.handle!=INVALID_HANDLE_VALUE,"Cannot deny body reads for cached-tab fixture");
        const auto denied=CreateFileW(first.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(denied!=INVALID_HANDLE_VALUE)CloseHandle(denied);
        Check(denied==INVALID_HANDLE_VALUE,"Cached-tab fixture does not actually deny body reads");
        WIN32_FILE_ATTRIBUTE_DATA stamp{};
        Check(GetFileAttributesExW(first.c_str(),GetFileExInfoStandard,&stamp)!=FALSE,
            "Read-denied cached file does not permit metadata checks");
        app.Switch(a);
        Check(app.currentDoc==firstDocument&&app.displayedTabId==app.tabs[a].id&&app.loading,
            "Cached tab was not displayed synchronously before its update check");
        Near(app.view.ScrollRatio(),0.25,"Synchronous cache display lost the saved scroll position");
        app.view.SetScrollRatio(0.63);
        LoadedPath(app,first);
        Check(app.currentDoc==firstDocument&&app.tabs[a].status.empty(),
            "Unchanged cached tab attempted a blocked body read");
        Near(app.view.ScrollRatio(),0.63,"Metadata completion rewound scrolling performed during its check");
        CloseHandle(bodyLock.handle);bodyLock.handle=INVALID_HANDLE_VALUE;

        // Paused monitoring makes the same-stamp replacement deterministic;
        // selecting a tab remains an explicit request to validate it.
        app.tabs[a].autoRefresh=false;app.tabs[b].autoRefresh=false;
        app.Switch(b);LoadedPath(app,second);
        Write(first,updated);
        const auto file=CreateFileW(first.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(file!=INVALID_HANDLE_VALUE,"Cannot restore fixture file timestamps");
        const auto restored=SetFileTime(file,&stamp.ftCreationTime,nullptr,&stamp.ftLastWriteTime);
        CloseHandle(file);
        Check(restored!=FALSE,"Cannot restore fixture file timestamps");
        WIN32_FILE_ATTRIBUTE_DATA sameStamp{};
        Check(GetFileAttributesExW(first.c_str(),GetFileExInfoStandard,&sameStamp)!=FALSE
            &&App::SameDocumentStamp(stamp,sameStamp),"F5 fixture did not preserve the cached file stamp");
        app.Switch(a);LoadedPath(app,first);
        Check(app.currentDoc==firstDocument&&app.currentDoc->source==original,
            "Same-stamp tab switch discarded its reusable document");
        Check(Key(app,VK_F5),"F5 was not handled for the cached tab");
        LoadedPath(app,first);
        Check(app.currentDoc->source==updated&&app.currentDoc!=firstDocument&&!app.tabs[a].autoRefresh,
            "F5 failed to bypass metadata reuse or resumed paused monitoring");
        const auto updatedDocument=app.currentDoc;

        app.Switch(b);LoadedPath(app,second);
        Check(fs::remove(first),"Cannot remove isolated missing-cache fixture");
        app.Switch(a);
        Check(app.currentDoc==updatedDocument&&app.displayedTabId==app.tabs[a].id,
            "A missing file prevented immediate display of its retained document");
        Until([&]{return !app.loading;},"Missing cached document did not finish its update check");
        Check(app.currentDoc==updatedDocument&&!app.tabs[a].status.empty(),
            "Missing-file update discarded cached content or hid its read error");
        const auto pausedCancel=app.readCancel;
        const auto pausedGeneration=app.loadGeneration;
        app.CheckActive();
        Check(app.readCancel==pausedCancel&&app.loadGeneration==pausedGeneration&&!app.loading,
            "Automatic checking restarted a paused cached document");
        Write(first,updated);app.LoadActive(true,true);LoadedPath(app,first);

        // Rejected completions must not interrupt the current cache validation,
        // including cancellation within the same background-check generation.
        app.Switch(b);
        const auto currentGeneration=app.loadGeneration;
        auto stale=std::make_unique<Loaded>();
        stale->id=app.tabs[b].id;stale->generation=currentGeneration;
        stale->cancel=std::make_shared<std::atomic_bool>(true);
        stale->doc=std::make_shared<mm::Document>(*updatedDocument);
        app.AcceptLoad(std::move(stale));
        Check(app.currentDoc==secondDocument&&app.loading,
            "Cancelled completion replaced the active cached tab or ended its pending check");
        const auto pendingCancel=app.readCancel;
        const auto closedId=app.tabs[a].id;
        app.Close(a);
        Check(app.currentDoc==secondDocument&&app.loadGeneration==currentGeneration&&app.readCancel==pendingCancel,
            "Closing an inactive tab restarted the active document load");
        Check(std::none_of(app.documentCache.begin(),app.documentCache.end(),[&](const auto& entry){return entry.id==closedId;}),
            "Closed-tab history retained its cached body");
        LoadedPath(app,second);
        auto closedResult=std::make_unique<Loaded>();
        closedResult->id=closedId;closedResult->generation=app.loadGeneration;closedResult->doc=updatedDocument;
        app.AcceptLoad(std::move(closedResult));
        Check(app.currentDoc==secondDocument,"Closed-tab completion replaced the active document");
    }
    {
        // Test actual owned string capacities without sending multi-megabyte
        // synthetic sources through parsing and layout just to fill the LRU.
        App cache;
        std::vector<Tab> registrations;
        auto remember=[&](unsigned long long id,size_t bytes){
            Tab tab;tab.id=id;tab.path=(root/(std::to_wstring(id)+L".md")).wstring();
            auto doc=std::make_shared<mm::Document>();doc->path=tab.path;doc->source.assign(bytes,'a');
            cache.RememberDocument(tab,doc,{},false);registrations.push_back(tab);
            return std::weak_ptr<mm::Document>(doc);
        };
        auto oldest=remember(1,128);
        for(unsigned long long id=2;id<=App::DocumentCacheCount+1;++id)remember(id,128);
        Check(cache.documentCache.size()==App::DocumentCacheCount&&oldest.expired(),
            "Document-count limit did not release the oldest cached body");
        Check(cache.FindDocument(registrations[1]).doc!=nullptr,"Cannot touch an existing cache entry");
        remember(20,128);
        Check(cache.FindDocument(registrations[1]).doc&&!cache.FindDocument(registrations[2]).doc,
            "Document cache did not evict the least recently used tab");
        for(const auto& tab:registrations)cache.ForgetDocument(tab.id);
        Check(cache.documentCache.empty()&&cache.documentCacheBytes==0,"Clearing closed documents leaked cache accounting");
        oldest=remember(30,9*1024*1024);
        remember(31,9*1024*1024);remember(32,9*1024*1024);remember(33,9*1024*1024);
        Check(oldest.expired()&&cache.documentCache.size()==3&&cache.documentCacheBytes<=App::DocumentCacheBytes,
            "Byte-budget overflow retained too many decoded document buffers");
        const auto oversized=remember(40,App::DocumentCacheBytes+1);
        Check(oversized.expired()&&cache.documentCacheBytes<=App::DocumentCacheBytes,
            "A single oversized decoded buffer bypassed the cache memory limit");
    }
    std::cout<<"PASS synchronous cached tabs, metadata-only reuse, reading position, forced reload, paused/missing/closed tabs, bounded LRU\n";
}

void SaveDuringDocumentRead(const fs::path& artifacts) {
    const auto root=artifacts/L"save-during-read";
    const auto first=root/L"first.md",second=root/L"second.md";
    const auto initial=LongDocument("Initial read snapshot");
    const auto latest=initial+"\nLast save must be observed after the pending read.\n";
    Write(first,initial);Write(second,LongDocument("Other tab"));
    Host host(root/L"session.ini");auto& app=host.app;
    app.OpenPaths({second.wstring()});LoadedPath(app,second);
    const auto secondDocument=app.currentDoc;
    app.OpenPaths({first.wstring()});LoadedPath(app,first);
    const auto a=IndexOf(app,first),b=IndexOf(app,second);
    // Remove real notification polling so the single explicit WM_TIMER below
    // is the only way the final save can cause a follow-up read.
    app.CancelWatchSetup();app.RetireWatches(std::move(app.watches));
    KillTimer(app.hwnd,1);KillTimer(app.hwnd,2);KillTimer(app.hwnd,4);
    auto takeCompletion=[&]{
        const auto deadline=GetTickCount64()+5000;
        while(GetTickCount64()<deadline){
            MSG message{};
            if(PeekMessageW(&message,app.hwnd,LoadDone,LoadDone,PM_REMOVE)){
                auto result=std::unique_ptr<Loaded>(reinterpret_cast<Loaded*>(message.lParam));
                if(result->cancel==app.readCancel)return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Cannot hold a real document completion before dispatch");
    };
    app.LoadActive(true,true);
    auto completed=takeCompletion();
    Check(completed->doc&&completed->doc->source==initial&&app.loading,
        "Save-during-read fixture did not retain the pre-save completion");
    const auto originalCancel=app.readCancel;
    Write(first,latest);
    SendMessageW(app.hwnd,WM_TIMER,2,0);
    SendMessageW(app.hwnd,WM_TIMER,2,0);
    Check(app.readRecheck==originalCancel,"Update notifications during a read were not coalesced");
    app.AcceptLoad(std::move(completed));
    Check(app.loading&&app.readCancel!=originalCancel&&!app.readRecheck,
        "Accepting the older snapshot did not schedule its pending metadata check");
    Until([&]{return !app.loading&&app.currentDoc&&app.currentDoc->source==latest;},
        "The final save during a read was lost after its only update notification");
    Check(!app.readRecheck,"Completed follow-up retained another unnecessary update request");

    // Cancellation must not lend the old tab's notification to a new request.
    app.LoadActive();completed=takeCompletion();SendMessageW(app.hwnd,WM_TIMER,2,0);
    const auto replacedCancel=app.readCancel;
    app.Switch(b);const auto switchedCancel=app.readCancel;
    Check(!app.readRecheck&&*replacedCancel,"Tab switch retained the old read's pending update");
    app.AcceptLoad(std::move(completed));
    Check(app.readCancel==switchedCancel&&!app.readRecheck&&app.currentDoc==secondDocument,
        "Cancelled completion transferred its update notification to another tab");
    LoadedPath(app,second);
    Check(app.readCancel==switchedCancel,"Switch destination inherited an unnecessary follow-up read");

    app.Switch(a);LoadedPath(app,first);
    app.LoadActive();completed=takeCompletion();SendMessageW(app.hwnd,WM_TIMER,2,0);
    app.Close(a);const auto closedCancel=app.readCancel;
    Check(!app.readRecheck,"Closing the active tab retained its pending update");
    app.AcceptLoad(std::move(completed));LoadedPath(app,second);
    Check(app.readCancel==closedCancel&&app.currentDoc==secondDocument,
        "Closed tab's completion restarted checking in its replacement tab");

    app.LoadActive();completed=takeCompletion();SendMessageW(app.hwnd,WM_TIMER,2,0);
    app.tabs[app.active].autoRefresh=false;app.LoadActive(false);
    const auto stoppedCancel=app.readCancel;
    app.AcceptLoad(std::move(completed));app.CheckActive();
    Check(!app.loading&&!app.readRecheck&&app.readCancel==stoppedCancel&&*stoppedCancel
        &&app.currentDoc==secondDocument,"Pausing a read did not cancel its pending follow-up update");
    std::cout<<"PASS last save during asynchronous load, coalesced follow-up, switch/close/pause cancellation\n";
}

void WatchUpdates(App& app, const fs::path& root) {
    const auto activePath = fs::path(app.tabs[app.active].path);
    Until([&] { return WatchCoversFolder(app, root) && WatchCoversFolder(app, activePath.parent_path()); },
        "Directory watches were not created");
    PumpFor(800); // Drain earlier tree/content notifications before measuring.
    const auto generation = Folder(app, root).generation;
    const auto updated = LongDocument("Updated") + "\nwatch-content-update-token\n";
    Write(activePath, updated);
    Until([&] { return !app.loading && app.currentDoc && app.currentDoc->source == updated; }, "Content watch did not reload active document");
    PumpFor(750);
    Check(Folder(app, root).generation == generation, "Pure content edit caused unnecessary recursive tree scan");

    const auto added = root / L"new-branch" / L"created.md";
    Write(added, "# Added Markdown\n\nlayout-marker\n");
    Until([&] { return Folder(app, root).generation > generation && TreeItem(app, added) != nullptr; }, "New Markdown did not update folder tree");
    Check(TreeItem(app, added.parent_path()) != nullptr, "New Markdown ancestor was not added");
    const auto afterAddition = Folder(app, root).generation;
    Check(fs::remove(added), "Cannot remove generated Markdown fixture");
    Until([&] { return Folder(app, root).generation > afterAddition && TreeItem(app, added.parent_path()) == nullptr; }, "Removing last Markdown did not prune folder");
    Check(app.currentDoc && app.currentDoc->path == activePath.wstring(), "Tree refresh changed the active document");
    Check(!IsWindowVisible(app.hwnd), "A host test unexpectedly showed its window");
    std::cout << "PASS watched content reload without tree scan, structural refresh, pruning after deletion\n";
}

void FolderListRemoval(App& app, const fs::path& root) {
    const auto ancestor = root / L"ancestor";
    const auto descendant = ancestor / L"deep" / L"three.MD";
    const auto sibling = root / L"ancestor-other" / L"keep.md";
    Write(sibling, "# Keep this sibling\n");
    app.RefreshTree();
    Until([&] { return TreeItem(app, sibling) != nullptr; }, "Sibling fixture did not appear");
    app.OpenPaths({descendant.wstring()});
    LoadedPath(app, descendant);
    const auto document = app.currentDoc;
    const auto tabCount = app.tabs.size();
    const auto tabId = app.tabs[app.active].id;
    const auto contents = mm::ReadDocument(descendant.wstring()).source;

    app.Command(ExpandAll);
    TreeView_SelectItem(app.treeH, TreeItem(app, root));
    auto itemPoint = [&](const fs::path& path) {
        RECT bounds{};
        Check(TreeView_GetItemRect(app.treeH, TreeItem(app, path), &bounds, TRUE) != FALSE,
            "Cannot locate context-menu fixture item");
        POINT point{bounds.left + 2, (bounds.top + bounds.bottom) / 2};
        Check(ClientToScreen(app.treeH, &point) != FALSE, "Cannot map tree hit-test point");
        return point;
    };
    const auto clickedFolder = app.FolderAt(itemPoint(ancestor));
    Check(clickedFolder == mm::NormalizePath(ancestor.wstring()), "Context menu used selection instead of clicked folder");
    Check(TreeView_GetSelection(app.treeH) == TreeItem(app, root), "Hit testing changed the selected item");
    Check(app.FolderAt(itemPoint(descendant)).empty(), "Markdown file received folder removal action");
    POINT empty{-5, -5};
    ClientToScreen(app.treeH, &empty);
    Check(app.FolderAt(empty).empty(), "Empty tree area received folder removal action");
    HMENU menu = app.TreeMenu(true);
    wchar_t label[64]{};
    const int length = GetMenuStringW(menu, RemoveFromList, label, 64, MF_BYCOMMAND);
    DestroyMenu(menu);
    Check(length > 0 && std::wstring(label) == L"リストから削除", "Folder removal menu label differs from requested text");
    menu = app.TreeMenu(false);
    const UINT nonFolderAction = GetMenuState(menu, RemoveFromList, MF_BYCOMMAND);
    DestroyMenu(menu);
    Check(nonFolderAction == UINT(-1), "Non-folder menu exposes folder removal action");

    // Model pending scan/read work and a dirty debounce timer while the popup
    // is open. Keep completed messages queued until after list removal.
    app.RefreshTree();
    const auto nestedGeneration = Folder(app, root).generation;
    const auto nestedCancel = Folder(app, root).cancel;
    app.LoadActive();
    const auto pendingLoadGeneration = app.loadGeneration;
    const auto pendingReadCancel = app.readCancel;
    Check(app.loading, "Cannot create pending active read before folder removal");
    app.treeDirty = true;
    Check(SetTimer(app.hwnd, 2, 50, nullptr) != 0, "Cannot create pending refresh timer");
    auto queued = std::make_unique<Scanned>();
    queued->folderId = Folder(app, root).id;
    queued->generation = Folder(app, root).generation;
    queued->tree = std::make_unique<mm::FolderNode>(mm::ScanFolder(root.wstring()));
    Check(PostMessageW(app.hwnd, TreeDone, 0, reinterpret_cast<LPARAM>(queued.get())) != FALSE,
        "Cannot queue completed folder scan");
    queued.release();
    auto queuedLoad = std::make_unique<Loaded>();
    queuedLoad->id = tabId;
    queuedLoad->generation = pendingLoadGeneration;
    queuedLoad->doc = std::make_shared<mm::Document>(*document);
    queuedLoad->doc->source = "# Stale read must never be installed\n";
    Check(PostMessageW(app.hwnd, LoadDone, 0, reinterpret_cast<LPARAM>(queuedLoad.get())) != FALSE,
        "Cannot queue completed document read");
    queuedLoad.release();
    const auto pendingWatchGeneration = app.watchGeneration;
    const auto pendingWatchCancel = app.watchCancel;
    auto staleWatches = [&] {
        auto result = std::make_unique<Watched>();
        result->generation = pendingWatchGeneration;
        result->plan[clickedFolder] = {true, true};
        auto forbiddenWatch = std::make_unique<Watch>();
        forbiddenWatch->path = clickedFolder;
        forbiddenWatch->recursive = true;
        forbiddenWatch->treeWatch = true;
        result->watches.push_back(std::move(forbiddenWatch));
        return result;
    };
    auto queuedWatches = staleWatches();
    const auto queuedWatchResult = reinterpret_cast<LPARAM>(queuedWatches.get());
    Check(PostMessageW(app.hwnd, WatchDone, 0, queuedWatchResult) != FALSE,
        "Cannot queue completed watcher setup");
    queuedWatches.release();
    app.RemoveFolderFromList(clickedFolder);
    Check(Folder(app, root).generation > nestedGeneration && nestedCancel && *nestedCancel,
        "Nested folder removal did not immediately invalidate and cancel its scan");
    Check(app.loadGeneration > pendingLoadGeneration && pendingReadCancel && *pendingReadCancel && !app.loading,
        "Nested folder removal did not immediately cancel its pending document read");
    Check(!app.treeDirty, "Nested folder removal retained pending tree refresh work");
    Check(app.watchGeneration > pendingWatchGeneration && pendingWatchCancel && *pendingWatchCancel,
        "Nested folder removal did not invalidate and cancel pending watcher setup");
    Check(TreeItem(app, ancestor) == nullptr && TreeItem(app, descendant) == nullptr, "Removed folder remains in sidebar");
    Check(TreeItem(app, sibling) != nullptr, "Removing folder also removed similarly named sibling");
    Check(app.tabs.size() == tabCount && app.tabs[app.active].id == tabId && app.currentDoc == document,
        "Folder removal changed the open tab or displayed document");
    Check(fs::is_directory(ancestor) && mm::ReadDocument(descendant.wstring()).source == contents,
        "Removing a folder from the list modified its filesystem contents");
    Check(!app.tabs[app.active].autoRefresh, "Removed folder's active tab kept automatic refresh enabled");
    Check(!WatchTouchesFolder(app, ancestor), "A watch still covers the removed folder or its descendants");
    // A worker may finish posting just after cancellation drained the queue.
    // Its old generation must still be rejected when delivered afterward.
    SendMessageW(app.hwnd, WatchDone, 0, reinterpret_cast<LPARAM>(staleWatches().release()));
    Check(!WatchTouchesFolder(app, ancestor), "Stale watcher completion reattached the removed folder");
    Pump();
    Check(TreeItem(app, ancestor) == nullptr, "Already completed scan restored the removed folder");
    Check(app.currentDoc == document, "Already completed read replaced the retained document");
    Until([&] { return WatchCoversFolder(app, sibling.parent_path()); },
        "Removing a folder stopped watching its retained sibling");

    const auto pausedReadCancel = app.readCancel;
    app.LoadActive(false); // Startup and unrelated tab closure must remain idle.
    Check(!app.loading && app.currentDoc == document && app.readCancel == pausedReadCancel,
        "Automatic loading created read work for the paused tab");
    const auto stoppedScanGeneration = Folder(app, root).generation;
    const auto stoppedLoadGeneration = app.loadGeneration;
    Write(descendant, contents + "\nremoved-folder-content-must-not-reload\n");
    Write(ancestor / L"new-subfolder" / L"ignored.md", "# Removed folder must not be explored\n");
    app.CheckActive(); // Window activation must also leave a paused tab alone.
    PumpFor(900);
    Check(Folder(app, root).generation == stoppedScanGeneration && app.loadGeneration == stoppedLoadGeneration,
        "Removed folder activity restarted folder exploration or document loading");
    Check(app.currentDoc == document && TreeItem(app, ancestor) == nullptr,
        "Removed folder activity changed the retained document or tree");
    Write(descendant, contents);

    const auto fresh = sibling.parent_path() / L"fresh.md";
    Write(fresh, "# Refresh completion marker\n");
    Until([&] { return TreeItem(app, fresh) != nullptr; }, "Retained sibling watch did not update the folder tree");
    Check(Folder(app, root).generation > stoppedScanGeneration, "Retained sibling change did not schedule a scan");
    Check(app.loadGeneration == stoppedLoadGeneration && app.currentDoc == document,
        "Retained sibling change reloaded the removed folder's document");
    Check(TreeItem(app, ancestor) == nullptr, "Retained sibling refresh restored the removed folder");
    Check(!WatchTouchesFolder(app, ancestor), "Tree refresh recreated a watch for the removed folder");

    // The directory notification can arrive before its new watch is installed;
    // a Markdown file created during that gap still needs the catch-up scan.
    const auto newlyCreated = root / L"empty-sibling-created-after-exclusion";
    Check(fs::create_directory(newlyCreated), "Cannot create empty sibling fixture");
    const auto gapFile = newlyCreated / L"created-during-watch-setup.md";
    Write(gapFile, "# New sibling watch setup gap\n");
    Until([&] { return TreeItem(app, gapFile) != nullptr; },
        "Markdown created before new sibling watch installation was missed");
    Until([&] { return WatchCoversFolder(app, newlyCreated); }, "New sibling directory did not receive a watch");
    Check(!WatchTouchesFolder(app, ancestor) && TreeItem(app, ancestor) == nullptr,
        "New sibling watcher setup resumed the excluded folder");
    Check(app.loadGeneration == stoppedLoadGeneration && app.currentDoc == document,
        "New sibling watcher setup reloaded the paused document");

    // Restored paused tabs can point to a file that no longer exists. Automatic
    // loading must not probe it or replace retained content with an error page.
    const auto activePath = app.tabs[app.active].path;
    app.tabs[app.active].path = (ancestor / L"missing-restored-paused-tab.md").wstring();
    app.LoadActive(false);
    Check(app.readCancel == pausedReadCancel && !app.loading && app.currentDoc == document,
        "Automatic load tried to read a missing restored paused tab");
    app.tabs[app.active].path = activePath;
    app.LoadActive();
    LoadedPath(app, descendant);
    Check(app.loadGeneration > stoppedLoadGeneration && app.currentDoc->source == contents
        && !app.tabs[app.active].autoRefresh, "Manual loading failed or resumed removed folder monitoring");
    app.Save();
    const auto activeSection = L"Tab" + std::to_wstring(app.active);
    Check(GetPrivateProfileIntW(activeSection.c_str(), L"AutoRefresh", 99, app.ini.c_str()) == 0,
        "Session did not persist the removed tab's paused automatic refresh");
    app.excludedFolders.clear();
    app.RestoreExcludedFolders();
    Check(app.excludedFolders.contains(clickedFolder), "Session did not restore excluded folder");
    auto upperCase = clickedFolder;
    CharUpperBuffW(upperCase.data(), static_cast<DWORD>(upperCase.size()));
    Check(app.excludedFolders.contains(upperCase), "Excluded folder matching is case sensitive");
    const auto restored = sibling.parent_path() / L"restored.md";
    Write(restored, "# Restored session refresh marker\n");
    app.RefreshTree();
    Until([&] { return TreeItem(app, restored) != nullptr; }, "Restored session refresh did not complete");
    Check(TreeItem(app, ancestor) == nullptr, "Session restoration forgot folder exclusion");

    app.OpenRoot(root.wstring());
    Until([&] { return TreeItem(app, descendant) != nullptr; }, "Reopening root did not restore excluded folder");
    Check(app.excludedFolders.empty(), "Explicit folder open kept old exclusions");
    Check(app.tabs[app.active].autoRefresh, "Explicit folder open did not resume automatic refresh for its descendant");
    Until([&] { return WatchCoversFolder(app, ancestor); },
        "Explicit folder open did not reinstall the descendant's directory watch");
    PumpFor(800); // Drain notifications that belong to the explicitly reopened root.
    LoadedPath(app, descendant);
    const auto rootDocument = app.currentDoc;
    app.RefreshTree();
    queued = std::make_unique<Scanned>();
    queued->folderId = Folder(app, root).id;
    queued->generation = Folder(app, root).generation;
    queued->tree = std::make_unique<mm::FolderNode>(mm::ScanFolder(root.wstring()));
    Check(PostMessageW(app.hwnd, TreeDone, 0, reinterpret_cast<LPARAM>(queued.get())) != FALSE,
        "Cannot queue root scan before removal");
    queued.release();
    const auto rootId = Folder(app, root).id;
    const auto rootCancel = Folder(app, root).cancel;
    app.treeDirty = true;
    Check(SetTimer(app.hwnd, 2, 50, nullptr) != 0, "Cannot create pending root refresh timer");
    app.RemoveFolderFromList(root.wstring());
    Check(!app.HasFolders() && !FolderTree(app, root) && TreeView_GetCount(app.treeH) == 0,
        "Removing root did not clear the folder list");
    Check(app.FindFolder(rootId) == nullptr && rootCancel && *rootCancel && !app.treeDirty,
        "Removing root did not invalidate and cancel its scan");
    Check(app.tabs.size() == tabCount && app.tabs[app.active].id == tabId && app.currentDoc == rootDocument,
        "Removing root changed open tabs or displayed document");
    Check(!WatchTouchesFolder(app, root), "Root removal retained a watch for its open descendants");
    Check(std::none_of(app.tabs.begin(), app.tabs.end(), [&](const auto& tab) {
        return App::IsWithinFolder(tab.path, mm::NormalizePath(root.wstring())) && tab.autoRefresh;
    }), "Root removal left a descendant tab's automatic refresh enabled");
    Pump();
    Check(!app.HasFolders() && !FolderTree(app, root) && TreeView_GetCount(app.treeH) == 0,
        "Queued scan resurrected removed root");
    Check(fs::is_directory(root) && mm::ReadDocument(descendant.wstring()).source == contents,
        "Root removal modified filesystem contents");
    const auto stoppedRootLoadGeneration = app.loadGeneration;
    Write(root / L"after-root-removal.md", "# Removed root must stay idle\n");
    Write(descendant, contents + "\nremoved-root-content-must-not-reload\n");
    app.CheckActive();
    PumpFor(900);
    Check(!app.HasFolders() && app.loadGeneration == stoppedRootLoadGeneration,
        "Removed root activity restarted folder exploration or document loading");
    Check(app.currentDoc == rootDocument && !app.HasFolders() && !FolderTree(app, root),
        "Removed root activity replaced the retained document or tree");
    Write(descendant, contents);
    app.Save();
    Check(GetPrivateProfileIntW(L"App", L"Roots", 9999, app.ini.c_str()) == 0,
        "Removed root was retained in saved session");

    app.OpenRoot(root.wstring());
    Until([&] { return TreeItem(app, descendant) != nullptr; }, "Cannot reopen root for stale context target check");
    const auto newRoot = root.parent_path() / L"new-root";
    const auto newFile = newRoot / L"new.md";
    Write(newFile, "# New root\n");
    const auto previousTree = FolderTree(app, root);
    app.OpenRoot(newRoot.wstring());
    Check(app.folders.size() == 2 && FolderTree(app, root) == previousTree && TreeItem(app, descendant) != nullptr,
        "Adding another folder replaced the previous folder's visible tree");
    // Removing an old folder's child must leave the newly added root intact.
    app.RemoveFolderFromList(clickedFolder);
    Check(app.FindFolder(root.wstring()) && app.FindFolder(newRoot.wstring())
        && app.excludedFolders.contains(clickedFolder) && TreeItem(app, newRoot) != nullptr,
        "Removing a child from an earlier folder altered the newly added registration");
    Until([&] { return TreeItem(app, newFile) != nullptr; }, "New root scan did not complete");
    Check(TreeItem(app, newFile) != nullptr && fs::exists(newFile), "Stale popup target damaged new root");
    Check(!IsWindowVisible(app.hwnd), "Folder removal test unexpectedly showed its window");
    std::cout << "PASS folder list removal, immediate scan/read cancellation, isolated sibling watches, paused tabs, stale completions\n";
}

void BoundedWatcherSetup(App& app, const fs::path& fixtureRoot) {
    while (!app.tabs.empty()) app.Close(static_cast<int>(app.tabs.size()) - 1);
    RemoveAllFolders(app);
    const auto root = fixtureRoot.parent_path() / L"bounded-watch-fixture";
    std::vector<fs::path> siblings;
    for (int index = 0; index < 80; ++index) {
        const auto folder = root / (L"sibling-" + std::to_wstring(index));
        Write(folder / L"seed.md", "# Bounded watcher fixture\n");
        siblings.push_back(folder);
    }
    const auto excluded = root / L"excluded";
    const auto excludedFile = excluded / L"deep" / L"keep-on-disk.md";
    Write(excludedFile, "# Stop all work inside this folder\n");
    app.OpenRoot(root.wstring());
    Until([&] {
        return TreeItem(app, excludedFile) != nullptr && !app.watchLimited
            && std::any_of(app.watches.begin(), app.watches.end(), [&](const auto& watch) {
                return watch->recursive && watch->path == mm::NormalizePath(root.wstring());
            });
    }, "Wide folder fixture did not install its initial recursive watch");

    // Hold only the watch worker. Removing the folder must return and clear
    // unsafe watches while its replacement plan is still unable to execute.
    // This checks the asynchronous boundary without a machine-speed threshold.
    struct Gate { std::atomic_bool started{false}, released{false}; };
    const auto gate = std::make_shared<Gate>();
    struct ReleaseGate {
        std::shared_ptr<Gate> gate;
        ~ReleaseGate() { gate->released = true; }
    } release{gate};
    app.watchWorker.Queue([gate] {
        gate->started = true;
        while (!gate->released) Sleep(1);
    });
    Until([&] { return gate->started.load(); }, "Cannot hold watcher setup worker");
    const auto previousWatchGeneration = app.watchGeneration;
    app.RemoveFolderFromList(excluded.wstring());
    Check(app.watchGeneration > previousWatchGeneration, "Removal did not queue a fresh bounded watcher plan");
    Check(!gate->released && app.watches.empty(), "Removal waited for watcher planning or retained an unsafe root watch");
    Check(TreeItem(app, excluded) == nullptr && !WatchTouchesFolder(app, excluded),
        "Wide fixture removal retained the excluded folder or its watch");
    gate->released = true;
    Until([&] { return app.watchLimited && !app.watches.empty(); }, "Wide watcher plan did not report its bounded fallback");
    Check(app.watches.size() <= 64, "Wide folder fixture exceeded the watcher handle limit");
    Check(!WatchTouchesFolder(app, excluded), "Bounded watcher installation covered the excluded folder");
    const auto unwatched = std::find_if(siblings.begin(), siblings.end(), [&](const auto& folder) {
        return !WatchCoversFolder(app, folder);
    });
    Check(unwatched != siblings.end(), "Wide fixture did not exercise any folder beyond the watcher limit");

    const auto manual = *unwatched / L"manual-refresh.md";
    Write(manual, "# Manual refresh includes folders beyond the watch limit\n");
    app.RefreshTree();
    Until([&] { return TreeItem(app, manual) != nullptr; }, "Manual refresh omitted an unwatched sibling");
    Check(app.watches.size() <= 64 && TreeItem(app, excluded) == nullptr && !WatchTouchesFolder(app, excluded),
        "Manual refresh exceeded watcher limits or restored excluded work");

    const auto fallback = *unwatched / L"periodic-fallback.md";
    Write(fallback, "# Periodic fallback includes retained unwatched folders\n");
    Until([&] { return app.watchLimited; }, "Manual refresh lost bounded watcher fallback state");
    SendMessageW(app.hwnd, WM_TIMER, 4, 0); // Exercise the periodic callback without waiting 30 seconds.
    Until([&] { return TreeItem(app, fallback) != nullptr; }, "Periodic fallback omitted an unwatched sibling");
    Check(app.watches.size() <= 64 && TreeItem(app, excluded) == nullptr && !WatchTouchesFolder(app, excluded),
        "Periodic fallback exceeded watcher limits or restored excluded work");
    Check(fs::exists(excludedFile), "Bounded watcher test modified the excluded fixture file");
    app.RemoveFolderFromList(root.wstring());
    Check(!WatchTouchesFolder(app, root), "Wide root removal retained directory watches");
    std::cout << "PASS asynchronous bounded watcher setup, 64-handle limit, unwatched-folder refresh, excluded-safe fallback\n";
}

void MultipleFolderRegistrations(const fs::path& artifacts) {
    const auto fixture = artifacts / L"multiple-folder-fixture";
    const auto first = fixture / L"資料A", second = fixture / L"資料B";
    const auto third = fixture / L"pending-C", fourth = fixture / L"pending-D";
    const auto firstFile = first / L"retained" / L"first.md";
    const auto excluded = first / L"excluded";
    const auto secondFile = second / L"second.md";
    Write(firstFile, "# First retained document\n");
    Write(excluded / L"hidden.md", "# Existing exclusion\n");
    Write(secondFile, "# Second retained document\n");
    Write(third / L"third.md", "# Third pending registration\n");
    Write(fourth / L"fourth.md", "# Fourth pending registration\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.OpenRoot(first.wstring());
    Until([&] { return TreeItem(app, firstFile) != nullptr; }, "First folder did not scan");
    app.OpenPaths({firstFile.wstring()});
    LoadedPath(app, firstFile);
    app.Command(ExpandAll);
    app.RemoveFolderFromList(excluded.wstring());
    Until([&] { return WatchCoversFolder(app, firstFile.parent_path()); }, "First retained branch did not receive its watch");
    const auto firstTree = FolderTree(app, first);
    const auto firstItem = TreeItem(app, first);
    const auto firstGeneration = Folder(app, first).generation;
    const auto firstDocument = app.currentDoc;
    const auto expandedBranch = mm::NormalizePath(firstFile.parent_path().wstring());
    Check(app.expanded.contains(expandedBranch), "First folder's expanded branch was not retained");
    app.OpenRoot(second.wstring());
    Check(app.folders.size() == 2 && FolderTree(app, first) == firstTree && !FolderTree(app, second)
        && TreeItem(app, first) == firstItem && TreeItem(app, firstFile) != nullptr && TreeItem(app, second) != nullptr,
        "Adding a folder replaced existing branches instead of appending a placeholder");
    Check(app.currentDoc == firstDocument && app.expanded.contains(expandedBranch)
        && app.excludedFolders.contains(mm::NormalizePath(excluded.wstring())) && TreeItem(app, excluded) == nullptr,
        "Adding another folder changed the current document, expansion state, or exclusions");
    Until([&] { return TreeItem(app, secondFile) != nullptr && WatchCoversFolder(app, second); },
        "Second folder did not independently finish scanning and watching");
    Check(Folder(app, first).generation == firstGeneration && FolderTree(app, first) == firstTree
        && TreeItem(app, first) == firstItem,
        "Adding an independent folder unnecessarily restarted the existing folder scan");
    const auto secondId = Folder(app, second).id;
    const auto secondBeforeReopen = Folder(app, second).generation;
    auto duplicate = mm::NormalizePath((second / L".." / second.filename()).wstring());
    CharUpperBuffW(duplicate.data(), static_cast<DWORD>(duplicate.size()));
    app.OpenRoot(duplicate);
    Check(app.folders.size() == 2 && Folder(app, second).id == secondId,
        "Normalized case-insensitive duplicate folder created another registration");
    Check(app.currentDoc == firstDocument && app.excludedFolders.contains(mm::NormalizePath(excluded.wstring()))
        && app.expanded.contains(expandedBranch), "Reopening a different root changed the first root's state");
    Until([&] { return Folder(app, second).generation > secondBeforeReopen
        && !Folder(app, second).scanning && TreeItem(app, secondFile) != nullptr; },
        "Reopened second folder did not settle");
    int topLevel = 0;
    for (auto item = TreeView_GetRoot(app.treeH); item; item = TreeView_GetNextSibling(app.treeH, item)) ++topLevel;
    Check(topLevel == 2, "The two registered folders were not displayed as two top-level branches");

    app.Save();
    const auto savedPaths = app.FolderPaths();
    Check(GetPrivateProfileIntW(L"App", L"Roots", 9999, app.ini.c_str()) == 2 && app.ReadIni(L"App", L"Root").empty(),
        "Multiple folders were not saved using the new registration list");
    {
        App restored;
        restored.ini = app.ini;
        restored.RestoreFolders();
        restored.RestoreExcludedFolders();
        Check(restored.FolderPaths() == savedPaths && restored.folders.size() == 2,
            "Restart restoration lost folder order or a registered root");
        Check(restored.folders[0]->id != restored.folders[1]->id
            && restored.folders[0]->pending.path == savedPaths[0] && restored.folders[1]->pending.path == savedPaths[1],
            "Restored roots do not have independent identities and placeholders");
        Check(restored.excludedFolders.contains(mm::NormalizePath(excluded.wstring())),
            "Restart restoration lost an existing root's excluded branch");
    }
    const auto legacyIni = fixture / L"legacy-session.ini";
    Write(legacyIni, std::string("\xff\xfe", 2));
    Check(WritePrivateProfileStringW(L"App", L"Root", first.c_str(), legacyIni.c_str()) != FALSE,
        "Cannot write legacy single-root session fixture");
    {
        App legacy;
        legacy.ini = legacyIni.wstring();
        legacy.RestoreFolders();
        Check(legacy.folders.size() == 1 && legacy.folders.front()->path == savedPaths[0],
            "Legacy Root entry did not migrate to the folder registration list");
        Check(WritePrivateProfileStringW(L"App", L"Roots", L"0", legacyIni.c_str()) != FALSE,
            "Cannot write explicitly empty root list");
        legacy.RestoreFolders();
        Check(!legacy.HasFolders(), "An explicitly empty root list restored the obsolete legacy Root");
    }

    app.OpenPaths({secondFile.wstring()});
    LoadedPath(app, secondFile);
    app.Switch(IndexOf(app, firstFile));
    LoadedPath(app, firstFile);
    Until([&] { return WatchCoversFolder(app, second); }, "Second root watch disappeared before removal test");
    const auto retainedDocument = app.currentDoc;
    const auto retainedSecondTree = FolderTree(app, second);
    app.RefreshFolder(Folder(app, first));
    const auto removedId = Folder(app, first).id;
    const auto removedCancel = Folder(app, first).cancel;
    app.RemoveFolderFromList(first.wstring());
    Check(app.folders.size() == 1 && !app.FindFolder(removedId) && removedCancel && *removedCancel,
        "Removing one registered root did not cancel and remove only its registration");
    Check(FolderTree(app, second) == retainedSecondTree && TreeItem(app, secondFile) != nullptr
        && WatchCoversFolder(app, second) && !WatchTouchesFolder(app, first),
        "Removing the first root interrupted the second root's tree or existing watcher");
    Check(app.currentDoc == retainedDocument && !app.tabs[IndexOf(app, firstFile)].autoRefresh
        && app.tabs[IndexOf(app, secondFile)].autoRefresh, "Root removal changed retained tabs or paused an unrelated tab");
    const auto stoppedLoadGeneration = app.loadGeneration;
    const auto remainingSecondGeneration = Folder(app, second).generation;
    Write(first / L"ignored-after-removal.md", "# Removed root must remain idle\n");
    Write(firstFile, "# Removed root update must remain paused\n");
    PumpFor(900);
    Check(!app.FindFolder(removedId) && Folder(app, second).generation == remainingSecondGeneration
        && app.loadGeneration == stoppedLoadGeneration && app.currentDoc == retainedDocument,
        "Removed root activity restarted work in another root or its retained document");
    const auto freshSecond = second / L"added-after-other-root-removal.md";
    Write(freshSecond, "# Still watched independently\n");
    Until([&] { return TreeItem(app, freshSecond) != nullptr; }, "Remaining root stopped detecting new Markdown files");
    app.Switch(IndexOf(app, secondFile));
    LoadedPath(app, secondFile);
    const std::string changedSecond = "# Second document still updates automatically\n";
    Write(secondFile, changedSecond);
    Until([&] { return !app.loading && app.currentDoc && app.currentDoc->source == changedSecond; },
        "Remaining root's document stopped updating automatically");

    struct Gate { std::atomic_bool started{false}, released{false}; };
    const auto gate = std::make_shared<Gate>();
    struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->released = true; } } release{gate};
    for (auto& worker : app.scanners) worker.Queue([gate] { gate->started = true; while (!gate->released) Sleep(1); });
    Until([&] { return gate->started.load(); }, "Cannot hold multi-folder scan worker");
    app.RefreshFolder(Folder(app, second));
    const auto pendingSecondGeneration = Folder(app, second).generation;
    app.OpenRoot(third.wstring());
    app.RefreshFolder(Folder(app, third));
    const auto oldThirdId = Folder(app, third).id;
    const auto oldThirdGeneration = Folder(app, third).generation;
    const auto oldThirdCancel = Folder(app, third).cancel;
    app.OpenRoot(fourth.wstring());
    app.RefreshFolder(Folder(app, fourth));
    Check(app.folders.size() == 3 && FolderTree(app, second) != nullptr && !FolderTree(app, third)
        && !FolderTree(app, fourth), "Adding a root during another scan dropped existing or pending registrations");
    app.RemoveFolderFromList(third.wstring());
    Check(!app.FindFolder(oldThirdId) && oldThirdCancel && *oldThirdCancel
        && Folder(app, second).scanning && Folder(app, second).cancel && !*Folder(app, second).cancel
        && Folder(app, second).generation >= pendingSecondGeneration && app.FindFolder(fourth.wstring()),
        "Removing one pending scan abandoned another registration's queued scan");
    app.OpenRoot(third.wstring());
    Check(Folder(app, third).id != oldThirdId, "Re-added folder reused a removed registration's identity");
    auto stale = std::make_unique<Scanned>();
    stale->folderId = oldThirdId;
    stale->generation = oldThirdGeneration;
    stale->tree = std::make_unique<mm::FolderNode>();
    stale->tree->path = mm::NormalizePath(third.wstring());
    stale->tree->name = third.filename().wstring();
    stale->tree->directory = true;
    SendMessageW(app.hwnd, TreeDone, 0, reinterpret_cast<LPARAM>(stale.release()));
    Check(!FolderTree(app, third), "Old completion was accepted into a newly registered folder at the same path");
    gate->released = true;
    Until([&] {
        return !Folder(app, second).scanning && TreeItem(app, secondFile) != nullptr
            && TreeItem(app, third / L"third.md") != nullptr && TreeItem(app, fourth / L"fourth.md") != nullptr;
    }, "Independent queued root scans did not all finish after adding and removing another root");
    Check(app.folders.size() == 3 && fs::exists(firstFile) && fs::exists(excluded / L"hidden.md"),
        "Multi-folder operations lost a registration or deleted fixture files");
    Check(!IsWindowVisible(app.hwnd), "Multiple-folder test unexpectedly showed its host window");
    std::cout << "PASS independent folder registrations, preserved branches/state, restart migration, selective removal, queued scan identities\n";
}

void StartupRegistrationChecks(const fs::path& artifacts) {
    const auto root = artifacts / L"startup-registration-fixture";
    const auto existing = root / L"existing.md";
    const auto paused = root / L"paused.md";
    const auto missing = root / L"missing.md";
    const auto directoryAsFile = root / L"directory.md";
    const auto missingRoot = root / L"missing-root";
    Write(existing, "# Existing registration\n");
    Write(paused, "# Paused registration metadata only\n");
    Check(fs::create_directory(directoryAsFile), "Cannot create wrong-type registration fixture");
    Host host(root / L"session.ini");
    auto& app = host.app;
    Check(WritePrivateProfileStringW(L"App", L"Root", missingRoot.c_str(), app.ini.c_str()) != FALSE,
        "Cannot create isolated legacy folder registration");
    app.RestoreFolders();
    app.ApplyTree(nullptr);
    const std::vector<fs::path> paths{existing, paused, missing, directoryAsFile};
    for (size_t index = 0; index < paths.size(); ++index) {
        Tab tab;
        tab.path = mm::NormalizePath(paths[index].wstring());
        tab.id = app.nextId++;
        tab.autoRefresh = index != 1;
        tab.registrationIssue = L"以前の存在確認結果";
        app.tabs.push_back(std::move(tab));
    }
    app.active = 1;
    app.RebuildTabs();
    const auto loadGeneration = app.loadGeneration;
    app.StartStartupCheck();
    Check(app.startupChecking, "Startup registration check did not start asynchronously");
    Until([&] { return !app.startupChecking; }, "Startup registration metadata check did not complete");
    Check(Folder(app, missingRoot).issue == L"フォルダが見つかりません", "Missing registered root was not marked for confirmation");
    Check(app.tabs[0].registrationIssue.empty() && app.tabs[1].registrationIssue.empty(),
        "Existing or paused existing file was incorrectly marked missing");
    Check(app.tabs[2].registrationIssue == L"ファイルが見つかりません", "Missing registered tab was not marked for confirmation");
    Check(app.tabs[3].registrationIssue == L"ファイルではありません", "Directory registered as a file was not identified");
    Check(app.TabTitle(2).find(L"[要確認]") != std::wstring::npos
        && app.TabTitle(3).find(L"[要確認]") != std::wstring::npos
        && app.TabTitle(1).find(L"[要確認]") == std::wstring::npos, "Registration confirmation markers are incorrect");
    Check(app.tabs.size() == paths.size() && app.active == 1 && !app.tabs[1].autoRefresh,
        "Existence checks removed registrations or resumed a paused tab");
    Check(!app.readCancel && !app.currentDoc && !app.loading && app.loadGeneration == loadGeneration,
        "Startup existence checks read document bodies or changed the displayed document");
    app.Save();
    Check(app.ReadIni(L"Roots", L"0") == mm::NormalizePath(missingRoot.wstring()),
        "Saving an unavailable root removed its registration");
    for (size_t index = 0; index < paths.size(); ++index) {
        const auto section = L"Tab" + std::to_wstring(index);
        Check(app.ReadIni(section.c_str(), L"Path") == mm::NormalizePath(paths[index].wstring()),
            "Saving registration status changed or dropped a registered file path");
    }
    Check(GetPrivateProfileIntW(L"Tab1", L"AutoRefresh", 99, app.ini.c_str()) == 0,
        "Startup existence check did not preserve paused refresh in the session");

    app.StartStartupCheck();
    const auto cancelGeneration = app.startupGeneration;
    const auto cancel = app.startupCancel;
    app.CancelStartupCheck();
    Check(cancel && *cancel && app.startupGeneration > cancelGeneration && !app.startupChecking,
        "CancelStartupCheck did not cancel and invalidate startup work");
    auto stale = std::make_unique<StartupChecked>();
    stale->generation = cancelGeneration;
    stale->checks = {
        {Folder(app, missingRoot).path, true, mm::PathPresence::Exists, ERROR_SUCCESS},
        {app.tabs[0].path, false, mm::PathPresence::WrongType, ERROR_SUCCESS}
    };
    SendMessageW(app.hwnd, StartupDone, 0, reinterpret_cast<LPARAM>(stale.release()));
    Pump();
    Check(Folder(app, missingRoot).issue == L"フォルダが見つかりません" && app.tabs[0].registrationIssue.empty(),
        "Cancelled startup completion overwrote current registration status");
    Check(!app.readCancel && !app.currentDoc && app.loadGeneration == loadGeneration,
        "Cancelled startup metadata work triggered a document read");

    Write(missing, "# Recovered registration\n");
    app.active = 2;
    app.RebuildTabs();
    app.LoadActive();
    LoadedPath(app, missing);
    Check(app.tabs[2].registrationIssue.empty() && app.TabTitle(2).find(L"[要確認]") == std::wstring::npos,
        "Successful manual file loading retained its stale registration issue");

    app.OpenRoot(root.wstring());
    Until([&] { return TreeItem(app, missing) != nullptr; }, "Startup cancellation fixture root did not open");
    const auto document = app.currentDoc;
    std::vector<std::wstring> issues;
    for (const auto& tab : app.tabs) issues.push_back(tab.registrationIssue);
    app.StartStartupCheck();
    const auto removedGeneration = app.startupGeneration;
    const auto removedCancel = app.startupCancel;
    stale = std::make_unique<StartupChecked>();
    stale->generation = removedGeneration;
    stale->checks = {
        {Folder(app, root).path, true, mm::PathPresence::Missing, ERROR_PATH_NOT_FOUND},
        {app.tabs[2].path, false, mm::PathPresence::Missing, ERROR_FILE_NOT_FOUND}
    };
    Check(PostMessageW(app.hwnd, StartupDone, 0, reinterpret_cast<LPARAM>(stale.get())) != FALSE,
        "Cannot queue startup existence result before root removal");
    stale.release();
    app.RemoveFolderFromList(root.wstring());
    Check(removedCancel && *removedCancel && app.startupGeneration > removedGeneration && !app.startupChecking,
        "List removal did not immediately cancel startup existence work");
    Pump();
    Check(!app.HasFolders() && !FolderTree(app, root)
        && app.tabs.size() == paths.size() && app.currentDoc == document,
        "Queued startup result reattached a removed root or changed retained tabs");
    for (size_t index = 0; index < issues.size(); ++index)
        Check(app.tabs[index].registrationIssue == issues[index], "Queued startup result changed a removed folder's tab status");
    Check(!IsWindowVisible(app.hwnd), "Startup registration checks unexpectedly showed their test window");
    std::cout << "PASS startup registration metadata, paused files, missing/wrong-type markers, persistence, cancellation, manual recovery\n";
}

void StartupRootContextChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"startup-root-fixture";
    const auto root = fixture / L"git";
    const auto documentPath = root / L"keep.md";
    const auto otherRoot = fixture / L"other-root";
    const auto missingRoot = fixture / L"missing-root";
    const std::string contents = "# Keep this fixture on disk\n";
    Write(documentPath, contents);
    Write(otherRoot / L"other.md", "# Different pending root\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.OpenPaths({documentPath.wstring()});
    LoadedPath(app, documentPath);
    const auto document = app.currentDoc;

    // Keep every long-running service busy so only the real root placeholder
    // is available, exactly as it is before startup scanning finishes.
    struct Gate {
        std::atomic_int started{0}, finished{0};
        std::atomic_bool released{false};
    };
    const auto gate = std::make_shared<Gate>();
    struct ReleaseGate {
        std::shared_ptr<Gate> gate;
        ~ReleaseGate() { gate->released = true; }
    } release{gate};
    auto hold = [gate] {
        ++gate->started;
        while (!gate->released) Sleep(1);
        ++gate->finished;
    };
    for (auto& worker : app.scanners) worker.Queue(hold);
    app.watchWorker.Queue(hold);
    app.readWorker.Queue(hold);
    Until([&] { return gate->started == 6; }, "Cannot hold startup scan/watch/read workers"); // 4 scanners + watch + read
    app.OpenRoot(root.wstring());
    app.RefreshTree();
    app.LoadActive();
    app.StartStartupCheck();
    Check(!FolderTree(app, root) && app.loading, "Startup root fixture did not retain pending tree/read work");

    auto rootPoint = [&](const fs::path& path) {
        const auto item = TreeItem(app, path);
        Check(!FolderTree(app, path) && TreeView_GetCount(app.treeH) == 1 && item != nullptr
            && item == TreeView_GetRoot(app.treeH) && TreeView_GetNextSibling(app.treeH, item) == nullptr,
            "Startup root must have exactly one real placeholder item");
        RECT bounds{};
        Check(TreeView_GetItemRect(app.treeH, item, &bounds, TRUE) != FALSE, "Cannot locate startup root label");
        POINT screen{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
        Check(ClientToScreen(app.treeH, &screen) != FALSE, "Cannot map startup root label point");
        return screen;
    };
    const POINT point = rootPoint(root);
    const auto clickedRoot = app.FolderAt(point);
    Check(clickedRoot == mm::NormalizePath(root.wstring()), "Startup root placeholder cannot target the pending folder");
    RECT treeClient{};
    Check(GetClientRect(app.treeH, &treeClient) != FALSE, "Cannot inspect tree client bounds");
    POINT outside{treeClient.right + app.D(8), treeClient.top + app.D(8)};
    Check(ClientToScreen(app.treeH, &outside) != FALSE, "Cannot map outside-tree point");
    Check(app.FolderAt(outside).empty(), "Clicking outside the tree targeted its root");
    app.sidebar = false;
    app.Layout();
    Check(app.FolderAt(point).empty(), "Hidden sidebar root still targets a folder");
    app.sidebar = true;
    app.Layout();

    struct MenuProbe {
        int popupCount = 0;
        bool initialized = false, removalEnabled = false, cancelPosted = false, cancelDelivered = false;
        std::wstring label;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
            UINT_PTR, DWORD_PTR reference) {
            auto& probe = *reinterpret_cast<MenuProbe*>(reference);
            if (message == WM_INITMENUPOPUP) {
                ++probe.popupCount;
                probe.initialized = true;
                auto menu = reinterpret_cast<HMENU>(wparam);
                wchar_t label[64]{};
                GetMenuStringW(menu, RemoveFromList, label, 64, MF_BYCOMMAND);
                probe.label = label;
                const auto state = GetMenuState(menu, RemoveFromList, MF_BYCOMMAND);
                probe.removalEnabled = state != UINT(-1) && !(state & (MF_DISABLED | MF_GRAYED));
                probe.cancelPosted = PostMessageW(window, WM_CANCELMODE, 0, 0) != FALSE;
            } else if (message == WM_ENTERIDLE && probe.initialized) {
                PostMessageW(window, WM_CANCELMODE, 0, 0);
            } else if (message == WM_CANCELMODE && probe.initialized) {
                probe.cancelDelivered = true;
                EndMenu();
            }
            return DefSubclassProc(window, message, wparam, lparam);
        }
    } probe;
    constexpr UINT_PTR probeId = 0x4d4d5654;
    Check(SetWindowSubclass(app.hwnd, MenuProbe::Proc, probeId, reinterpret_cast<DWORD_PTR>(&probe)) != FALSE,
        "Cannot install startup root popup probe");
    struct ProbeLifetime {
        HWND window;
        UINT_PTR id;
        ~ProbeLifetime() { RemoveWindowSubclass(window, MenuProbe::Proc, id); }
    } probeLifetime{app.hwnd, probeId};
    SendMessageW(app.treeH, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(app.treeH), MAKELPARAM(point.x, point.y));
    Check(probe.popupCount == 1 && probe.initialized && probe.removalEnabled && probe.label == L"リストから削除",
        "Tree WM_CONTEXTMENU did not show exactly one enabled removal menu for its pending root");
    Check(probe.cancelPosted && probe.cancelDelivered, "Startup root context menu did not cancel through its posted message");
    Pump();
    probe = {};
    POINT mousePoint = point;
    Check(ScreenToClient(app.treeH, &mousePoint) != FALSE, "Cannot map root mouse input coordinates");
    SendMessageW(app.treeH, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(mousePoint.x, mousePoint.y));
    SendMessageW(app.treeH, WM_RBUTTONUP, 0, MAKELPARAM(mousePoint.x, mousePoint.y));
    Check(probe.popupCount == 1 && probe.removalEnabled && probe.label == L"リストから削除"
        && probe.cancelPosted && probe.cancelDelivered,
        "Native root right-click did not open and cancel exactly one enabled removal menu");
    Pump();
    probe = {};
    TreeView_SelectItem(app.treeH, TreeItem(app, root));
    SendMessageW(app.treeH, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(app.treeH), MAKELPARAM(-1, -1));
    RemoveWindowSubclass(app.hwnd, MenuProbe::Proc, probeId);
    Check(probe.popupCount == 1 && probe.removalEnabled && probe.label == L"リストから削除"
        && probe.cancelPosted && probe.cancelDelivered,
        "Keyboard context menu did not open and cancel exactly one enabled removal menu for the selected root");
    Check(app.FindFolder(clickedRoot) && app.folders.size() == 1 && !FolderTree(app, root) && !IsWindowVisible(app.hwnd),
        "Inspecting the root menu altered the folder or showed the hidden host");

    const auto scanGeneration = Folder(app, root).generation, loadGeneration = app.loadGeneration;
    const auto folderId = Folder(app, root).id;
    const auto watchGeneration = app.watchGeneration, startupGeneration = app.startupGeneration;
    const auto scanCancel = Folder(app, root).cancel, readCancel = app.readCancel;
    const auto watchCancel = app.watchCancel, startupCancel = app.startupCancel;
    auto staleTree = std::make_unique<Scanned>();
    staleTree->folderId = folderId;
    staleTree->generation = scanGeneration;
    staleTree->tree = std::make_unique<mm::FolderNode>();
    staleTree->tree->path = clickedRoot;
    staleTree->tree->name = L"git";
    staleTree->tree->directory = true;
    Check(PostMessageW(app.hwnd, TreeDone, 0, reinterpret_cast<LPARAM>(staleTree.get())) != FALSE,
        "Cannot queue stale startup tree");
    staleTree.release();
    auto staleRead = std::make_unique<Loaded>();
    staleRead->generation = loadGeneration;
    staleRead->id = app.tabs[app.active].id;
    staleRead->doc = std::make_shared<mm::Document>(*document);
    staleRead->doc->source = "# Stale startup read must not replace content\n";
    Check(PostMessageW(app.hwnd, LoadDone, 0, reinterpret_cast<LPARAM>(staleRead.get())) != FALSE,
        "Cannot queue stale startup read");
    staleRead.release();
    auto staleStartup = std::make_unique<StartupChecked>();
    staleStartup->generation = startupGeneration;
    staleStartup->checks = {{app.tabs[app.active].path, false, mm::PathPresence::Missing, ERROR_FILE_NOT_FOUND}};
    Check(PostMessageW(app.hwnd, StartupDone, 0, reinterpret_cast<LPARAM>(staleStartup.get())) != FALSE,
        "Cannot queue stale startup existence result");
    staleStartup.release();
    auto staleWatches = std::make_unique<Watched>();
    staleWatches->generation = watchGeneration;
    staleWatches->plan[clickedRoot] = {true, true};
    auto forbiddenWatch = std::make_unique<Watch>();
    forbiddenWatch->path = clickedRoot;
    forbiddenWatch->recursive = true;
    forbiddenWatch->treeWatch = true;
    staleWatches->watches.push_back(std::move(forbiddenWatch));
    Check(PostMessageW(app.hwnd, WatchDone, 0, reinterpret_cast<LPARAM>(staleWatches.get())) != FALSE,
        "Cannot queue stale startup watcher result");
    staleWatches.release();

    app.RemoveFolderFromList(clickedRoot);
    Check(!app.HasFolders() && !FolderTree(app, root) && TreeView_GetCount(app.treeH) == 0,
        "Root placeholder removal required a completed folder tree");
    Check(scanCancel && *scanCancel && app.FindFolder(folderId) == nullptr
        && readCancel && *readCancel && app.loadGeneration > loadGeneration && !app.loading,
        "Root placeholder removal did not immediately cancel pending scans and reads");
    Check(watchCancel && *watchCancel && app.watchGeneration > watchGeneration
        && startupCancel && *startupCancel && app.startupGeneration > startupGeneration && !app.startupChecking,
        "Root placeholder removal did not cancel watcher setup and existence checking");
    Check(!WatchTouchesFolder(app, root) && !app.tabs[app.active].autoRefresh && app.currentDoc == document,
        "Root placeholder removal retained monitoring or changed the displayed document");
    Pump();
    Check(!app.HasFolders() && !FolderTree(app, root) && app.currentDoc == document
        && app.tabs[app.active].registrationIssue.empty() && !WatchTouchesFolder(app, root),
        "Stale startup completion restored work after root removal");
    Check(fs::is_directory(root) && mm::ReadDocument(documentPath.wstring()).source == contents,
        "Root placeholder list removal changed files on disk");

    app.OpenRoot(missingRoot.wstring());
    app.RefreshTree();
    const auto missingPoint = rootPoint(missingRoot);
    Check(TreeItem(app, root) == nullptr && app.FolderAt(missingPoint) == mm::NormalizePath(missingRoot.wstring()),
        "Missing startup root did not replace the previous root placeholder");
    app.RemoveFolderFromList(app.FolderAt(missingPoint));
    Check(!app.HasFolders() && !FolderTree(app, missingRoot) && !fs::exists(missingRoot),
        "Missing registered root was not removable without a folder tree");

    app.OpenRoot(otherRoot.wstring());
    rootPoint(otherRoot);
    Check(TreeItem(app, missingRoot) == nullptr && TreeItem(app, root) == nullptr,
        "Opening a different folder retained an old root placeholder");
    const auto otherGeneration = Folder(app, otherRoot).generation;
    const auto exclusionsBeforeStaleTarget = app.excludedFolders;
    app.RemoveFolderFromList(clickedRoot);
    Check(app.FindFolder(otherRoot.wstring()) && Folder(app, otherRoot).generation == otherGeneration
        && app.excludedFolders == exclusionsBeforeStaleTarget, "Stale root target removed the newly opened root");
    app.RemoveFolderFromList(otherRoot.wstring());

    // A previous root can become a valid visible child of a newly opened root.
    // Its old popup still belongs to the old root context and must be ignored.
    const auto childRoot = root / L"child";
    Write(childRoot / L"child.md", "# Visible former root\n");
    const auto oldRootContext = mm::NormalizePath(childRoot.wstring());
    app.OpenRoot(childRoot.wstring());
    const auto oldRootId = Folder(app, childRoot).id;
    app.RemoveFolderFromList(childRoot.wstring());
    app.OpenRoot(root.wstring());
    app.ApplyTree(std::make_unique<mm::FolderNode>(mm::ScanFolder(root.wstring())));
    Check(TreeItem(app, childRoot) != nullptr, "Former root is not visible inside the new parent root");
    const auto exclusionsBeforeOldMenu = app.excludedFolders;
    app.RunTreeMenuCommand(RemoveFromList, oldRootContext, oldRootId);
    Check(app.FindFolder(clickedRoot) && app.excludedFolders == exclusionsBeforeOldMenu && TreeItem(app, childRoot) != nullptr,
        "Stale root popup excluded the former root after it became a visible descendant");
    app.RemoveFolderFromList(root.wstring());
    gate->released = true;
    Until([&] { return gate->finished == 6; }, "Held startup workers did not release");
    Pump();
    Check(!app.HasFolders() && !FolderTree(app, root) && app.currentDoc == document && fs::exists(documentPath)
        && fs::exists(otherRoot / L"other.md"), "Releasing cancelled startup workers restored a removed root or changed files");
    Check(!IsWindowVisible(app.hwnd), "Startup root test unexpectedly showed its host window");
    std::cout << "PASS single startup root item, native context/right-click menu, pending/missing removal, cancellation, stale targets\n";
}

struct RenderedHostImage {
    int width = 0, height = 0;
    std::vector<DWORD> pixels;
};

void SaveHostImage(App& app, const fs::path& output, RenderedHostImage* rendered = nullptr) {
    RECT client{};
    GetClientRect(app.hwnd, &client);
    HDC reference = GetDC(app.hwnd);
    Check(reference != nullptr, "Cannot obtain test window DC");
    HDC target = CreateCompatibleDC(reference);
    HBITMAP pixels = CreateCompatibleBitmap(reference, client.right, client.bottom);
    ReleaseDC(app.hwnd, reference);
    if (!target || !pixels) {
        if (target) DeleteDC(target);
        if (pixels) DeleteObject(pixels);
        throw std::runtime_error("Cannot allocate host snapshot bitmap");
    }
    auto previous = SelectObject(target, pixels);
    SendMessageW(app.hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target), PRF_CLIENT);
    // Render each direct child at its client-relative position. WM_PRINT on
    // a captioned root otherwise adds its non-client offset to this client DC.
    for (HWND child=GetWindow(app.hwnd,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT)) {
        if (!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) continue;
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(HWND_DESKTOP,app.hwnd,reinterpret_cast<POINT*>(&bounds),2);
        int saved=SaveDC(target);SetViewportOrgEx(target,bounds.left,bounds.top,nullptr);
        IntersectClipRect(target,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top);
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(target),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND|PRF_CHILDREN);
        RestoreDC(target,saved);
        // Owner-drawn tabs handle WM_PRINTCLIENT themselves. Explicitly print
        // their native overflow child so screenshots include the actual arrows.
        if (HWND arrows = FindWindowExW(child, nullptr, UPDOWN_CLASSW, nullptr);
            arrows && (GetWindowLongPtrW(arrows, GWL_STYLE) & WS_VISIBLE)) {
            GetWindowRect(arrows, &bounds); MapWindowPoints(HWND_DESKTOP, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
            saved = SaveDC(target); SetViewportOrgEx(target, bounds.left, bounds.top, nullptr);
            IntersectClipRect(target, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
            SendMessageW(arrows, WM_PRINT, reinterpret_cast<WPARAM>(target), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            RestoreDC(target, saved);
        }
    }
    SelectObject(target, previous);
    DeleteDC(target);
    if (rendered) {
        rendered->width = client.right;
        rendered->height = client.bottom;
        rendered->pixels.resize(static_cast<size_t>(client.right) * client.bottom);
        BITMAPINFO format{};
        format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth = client.right;
        format.bmiHeader.biHeight = -client.bottom;
        format.bmiHeader.biPlanes = 1;
        format.bmiHeader.biBitCount = 32;
        format.bmiHeader.biCompression = BI_RGB;
        HDC conversion = GetDC(app.hwnd);
        const auto rows = conversion ? GetDIBits(conversion, pixels, 0, client.bottom,
            rendered->pixels.data(), &format, DIB_RGB_COLORS) : 0;
        if (conversion) ReleaseDC(app.hwnd, conversion);
        if (rows != client.bottom) {
            DeleteObject(pixels);
            throw std::runtime_error("Cannot read rendered host pixels");
        }
    }
    Gdiplus::Status status;
    {
        Gdiplus::Bitmap bitmap(pixels, nullptr);
        const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        status = bitmap.Save(output.c_str(), &png, nullptr);
    }
    DeleteObject(pixels);
    Check(status == Gdiplus::Ok, "Cannot save host snapshot PNG");
}

void RootFolderReorderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"root-folder-reordering";
    const std::vector<fs::path> roots{fixture / L"資料A", fixture / L"資料B", fixture / L"資料C"};
    for (const auto& root : roots) Write(root / L"child" / L"document.md", "# Folder order\n\nThe displayed document survives folder reordering.\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    for (const auto& root : roots) app.OpenRoot(root.wstring());
    Until([&] { return std::all_of(roots.begin(), roots.end(), [&](const auto& root) {
        return FolderTree(app, root) && !Folder(app, root).scanning && WatchCoversFolder(app, root);
    }); }, "Reordering fixture did not finish scanning and watching");
    const auto document = roots[0] / L"child" / L"document.md";
    app.OpenPaths({document.wstring()});
    LoadedPath(app, document);
    app.Command(ExpandAll);
    auto orderIs = [&](std::initializer_list<int> indices) {
        std::vector<std::wstring> expected, native;
        for (int index : indices) expected.push_back(mm::NormalizePath(roots[index].wstring()));
        for (auto item = TreeView_GetRoot(app.treeH); item; item = TreeView_GetNextSibling(app.treeH, item)) {
            TVITEMW info{}; info.mask = TVIF_PARAM; info.hItem = item;
            Check(TreeView_GetItem(app.treeH, &info) != FALSE, "Cannot inspect reordered root item");
            native.push_back(reinterpret_cast<mm::FolderNode*>(info.lParam)->path);
        }
        Check(app.FolderPaths() == expected && native == expected, "Registered and native top-level folder orders disagree");
    };
    auto pointAt = [&](HTREEITEM item, int half = 0, bool icon = false) {
        TreeView_EnsureVisible(app.treeH, item);
        RECT row{};
        Check(TreeView_GetItemRect(app.treeH, item, &row, TRUE) != FALSE, "Cannot locate folder drag target");
        POINT point{row.left + (row.right - row.left) / 2,
            half < 0 ? row.top + 2 : half > 0 ? row.bottom - 2 : (row.top + row.bottom) / 2};
        if (icon) {
            bool found = false;
            for (LONG x = row.left - 1; x >= 0; --x) {
                TVHITTESTINFO hit{}; hit.pt = {x, point.y};
                if (TreeView_HitTest(app.treeH, &hit) == item && (hit.flags & TVHT_ONITEMICON)) {
                    point.x = x; found = true; break;
                }
            }
            Check(found, "Registered folder icon has no native hit region");
        }
        return point;
    };
    auto mouse = [&](UINT message, POINT point) {
        SendMessageW(app.treeH, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
    };
    auto click = [&](HTREEITEM item) {
        const auto point = pointAt(item); mouse(WM_LBUTTONDOWN, point); mouse(WM_LBUTTONUP, point);
    };
    const auto clicked = TreeItem(app, roots[1]);
    const auto clickedPath = mm::NormalizePath(roots[1].wstring());
    const auto clickPoint = pointAt(clicked);
    mouse(WM_LBUTTONDOWN, clickPoint);
    Check(app.rootDragId == Folder(app, roots[1]).id && GetCapture() == app.treeH
        && (TreeView_GetItemState(app.treeH, clicked, TVIS_EXPANDED) & TVIS_EXPANDED),
        "Root click must retain expansion while waiting to distinguish a drag");
    mouse(WM_LBUTTONUP, clickPoint);
    Check(!(TreeView_GetItemState(app.treeH, clicked, TVIS_EXPANDED) & TVIS_EXPANDED)
        && !app.expanded.contains(clickedPath), "Ordinary registered-folder click did not collapse on release");
    click(clicked);
    Check((TreeView_GetItemState(app.treeH, clicked, TVIS_EXPANDED) & TVIS_EXPANDED)
        && app.expanded.contains(clickedPath), "Second registered-folder click did not reopen the folder");

    // Child folders keep their click behavior; neither they nor files can
    // initiate the operation that rearranges registered top-level folders.
    for (const auto& path : {roots[0] / L"child", document}) {
        const auto item = TreeItem(app, path);
        const auto source = pointAt(item), target = pointAt(TreeItem(app, roots[2]));
        mouse(WM_LBUTTONDOWN, source); mouse(WM_MOUSEMOVE, target);
        Check(app.rootDragId == 0 && !app.rootDragging, "A child folder or file started registered-folder dragging");
        mouse(WM_LBUTTONUP, target);
        orderIs({0, 1, 2});
        if (path != document) click(item);
    }
    LoadedPath(app, document);

    struct Gate { std::atomic_bool started{false}, released{false}; };
    const auto gate = std::make_shared<Gate>();
    struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->released = true; } } release{gate};
    for (auto& worker : app.scanners) worker.Queue([gate] { gate->started = true; while (!gate->released) Sleep(1); });
    Until([&] { return gate->started.load(); }, "Cannot hold scan worker for folder reordering");
    app.RefreshFolder(Folder(app, roots[1]));
    struct RootState { FolderRegistration* entry; mm::FolderNode* tree; unsigned long long id, generation;
        std::shared_ptr<std::atomic_bool> cancel; bool scanning; };
    std::vector<RootState> rootStates;
    for (const auto& root : roots) {
        auto& entry = Folder(app, root);
        rootStates.push_back({&entry, entry.tree.get(), entry.id, entry.generation, entry.cancel, entry.scanning});
    }
    struct NodeState { std::wstring path; HTREEITEM item, parent; mm::FolderNode* node; UINT expanded; };
    std::vector<NodeState> nodes;
    app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) {
        nodes.push_back({node->path, item, TreeView_GetParent(app.treeH, item), node,
            TreeView_GetItemState(app.treeH, item, TVIS_EXPANDED) & TVIS_EXPANDED});
    });
    struct WatchState { Watch* watch; HANDLE dir, event; };
    std::vector<WatchState> watches;
    for (const auto& watch : app.watches) watches.push_back({watch.get(), watch->dir, watch->event});
    const auto expanded = app.expanded;
    const auto displayed = app.currentDoc;
    const auto loadGeneration = app.loadGeneration, watchGeneration = app.watchGeneration;
    auto preserved = [&] {
        for (const auto& before : rootStates)
            Check(app.FindFolder(before.id) == before.entry && before.entry->tree.get() == before.tree
                && before.entry->generation == before.generation && before.entry->cancel == before.cancel
                && before.entry->scanning == before.scanning && (!before.cancel || !*before.cancel),
                "Folder reordering replaced a model, restarted a scan, or cancelled pending work");
        for (const auto& before : nodes) {
            TVITEMW info{}; info.mask = TVIF_PARAM; info.hItem = before.item;
            Check(TreeItem(app, fs::path(before.path)) == before.item && TreeView_GetItem(app.treeH, &info)
                && reinterpret_cast<mm::FolderNode*>(info.lParam) == before.node
                && TreeView_GetParent(app.treeH, before.item) == before.parent
                && (TreeView_GetItemState(app.treeH, before.item, TVIS_EXPANDED) & TVIS_EXPANDED) == before.expanded,
                "Folder reordering rebuilt native items or changed a child hierarchy/expansion");
        }
        Check(app.expanded == expanded && app.currentDoc == displayed && app.loadGeneration == loadGeneration
            && app.watchGeneration == watchGeneration && app.watches.size() == watches.size(),
            "Folder reordering altered document, expansion, or watcher state");
        for (size_t index = 0; index < watches.size(); ++index)
            Check(app.watches[index].get() == watches[index].watch && app.watches[index]->dir == watches[index].dir
                && app.watches[index]->event == watches[index].event, "Folder reordering replaced an existing watcher handle");
    };
    auto beginDrag = [&](int source, int target, bool after, bool icon = false) {
        const auto from = pointAt(TreeItem(app, roots[source]), 0, icon);
        const auto to = pointAt(TreeItem(app, roots[target]), after ? 1 : -1);
        mouse(WM_LBUTTONDOWN, from);
        Check(app.rootDragId == Folder(app, roots[source]).id && !app.rootDragging && GetCapture() == app.treeH,
            "Registered-folder mouse down did not establish a captured drag candidate");
        mouse(WM_MOUSEMOVE, to);
        Check(app.rootDragging, "Moving between root rows did not pass the native drag threshold");
        return to;
    };
    auto endDrag = [&](POINT point) {
        mouse(WM_LBUTTONUP, point);
        Check(!app.rootDragging && app.rootDragId == 0 && GetCapture() != app.treeH,
            "Registered-folder drag did not release its capture and candidate");
    };
    endDrag(beginDrag(2, 0, false));
    orderIs({2, 0, 1}); preserved();
    endDrag(beginDrag(0, 1, true, true));
    orderIs({2, 1, 0}); preserved();
    beginDrag(0, 2, false);
    const auto childTarget = pointAt(TreeItem(app, roots[1] / L"child"));
    mouse(WM_MOUSEMOVE, childTarget);
    endDrag(childTarget);
    orderIs({2, 1, 0}); preserved();
    for (int cancel = 0; cancel < 5; ++cancel) {
        if (cancel == 4) { app.searching = true; app.Layout(); }
        const auto target = beginDrag(0, 2, false);
        if (cancel == 0) SendMessageW(app.treeH, WM_KEYDOWN, VK_ESCAPE, 0);
        else if (cancel == 1) SendMessageW(app.treeH, WM_CANCELMODE, 0, 0);
        else if (cancel == 2) SetCapture(app.view.Handle());
        else if (cancel == 3) app.Command(Sidebar);
        else Check(Key(app, VK_ESCAPE) && app.searching,
            "Escape closed search instead of cancelling the active folder drag first");
        Check(!app.rootDragging && app.rootDragId == 0 && GetCapture() != app.treeH,
            "Cancelling registered-folder dragging left an active drag or capture");
        if (cancel == 2) ReleaseCapture();
        if (cancel == 3) app.Command(Sidebar);
        if (cancel == 4) { app.searching = false; app.Layout(); }
        mouse(WM_LBUTTONUP, target);
        orderIs({2, 1, 0}); preserved();
    }
    app.Save();
    {
        App restored; restored.ini = app.ini; restored.RestoreFolders();
        Check(restored.FolderPaths() == app.FolderPaths(), "Restart restoration lost the dragged folder order");
    }
    gate->released = true;
    Until([&] { return !Folder(app, roots[1]).scanning; }, "Reordered pending folder scan did not finish");
    orderIs({2, 1, 0});
    app.dark = true; app.ApplyTheme();
    SaveHostImage(app, fixture / L"reordered-dark.png");
    beginDrag(0, 2, false);
    SaveHostImage(app, fixture / L"insertion-mark-dark.png");
    Check(Key(app, VK_ESCAPE), "Cannot cancel insertion-mark preview");
    // A short viewport exercises edge scrolling without creating more roots or
    // doing more file I/O. The timer runs only while an actual drag is active.
    RECT bounds{};GetWindowRect(app.treeH, &bounds);
    MapWindowPoints(HWND_DESKTOP, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
    MoveWindow(app.treeH, bounds.left, bounds.top, bounds.right - bounds.left, app.D(90), TRUE);
    TreeView_SelectSetFirstVisible(app.treeH, TreeView_GetRoot(app.treeH));
    const auto start = pointAt(TreeView_GetRoot(app.treeH));
    RECT client{};GetClientRect(app.treeH, &client);
    const POINT edge{start.x, client.bottom - 1};
    mouse(WM_LBUTTONDOWN, start);mouse(WM_MOUSEMOVE, edge);
    Check(app.rootDragging, "Edge scrolling did not start a root drag");
    const int beforeScroll = GetScrollPos(app.treeH, SB_VERT);
    SendMessageW(app.treeH, WM_TIMER, RootDragTimer, 0);
    Check(GetScrollPos(app.treeH, SB_VERT) > beforeScroll, "Dragging at the lower edge did not scroll the tree");
    Check(Key(app, VK_ESCAPE), "Cannot cancel edge scrolling");
    MoveWindow(app.treeH, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, TRUE);
    orderIs({2, 1, 0});
    Check(!IsWindowVisible(app.hwnd), "Folder-reordering test unexpectedly showed its host window");
    std::wcout << L"Folder reordering rendering: " << (fixture / L"reordered-dark.png").wstring() << L"\n";
    std::cout << "PASS native root drag ordering, excluded child targets, clicks/cancellation, stable resources, saved order\n";
}

struct TreeMutationTrace {
    bool redrawSuspended = false;
    int mutationsWhileSuspended = 0, titleMutations = 0, iconMutations = 0;
    int insertions = 0, deletions = 0, reads = 0;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto& trace = *reinterpret_cast<TreeMutationTrace*>(data);
        if (message == WM_SETREDRAW) {
            if (!w) trace.redrawSuspended = true;
            const auto result = DefSubclassProc(window, message, w, l);
            if (w) trace.redrawSuspended = false;
            return result;
        }
        if ((message == TVM_SETITEMW || message == TVM_SETITEMA) && l) {
            const auto mask = reinterpret_cast<TVITEMW*>(l)->mask;
            if (trace.redrawSuspended) ++trace.mutationsWhileSuspended;
            if (mask & TVIF_TEXT) ++trace.titleMutations;
            if (mask & (TVIF_IMAGE | TVIF_SELECTEDIMAGE)) ++trace.iconMutations;
        }
        if (message == TVM_INSERTITEMW || message == TVM_INSERTITEMA) ++trace.insertions;
        if (message == TVM_DELETEITEM) ++trace.deletions;
        if (message == TVM_GETITEMW || message == TVM_GETITEMA) ++trace.reads;
        return DefSubclassProc(window, message, w, l);
    }
};

void DeepTreeRefreshChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"deep-tree-refresh";
    fs::create_directories(fixture);
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    TreeMutationTrace trace;
    Check(SetWindowSubclass(app.treeH, TreeMutationTrace::Proc, 0x4d5244,
        reinterpret_cast<DWORD_PTR>(&trace)) != FALSE, "Cannot trace native tree mutations");
    struct RemoveTrace {
        HWND window;
        ~RemoveTrace() { if (IsWindow(window)) RemoveWindowSubclass(window, TreeMutationTrace::Proc, 0x4d5244); }
    } removeTrace{app.treeH};

    // This anonymous topology reproduced the live TV_GetShownIndexItem hang.
    // It contains only depth/expansion bits, with no user paths or addresses.
    const auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
    std::ifstream topology(sourceRoot / L"tests" / L"fixtures" / L"tree-refresh-topology.txt");
    Check(bool(topology), "Cannot read anonymous deep-tree regression fixture");
    std::string comment;std::getline(topology, comment);
    struct Entry { int depth; bool expanded; };
    std::vector<Entry> entries;
    int depth = 0, expanded = 0;
    while (topology >> depth >> expanded) entries.push_back({depth, expanded != 0});
    Check(entries.size() == 11355, "Anonymous deep-tree fixture has an unexpected item count");
    size_t position = 0;
    std::function<mm::FolderNode(const fs::path&)> read = [&](const fs::path& parent) {
        const size_t index = position++;
        const auto entry = entries[index];
        const bool directory = entry.depth == 0 || (position < entries.size() && entries[position].depth > entry.depth);
        const auto name = directory ? L"diagnostic-folder-" + std::to_wstring(index)
                                    : L"document-" + std::to_wstring(index) + L".md";
        const auto path = parent / name;
        mm::FolderNode node{name, path.wstring(), directory, {}};
        if (entry.expanded) app.expanded.insert(node.path);
        while (position < entries.size() && entries[position].depth > entry.depth) {
            Check(entries[position].depth == entry.depth + 1, "Invalid anonymous topology depth transition");
            node.children.push_back(read(path));
        }
        return node;
    };
    std::unique_ptr<mm::FolderNode> firstTree;
    while (position < entries.size()) {
        Check(entries[position].depth == 0, "Invalid anonymous topology root depth");
        auto tree = std::make_unique<mm::FolderNode>(read(fixture));
        auto folder = std::make_unique<FolderRegistration>();
        folder->id = app.nextFolderId++;
        folder->path = tree->path;
        folder->pending = {tree->name, tree->path, true, {}};
        if (app.folders.empty()) firstTree = std::move(tree);
        else folder->tree = std::move(tree);
        app.folders.push_back(std::move(folder));
    }
    Check(app.folders.size() == 4, "Anonymous topology did not produce four registered roots");
    app.RebuildTree();
    app.FlushTreeTasks();
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE,
        GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), app.D(1000), app.D(650),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT bounds{};Check(GetWindowRect(app.treeH, &bounds), "Cannot locate deep-tree viewport");
    MapWindowPoints(HWND_DESKTOP, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
    MoveWindow(app.treeH, bounds.left, bounds.top, app.D(120), app.D(110), TRUE);
    UpdateWindow(app.hwnd);
    std::cout << "deep-tree: replace pending first root with 9781 nested rows" << std::endl;
    const auto start = std::chrono::steady_clock::now();
    app.folders.front()->issue = L"temporary issue"; // Force a real text/width update.
    app.ReplaceFolderTree(*app.folders.front(), std::move(firstTree));
    app.FlushTreeTasks();
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Check(TreeView_GetCount(app.treeH) == 11355, "Deep refresh lost native tree items");
    Check(!trace.redrawSuspended && trace.mutationsWhileSuspended == 0,
        "Tree text/image mutation occurred while redraw was suspended");
    Check(trace.titleMutations > 0, "Deep refresh did not exercise an actual root text change");
    // The placeholder row becomes the root of the 9,781-row tree, so 9,780 rows are new.
    Check(app.treeStats.rowsInserted == 9780 && app.treeStats.rowsDeleted == 0,
        "Placeholder replacement did not reuse the root row or deleted rows");
    auto checkIcons = [&] {
        app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) {
            if (!node || !node->directory) return;
            TVITEMW info{};info.mask = TVIF_STATE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            info.hItem = item;info.stateMask = TVIS_EXPANDED;
            Check(TreeView_GetItem(app.treeH, &info) != FALSE, "Cannot read refreshed folder icon state");
            const int expected = (info.state & TVIS_EXPANDED) ? 1 : 0;
            Check(info.iImage == expected && info.iSelectedImage == expected,
                "Deferred icon update disagrees with actual native expansion state");
        });
    };
    checkIcons();
    // A scan that found nothing new must keep every native row and the model
    // the rows point to; the old path deleted and reinserted all of them.
    {
        auto& first = *app.folders.front();
        auto identical = std::make_unique<Scanned>();
        identical->folderId = first.id;
        identical->generation = first.generation;
        identical->tree = std::make_unique<mm::FolderNode>(*first.tree);
        const auto* model = first.tree.get();
        const auto rootItem = TreeItem(app, first.path);
        trace.insertions = trace.deletions = 0;
        const auto unchangedStart = std::chrono::steady_clock::now();
        app.AcceptTree(std::move(identical));
        const double unchangedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - unchangedStart).count();
        Check(trace.insertions == 0 && trace.deletions == 0, "Identical rescan rebuilt native tree rows");
        Check(first.tree.get() == model && TreeItem(app, first.path) == rootItem && TreeView_GetCount(app.treeH) == 11355,
            "Identical rescan replaced the folder model or its native root");
        std::cout << "deep-tree: identical 9781-row rescan accepted in " << unchangedMs << " ms without row changes" << std::endl;
    }
    app.folders.front()->issue.clear();
    app.RebuildTree();
    checkIcons();
    Check(trace.mutationsWhileSuspended == 0, "Full rebuild changed native item data while redraw was suspended");
    const auto textBefore = trace.titleMutations, iconsBefore = trace.iconMutations;
    app.UpdateRootTitle();
    app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) {
        if (node && node->directory) app.UpdateFolderItem(item, false);
    });
    Check(trace.titleMutations == textBefore && trace.iconMutations == iconsBefore,
        "Unchanged tree titles or folder icons caused redundant native mutations");
    ShowWindow(app.hwnd, SW_HIDE);
    Write(fixture / L"result.txt", "PASS: 11355 rows; first-root nested refresh completed in "
        + std::to_string(elapsed) + " ms; no text/image changes while redraw was suspended.\n");
    std::cout << "PASS deep native tree refresh: " << elapsed << " ms, correct deferred icons and mutation ordering\n";
}

void SmallTreeRescanChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"small-tree-rescan";
    fs::create_directories(fixture);
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    const auto originalLoadGeneration = app.loadGeneration;
    std::vector<fs::path> roots, lastFiles, nestedFiles;
    for (int index = 0; index < 4; ++index) {
        const auto root = fixture / (L"root-" + std::to_wstring(index));
        roots.push_back(root);
        auto folder = std::make_unique<FolderRegistration>();
        folder->id = app.nextFolderId++;
        folder->path = root.wstring();
        folder->pending = {root.filename().wstring(), root.wstring(), true, {}};
        folder->tree = std::make_unique<mm::FolderNode>(folder->pending);
        for (int file = 0; file < 4; ++file) {
            const auto name = L"document-" + std::to_wstring(file) + L".md";
            const auto path = root / name;
            folder->tree->children.push_back({name, path.wstring(), false, {}});
            if (file == 3) lastFiles.push_back(path);
        }
        const auto nested = root / L"nested";
        const auto deep = nested / L"deep";
        const auto nestedFile = deep / L"selected.md";
        nestedFiles.push_back(nestedFile);
        folder->tree->children.push_back({L"nested", nested.wstring(), true,
            {{L"deep", deep.wstring(), true,
                {{L"selected.md", nestedFile.wstring(), false, {}}}}}});
        folder->tree->children.sort(mm::FolderNodeBefore); // Scan order, so replacements are true differences.
        // The first root has descendants but remains collapsed. This makes
        // visible indices differ from the native tree's structural traversal.
        if (index != 0) app.expanded.insert(root.wstring());
        app.folders.push_back(std::move(folder));
    }
    app.RebuildTree();
    Tab tab;tab.id = app.nextId++;app.tabs.push_back(std::move(tab));app.active = 0;
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE,
        GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), app.D(1000), app.D(650),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    auto shortenViewport = [&] {
        RECT bounds{};Check(GetWindowRect(app.treeH, &bounds), "Cannot locate small-tree viewport");
        MapWindowPoints(HWND_DESKTOP, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        MoveWindow(app.treeH, bounds.left, bounds.top, bounds.right - bounds.left, app.D(110), TRUE);
    };
    shortenViewport();
    UpdateWindow(app.hwnd);
    auto checkSelected = [&] {
        const auto item = TreeItem(app, app.tabs.front().path);
        Check(item && TreeView_GetSelection(app.treeH) == item,
            "Small-tree rescan changed the selected file");
        Check(!app.loading && app.loadGeneration == originalLoadGeneration,
            "Small-tree selection or rescan started a redundant document read");
    };
    auto select = [&](const fs::path& path) {
        app.tabs.front().path = path.wstring();app.SelectTreeFile();checkSelected();
        RECT row{}, client{};
        Check(TreeView_GetItemRect(app.treeH, TreeView_GetSelection(app.treeH), &row, FALSE)
            && GetClientRect(app.treeH, &client) && row.top >= client.top && row.bottom <= client.bottom,
            "Small-tree selection did not make the selected file fully visible");
    };
    auto replace = [&](size_t index, bool grow) {
        auto& folder = *app.folders[index];
        auto replacement = std::make_unique<mm::FolderNode>(*folder.tree);
        const auto temporary = roots[index] / L"temporary.md";
        const bool present = std::erase_if(replacement->children, [&](const auto& node) {return node.path == temporary.wstring();}) != 0;
        if (grow) { replacement->children.push_back({L"temporary.md", temporary.wstring(), false, {}}); replacement->children.sort(mm::FolderNodeBefore); }
        app.ReplaceFolderTree(folder, std::move(replacement));
        app.FlushTreeTasks();
        const size_t expectedChanges = (present ? 1 : 0) + (grow ? 1 : 0) - (present && grow ? 2 : 0);
        Check(app.treeStats.rowsInserted + app.treeStats.rowsDeleted == expectedChanges,
            "Small-tree replacement changed more rows than the difference");
    };
    std::cout << "small-tree: repeated selection, partial rescan and full rebuild" << std::endl;
    for (int repeat = 0; repeat < 24; ++repeat) {
        std::cout << "small-tree: iteration " << repeat + 1 << std::endl;
        select(nestedFiles.back());
        replace(0, (repeat & 1) != 0);checkSelected();
        replace(1, (repeat & 1) == 0);checkSelected();
        select(lastFiles[2]);
        replace(2, (repeat & 1) != 0);checkSelected();
        app.RebuildTree();checkSelected();
        select(nestedFiles[1]);
        replace(1, (repeat & 1) != 0);checkSelected();
        select(lastFiles.back());
    }
    std::cout << "small-tree: hidden sidebar rescan and rebuild" << std::endl;
    app.sidebar = false;app.Layout();
    auto checkHidden = [&] {
        Check(!IsWindowVisible(app.treeH) && !(GetWindowLongPtrW(app.treeH, GWL_STYLE) & WS_VISIBLE),
            "Refreshing folders made a hidden sidebar visible");
    };
    checkHidden();
    replace(3, true);checkSelected();checkHidden();
    app.RebuildTree();checkSelected();checkHidden();
    app.sidebar = true;app.Layout();shortenViewport();
    Check(IsWindowVisible(app.treeH), "Restoring the sidebar did not show its folder control");
    select(nestedFiles.back());
    ShowWindow(app.hwnd, SW_HIDE);
    Write(fixture / L"result.txt", "PASS: 4 roots, 24 rescan/rebuild cycles, selected path and hidden sidebar preservation.\n");
    std::cout << "PASS small native tree rescan/rebuild cycles and hidden sidebar preservation" << std::endl;
}

void LargeTreeSelectionChecks(const fs::path& artifacts) {
    // Run in a separate CTest process with a timeout: a native SendMessage hang
    // cannot be interrupted by the message-pumping Until helper.
    const auto fixture = artifacts / L"large-tree-selection";
    fs::create_directories(fixture);
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    const auto originalLoadGeneration = app.loadGeneration;
    constexpr int leafCounts[] = {3000, 6000, 3000};
    std::vector<fs::path> roots, firstFiles, lastFiles, nestedFiles;
    for (int rootIndex = 0; rootIndex < 3; ++rootIndex) {
        const auto path = fixture / (L"root-" + std::to_wstring(rootIndex));
        roots.push_back(path);
        auto folder = std::make_unique<FolderRegistration>();
        folder->id = app.nextFolderId++;
        folder->path = path.wstring();
        folder->pending = {path.filename().wstring(), path.wstring(), true, {}};
        folder->tree = std::make_unique<mm::FolderNode>(folder->pending);
        auto& children = folder->tree->children;
        for (int leaf = 0; leaf < leafCounts[rootIndex]; ++leaf) {
            const auto name = L"document-" + std::to_wstring(100000 + leaf) + L".md";
            const auto file = path / name;
            children.push_back({name, file.wstring(), false, {}});
            if (leaf == 0) firstFiles.push_back(file);
            if (leaf + 1 == leafCounts[rootIndex]) lastFiles.push_back(file);
        }
        const auto nested = path / L"nested";
        const auto deep = nested / L"deep";
        const auto nestedFile = deep / L"selected.md";
        nestedFiles.push_back(nestedFile);
        children.push_back({L"nested", nested.wstring(), true,
            {{L"deep", deep.wstring(), true,
                {{L"selected.md", nestedFile.wstring(), false, {}}}}}});
        app.expanded.insert(path.wstring());
        app.folders.push_back(std::move(folder));
    }
    std::cout << "large-tree: inserting 12012 native rows" << std::endl;
    app.RebuildTree();
    app.FlushTreeTasks();
    Check(TreeView_GetCount(app.treeH) == 12012, "Large tree did not retain every synthetic item");
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE,
        GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), app.D(1000), app.D(650),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(app.hwnd);
    Check(IsWindowVisible(app.treeH) != FALSE, "Large-tree regression requires a visible native control");

    auto selectedPath = [&] {
        TVITEMW info{};info.mask = TVIF_PARAM;info.hItem = TreeView_GetSelection(app.treeH);
        Check(info.hItem && TreeView_GetItem(app.treeH, &info), "Large tree lost its selected item");
        auto* node = reinterpret_cast<mm::FolderNode*>(info.lParam);
        Check(node != nullptr, "Large tree selection lost its model pointer");
        return node->path;
    };
    auto assertVisible = [&] {
        RECT row{}, client{};
        Check(TreeView_GetItemRect(app.treeH, TreeView_GetSelection(app.treeH), &row, FALSE)
            && GetClientRect(app.treeH, &client) && row.top >= client.top && row.bottom <= client.bottom,
            "Selecting a distant tree file did not bring its entire row into the viewport");
    };
    auto select = [&](const fs::path& path) {
        const auto started = GetTickCount64();
        if (app.tabs.empty()) {
            Tab tab;tab.id = app.nextId++;app.tabs.push_back(std::move(tab));
        }
        app.active = 0;app.tabs.front().path = path.wstring();
        app.SelectTreeFile();
        Check(selectedPath() == path.wstring(), "Large-tree selection chose a different file");
        assertVisible();
        Check(!app.loading && app.loadGeneration == originalLoadGeneration,
            "Synchronizing tree selection started a redundant document read");
        const auto elapsed = GetTickCount64() - started;
        std::cout << "large-tree: selection completed in " << elapsed << " ms" << std::endl;
    };
    std::cout << "large-tree: distant first/last selection" << std::endl;
    for (int repeat = 0; repeat < 3; ++repeat) {
        select(firstFiles.front());
        select(lastFiles.back());
        select(lastFiles[1]);
        select(firstFiles[1]);
    }
    std::cout << "large-tree: collapsed preceding root and partial replacement" << std::endl;
    select(lastFiles.back());
    TreeView_Expand(app.treeH, TreeItem(app, roots.front()), TVE_COLLAPSE);
    auto& precedingFolder = *app.folders.front();
    app.ReplaceFolderTree(precedingFolder, std::make_unique<mm::FolderNode>(*precedingFolder.tree));
    select(lastFiles[1]);
    TreeView_Expand(app.treeH, TreeItem(app, roots.front()), TVE_EXPAND);
    select(lastFiles.back());
    std::cout << "large-tree: collapsed ancestors" << std::endl;
    const auto middleRoot = TreeItem(app, roots[1]);
    Check(middleRoot != nullptr, "Large-tree middle root is absent");
    app.treeUpdating = true;
    TreeView_Expand(app.treeH, middleRoot, TVE_COLLAPSE);
    app.treeUpdating = false;
    select(nestedFiles[1]);
    Check((TreeView_GetItemState(app.treeH, middleRoot, TVIS_EXPANDED) & TVIS_EXPANDED) != 0,
        "Selecting inside a collapsed tree left its root collapsed");
    const auto unrelatedRoot = TreeItem(app, roots.front());
    const auto topBefore = TreeView_GetFirstVisible(app.treeH);
    TVITEMW topInfo{};topInfo.mask = TVIF_PARAM;topInfo.hItem = topBefore;
    Check(topBefore && TreeView_GetItem(app.treeH, &topInfo), "Cannot remember large-tree viewport");
    const auto topPath = reinterpret_cast<mm::FolderNode*>(topInfo.lParam)->path;
    auto dumpViewport = [&](const char* stage) {
        auto describe = [&](const char* name, HTREEITEM item) {
            TVITEMW info{};info.mask = TVIF_PARAM | TVIF_STATE;info.stateMask = TVIS_EXPANDED;info.hItem = item;
            RECT bounds{};
            const bool hasInfo = item && TreeView_GetItem(app.treeH, &info);
            const bool hasRect = item && TreeView_GetItemRect(app.treeH, item, &bounds, FALSE);
            auto* node = hasInfo ? reinterpret_cast<mm::FolderNode*>(info.lParam) : nullptr;
            std::cout << "large-tree diagnostic: " << stage << ' ' << name
                << " path=" << (node ? mm::Utf8(node->path) : "<absent>")
                << " expanded=" << ((info.state & TVIS_EXPANDED) != 0)
                << " rect-valid=" << hasRect << " rect=" << bounds.top << ',' << bounds.bottom << std::endl;
        };
        describe("expected-top", TreeItem(app, topPath));
        describe("actual-top", TreeView_GetFirstVisible(app.treeH));
        describe("selected", TreeView_GetSelection(app.treeH));
        describe("middle-root", TreeItem(app, roots[1]));
        describe("nested", TreeItem(app, roots[1] / L"nested"));
        describe("deep", TreeItem(app, roots[1] / L"nested" / L"deep"));
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL};GetScrollInfo(app.treeH, SB_VERT, &scroll);
        std::cout << "large-tree diagnostic: " << stage << " scrollbar=" << scroll.nMin << ',' << scroll.nMax
            << " page=" << scroll.nPage << " position=" << scroll.nPos
            << " native-count=" << TreeView_GetCount(app.treeH)
            << " middle-model-children=" << app.folders[1]->tree->children.size() << std::endl;
    };
    dumpViewport("before-rescan");
    std::cout << "large-tree: rescanning selected root" << std::endl;
    auto& replacementFolder = *app.folders[1];
    app.ReplaceFolderTree(replacementFolder, std::make_unique<mm::FolderNode>(*replacementFolder.tree));
    dumpViewport("after-rescan");
    Check(TreeItem(app, roots.front()) == unrelatedRoot,
        "Large-tree rescan unnecessarily replaced an unrelated root");
    Check(selectedPath() == nestedFiles[1].wstring(), "Large-tree rescan lost the selected document");
    Check(TreeView_GetFirstVisible(app.treeH) == TreeItem(app, topPath),
        "Large-tree rescan changed the visible top row");
    assertVisible();
    std::cout << "large-tree: complete rebuild with restored selection" << std::endl;
    app.RebuildTree();
    Check(selectedPath() == nestedFiles[1].wstring(), "Large-tree rebuild lost the selected document");
    Check(TreeView_GetFirstVisible(app.treeH) == TreeItem(app, topPath),
        "Large-tree rebuild changed the visible top row");
    assertVisible();
    std::cout << "large-tree: one file removed and one added above the viewport" << std::endl;
    {
        // A real difference in the middle root: the first document goes away
        // and a new one sorts before everything else. Only those two rows may
        // change, and the selection and the viewport stay where they were.
        auto changed = std::make_unique<mm::FolderNode>(*replacementFolder.tree);
        const auto removed = firstFiles[1].wstring();
        std::erase_if(changed->children, [&](const auto& node) { return node.path == removed; });
        changed->children.push_front({L"document-000000.md", (roots[1] / L"document-000000.md").wstring(), false, {}});
        app.ReplaceFolderTree(replacementFolder, std::move(changed));
        app.FlushTreeTasks();
        Check(app.treeStats.rowsInserted == 1 && app.treeStats.rowsDeleted == 1, "Two-row difference touched other rows");
        Check(TreeItem(app, firstFiles[1]) == nullptr && TreeItem(app, roots[1] / L"document-000000.md") != nullptr,
            "Two-row difference was not applied");
        Check(selectedPath() == nestedFiles[1].wstring(), "Two-row difference lost the selected document");
        Check(TreeView_GetFirstVisible(app.treeH) == TreeItem(app, topPath), "Two-row difference changed the visible top row");
        assertVisible();
    }
    select(firstFiles.front());
    select(lastFiles.back());
    ShowWindow(app.hwnd, SW_HIDE);
    Write(fixture / L"result.txt", "PASS: 12012 native rows, distant selection, collapsed ancestors, rescan and rebuild restore.\n");
    std::cout << "PASS large native tree selection, collapsed ancestors, viewport and rescan restoration" << std::endl;
}

void TreeSelectionRenderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"tree-selection-rendering";
    const auto root = fixture / L"SelectedFolderReadable";
    const auto file = root / L"SelectedFileReadable.md";
    Write(file, "# Tree selection contrast\n\nRendered native selection must remain readable.\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.OpenRoot(root.wstring());
    Until([&] { return FolderTree(app, root) && TreeItem(app, file) != nullptr; }, "Selection rendering fixture did not scan");
    app.OpenPaths({file.wstring()});
    LoadedPath(app, file);
    app.Command(ExpandAll);
    app.sideWidth = 280;
    app.Layout();

    auto luminance = [](DWORD pixel) {
        auto linear = [](unsigned component) {
            const auto value = component / 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear((pixel >> 16) & 255) + 0.7152 * linear((pixel >> 8) & 255)
            + 0.0722 * linear(pixel & 255);
    };
    auto labelBounds = [&](HTREEITEM item, const RenderedHostImage& image) {
        RECT bounds{};
        Check(TreeView_GetItemRect(app.treeH, item, &bounds, TRUE) != FALSE, "Cannot locate rendered selection label");
        MapWindowPoints(app.treeH, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        // Exclude the native focus rectangle and edge padding. The remaining
        // pixels contain label text and its actual painted selection background.
        const auto inset = std::max(1, app.D(2));
        bounds.left += inset;
        bounds.top += inset;
        bounds.right -= inset;
        bounds.bottom -= inset;
        bounds.left = std::clamp<LONG>(bounds.left, 0, image.width);
        bounds.top = std::clamp<LONG>(bounds.top, 0, image.height);
        bounds.right = std::clamp<LONG>(bounds.right, 0, image.width);
        bounds.bottom = std::clamp<LONG>(bounds.bottom, 0, image.height);
        Check(bounds.right > bounds.left && bounds.bottom > bounds.top, "Rendered selection label has no inspectable area");
        return bounds;
    };
    auto background = [&](const RenderedHostImage& image, RECT bounds) {
        std::map<DWORD, size_t> colors;
        for (LONG y = bounds.top; y < bounds.bottom; ++y)
            for (LONG x = bounds.left; x < bounds.right; ++x)
                ++colors[image.pixels[static_cast<size_t>(y) * image.width + x] & 0x00ffffff];
        const auto common = std::max_element(colors.begin(), colors.end(), [](const auto& left, const auto& right) {
            return left.second < right.second;
        });
        Check(common != colors.end(), "Rendered selection label contains no colors");
        return common->first;
    };

    // Exercise both folder and plain-file rows, with actual Win32 keyboard
    // focus on the tree and on the document view. Return to light mode afterward
    // to catch stale custom-draw state as well as the original white dark row.
    for (const bool dark : {true, false}) {
        app.dark = dark;
        app.ApplyTheme();
        for (const bool folder : {true, false}) {
            const auto selected = TreeItem(app, folder ? root : file);
            const auto unselected = TreeItem(app, folder ? file : root);
            Check(selected != nullptr && unselected != nullptr, "Selection rendering tree item is missing");
            app.treeUpdating = true;
            TreeView_SelectItem(app.treeH, selected);
            app.treeUpdating = false;
            TreeView_EnsureVisible(app.treeH, selected);
            for (const bool focused : {true, false}) {
                const HWND focusTarget = focused ? app.treeH : app.view.Handle();
                SetFocus(focusTarget);
                Check(GetFocus() == focusTarget, "Cannot establish actual keyboard focus for hidden selection rendering");
                Check(TreeView_GetSelection(app.treeH) == selected
                    && (TreeView_GetItemState(app.treeH, selected, TVIS_SELECTED) & TVIS_SELECTED),
                    "Native selected state was lost before painting");
                const std::wstring name = std::wstring(dark ? L"dark-" : L"light-")
                    + (folder ? L"folder-" : L"file-") + (focused ? L"focused.png" : L"unfocused.png");
                RenderedHostImage image;
                SaveHostImage(app, fixture / name, &image);
                const auto selectedBounds = labelBounds(selected, image);
                const auto selectedColor = background(image, selectedBounds);
                const auto unselectedColor = background(image, labelBounds(unselected, image));
                const auto selectedLuminance = luminance(selectedColor);
                Check(dark ? selectedLuminance < 0.25 : selectedLuminance > 0.50,
                    dark ? "Dark selected row rendered a bright background" : "Returning to light mode retained a dark selected row");
                int colorDifference = 0;
                for (unsigned shift : {0u, 8u, 16u})
                    colorDifference = std::max(colorDifference,
                        std::abs(int((selectedColor >> shift) & 255) - int((unselectedColor >> shift) & 255)));
                Check(colorDifference >= 8, "Rendered selected row cannot be distinguished from an unselected row");

                size_t readableTextPixels = 0;
                for (LONG y = selectedBounds.top; y < selectedBounds.bottom; ++y) {
                    for (LONG x = selectedBounds.left; x < selectedBounds.right; ++x) {
                        const auto ink = luminance(image.pixels[static_cast<size_t>(y) * image.width + x]);
                        const auto contrast = (std::max(ink, selectedLuminance) + 0.05)
                            / (std::min(ink, selectedLuminance) + 0.05);
                        if (contrast >= 4.5) ++readableTextPixels;
                    }
                }
                const auto labelArea = static_cast<size_t>(selectedBounds.right - selectedBounds.left)
                    * (selectedBounds.bottom - selectedBounds.top);
                Check(readableTextPixels >= std::max<size_t>(20, labelArea / 100),
                    "Rendered selected label lacks readable text contrast against its background");
                Check(TreeView_GetSelection(app.treeH) == selected
                    && (TreeView_GetItemState(app.treeH, selected, TVIS_SELECTED) & TVIS_SELECTED),
                    "Selection painting changed the native selected state");
            }
        }
    }
    Check(!IsWindowVisible(app.hwnd), "Selection rendering test unexpectedly showed its host window");
    std::wcout << L"Tree selection renderings: " << fixture.wstring() << L"\n";
    std::cout << "PASS rendered folder/file selection contrast, actual focus states, dark-to-light transition\n";
}

double ScrollbarTrackBrightness(HWND control, HWND host, const RenderedHostImage& image, bool horizontal) {
    SCROLLBARINFO info{sizeof(info)};
    Check(GetScrollBarInfo(control, horizontal ? OBJID_HSCROLL : OBJID_VSCROLL, &info) != FALSE
        && !(info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)),
        "Overflowing native scrollbar is not visible");
    RECT track = info.rcScrollBar;
    const LONG origin = horizontal ? track.left : track.top;
    const LONG end = horizontal ? track.right : track.bottom;
    LONG from = origin + info.dxyLineButton, to = end - info.dxyLineButton;
    const LONG thumbStart = origin + info.xyThumbTop, thumbEnd = origin + info.xyThumbBottom;
    if (thumbStart - from > to - thumbEnd) to = thumbStart;
    else from = thumbEnd;
    if (horizontal) { track.left = from + 2; track.right = to - 2; }
    else { track.top = from + 2; track.bottom = to - 2; }
    InflateRect(&track, horizontal ? 0 : -2, horizontal ? -2 : 0);
    MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT*>(&track), 2);
    Check(track.left >= 0 && track.top >= 0 && track.right <= image.width && track.bottom <= image.height
        && track.right > track.left && track.bottom > track.top, "Native scrollbar track has no inspectable image area");
    double sum = 0;
    size_t count = 0;
    for (LONG y = track.top; y < track.bottom; ++y) for (LONG x = track.left; x < track.right; ++x) {
        const auto pixel = image.pixels[static_cast<size_t>(y) * image.width + x];
        sum += (((pixel >> 16) & 255) + ((pixel >> 8) & 255) + (pixel & 255)) / (3.0 * 255.0);
        ++count;
    }
    return sum / count;
}

void TreeScrollbarRenderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"tree-scrollbar-rendering";
    const auto root = fixture / L"The_long_folder_label_extends_beyond_the_sidebar";
    for (int index = 0; index < 48; ++index)
        Write(root / (L"Entry_" + std::to_wstring(index)
            + L"_The_long_Markdown_document_filename_extends_beyond_the_sidebar.md"), "# Scrollbar rendering\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.sideWidth = 200;
    SetWindowPos(app.hwnd, nullptr, 0, 0, app.D(1000), app.D(650), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Native scrollbar visibility and non-client painting require a visible
    // HWND; keep the test outside the desktop and away from the user's focus.
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    app.OpenRoot(root.wstring());
    Until([&] { return FolderTree(app, root) != nullptr; }, "Scrollbar fixture did not scan");
    app.Command(ExpandAll);
    const auto model = FolderTree(app, root);
    const auto rootItem = TreeItem(app, root);
    const auto selected = TreeView_GetChild(app.treeH, rootItem);
    Check(selected != nullptr, "Scrollbar fixture has no file rows");
    app.treeUpdating = true;
    TreeView_SelectItem(app.treeH, selected);
    app.treeUpdating = false;

    auto scroll = [&](int bar) {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        Check(GetScrollInfo(app.treeH, bar, &info) != FALSE, "Native tree scrollbar has no scroll information");
        Check(info.nMax - static_cast<int>(info.nPage) + 1 > info.nMin, "Long tree fixture did not overflow its viewport");
        return info;
    };
    const auto initial = scroll(SB_HORZ);
    SendMessageW(app.treeH, WM_HSCROLL, SB_RIGHT, 0);
    const auto right = scroll(SB_HORZ);
    Check(right.nPos > initial.nMin && right.nPos == right.nMax - static_cast<int>(right.nPage) + 1,
        "Native horizontal scrolling did not reach the right boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_LINERIGHT, 0);
    Check(scroll(SB_HORZ).nPos == right.nPos, "Horizontal scrolling passed the right boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_LEFT, 0);
    Check(scroll(SB_HORZ).nPos == initial.nMin, "Native horizontal scrolling did not reach the left boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_LINELEFT, 0);
    Check(scroll(SB_HORZ).nPos == initial.nMin, "Horizontal scrolling passed the left boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_LINERIGHT, 0);
    SendMessageW(app.treeH, WM_VSCROLL, SB_LINEDOWN, 0);
    const auto horizontal = scroll(SB_HORZ), vertical = scroll(SB_VERT);
    Check(horizontal.nPos > horizontal.nMin && vertical.nPos > vertical.nMin,
        "Cannot establish nonzero tree scroll positions for theme preservation");

    int phase = 0;
    for (bool dark : {true, false, true}) {
        app.dark = dark;
        app.ApplyTheme();
        const auto h = scroll(SB_HORZ), v = scroll(SB_VERT);
        RenderedHostImage image;
        SaveHostImage(app, fixture / (std::to_wstring(++phase) + (dark ? L"-dark.png" : L"-light.png")), &image);
        // Theme metrics may alter the content extent by a pixel; the viewport
        // and the user's position must remain stable.
        Check(h.nPos == horizontal.nPos && h.nPage == horizontal.nPage
            && v.nPos == vertical.nPos && v.nPage == vertical.nPage,
            "Changing theme altered the native tree scroll position or viewport");
        Check(FolderTree(app, root) == model && TreeItem(app, root) == rootItem
            && TreeView_GetSelection(app.treeH) == selected, "Changing scrollbar theme rebuilt the tree or lost selection");
        for (bool horizontalBar : {true, false}) {
            const double brightness = ScrollbarTrackBrightness(app.treeH, app.hwnd, image, horizontalBar);
            std::cout << "Tree " << (horizontalBar ? "horizontal" : "vertical") << " scrollbar "
                << (dark ? "dark" : "light") << " track brightness: " << brightness << '\n';
            Check(dark ? brightness < 0.45 : brightness > 0.70,
                dark ? "Dark native scrollbar track rendered a bright background" : "Light native scrollbar track retained a dark background");
        }
        SendMessageW(app.treeH, WM_HSCROLL, SB_RIGHT, 0);
        const auto limit = scroll(SB_HORZ);
        Check(limit.nPos == limit.nMax - static_cast<int>(limit.nPage) + 1,
            "Themed horizontal scrollbar did not reach its right boundary");
        SendMessageW(app.treeH, WM_HSCROLL, SB_LINERIGHT, 0);
        Check(scroll(SB_HORZ).nPos == limit.nPos, "Themed horizontal scrolling exceeded its range");
        SendMessageW(app.treeH, WM_HSCROLL, SB_LEFT, 0);
        Check(scroll(SB_HORZ).nPos == h.nMin, "Themed horizontal scrollbar did not reach its left boundary");
        SendMessageW(app.treeH, WM_HSCROLL, SB_LINERIGHT, 0);
        Check(scroll(SB_HORZ).nPos == horizontal.nPos, "Themed horizontal scroll step changed");
    }
    const auto displayed = app.currentDoc;
    const auto loadGeneration = app.loadGeneration, watchGeneration = app.watchGeneration;
    const auto scanGeneration = Folder(app, root).generation;
    std::vector<std::pair<const Watch*, HANDLE>> watchers;
    for (const auto& watch : app.watches) watchers.emplace_back(watch.get(), watch->dir);
    RECT client{}; GetClientRect(app.treeH, &client);
    POINT wheelPoint{client.right / 2, client.bottom / 2}; ClientToScreen(app.treeH, &wheelPoint);
    auto wheel = [&](int delta, WORD keys = MK_SHIFT) {
        const auto verticalBefore = scroll(SB_VERT).nPos;
        SendMessageW(app.treeH, WM_MOUSEWHEEL, MAKEWPARAM(keys, static_cast<WORD>(delta)),
            MAKELPARAM(wheelPoint.x, wheelPoint.y));
        if (keys == MK_SHIFT)
            Check(scroll(SB_VERT).nPos == verticalBefore, "Shift+wheel changed the vertical tree position");
    };
    SendMessageW(app.treeH, WM_HSCROLL, SB_LEFT, 0);
    const auto left = scroll(SB_HORZ).nPos;
    wheel(-WHEEL_DELTA);
    const auto oneNotch = scroll(SB_HORZ).nPos;
    Check(oneNotch > left, "Shift+wheel down did not move the tree right");
    wheel(WHEEL_DELTA);
    Check(scroll(SB_HORZ).nPos == left, "Shift+wheel up did not move the tree back left");
    for (int part = 0; part < 3; ++part) {
        wheel(-WHEEL_DELTA / 4);
        Check(scroll(SB_HORZ).nPos == left, "Partial Shift+wheel input moved before accumulating one notch");
    }
    wheel(-WHEEL_DELTA / 4);
    Check(scroll(SB_HORZ).nPos == oneNotch, "Four small downward wheel deltas did not equal one notch");
    for (int part = 0; part < 3; ++part) {
        wheel(WHEEL_DELTA / 4);
        Check(scroll(SB_HORZ).nPos == oneNotch, "Partial upward wheel input moved before accumulating one notch");
    }
    wheel(WHEEL_DELTA / 4);
    Check(scroll(SB_HORZ).nPos == left, "Four small upward wheel deltas did not move back left");
    wheel(WHEEL_DELTA);
    Check(scroll(SB_HORZ).nPos == left, "Shift+wheel scrolled past the left boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_RIGHT, 0);
    const auto rightLimit = scroll(SB_HORZ);
    wheel(-WHEEL_DELTA);
    Check(scroll(SB_HORZ).nPos == rightLimit.nPos
        && rightLimit.nPos == rightLimit.nMax - static_cast<int>(rightLimit.nPage) + 1,
        "Shift+wheel scrolled past the right boundary");
    SendMessageW(app.treeH, WM_HSCROLL, SB_LEFT, 0);
    wheel(-WHEEL_DELTA);
    for (WORD keys : {WORD(0), WORD(MK_CONTROL | MK_SHIFT)}) {
        wheel(-WHEEL_DELTA, keys);
        Check(scroll(SB_HORZ).nPos == oneNotch, "Normal wheel or Ctrl+Shift+wheel entered Shift horizontal scrolling");
    }
    Check(FolderTree(app, root) == model && TreeItem(app, root) == rootItem && TreeView_GetSelection(app.treeH) == selected
        && app.currentDoc == displayed && app.loadGeneration == loadGeneration && Folder(app, root).generation == scanGeneration
        && app.watchGeneration == watchGeneration && app.watches.size() == watchers.size(),
        "Tree wheel input changed the document, model, selection, scan, or watcher state");
    for (size_t index = 0; index < watchers.size(); ++index)
        Check(app.watches[index].get() == watchers[index].first && app.watches[index]->dir == watchers[index].second,
            "Tree wheel input replaced an existing watcher");
    ShowWindow(app.hwnd, SW_HIDE);
    std::wcout << L"Tree scrollbar renderings: " << fixture.wstring() << L"\n";
    std::cout << "PASS native scrollbar pixels/themes, Shift+wheel direction/remainders/bounds, unchanged document/tree/watchers\n";
}

void DocumentScrollbarRenderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"document-scrollbar-rendering";
    const auto path = fixture / L"LongDocument.md";
    Write(path, LongDocument("Document scrollbar theme") + "\n```text\n" + std::string(180, 'W') + "\n```\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), app.D(1000), app.D(650), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    app.OpenPaths({path.wstring()});
    LoadedPath(app, path);
    LayoutDocument(app);
    const auto view = app.view.Handle();
    RedrawWindow(view, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    auto scroll = [&](int bar) {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        Check(GetScrollInfo(view, bar, &info) != FALSE
            && info.nMax - static_cast<int>(info.nPage) + 1 > info.nMin,
            "Long document or code line did not overflow its native viewport");
        return info;
    };
    for (UINT message : {WM_VSCROLL, WM_HSCROLL}) {
        SendMessageW(view, message, SB_TOP, 0);
        SendMessageW(view, message, SB_LINEDOWN, 0);
    }
    const auto vertical = scroll(SB_VERT), horizontal = scroll(SB_HORZ);
    Check(vertical.nPos > vertical.nMin && horizontal.nPos > horizontal.nMin,
        "Cannot establish nonzero document scroll positions");
    const auto document = app.currentDoc;
    const auto loadGeneration = app.loadGeneration;
    int phase = 0;
    for (bool dark : {true, false, true}) {
        app.dark = dark; app.ApplyTheme();
        RenderedHostImage image;
        SaveHostImage(app, fixture / (std::to_wstring(++phase) + (dark ? L"-dark.png" : L"-light.png")), &image);
        Check(app.currentDoc == document && app.loadGeneration == loadGeneration,
            "Changing scrollbar theme replaced or reloaded the displayed document");
        for (bool horizontalBar : {false, true}) {
            const int bar = horizontalBar ? SB_HORZ : SB_VERT;
            const UINT message = horizontalBar ? WM_HSCROLL : WM_VSCROLL;
            const auto before = horizontalBar ? horizontal : vertical;
            const auto current = scroll(bar);
            Check(current.nPos == before.nPos && current.nPage == before.nPage,
                "Changing theme moved the document's scroll position or viewport");
            const double brightness = ScrollbarTrackBrightness(view, app.hwnd, image, horizontalBar);
            std::cout << "Document " << (horizontalBar ? "horizontal" : "vertical") << " scrollbar "
                << (dark ? "dark" : "light") << " track brightness: " << brightness << '\n';
            Check(dark ? brightness < 0.45 : brightness > 0.70,
                dark ? "Dark document scrollbar track rendered a bright background" : "Light document scrollbar track retained a dark background");
            SendMessageW(view, message, SB_BOTTOM, 0);
            const auto limit = scroll(bar);
            Check(limit.nPos == limit.nMax - static_cast<int>(limit.nPage) + 1,
                "Themed document scrollbar did not reach its lower/right boundary");
            SendMessageW(view, message, SB_LINEDOWN, 0);
            Check(scroll(bar).nPos == limit.nPos, "Themed document scrolling exceeded its upper range");
            SendMessageW(view, message, SB_TOP, 0);
            SendMessageW(view, message, SB_LINEUP, 0);
            Check(scroll(bar).nPos == current.nMin, "Themed document scrolling passed its top/left boundary");
            SendMessageW(view, message, SB_LINEDOWN, 0);
            Check(scroll(bar).nPos == before.nPos, "Themed document scroll step changed");
        }
    }
    ShowWindow(app.hwnd, SW_HIDE);
    std::wcout << L"Document scrollbar renderings: " << fixture.wstring() << L"\n";
    std::cout << "PASS document scrollbar pixels, native movement/ranges, dark-light-dark position and document preservation\n";
}

void TabOverflowRenderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"tab-overflow-rendering";
    std::vector<std::wstring> paths;
    for (int index = 0; index < 20; ++index) {
        const auto path = fixture / (L"Document-" + std::to_wstring(index) + L".md");
        Write(path, LongDocument("Tab overflow " + std::to_string(index))); paths.push_back(path.wstring());
    }
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.dark = true; app.ApplyTheme();
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(6000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), app.D(1000), app.D(650), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    app.OpenPaths(paths); LoadedPath(app, fs::path(paths.back())); LayoutDocument(app);
    app.view.SetScrollRatio(0.35); RedrawWindow(app.view.Handle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    auto arrows = [&] { return FindWindowExW(app.tabsH, nullptr, UPDOWN_CLASSW, nullptr); };
    auto overflow = [&] { const auto child = arrows(); return child && IsWindowVisible(child); };
    Check(overflow(), "Twenty native tabs did not produce an overflow arrow control");
    auto position = [&] {
        // This child has no numeric buddy; its buddy-parse error is unrelated
        // to the valid native scrolling position returned by UDM_GETPOS32.
        return static_cast<int>(SendMessageW(arrows(), UDM_GETPOS32, 0, 0));
    };
    auto firstLeft = [&] {
        RECT row{}; Check(TabCtrl_GetItemRect(app.tabsH, 0, &row) != FALSE, "Cannot inspect the first native tab rectangle");
        return row.left;
    };
    struct Reading { int active, selected, font, tabFont; unsigned long long id, generation;
        std::shared_ptr<mm::Document> document; double scroll; std::vector<unsigned long long> order; };
    auto capture = [&] {
        Reading result{app.active, TabCtrl_GetCurSel(app.tabsH), app.view.FontSize(), app.tabs[app.active].font,
            app.tabs[app.active].id, app.loadGeneration, app.currentDoc, app.view.ScrollRatio()};
        for (const auto& tab : app.tabs) result.order.push_back(tab.id);
        return result;
    };
    auto reading = capture();
    auto unchanged = [&] {
        std::vector<unsigned long long> order;
        for (const auto& tab : app.tabs) order.push_back(tab.id);
        Check(app.active == reading.active && TabCtrl_GetCurSel(app.tabsH) == reading.selected
            && app.tabs[app.active].id == reading.id && app.currentDoc == reading.document
            && app.loadGeneration == reading.generation && app.view.FontSize() == reading.font
            && app.tabs[app.active].font == reading.tabFont && order == reading.order,
            "Scrolling the tab strip selected/reloaded a document or changed its font/order");
        Near(app.view.ScrollRatio(), reading.scroll, "Scrolling the tab strip moved the displayed document");
    };
    auto wheel = [&](HWND receiver, int delta, WORD keys = 0) {
        RECT bounds{}; GetWindowRect(receiver, &bounds);
        SendMessageW(receiver, WM_MOUSEWHEEL, MAKEWPARAM(keys, static_cast<WORD>(delta)),
            MAKELPARAM(bounds.left + 4, bounds.top + 4)); unchanged();
    };
    auto render = [&](const wchar_t* name) {
        RenderedHostImage image; SaveHostImage(app, fixture / name, &image);
        RECT bounds{}; Check(GetWindowRect(arrows(), &bounds) != FALSE, "Cannot locate rendered overflow arrows");
        MapWindowPoints(HWND_DESKTOP, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        RECT tab{}; Check(TabCtrl_GetItemRect(app.tabsH, 0, &tab) != FALSE, "Cannot inspect tab row for arrow alignment");
        MapWindowPoints(app.tabsH, app.hwnd, reinterpret_cast<POINT*>(&tab), 2);
        Check(std::abs((bounds.top + bounds.bottom) - (tab.top + tab.bottom)) <= 1,
            "Overflow arrows are not vertically centered on the tab row");
        Check(bounds.left >= 0 && bounds.right <= image.width && bounds.top >= 0 && bounds.bottom <= image.height,
            "Rendered overflow arrows are outside the host image");
        for (int half = 0; half < 2; ++half) {
            RECT button = bounds;
            if (half == 0) button.right = (bounds.left + bounds.right) / 2;
            else button.left = (bounds.left + bounds.right) / 2;
            InflateRect(&button, -2, -2);
            std::map<DWORD, size_t> colors;
            for (LONG y = button.top; y < button.bottom; ++y) for (LONG x = button.left; x < button.right; ++x)
                ++colors[image.pixels[static_cast<size_t>(y) * image.width + x] & 0x00ffffff];
            Check(!colors.empty(), "Overflow arrow has no inspectable pixels");
            const auto common = std::max_element(colors.begin(), colors.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            const auto color = common->first;
            const double brightness = (((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255)) / (3.0 * 255.0);
            std::cout << "Tab " << (half ? "right" : "left") << " arrow " << (app.dark ? "dark" : "light")
                << " background brightness: " << brightness << '\n';
            Check(app.dark ? brightness < 0.45 : brightness > 0.70,
                "Native tab overflow arrow background does not match the selected theme");
        }
        unchanged();
    };
    render(L"initial-dark.png");
    wheel(app.tabsH, WHEEL_DELTA * 24);
    const auto left = position();
    const auto initialLeft = firstLeft();
    Check(left == 0 && initialLeft >= 0 && initialLeft < app.D(8),
        "A multi-notch wheel event did not bring the actual first tab to the left edge");
    wheel(app.tabsH, -WHEEL_DELTA * 4);
    const auto multiPosition = position();
    const auto multiLeft = firstLeft();
    wheel(app.tabsH, WHEEL_DELTA * 4);
    Check(position() == left && firstLeft() == initialLeft, "Multi-notch reverse scrolling did not restore the first tab");
    for (int notch = 0; notch < 4; ++notch) wheel(app.tabsH, -WHEEL_DELTA);
    Check(position() == multiPosition && firstLeft() == multiLeft,
        "Multi-notch and repeated single-notch wheel events moved different native tab rectangles");
    for (int notch = 0; notch < 4; ++notch) wheel(app.tabsH, WHEEL_DELTA);
    Check(position() == left && firstLeft() == initialLeft, "Repeated reverse scrolling did not restore the first tab");
    wheel(app.tabsH, -WHEEL_DELTA);
    const auto oneNotch = position();
    Check(oneNotch > left && firstLeft() < initialLeft, "Unmodified wheel did not move the native tab strip right");
    wheel(app.tabsH, WHEEL_DELTA);
    Check(position() == left && firstLeft() == initialLeft, "Unmodified wheel did not move the native tab strip back left");
    for (const WORD keys : {WORD(0), WORD(MK_SHIFT)}) {
        wheel(app.tabsH, -WHEEL_DELTA, keys);
        Check(position() == oneNotch, "Tab wheel with or without Shift did not move right");
        wheel(app.tabsH, WHEEL_DELTA, keys);
        Check(position() == left, "Tab wheel with or without Shift did not return left");
        for (int direction : {-1, 1}) {
            const auto before = position();
            for (int part = 0; part < 3; ++part) {
                wheel(app.tabsH, direction * WHEEL_DELTA / 4, keys);
                Check(position() == before, "Partial tab wheel input moved before reaching one notch");
            }
            wheel(app.tabsH, direction * WHEEL_DELTA / 4, keys);
            Check(position() == (direction < 0 ? oneNotch : left), "Four small tab wheel inputs did not equal one notch");
        }
        wheel(arrows(), -WHEEL_DELTA, keys);
        Check(position() == oneNotch, "Wheel over the native arrow child did not move the tab strip");
        wheel(arrows(), WHEEL_DELTA, keys);
        Check(position() == left, "Wheel over the native arrow child did not return the tab strip");
    }
    SetFocus(app.view.Handle());
    Check(GetFocus() == app.view.Handle(), "Cannot focus the document before pointer-routed tab wheel input");
    RECT strip{}; GetWindowRect(app.tabsH, &strip);
    MSG routed{}; routed.hwnd = app.view.Handle(); routed.message = WM_MOUSEWHEEL;
    const auto plus = std::find_if(app.buttons.begin(), app.buttons.end(), [](const auto& item) { return item.first == NewTab; });
    Check(plus != app.buttons.end(), "New-tab button is missing from the tab band");
    RECT plusBounds{}; GetWindowRect(plus->second, &plusBounds);
    for (const WORD keys : {WORD(0), WORD(MK_SHIFT)}) {
        routed.wParam = MAKEWPARAM(keys, static_cast<WORD>(-WHEEL_DELTA));
        routed.lParam = MAKELPARAM(strip.left + 20, strip.top + 12);
        Check(app.TabWheel(routed) && position() == oneNotch, "Pointer over tabs did not route wheel input away from document focus");
        unchanged();
        RECT arrowBounds{}; GetWindowRect(arrows(), &arrowBounds);
        routed.wParam = MAKEWPARAM(keys, WHEEL_DELTA);
        routed.lParam = MAKELPARAM((arrowBounds.left + arrowBounds.right) / 2, (arrowBounds.top + arrowBounds.bottom) / 2);
        Check(app.TabWheel(routed) && position() == left, "Pointer over arrows did not route wheel input away from document focus");
        unchanged();
        routed.wParam = MAKEWPARAM(keys, static_cast<WORD>(-WHEEL_DELTA));
        routed.lParam = MAKELPARAM((plusBounds.left + plusBounds.right) / 2, (plusBounds.top + plusBounds.bottom) / 2);
        Check(app.TabWheel(routed) && position() == oneNotch, "Pointer over the plus button did not scroll the tab strip");
        unchanged();
        wheel(app.tabsH, WHEEL_DELTA, keys);
    }
    for (const WORD keys : {WORD(MK_CONTROL), WORD(MK_CONTROL | MK_SHIFT)}) {
        routed.wParam = MAKEWPARAM(keys, static_cast<WORD>(-WHEEL_DELTA));
        routed.lParam = MAKELPARAM(strip.left + 20, strip.top + 12);
        Check(!app.TabWheel(routed) && position() == left, "Ctrl wheel over tabs was consumed as tab scrolling");
        unchanged();
    }
    RECT documentBounds{}; GetWindowRect(app.view.Handle(), &documentBounds);
    routed.lParam = MAKELPARAM((documentBounds.left + documentBounds.right) / 2,
        (documentBounds.top + documentBounds.bottom) / 2);
    for (const WORD keys : {WORD(0), WORD(MK_SHIFT)}) {
        routed.wParam = MAKEWPARAM(keys, static_cast<WORD>(-WHEEL_DELTA));
        Check(!app.TabWheel(routed) && position() == left, "Wheel over the document was consumed as tab scrolling");
        unchanged();
    }
    routed.wParam = MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA));
    SendMessageW(routed.hwnd, routed.message, routed.wParam, routed.lParam);
    Check(app.view.ScrollRatio() > reading.scroll, "Unmodified wheel over the document did not retain normal scrolling");
    app.view.SetScrollRatio(reading.scroll);
    unchanged();
    wheel(app.tabsH, WHEEL_DELTA);
    wheel(app.tabsH, WHEEL_DELTA);
    Check(position() == left, "Tab wheel passed the left overflow limit");
    wheel(app.tabsH, -WHEEL_DELTA * 24);
    const auto right = position();
    Check(right > left, "Native tab overflow has no rightward range");
    wheel(arrows(), -WHEEL_DELTA);
    Check(position() == right, "Tab wheel passed the right overflow limit");
    wheel(app.tabsH, WHEEL_DELTA * 24);
    auto arrowClick = [&](bool rightButton) {
        const auto child = arrows(); RECT bounds{}; GetClientRect(child, &bounds);
        const POINT point{bounds.right * (rightButton ? 3 : 1) / 4, bounds.bottom / 2};
        // Prequeue release as well, so native tracking implementations with a
        // nested message loop terminate without physical input.
        PostMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y)); Pump(); unchanged();
    };
    arrowClick(true);
    Check(position() > left && firstLeft() < initialLeft, "Native right arrow mouse click did not move the tab strip");
    arrowClick(false);
    Check(position() == left && firstLeft() == initialLeft, "Native left arrow mouse click did not move the tab strip back");
    wheel(app.tabsH, -WHEEL_DELTA);
    int phase = 0;
    for (bool dark : {true, false, true}) {
        app.dark = dark; app.ApplyTheme();
        const auto name = std::to_wstring(++phase) + (dark ? L"-dark.png" : L"-light.png");
        render(name.c_str());
    }
    while (app.tabs.size() > 2) app.Close(0);
    LoadedPath(app, fs::path(paths.back()));
    RedrawWindow(app.view.Handle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); reading = capture();
    Check(!overflow(), "Closing tabs did not remove the need for overflow arrows");
    const auto fittedLeft = firstLeft(); wheel(app.tabsH, -WHEEL_DELTA * 24);
    Check(firstLeft() == fittedLeft, "Wheel moved a tab strip that fits its viewport");
    app.OpenPaths(paths); LoadedPath(app, fs::path(paths.back()));
    RedrawWindow(app.view.Handle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); reading = capture();
    Check(overflow(), "Reopening tabs did not recreate native overflow arrows");
    render(L"reopened-dark.png");
    // Use a realistic window width; Windows clamps oversized top-level
    // windows to the monitor's maximum tracking size.
    while (app.tabs.size() > 6) app.Close(app.active == 0 ? 1 : 0);
    LoadedPath(app, fs::path(paths.back()));
    SetWindowPos(app.hwnd, nullptr, 0, 0, app.D(1300), app.D(650), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Check(!overflow(), "Widening the window did not fit all native tabs");
    SetWindowPos(app.hwnd, nullptr, 0, 0, app.D(800), app.D(650), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(app.view.Handle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); reading = capture();
    Check(overflow(), "Narrowing the window did not restore overflow arrows");
    render(L"resized-dark.png");
    ShowWindow(app.hwnd, SW_HIDE);
    std::wcout << L"Tab overflow renderings: " << fixture.wstring() << L"\n";
    std::cout << "PASS centered themed tab overflow arrows, native clicks, pointer-routed wheel with/without Shift, remainders/ranges, stable document\n";
}

void ActiveTabRenderingChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"active-tab-rendering";
    const std::vector<fs::path> paths{fixture / L"README.md", fixture / L"DESIGN.md", fixture / L"SECURITY.md"};
    for (const auto& path : paths) Write(path, "# " + path.stem().string() + "\n\nActive tab rendering verification.\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    SetWindowPos(app.hwnd, nullptr, 0, 0, app.D(1000), app.D(700), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    app.OpenPaths({paths[0].wstring(), paths[1].wstring(), paths[2].wstring()});
    LoadedPath(app, paths[2]);
    app.dark = true;
    app.ApplyTheme();
    SetFocus(app.tabsH);
    // Hidden windows discard update regions. Keep this test window outside the
    // virtual desktop, without activation or a taskbar entry, to exercise paint.
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000),
        GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    auto clearUpdate = [&] {
        ValidateRect(app.tabsH, nullptr);
        Check(!GetUpdateRect(app.tabsH, nullptr, FALSE), "Cannot clear prior tab paint work before switching");
    };
    auto tabPoint = [&](int index, bool close = false) {
        RECT bounds{};
        Check(TabCtrl_GetItemRect(app.tabsH, index, &bounds) != FALSE, "Cannot locate native tab click target");
        return POINT{close ? bounds.right - app.D(12) : bounds.left + (bounds.right - bounds.left) / 3,
            (bounds.top + bounds.bottom) / 2};
    };
    auto mouseClick = [&](POINT point) {
        SendMessageW(app.tabsH, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        SendMessageW(app.tabsH, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    };
    auto verify = [&](const wchar_t* name) {
        RECT strip{}, update{};
        GetClientRect(app.tabsH, &strip);
        // Check before pumping messages or using WM_PRINT. A forced snapshot
        // alone redraws every tab and would hide the stale-decoration defect.
        const bool pending = GetUpdateRect(app.tabsH, &update, FALSE) != FALSE;
        Check(pending && update.left <= strip.left && update.top <= strip.top
            && update.right >= strip.right && update.bottom >= strip.bottom,
            "Tab transition did not invalidate the entire strip before painting");
        SendMessageW(app.tabsH, WM_PAINT, 0, 0);
        Check(!GetUpdateRect(app.tabsH, nullptr, FALSE), "Native tab paint did not consume the pending repaint");
        Check(app.active >= 0 && TabCtrl_GetCurSel(app.tabsH) == app.active, "Native and document tab selection disagree");
        LoadedPath(app, fs::path(app.tabs[app.active].path));
        RenderedHostImage image;
        SaveHostImage(app, fixture / name, &image);
        int decorated = 0;
        for (int index = 0; index < TabCtrl_GetItemCount(app.tabsH); ++index) {
            RECT bounds{};
            TabCtrl_GetItemRect(app.tabsH, index, &bounds);
            MapWindowPoints(app.tabsH, app.hwnd, reinterpret_cast<POINT*>(&bounds), 2);
            bounds.left += app.D(6);
            bounds.right -= app.D(6);
            bounds.bottom = bounds.top + std::max(1, app.D(2));
            Check(bounds.left >= 0 && bounds.right <= image.width && bounds.top >= 0 && bounds.bottom <= image.height,
                "Rendered tab accent is outside the captured host image");
            size_t blue = 0, total = 0;
            for (LONG y = bounds.top; y < bounds.bottom; ++y) for (LONG x = bounds.left; x < bounds.right; ++x) {
                const auto pixel = image.pixels[static_cast<size_t>(y) * image.width + x];
                const int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blueChannel = pixel & 255;
                if (blueChannel > red + 30 && blueChannel > green + 20) ++blue;
                ++total;
            }
            const bool hasAccent = total > 0 && blue > total / 2;
            Check(hasAccent == (index == app.active), "Rendered top accent belongs to an inactive tab or is missing from the active tab");
            if (hasAccent) ++decorated;
        }
        Check(decorated == 1, "Exactly one active tab must have a blue top accent");
    };

    clearUpdate();
    mouseClick(tabPoint(0));
    Check(app.active == 0, "Mouse selection did not activate README");
    verify(L"readme-selected.png");
    clearUpdate();
    mouseClick(tabPoint(1));
    Check(app.active == 1, "Mouse selection did not activate DESIGN");
    verify(L"design-selected.png");
    clearUpdate();
    Check(Key(app, VK_TAB, true) && app.active == 2, "Ctrl+Tab did not activate SECURITY");
    verify(L"ctrl-tab-selected.png");
    const auto activeId = app.tabs[app.active].id;
    clearUpdate();
    const auto from = tabPoint(2), to = tabPoint(0);
    SendMessageW(app.tabsH, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(app.tabsH, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(app.tabsH, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
    Check(app.active == 0 && app.tabs[0].id == activeId, "Reordering changed the active document identity");
    verify(L"reordered-active.png");
    clearUpdate();
    mouseClick(tabPoint(app.active, true));
    Check(app.tabs.size() == 2 && app.tabs[app.active].id != activeId, "Closing the active tab did not select a remaining document");
    verify(L"closed-active.png");
    ShowWindow(app.hwnd, SW_HIDE);
    std::wcout << L"Active tab renderings: " << fixture.wstring() << L"\n";
    std::cout << "PASS full-strip tab invalidation before paint, mouse/keyboard/reorder/close, single active accent\n";
}

void Snapshots(App& app, const fs::path& sourceRoot, const fs::path& outputDirectory) {
    fs::create_directories(outputDirectory);
    const auto fixtures = sourceRoot / L"tests" / L"fixtures";
    const auto welcome = fixtures / L"はじめに.md";
    Check(fs::exists(welcome), "Host screenshot fixture is missing");

    // Close generated test tabs so snapshots show the bundled demonstration.
    while (!app.tabs.empty()) app.Close(static_cast<int>(app.tabs.size()) - 1);
    RemoveAllFolders(app);
    app.fontDefault = 16;
    app.OpenRoot(fixtures.wstring());
    Until([&] { return FolderTree(app, fixtures) != nullptr; }, "Screenshot folder scan timed out");
    app.OpenPaths({welcome.wstring()});
    LoadedPath(app, welcome);
    const auto referenceFolder = sourceRoot / L"docs";
    app.OpenRoot(referenceFolder.wstring());
    Until([&] { return FolderTree(app, referenceFolder) != nullptr; }, "Second screenshot folder scan timed out");
    Check(FolderTree(app, fixtures) != nullptr && app.folders.size() == 2,
        "Adding the second screenshot folder replaced the first");
    app.sidebar = true;
    app.sideWidth = 280;
    app.searching = false;
    app.wide = false;
    app.Command(ExpandAll);
    RECT window{0, 0, 1100, 780};
    Check(AdjustWindowRectExForDpi(&window, static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE)), app.dpi) != FALSE,
        "Cannot calculate screenshot window size");
    SetWindowPos(app.hwnd, nullptr, 0, 0, window.right - window.left, window.bottom - window.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    app.Layout();
    for (bool dark : {false, true}) {
        app.dark = dark;
        app.ApplyTheme();
        Check(Key(app, '0', true) && app.view.FontSize() == 16, "Ctrl+0 did not restore 100 percent");
        app.view.SetScrollRatio(0);
        SaveHostImage(app, outputDirectory / (dark ? L"host-dark.png" : L"host-light.png"));
        SendMessageW(app.view.Handle(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 4 * WHEEL_DELTA), 0);
        Check(app.view.FontSize() == 20 && app.tabs[app.active].font == 20, "Ctrl+wheel did not apply 125 percent");
        SaveHostImage(app, outputDirectory / (dark ? L"host-zoom-125-dark.png" : L"host-zoom-125-light.png"));
        Check(Key(app, '0', true), "Cannot reset zoom before sidebar snapshot");
        const auto originalWidth = app.sideWidth;
        RECT splitter{};GetClientRect(app.splitH, &splitter);
        SendMessageW(app.splitH, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitter.right / 2, app.D(90)));
        POINT destination{app.D(400), app.D(175)};ClientToScreen(app.hwnd, &destination);
        auto point = destination;ScreenToClient(app.splitH, &point);
        SendMessageW(app.splitH, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        point = destination;ScreenToClient(app.splitH, &point);
        SendMessageW(app.splitH, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        Check(std::abs(app.sideWidth - 400) <= 1, "Sidebar snapshot did not resize through native dragging");
        SaveHostImage(app, outputDirectory / (dark ? L"host-sidebar-wide-dark.png" : L"host-sidebar-wide-light.png"));
        SendMessageW(app.splitH, WM_MOUSELEAVE, 0, 0);
        app.sideWidth = originalWidth;app.Layout(true);
    }
    Check(!IsWindowVisible(app.hwnd), "Snapshot capture must not show the test window");
    std::wcout << L"Host screenshots: " << outputDirectory.wstring() << L"\n";
}

} // namespace

namespace {

Watch* WatchFor(App& app, const fs::path& folder) {
    const auto path = mm::NormalizePath(folder.wstring());
    for (const auto& watch : app.watches)
        if (_wcsicmp(watch->path.c_str(), path.c_str()) == 0 || (watch->recursive && App::IsWithinFolder(path, watch->path))) return watch.get();
    return nullptr;
}

void TreeRescanEfficiencyChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"tree-rescan-efficiency";
    const auto rootA = fixture / L"root-a", rootB = fixture / L"root-b";
    Write(rootA / L"same.md", "# A\n");
    Write(rootA / L"nested" / L"inner.md", "# Inner\n");
    Write(rootB / L"same.md", "# B\n");
    Write(rootB / L"other.md", "# Other\n");
    Host host(fixture / L"session.ini");
    auto& app = host.app;
    TreeMutationTrace trace;
    Check(SetWindowSubclass(app.treeH, TreeMutationTrace::Proc, 0x4d5245, reinterpret_cast<DWORD_PTR>(&trace)) != FALSE,
        "Cannot trace native tree messages");
    struct RemoveTrace {
        HWND window;
        ~RemoveTrace() { if (IsWindow(window)) RemoveWindowSubclass(window, TreeMutationTrace::Proc, 0x4d5245); }
    } removeTrace{app.treeH};
    app.OpenRoot(rootA.wstring());
    app.OpenRoot(rootB.wstring());
    Until([&] { return FolderTree(app, rootA) && FolderTree(app, rootB); }, "Efficiency fixture roots did not scan");
    Until([&] { return WatchCoversFolder(app, rootA) && WatchCoversFolder(app, rootB); }, "Efficiency fixture roots were not watched");
    // Consume whatever is still queued on root A's watch and wait until no
    // rescan, watcher setup or debounce is pending, so that every later
    // "nothing else happened" check starts from rest.
    auto settle = [&](const char* message) {
        Until([&] {
            auto* watch = WatchFor(app, rootA);
            while (watch && watch->pending && WaitForSingleObject(watch->event, 0) == WAIT_OBJECT_0) app.PollWatches();
            return !app.treeDirty && app.watchRescanFolders.empty() && !app.watchSetupPending
                && !Folder(app, rootA).scanning && !Folder(app, rootB).scanning;
        }, message);
    };
    PumpFor(800);
    settle("Fixture roots did not settle after the initial scans");

    std::cout << "rescan-efficiency: identical rescan keeps native rows" << std::endl;
    const auto nestedItem = TreeItem(app, rootA / L"nested");
    Check(nestedItem != nullptr, "Nested fixture folder is absent");
    TreeView_Expand(app.treeH, nestedItem, TVE_EXPAND);
    const auto innerItem = TreeItem(app, rootA / L"nested" / L"inner.md");
    const auto* modelA = FolderTree(app, rootA);
    const auto generationA = Folder(app, rootA).generation, generationB = Folder(app, rootB).generation;
    trace.insertions = trace.deletions = 0;
    app.RefreshFolder(Folder(app, rootA));
    Until([&] { return !Folder(app, rootA).scanning; }, "Unchanged rescan did not finish");
    Check(Folder(app, rootA).generation > generationA, "Rescan did not advance the folder generation");
    Check(trace.insertions == 0 && trace.deletions == 0, "Unchanged rescan rebuilt native tree rows");
    Check(FolderTree(app, rootA) == modelA, "Unchanged rescan replaced the folder model");
    Check(TreeItem(app, rootA / L"nested") == nestedItem && TreeItem(app, rootA / L"nested" / L"inner.md") == innerItem,
        "Unchanged rescan replaced native items");
    Check((TreeView_GetItemState(app.treeH, nestedItem, TVIS_EXPANDED) & TVIS_EXPANDED) != 0,
        "Unchanged rescan collapsed an expanded folder");
    Check(Folder(app, rootB).generation == generationB, "Rescanning one root touched another root");

    std::cout << "rescan-efficiency: change in one root rescans only that root" << std::endl;
    settle("Watches did not settle before the targeted rescan check");
    const auto generationA2 = Folder(app, rootA).generation, generationB2 = Folder(app, rootB).generation;
    trace.insertions = trace.deletions = 0;
    Write(rootA / L"added.md", "# Added\n");
    Until([&] { return TreeItem(app, rootA / L"added.md") != nullptr; }, "Added Markdown did not appear in the tree");
    Check(Folder(app, rootA).generation > generationA2, "A change in root A did not rescan root A");
    settle("Watches did not settle after the targeted rescan");
    Check(Folder(app, rootB).generation == generationB2, "A change in root A rescanned the unrelated root B");
    Check(trace.insertions == 1 && trace.deletions == 0, "One added file changed more than one native row");
    Check(TreeItem(app, rootB / L"other.md") != nullptr, "Targeted rescan lost rows of the untouched root");

    std::cout << "rescan-efficiency: non-Markdown notifications never walk the tree" << std::endl;
    Check(app.knownFolders.contains(mm::NormalizePath((rootA / L"nested").wstring()))
        && app.knownFolders.contains(mm::NormalizePath(rootB.wstring()))
        && !app.knownFolders.contains(mm::NormalizePath((rootA / L"same.md").wstring())),
        "Known folder set does not match the displayed tree");
    settle("Watches did not settle before the non-Markdown notification");
    auto* watchA = WatchFor(app, rootA);
    Check(watchA != nullptr && watchA->pending, "Root A watch is missing or not armed");
    Write(rootA / L"noise.txt", "not markdown");
    Check(WaitForSingleObject(watchA->event, 3000) == WAIT_OBJECT_0, "Non-Markdown creation did not signal its watch");
    trace.reads = 0;
    app.PollWatches();
    Check(trace.reads == 0, "A non-Markdown notification walked the native tree");
    Check(!app.treeDirty, "A non-Markdown file marked the tree as changed");
    Check(watchA->pending, "Watch was not re-armed after a non-Markdown notification");

    std::cout << "rescan-efficiency: overflow rescans the affected root only" << std::endl;
    settle("Watches did not settle before the overflow check");
    watchA = WatchFor(app, rootA);
    Check(watchA != nullptr && watchA->pending, "Root A watch is missing before the overflow check");
    watchA->Cancel();
    if (watchA->pending) { DWORD ignored = 0; GetOverlappedResult(watchA->dir, &watchA->ov, &ignored, TRUE); }
    watchA->pending = false;
    // Smaller than a notification header (12 bytes plus the name), so every
    // completion reports an overflow instead of a record.
    watchA->data.assign(8, 0);
    if (!watchA->Arm()) { watchA->data.assign(16, 0); Check(watchA->Arm(), "Cannot re-arm the shrunken watch"); }
    Check(watchA->data.size() <= 16, "Shrunken watch buffer still fits a burst record");
    const auto overflowsBefore = app.watchOverflowCount;
    const auto generationA3 = Folder(app, rootA).generation, generationB3 = Folder(app, rootB).generation;
    const auto burst = rootA / L"burst";
    for (int i = 0; i < 40; ++i) Write(burst / (L"file-" + std::to_wstring(i) + L".txt"), "x");
    Write(burst / L"after-overflow.md", "# After overflow\n");
    Until([&] { return TreeItem(app, burst / L"after-overflow.md") != nullptr; }, "Markdown after a watch overflow did not appear", 10000);
    Check(app.watchOverflowCount > overflowsBefore, "The shrunken watch never reported an overflow");
    Check(Folder(app, rootA).generation > generationA3, "Overflow did not rescan root A");
    settle("Watches did not settle after the overflow");
    Check(Folder(app, rootB).generation == generationB3, "Overflow in root A rescanned root B");

    std::cout << "rescan-efficiency: removal followed by immediate re-registration" << std::endl;
    settle("Watches did not settle before re-registration");
    {
        const auto normalizedA = mm::NormalizePath(rootA.wstring());
        const auto oldId = Folder(app, rootA).id;
        app.RemoveFolderFromList(normalizedA);
        app.OpenRoot(rootA.wstring());
        Check(app.FindFolder(normalizedA) != nullptr && Folder(app, rootA).id != oldId, "Re-registration reused the removed registration");
        Until([&] { return !app.TreeTasksPending() && FolderTree(app, rootA) && TreeItem(app, rootA / L"nested" / L"inner.md") != nullptr; },
            "Re-registered root did not scan while the old rows were being removed", 20000);
        int rootsA = 0;
        for (auto row = TreeView_GetRoot(app.treeH); row; row = TreeView_GetNextSibling(app.treeH, row)) {
            auto* owner = app.FolderForItem(row);
            if (owner && _wcsicmp(owner->path.c_str(), normalizedA.c_str()) == 0) ++rootsA;
        }
        Check(rootsA == 1 && app.retiringFolders.empty(), "Rows of the removed registration survived its re-registration");
        Check(TreeItem(app, rootB / L"other.md") != nullptr, "Re-registration disturbed the other root");
        Until([&] { return WatchCoversFolder(app, rootA); }, "Re-registered root was not watched again");
        // The row index must name the new registration's row, not the retired one.
        app.OpenPaths({(rootA / L"nested" / L"inner.md").wstring()});
        LoadedPath(app, rootA / L"nested" / L"inner.md");
        Check(TreeView_GetSelection(app.treeH) == TreeItem(app, rootA / L"nested" / L"inner.md"),
            "Opening a file of the re-registered root selected the wrong row");
    }

    std::cout << "rescan-efficiency: duplicate tab names" << std::endl;
    app.OpenPaths({(rootA / L"same.md").wstring(), (rootB / L"same.md").wstring()});
    LoadedPath(app, rootB / L"same.md");
    const int tabA = IndexOf(app, rootA / L"same.md"), tabB = IndexOf(app, rootB / L"same.md");
    Check(app.TabTitle(tabA).find(L" — root-a") != std::wstring::npos && app.TabTitle(tabB).find(L" — root-b") != std::wstring::npos,
        "Duplicate file names did not receive their parent folder");
    app.Close(tabA);
    Check(app.TabTitle(IndexOf(app, rootB / L"same.md")).find(L" — ") == std::wstring::npos,
        "Closing the duplicate did not drop the parent folder from the title");

    std::cout << "rescan-efficiency: large differences run in bounded slices" << std::endl;
    settle("Watches did not settle before the sliced update");
    {
        // A synthetic branch of 3,030 rows appears and disappears. Both changes
        // exceed the synchronous budget, so they must run through timer slices
        // while the message loop keeps turning, with no slice much longer than
        // the 25 ms budget.
        auto& folder = Folder(app, rootA);
        auto before = std::make_unique<mm::FolderNode>(*folder.tree);
        auto grown = std::make_unique<mm::FolderNode>(*folder.tree);
        mm::FolderNode synthetic{L"synthetic", (rootA / L"synthetic").wstring(), true, {}};
        for (int group = 0; group < 30; ++group) {
            mm::FolderNode branch{L"group-" + std::to_wstring(group), (rootA / L"synthetic" / (L"group-" + std::to_wstring(group))).wstring(), true, {}};
            for (int file = 0; file < 100; ++file) {
                const auto name = L"doc-" + std::to_wstring(file) + L".md";
                branch.children.push_back({name, (rootA / L"synthetic" / branch.name / name).wstring(), false, {}});
            }
            synthetic.children.push_back(std::move(branch));
        }
        // Scan results are always in FolderNodeBefore order; a hand-made tree must be too.
        grown->children.push_back(std::move(synthetic));
        grown->children.sort(mm::FolderNodeBefore);
        auto describe = [&](const char* label) {
            const auto& s = app.treeStats;
            return std::string(label) + ": inserted=" + std::to_string(s.rowsInserted) + " deleted=" + std::to_string(s.rowsDeleted)
                + " slices=" + std::to_string(s.slices) + " maxSlice=" + std::to_string(s.maxSliceMs) + " ms total=" + std::to_string(s.totalMs) + " ms";
        };
        app.ReplaceFolderTree(folder, std::move(grown));
        Check(app.TreeTasksPending(), "A 3,030-row insertion ran synchronously");
        Until([&] { return !app.TreeTasksPending(); }, "Sliced insertion did not finish", 60000);
        // 25 ms budget checked after each row; allow for a loaded machine.
        if (!(app.treeStats.rowsInserted == 3031 && app.treeStats.slices > 1 && app.treeStats.maxSliceMs < 100))
            throw std::runtime_error(describe("Sliced insertion exceeded its per-slice budget or skipped slicing"));
        Check(TreeItem(app, rootA / L"synthetic" / L"group-29" / L"doc-99.md") != nullptr, "Sliced insertion lost rows");
        app.ReplaceFolderTree(folder, std::move(before));
        Check(app.TreeTasksPending(), "A 3,031-row deletion ran synchronously");
        Until([&] { return !app.TreeTasksPending(); }, "Sliced deletion did not finish", 60000);
        if (!(app.treeStats.rowsDeleted == 3031 && app.treeStats.slices > 1 && app.treeStats.maxSliceMs < 100))
            throw std::runtime_error(describe("Sliced deletion exceeded its per-slice budget or skipped slicing"));
        Check(TreeItem(app, rootA / L"synthetic") == nullptr && TreeItem(app, rootA / L"nested" / L"inner.md") != nullptr,
            "Sliced deletion removed the wrong rows");
        std::cout << "rescan-efficiency: sliced update max slice " << app.treeStats.maxSliceMs << " ms over " << app.treeStats.slices << " slices" << std::endl;
    }
    std::cout << "rescan-efficiency: many new siblings under one folder run in bounded slices" << std::endl;
    settle("Watches did not settle before the sibling batch");
    {
        // Files added directly to an existing folder used to get their rows
        // synchronously in MergeTree; only folders with children were queued.
        auto& folder = Folder(app, rootA);
        const auto rootRow = app.RootRow(folder);
        Check(rootRow != nullptr && (TreeView_GetItemState(app.treeH, rootRow, TVIS_EXPANDED) & TVIS_EXPANDED),
            "Root A row is missing or collapsed before the sibling batch");
        auto before = std::make_unique<mm::FolderNode>(*folder.tree);
        auto grown = std::make_unique<mm::FolderNode>(*folder.tree);
        for (int file = 0; file < 3000; ++file) {
            const auto name = L"bulk-" + std::to_wstring(file) + L".md";
            grown->children.push_back({name, (rootA / name).wstring(), false, {}});
        }
        grown->children.sort(mm::FolderNodeBefore);
        auto describe = [&](const char* label) {
            const auto& s = app.treeStats;
            return std::string(label) + ": inserted=" + std::to_string(s.rowsInserted) + " deleted=" + std::to_string(s.rowsDeleted)
                + " slices=" + std::to_string(s.slices) + " maxSlice=" + std::to_string(s.maxSliceMs) + " ms total=" + std::to_string(s.totalMs) + " ms";
        };
        trace.insertions = trace.deletions = 0;
        app.ReplaceFolderTree(folder, std::move(grown));
        Check(app.TreeTasksPending(), "3,000 sibling rows were inserted synchronously");
        Until([&] { return !app.TreeTasksPending(); }, "Sliced sibling insertion did not finish", 60000);
        if (!(app.treeStats.rowsInserted == 3000 && app.treeStats.rowsDeleted == 0 && app.treeStats.slices > 1 && app.treeStats.maxSliceMs < 100))
            throw std::runtime_error(describe("Sibling batch exceeded its per-slice budget or skipped slicing"));
        Check(trace.insertions == 3000 && trace.deletions == 0, "Sibling batch changed rows other than the new files");
        Check((TreeView_GetItemState(app.treeH, rootRow, TVIS_EXPANDED) & TVIS_EXPANDED) != 0, "Sibling batch left root A collapsed");
        // Rows keep the model order around the batch: added.md, bulk-0 ... bulk-999, same.md.
        const auto firstBulk = TreeItem(app, rootA / L"bulk-0.md"), lastBulk = TreeItem(app, rootA / L"bulk-999.md");
        Check(firstBulk != nullptr && lastBulk != nullptr && TreeItem(app, rootA / L"bulk-2999.md") != nullptr, "Sibling batch lost rows");
        Check(TreeView_GetPrevSibling(app.treeH, firstBulk) == TreeItem(app, rootA / L"added.md")
            && TreeView_GetNextSibling(app.treeH, lastBulk) == TreeItem(app, rootA / L"same.md"), "Sibling batch rows are out of order");
        std::cout << "rescan-efficiency: sibling batch max slice " << app.treeStats.maxSliceMs << " ms over " << app.treeStats.slices << " slices" << std::endl;
        app.ReplaceFolderTree(folder, std::move(before));
        Until([&] { return !app.TreeTasksPending(); }, "Sliced sibling deletion did not finish", 60000);
        if (!(app.treeStats.rowsDeleted == 3000 && app.treeStats.rowsInserted == 0))
            throw std::runtime_error(describe("Removing the sibling batch changed other rows"));
        Check(TreeItem(app, rootA / L"bulk-0.md") == nullptr && TreeItem(app, rootA / L"same.md") != nullptr, "Removing the sibling batch removed the wrong rows");
        Check((TreeView_GetItemState(app.treeH, rootRow, TVIS_EXPANDED) & TVIS_EXPANDED) != 0, "Removing the sibling batch left root A collapsed");
        std::cout << "rescan-efficiency: sibling batch removal max slice " << app.treeStats.maxSliceMs << " ms over " << app.treeStats.slices << " slices, " << app.treeStats.totalMs << " ms" << std::endl;
    }

    std::cout << "rescan-efficiency: a folder moved into a root appears with its contents" << std::endl;
    settle("Watches did not settle before the move");
    {
        // Moving a directory within a volume reports REMOVED and ADDED for the
        // directory alone; the files inside never notify. The added name must
        // still be examined on disk.
        const auto staging = fixture / L"staging" / L"moved-in";
        Write(staging / L"inside.md", "# Moved in\n");
        const auto destination = rootA / L"moved-in";
        Check(MoveFileExW(staging.c_str(), destination.c_str(), 0) != FALSE, "Cannot move the staged folder into root A");
        Until([&] { return TreeItem(app, destination / L"inside.md") != nullptr; }, "Markdown inside a moved-in folder did not appear", 10000);
    }

    std::cout << "rescan-efficiency: requests during a scan coalesce into one follow-up" << std::endl;
    settle("Watches did not settle before the coalescing check");
    {
        struct Gate { std::atomic_bool started{false}, released{false}; };
        const auto gate = std::make_shared<Gate>();
        struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->released = true; } } release{gate};
        for (auto& worker : app.scanners) worker.Queue([gate] { gate->started = true; while (!gate->released) Sleep(1); });
        Until([&] { return gate->started.load(); }, "Cannot hold the scan workers");
        auto& folder = Folder(app, rootA);
        app.RefreshFolder(folder);
        const auto running = folder.generation;
        Check(folder.scanning && !folder.rescanRequested, "First request did not start a scan");
        app.RefreshFolder(folder);
        app.RefreshFolder(folder);
        Check(folder.generation == running && folder.scanning && folder.rescanRequested, "Requests during a scan restarted it instead of waiting");
        gate->released = true;
        Until([&] { return !folder.scanning && !app.TreeTasksPending(); }, "Coalesced rescan did not finish", 10000);
        Check(folder.generation == running + 1 && !folder.rescanRequested, "Requests during a scan did not produce exactly one follow-up scan");
    }

    std::cout << "rescan-efficiency: the completion poll stops without watches" << std::endl;
    {
        const auto pollsWhileWatching = app.pollCount;
        PumpFor(700);
        Check(app.pollCount > pollsWhileWatching, "The completion poll is not running while folders are watched");
        RemoveAllFolders(app);
        for (int i = int(app.tabs.size()) - 1; i >= 0; --i) if (!app.tabs[i].path.empty()) app.Close(i);
        Until([&] { return app.watches.empty() && !app.watchSetupPending; }, "Watches did not go away with the folders and tabs");
        const auto pollsWithoutWatches = app.pollCount;
        PumpFor(700);
        Check(app.pollCount == pollsWithoutWatches, "The completion poll keeps running without watches");
        app.OpenRoot(rootB.wstring());
        Until([&] { return !app.watches.empty(); }, "Re-added root was not watched");
        const auto pollsAfterReturn = app.pollCount;
        PumpFor(700);
        Check(app.pollCount > pollsAfterReturn, "The completion poll did not resume with a new watch");
    }
    Write(fixture / L"result.txt", "PASS: identical rescan keeps rows, targeted rescans, no tree walks for non-Markdown, overflow recovery, duplicate titles, sliced updates, sibling batches, moved-in folders, coalesced rescans, idle poll.\n");
    std::cout << "PASS tree rescan efficiency: unchanged rescans, targeted roots, notification handling, overflow, tab titles" << std::endl;
}

void RefreshPrunesMissingMarkdown(const fs::path& artifacts) {
    const auto fixture = artifacts / L"refresh-prunes-missing-markdown";
    const auto root = fixture / L"root", otherRoot = fixture / L"other-root";
    const auto nested = root / L"nested", deep = nested / L"deep";
    const auto missing = deep / L"last.MD", keep = root / L"keep.md", other = otherRoot / L"other.md";
    const auto lateFolder = otherRoot / L"late", late = lateFolder / L"late.md";
    const auto nonMarkdown = nested / L"readme.txt";
    const auto contents = LongDocument("Retain the displayed missing document");
    Write(missing, contents);
    Write(keep, "# Keep this Markdown\n");
    Write(other, "# Keep the other root\n");
    Write(late, "# Remove after the explicit scans, before watcher setup\n");
    Write(nonMarkdown, "Keep this non-Markdown file on disk.\n");

    Host host(fixture / L"session.ini");
    auto& app = host.app;
    app.OpenRoot(root.wstring());
    app.OpenRoot(otherRoot.wstring());
    app.OpenPaths({missing.wstring()});
    LoadedPath(app, missing);
    LayoutDocument(app);
    Until([&] {
        return TreeItem(app, missing) && TreeItem(app, keep) && TreeItem(app, other) && TreeItem(app, late)
            && !Folder(app, root).scanning && !Folder(app, otherRoot).scanning
            && !app.TreeTasksPending() && !app.watchSetupPending && !app.treeDirty
            && app.watchRescanFolders.empty() && WatchCoversFolder(app, root) && WatchCoversFolder(app, otherRoot);
    }, "Missing-Markdown refresh fixture did not settle", 10000);
    const auto rootRow = TreeItem(app, root), keepRow = TreeItem(app, keep);
    const auto otherRootRow = TreeItem(app, otherRoot), otherRow = TreeItem(app, other);
    const auto document = app.currentDoc;
    const auto activeId = app.tabs[app.active].id;
    const auto tabCount = app.tabs.size(), folderCount = app.folders.size();
    const auto generation = Folder(app, root).generation;

    // Lose the deletion notification deliberately. Hold only watcher setup so
    // Reload must start its scan without waiting for a replacement watch plan.
    app.CancelWatchSetup();
    app.RetireWatches(std::move(app.watches));
    app.UpdateWatchTimer();
    KillTimer(app.hwnd, 4);
    struct Gate { std::atomic_bool started{false}, released{false}; };
    const auto gate = std::make_shared<Gate>();
    struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { gate->released = true; } } release{gate};
    app.watchWorker.Queue([gate] {
        gate->started = true;
        while (!gate->released) Sleep(1);
    });
    Until([&] { return gate->started.load(); }, "Cannot hold watcher setup during explicit refresh");
    Check(fs::remove(missing) && !fs::exists(missing), "Cannot remove the isolated Markdown refresh fixture");
    Check(TreeItem(app, missing) != nullptr && app.watches.empty(), "Deleted fixture row disappeared before explicit refresh");
    app.Command(Reload);
    Until([&] {
        return Folder(app, root).generation > generation && !Folder(app, root).scanning
            && !Folder(app, otherRoot).scanning && !app.TreeTasksPending() && !app.loading
            && !TreeItem(app, missing) && !TreeItem(app, deep) && !TreeItem(app, nested);
    }, "Explicit refresh did not prune missing Markdown and its empty ancestors while watcher setup was blocked", 10000);
    Check(!gate->released && app.watchSetupPending, "Refresh waited for watcher setup instead of scanning independently");
    Check(TreeItem(app, root) == rootRow && TreeItem(app, keep) == keepRow
        && TreeItem(app, otherRoot) == otherRootRow && TreeItem(app, other) == otherRow,
        "Pruning the missing Markdown replaced retained roots or unrelated file rows");
    Check(app.folders.size() == folderCount && app.tabs.size() == tabCount && app.active >= 0
        && app.tabs[app.active].id == activeId && app.displayedTabId == activeId
        && app.currentDoc == document && app.currentDoc->source == contents,
        "Pruning missing Markdown removed its tab or discarded its displayed document");
    Check(fs::is_directory(root) && fs::is_directory(nested) && fs::is_directory(deep)
        && fs::is_regular_file(nonMarkdown) && fs::is_regular_file(keep) && fs::is_regular_file(other),
        "Sidebar pruning deleted a real folder, non-Markdown file, or retained Markdown");
    Check(!app.rowsByPath.contains(mm::NormalizePath(missing.wstring())), "Deleted Markdown remains in the native row index");

    // The registered top-level folder stays even when its last Markdown goes;
    // only empty descendants disappear from the sidebar.
    const auto emptyGeneration = Folder(app, root).generation;
    Check(fs::remove(keep) && !fs::exists(keep), "Cannot remove the final Markdown from the isolated root");
    app.Command(Reload);
    Until([&] {
        const auto* tree = FolderTree(app, root);
        return Folder(app, root).generation > emptyGeneration && !Folder(app, root).scanning
            && !Folder(app, otherRoot).scanning && !app.TreeTasksPending() && !app.loading
            && tree && tree->children.empty() && !TreeItem(app, keep);
    }, "Refreshing an empty Markdown root did not remove its last child", 10000);
    Check(!gate->released && app.watchSetupPending && app.folders.size() == folderCount
        && TreeItem(app, root) == rootRow && TreeView_GetChild(app.treeH, rootRow) == nullptr,
        "Refreshing an empty Markdown root removed its top-level registration or waited for watcher setup");
    Check(TreeItem(app, otherRoot) == otherRootRow && TreeItem(app, other) == otherRow
        && app.tabs.size() == tabCount && app.active >= 0 && app.tabs[app.active].id == activeId
        && app.displayedTabId == activeId && app.currentDoc == document && app.currentDoc->source == contents,
        "Refreshing an empty root changed another root or the retained document tab");
    Check(fs::is_directory(root) && fs::is_directory(nested) && fs::is_directory(deep)
        && fs::is_regular_file(nonMarkdown) && fs::is_regular_file(other)
        && !app.rowsByPath.contains(mm::NormalizePath(keep.wstring())),
        "Empty-root refresh deleted real folders or unrelated files, or kept the missing row index");

    // A second change can land after the immediate scan but before new watches
    // are armed. The post-install scan must cover this unobserved interval.
    Check(app.watches.empty() && TreeItem(app, late) != nullptr && TreeItem(app, lateFolder) != nullptr,
        "Late-deletion fixture did not retain its row while watcher setup was blocked");
    const auto lateGeneration = Folder(app, otherRoot).generation;
    Check(fs::remove(late) && !fs::exists(late), "Cannot delete Markdown during the unwatched scan-to-install interval");
    gate->released = true;
    Until([&] {
        return !app.watchSetupPending && WatchCoversFolder(app, otherRoot)
            && Folder(app, otherRoot).generation > lateGeneration && !Folder(app, otherRoot).scanning
            && !Folder(app, root).scanning && !app.TreeTasksPending()
            && !TreeItem(app, late) && !TreeItem(app, lateFolder);
    }, "Watcher installation did not rescan Markdown deleted after the immediate refresh", 10000);
    Check(TreeItem(app, root) == rootRow && !TreeItem(app, missing) && !TreeItem(app, nested) && !TreeItem(app, keep),
        "Watcher setup changed the retained empty root or restored a pruned Markdown branch");
    Check(TreeItem(app, otherRoot) == otherRootRow && TreeItem(app, other) == otherRow
        && fs::is_directory(lateFolder) && fs::is_regular_file(other) && fs::is_regular_file(nonMarkdown)
        && app.folders.size() == folderCount && app.tabs.size() == tabCount && app.active >= 0
        && app.tabs[app.active].id == activeId && app.displayedTabId == activeId
        && app.currentDoc == document && app.currentDoc->source == contents,
        "Post-install rescan changed retained files, folders, or the displayed missing document");
    Check(!app.rowsByPath.contains(mm::NormalizePath(late.wstring())), "Post-install pruning kept the late-deleted Markdown index");
    std::cout << "PASS explicit refresh prunes missing Markdown and empty descendants, keeps roots and tabs, and covers changes before watcher installation" << std::endl;
}

void SessionParserChecks(const fs::path& artifacts) {
    const auto fixture = artifacts / L"session-parser";
    fs::create_directories(fixture);
    const std::wstring content =
        L"; comment line\r\n"
        L"[App]\r\n"
        L"Plain=hello world\r\n"
        L"  Spaced  =   padded value   \r\n"
        L"Quoted=\"  quoted value  \"\r\n"
        L"Single='single'\r\n"
        L"Mismatched=\"open\r\n"
        L"Number=42\r\n"
        L"Negative=-7\r\n"
        L"Junk=12abc\r\n"
        L"Hex=0x10\r\n"
        L"Big=4294967295\r\n"
        L"Oct=010\r\n"
        L"Empty=\r\n"
        L"Blank=   \r\n"
        L"Dup=first\r\n"
        L"dup=second\r\n"
        L"NoEquals\r\n"
        L"Japanese=日本語の値\r\n"
        L"Tab=\ttabbed\r\n"
        L"Quote2=\"a\" b\r\n"
        L"Semi=value ; comment\r\n"
        L"\r\n"
        L"[Second]\r\n"
        L"Key=v\r\n"
        L"[app]\r\n"
        L"Later=1\r\n"
        L"Plain=overridden\r\n"
        L"[Third]\r\n"
        L"Key=w\r\n"
        L"[Fourth\r\n"
        L"Key=x\r\n";
    struct Case { const wchar_t* section; const wchar_t* key; };
    const Case cases[] = {
        {L"App", L"Plain"}, {L"app", L"spaced"}, {L"App", L"Quoted"}, {L"App", L"Single"}, {L"App", L"Mismatched"},
        {L"App", L"Number"}, {L"App", L"Negative"}, {L"App", L"Junk"}, {L"App", L"Hex"}, {L"App", L"Big"}, {L"App", L"Oct"},
        {L"App", L"Empty"}, {L"App", L"Blank"}, {L"App", L"Dup"}, {L"App", L"NoEquals"}, {L"App", L"Japanese"},
        {L"App", L"Tab"}, {L"App", L"Quote2"}, {L"App", L"Semi"}, {L"App", L"Later"}, {L"App", L"Missing"},
        {L"Second", L"Key"}, {L"Third", L"Key"}, {L"Fourth", L"Key"}, {L"Nowhere", L"Key"}};
    const std::wstring sentinel = L"\x1f" L"default";
    auto compare = [&](const fs::path& file, const std::string& label) {
        const auto session = SessionFile::Load(file.wstring());
        for (const auto& item : cases) {
            std::vector<wchar_t> buffer(4096);
            const auto length = GetPrivateProfileStringW(item.section, item.key, sentinel.c_str(), buffer.data(), DWORD(buffer.size()), file.c_str());
            const std::wstring expected(buffer.data(), length);
            const auto actual = session.Get(item.section, item.key, sentinel.c_str());
            if (actual != expected || session.Has(item.section, item.key) != (expected != sentinel))
                throw std::runtime_error(label + ": string mismatch for " + mm::Utf8(item.section) + "/" + mm::Utf8(item.key)
                    + " expected [" + mm::Utf8(expected) + "] parsed [" + mm::Utf8(actual) + "]");
            const int expectedInt = int(GetPrivateProfileIntW(item.section, item.key, 12345, file.c_str()));
            const int actualInt = session.Int(item.section, item.key, 12345);
            if (actualInt != expectedInt)
                throw std::runtime_error(label + ": integer mismatch for " + mm::Utf8(item.section) + "/" + mm::Utf8(item.key)
                    + " expected " + std::to_string(expectedInt) + " parsed " + std::to_string(actualInt));
        }
        // Windows lists a repeated section name again; SessionFile reports each
        // name once (only the first section is ever consulted), so the expected
        // list is collapsed the same way before comparing order and spelling.
        std::vector<wchar_t> names(4096);
        GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), file.c_str());
        std::vector<std::wstring> expectedNames;
        for (const wchar_t* name = names.data(); *name; name += wcslen(name) + 1) {
            bool seen = false;
            for (const auto& known : expectedNames) if (_wcsicmp(known.c_str(), name) == 0) seen = true;
            if (!seen) expectedNames.emplace_back(name);
        }
        const auto actualNames = session.SectionNames();
        bool sameNames = actualNames.size() == expectedNames.size();
        for (size_t i = 0; sameNames && i < actualNames.size(); ++i) sameNames = _wcsicmp(actualNames[i].c_str(), expectedNames[i].c_str()) == 0;
        if (!sameNames) throw std::runtime_error(label + ": section names differ from GetPrivateProfileSectionNamesW");
    };
    // The Japanese value exercises the DBCS path only under code page 932; other
    // code pages replace it identically on both sides of the comparison.
    const auto ansiFile = fixture / L"ansi.ini";
    {
        const int size = WideCharToMultiByte(CP_ACP, 0, content.data(), int(content.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(size_t(size), '\0');
        WideCharToMultiByte(CP_ACP, 0, content.data(), int(content.size()), bytes.data(), size, nullptr, nullptr);
        Write(ansiFile, bytes);
    }
    compare(ansiFile, "ANSI session");
    const auto unicodeFile = fixture / L"unicode.ini";
    {
        std::string bytes("\xff\xfe", 2);
        bytes.append(reinterpret_cast<const char*>(content.data()), content.size() * sizeof(wchar_t));
        Write(unicodeFile, bytes);
    }
    compare(unicodeFile, "UTF-16 session");
    Check(SessionFile::Load((fixture / L"absent.ini").wstring()).sections.empty(), "A missing session file must parse as empty");

    // A real saved session must round-trip through the parser exactly as the profile API reads it.
    const auto document = fixture / L"saved.md";
    Write(document, "# Saved session\n");
    {
        Host host(fixture / L"session.ini");
        auto& app = host.app;
        app.OpenPaths({document.wstring()});
        LoadedPath(app, document);
        app.tabs.front().scroll = 0.25;
        Check(app.Save(), "Cannot save parser fixture session");
        const auto session = SessionFile::Load(app.ini);
        Check(session.Int(L"App", L"Format", 0) == 1 && session.Int(L"App", L"Tabs", 0) == int(app.tabs.size()), "Saved header did not parse");
        Check(session.Get(L"Tab0", L"Path") == app.ReadIni(L"Tab0", L"Path") && session.Get(L"Tab0", L"Path") == app.tabs.front().path,
            "Saved Unicode tab path did not parse");
        Check(session.Get(L"Tab0", L"Scroll") == app.ReadIni(L"Tab0", L"Scroll"), "Saved scroll value did not parse");
        Check(session.Has(L"App", L"Roots") && !session.Has(L"App", L"Nope"), "Key presence did not parse");
        App restored;
        restored.ini = app.ini;
        restored.RestoreFolders();
        restored.RestoreExcludedFolders();
        Check(restored.FolderPaths() == app.FolderPaths() && restored.excludedFolders == app.excludedFolders,
            "Argument-free restore helpers must read the current file");
    }
    Write(fixture / L"result.txt", "PASS: session parser matches GetPrivateProfile* for ANSI and UTF-16 files and saved sessions.\n");
    std::cout << "PASS session parser differential checks" << std::endl;
}

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR graphics = 0;
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize native controls");
        const auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        int argumentCount = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        fs::path snapshotDirectory;
        const bool largeTreeRegression = arguments && argumentCount > 1
            && std::wstring(arguments[1]) == L"--tree-scroll-regression";
        const bool smallTreeRegression = arguments && argumentCount > 1
            && std::wstring(arguments[1]) == L"--tree-scroll-small";
        if (arguments && argumentCount > 1 && std::wstring(arguments[1]) == L"--snapshots")
            snapshotDirectory = argumentCount > 2 ? fs::path(arguments[2]) : sourceRoot / L"artifacts" / L"host-snapshots";
        if (arguments) LocalFree(arguments);
        PruneOldRuns(sourceRoot / L"artifacts" / L"host-tests");
        const auto artifacts = sourceRoot / L"artifacts" / L"host-tests"
            / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (largeTreeRegression || smallTreeRegression) {
            SmallTreeRescanChecks(artifacts);
            if (largeTreeRegression) { DeepTreeRefreshChecks(artifacts);LargeTreeSelectionChecks(artifacts); }
        } else {
        const auto root = artifacts / L"日本語 # フォルダ";
        Write(root / L"one.md", LongDocument("One"));
        Write(root / L"two.md", LongDocument("Two"));
        Write(root / L"ancestor" / L"deep" / L"three.MD", LongDocument("Three"));
        Write(root / L"no-md" / L"deeper" / L"ignored.txt", "No Markdown here");
        Write(root / L"note.txt", "Not Markdown");
        Write(root / L"open-only.markdown", LongDocument("Explicit Markdown"));
        {
            Host host(artifacts / L"session.ini");
            TreeAndSidebar(host.app, root);
            SidebarSplitter(host.app, root);
            TabsAndState(host.app, root);
            Session(host.app);
            WatchUpdates(host.app, root);
            FolderListRemoval(host.app, root);
            BoundedWatcherSetup(host.app, root);
            if (!snapshotDirectory.empty()) Snapshots(host.app, sourceRoot, snapshotDirectory);
        }
        MultipleFolderRegistrations(artifacts);
        TreeRescanEfficiencyChecks(artifacts);
        RefreshPrunesMissingMarkdown(artifacts);
        SessionParserChecks(artifacts);
        StartupRegistrationChecks(artifacts);
        StartupRootContextChecks(artifacts);
        RootFolderReorderingChecks(artifacts);
        TreeSelectionRenderingChecks(artifacts);
        TreeScrollbarRenderingChecks(artifacts);
        DocumentScrollbarRenderingChecks(artifacts);
        TabOverflowRenderingChecks(artifacts);
        ActiveTabRenderingChecks(artifacts);
        CachedTabDocuments(artifacts);
        SaveDuringDocumentRead(artifacts);
        Write(artifacts / L"result.txt", "PASS: all native host integration tests passed.\n");
        }
        std::wcout << L"Host artifacts: " << artifacts.wstring() << L"\n";
        std::cout << "host_tests: all checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "host_tests FAILED: " << error.what() << '\n';
        result = 1;
    }
    Gdiplus::GdiplusShutdown(graphics);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
