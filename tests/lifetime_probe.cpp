// Redirect Restore to isolated LOCALAPPDATA; never read or replace the user's session.
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <cstring>
static std::wstring probeAppData;
static HRESULT ProbeKnownFolder(REFKNOWNFOLDERID id, DWORD, HANDLE, PWSTR* path) {
    if (id != FOLDERID_LocalAppData || probeAppData.empty()) return E_INVALIDARG;
    const auto bytes = (probeAppData.size() + 1) * sizeof(wchar_t);
    *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!*path) return E_OUTOFMEMORY;
    std::memcpy(*path, probeAppData.c_str(), bytes);
    return S_OK;
}
#define SHGetKnownFolderPath ProbeKnownFolder
#define main IncludedHostTestsMain
#include "host_tests.cpp"
#undef main
#undef SHGetKnownFolderPath

namespace {
void SameTabs(const App& expected, const App& actual) {
    Check(actual.tabs.size() == expected.tabs.size(), "Session restoration lost registered tabs");
    Check(actual.active == expected.active, "Session restoration changed the active tab");
    for (size_t i = 0; i < expected.tabs.size(); ++i) {
        const auto& a = expected.tabs[i];
        const auto& b = actual.tabs[i];
        Check(a.path == b.path && a.font == b.font && a.source == b.source && a.cp932 == b.cp932
            && a.autoRefresh == b.autoRefresh && std::abs(a.scroll - b.scroll) < 0.000002,
            "Session restoration changed tab order, path, font, scroll, or flags");
    }
}

std::string FileBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "Cannot read isolated session bytes");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteIni(const fs::path& path, const std::wstring& contents) {
    std::wstring utf16 = L"\ufeff" + contents;
    Write(path, std::string(reinterpret_cast<const char*>(utf16.data()), utf16.size() * sizeof(wchar_t)));
}

void SessionRoundTrips(const fs::path& artifacts, std::ostream& records) {
    probeAppData = (artifacts / L"roundtrip-appdata").wstring();
    const auto session = fs::path(probeAppData) / L"MMViewer" / L"session.ini";
    fs::create_directories(session.parent_path());
    Host host(session);
    auto& app = host.app;
    for (int target : {32, 100, 256}) {
        std::vector<std::wstring> paths;
        for (int index = static_cast<int>(app.tabs.size()); index < target; ++index) {
            const auto file = artifacts / L"documents" / (std::to_wstring(index) + L"-日本語.md");
            Write(file, "# Session regression\n\nA small document.\n");
            paths.push_back(file.wstring());
        }
        const auto generation = app.loadGeneration;
        const auto openStart = GetTickCount64();
        app.OpenPaths(paths);
        const auto openMs = GetTickCount64() - openStart;
        Check(app.loadGeneration == generation + 1, "Multi-file open started more than one document read");
        Check(app.tabs.size() == size_t(target), "Multi-file open lost or duplicated paths");
        LoadedPath(app, fs::path(paths.back()));
        for (size_t i = 0; i < app.tabs.size(); ++i) {
            auto& tab = app.tabs[i];
            tab.font = 10 + int(i % 31);
            tab.scroll = double(i % 17) / 17;
            tab.source = (i % 4) != 0;
            tab.cp932 = (i % 2) != 0;
            tab.autoRefresh = (i % 3) != 0;
        }
        const auto previousSession = fs::exists(session) ? FileBytes(session) : std::string();
        const auto saveStart = GetTickCount64();
        Check(app.Save(), "Cannot save session regression fixture");
        const auto saveMs = GetTickCount64() - saveStart;
        if (!previousSession.empty()) Check(FileBytes(session.wstring() + L".bak") == previousSession,
            "Atomic replacement did not retain the previous complete session backup");
        App restored;
        const auto restoreStart = GetTickCount64();
        restored.Restore();
        const auto restoreMs = GetTickCount64() - restoreStart;
        SameTabs(app, restored);
        const auto bytes = FileBytes(session);
        Check(bytes.size() > 2 && BYTE(bytes[0]) == 0xff && BYTE(bytes[1]) == 0xfe,
            "Session writer did not retain UTF-16 LE format");
        Check(GetPrivateProfileIntW(L"App", L"Format", 0, session.c_str()) == 1
            && GetPrivateProfileIntW(L"App", L"Tabs", 0, session.c_str()) == unsigned(target),
            "Session writer broke existing Format 1 profile reads");
        records << "{\"phase\":\"session\",\"opened\":" << app.tabs.size()
            << ",\"restored\":" << restored.tabs.size() << ",\"active_before\":" << app.active
            << ",\"active_after\":" << restored.active << ",\"open_ui_ms\":" << openMs
            << ",\"save_ui_ms\":" << saveMs << ",\"restore_ms\":" << restoreMs << "}\n" << std::flush;
        std::cout << "PASS session opened=" << app.tabs.size() << " restored=" << restored.tabs.size()
                  << " open_ms=" << openMs << " save_ms=" << saveMs << " restore_ms=" << restoreMs << std::endl;
    }
    const auto before = FileBytes(session);
    HANDLE held = CreateFileW(session.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "Cannot lock isolated session for replacement failure check");
    const auto fontBefore = app.tabs.front().font;
    app.tabs.front().font = fontBefore == 40 ? 10 : fontBefore + 1;
    const bool saved = app.Save();
    CloseHandle(held);
    Check(!saved && FileBytes(session) == before && !fs::exists(session.wstring() + L".tmp"),
        "Failed replacement changed the previous session or left a partial temporary file");
    app.tabs.front().font = fontBefore;
    App retained;
    retained.Restore();
    SameTabs(app, retained);
    std::cout << "PASS failed replacement preserves the previous complete session\n";
}

void SparseSessionChecks(const fs::path& artifacts) {
    for (const std::wstring count : {L"2147483647", L"4294967295", L"-1", L"invalid", L"0"}) {
        probeAppData = (artifacts / (L"count-" + count)).wstring();
        const auto session = fs::path(probeAppData) / L"MMViewer" / L"session.ini";
        const auto first = (artifacts / L"first.md").wstring();
        const auto last = (artifacts / L"last.md").wstring();
        WriteIni(session, L"[App]\r\nFormat=1\r\nTabs=" + count + L"\r\nActive=12\r\n"
            L"[Tab12]\r\nPath=" + last + L"\r\nScroll=nan\r\nSource=1\r\nCP932=1\r\nAutoRefresh=0\r\n"
            L"[Tab3]\r\nPath=\r\n[Tab2]\r\nPath=" + first + L"\r\nScroll=inf\r\n"
            L"[Tabinvalid]\r\nPath=wrong.md\r\n[Tab-1]\r\nPath=wrong.md\r\n"
            L"[Tab184467440737095516160]\r\nPath=wrong.md\r\n");
        App restored;
        restored.Restore();
        Check(restored.tabs.size() == 2 && restored.tabs[0].path == first && restored.tabs[1].path == last
            && restored.active == 1 && restored.tabs[1].source && restored.tabs[1].cp932 && !restored.tabs[1].autoRefresh
            && restored.tabs[0].scroll == 0 && restored.tabs[1].scroll == 0,
            "Sparse section restoration trusted a broken count or lost numeric order/active mapping");
    }
    probeAppData = (artifacts / L"empty-paths").wstring();
    WriteIni(fs::path(probeAppData) / L"MMViewer" / L"session.ini",
        L"[App]\r\nFormat=1\r\nTabs=2147483647\r\nActive=500\r\n[Tab0]\r\nPath=\r\n[Tab500]\r\nPath=\r\n");
    App empty;
    empty.Restore();
    Check(empty.tabs.empty() && empty.active == -1, "Empty paths created ghost tabs or an invalid active index");
    std::cout << "PASS damaged counts, sparse numeric sections, empty paths, and active-index mapping\n";
}

void BatchedOpenChecks(const fs::path& artifacts) {
    const auto first = artifacts / L"batch-first.md";
    const auto second = artifacts / L"batch-second.md";
    const auto third = artifacts / L"batch-third.md";
    Write(first, LongDocument("First"));
    Write(second, LongDocument("Second"));
    Write(third, LongDocument("Third"));
    Host host(artifacts / L"batch.ini");
    auto& app = host.app;
    app.AddEmpty();
    const auto emptyGeneration = app.loadGeneration;
    app.OpenPaths({first.wstring(), second.wstring(), first.wstring()});
    Check(app.tabs.size() == 2 && app.active == IndexOf(app, first) && app.loadGeneration == emptyGeneration + 1
        && std::none_of(app.tabs.begin(), app.tabs.end(), [](const auto& tab) { return tab.path.empty(); }),
        "Batched open did not replace an empty tab, deduplicate, or select the final duplicate");
    LoadedPath(app, first);
    LayoutDocument(app);
    app.view.SetScrollRatio(0.62);
    SaveHostImage(app, artifacts / L"batch-before.png");
    const auto firstScroll = app.view.ScrollRatio();
    Check(firstScroll > 0.3, "Batched-open scroll fixture did not scroll");
    const auto beforeSame = app.loadGeneration;
    app.OpenPaths({second.wstring(), first.wstring()});
    Check(app.loadGeneration == beforeSame + 1 && app.tabs.size() == 2 && app.active == IndexOf(app, first),
        "Duplicate-only batch changed registration count or final selection");
    LoadedPath(app, first);
    SaveHostImage(app, artifacts / L"batch-duplicate.png");
    Near(app.view.ScrollRatio(), firstScroll, "Duplicate-only batch lost the current reading position");
    const auto beforeAdded = app.loadGeneration;
    app.OpenPaths({second.wstring(), third.wstring(), second.wstring()});
    Check(app.loadGeneration == beforeAdded + 1 && app.tabs.size() == 3 && app.active == IndexOf(app, second),
        "Mixed duplicate/new batch lost its final selection or read coalescing");
    LoadedPath(app, second);
    Near(app.tabs[IndexOf(app, first)].scroll, firstScroll, "Opening a batch overwrote the departing tab's scroll");
    app.Switch(IndexOf(app, first));
    LoadedPath(app, first);
    SaveHostImage(app, artifacts / L"batch-returned.png");
    Near(app.view.ScrollRatio(), firstScroll, "Returning after a batch lost the stored reading position");
    std::cout << "PASS batched-open empty replacement, duplicate selection, single read, and scroll retention\n";
}
struct SessionLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit SessionLock(const fs::path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "Cannot hold isolated session sharing lock");
    }
    void Release() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
    ~SessionLock() { Release(); }
};

struct CloseProbe {
    App* app = nullptr;
    std::function<void()> reenter;
    int answer = IDCANCEL;
    bool replied = false;
    std::string error;
};
CloseProbe* closeProbe = nullptr;

BOOL CALLBACK FindCloseDialog(HWND window, LPARAM output) {
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (wcscmp(type, L"#32770") == 0 && closeProbe && GetWindow(window, GW_OWNER) == closeProbe->app->hwnd) {
        *reinterpret_cast<HWND*>(output) = window;
        return FALSE;
    }
    return TRUE;
}
LRESULT CALLBACK HideCloseDialog(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE) {
        wchar_t type[64]{};
        GetClassNameW(HWND(w), type, 64);
        if (wcscmp(type, L"#32770") == 0) SetWindowPos(HWND(w), nullptr,
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200,
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 200,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return CallNextHookEx(nullptr, code, w, l);
}
void CALLBACK AnswerCloseDialog(HWND, UINT, UINT_PTR timer, DWORD) {
    auto* probe = closeProbe;
    if (!probe || probe->replied) return;
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindCloseDialog, LPARAM(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer);
    ShowWindow(dialog, SW_HIDE);
    probe->replied = true;
    try {
        Check(GetDlgItem(dialog, IDYES) && GetDlgItem(dialog, IDNO) && GetDlgItem(dialog, IDCANCEL),
            "Save failure dialog does not offer retry, discard, and cancel");
        SendMessageW(probe->app->hwnd, WM_CLOSE, 0, 0);
        SendMessageW(probe->app->hwnd, WM_TIMER, 3, 0);
        Check(IsWindow(probe->app->hwnd) && !probe->app->closing && probe->app->closePrompt,
            "Reentrant close destroyed the host while its save failure dialog was open");
        probe->reenter();
    } catch (const std::exception& error) { probe->error = error.what(); }
    PostMessageW(dialog, WM_COMMAND, probe->error.empty() ? probe->answer : IDNO, 0);
}
void CloseWithAnswer(App& app, int answer, std::function<void()> reenter) {
    CloseProbe state;
    state.app = &app;
    state.answer = answer;
    state.reenter = std::move(reenter);
    const auto hook = SetWindowsHookExW(WH_CBT, HideCloseDialog, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "Cannot keep isolated close dialog offscreen");
    closeProbe = &state;
    const auto timer = SetTimer(nullptr, 0, 50, AnswerCloseDialog);
    if (!timer) { closeProbe = nullptr; UnhookWindowsHookEx(hook); throw std::runtime_error("Cannot arm close dialog response"); }
    SendMessageW(app.hwnd, WM_CLOSE, 0, 0);
    KillTimer(nullptr, timer);
    UnhookWindowsHookEx(hook);
    closeProbe = nullptr;
    Check(state.replied && state.error.empty(), "Close failure modal callback failed");
    Check(!app.closePrompt, "Close dialog left its reentry guard active");
}

void SaveFailureChecks(const fs::path& artifacts) {
    const auto document = artifacts / L"save-failure-document.md";
    Write(document, "# Save failure recovery\n");
    {
        const auto session = artifacts / L"auto-save-recovery.ini";
        Host host(session);
        auto& app = host.app;
        app.OpenPaths({document.wstring()});
        LoadedPath(app, document);
        Check(app.Save(), "Cannot create autosave recovery fixture");
        const auto previous = FileBytes(session);
        SessionLock lock(session);
        app.tabs.front().font = 21;
        SendMessageW(app.hwnd, WM_TIMER, 3, 0);
        Check(app.saveRetryPending && !app.notice.empty() && IsWindow(app.hwnd) && !app.closing,
            "Autosave failure was silent or stopped the app");
        lock.Release();
        app.Dirty();
        PumpFor(900);
        Check(FileBytes(session) == previous && app.saveRetryPending,
            "Further edits collapsed the failed-save retry backoff");
        Until([&] { return !app.saveRetryPending && GetPrivateProfileIntW(L"Tab0", L"Font", 0, session.c_str()) == 21; },
            "Autosave did not recover after releasing the session lock", 12000);
        std::cout << "PASS autosave failure notice, bounded retry, and automatic recovery\n";
    }
    for (const auto answer : {IDCANCEL, IDNO, IDYES}) {
        const auto session = artifacts / (L"close-save-" + std::to_wstring(answer) + L".ini");
        Host host(session);
        auto& app = host.app;
        app.OpenPaths({document.wstring()});
        LoadedPath(app, document);
        Check(app.Save(), "Cannot create close save failure fixture");
        const auto previous = FileBytes(session);
        const auto shown = app.currentDoc;
        const auto active = app.active;
        SessionLock lock(session);
        app.tabs.front().font = 22;
        CloseWithAnswer(app, answer, [&] { if (answer == IDYES) lock.Release(); });
        if (answer == IDCANCEL) {
            Check(IsWindow(app.hwnd) && !app.closing && app.active == active && app.currentDoc == shown
                && app.saveRetryPending && FileBytes(session) == previous,
                "Cancelling a failed close did not preserve the running app and prior session");
            lock.Release();
            SendMessageW(app.hwnd, WM_TIMER, 3, 0);
            Check(!app.saveRetryPending && GetPrivateProfileIntW(L"Tab0", L"Font", 0, session.c_str()) == 22,
                "Cancelling close left normal saving disabled");
        } else {
            Check(!IsWindow(app.hwnd) && app.closing, "Requested retry/discard did not complete closing");
            if (answer == IDNO) Check(FileBytes(session) == previous, "Discarding a failed save changed the prior session");
            else Check(GetPrivateProfileIntW(L"Tab0", L"Font", 0, session.c_str()) == 22, "Retry did not save before closing");
        }
    }
    std::cout << "PASS save failure close cancellation, retry, discard, and modal reentry guard\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR graphics = 0;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize controls");
        const auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        PruneOldRuns(sourceRoot / L"artifacts" / L"session-regression");
        const auto artifacts = sourceRoot / L"artifacts" / L"session-regression"
            / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(artifacts);
        std::ofstream records(artifacts / L"results.jsonl", std::ios::binary);
        Check(bool(records), "Cannot open regression output");
        std::cout << "Artifacts: " << mm::Utf8(artifacts.wstring()) << std::endl;
        SessionRoundTrips(artifacts, records);
        SparseSessionChecks(artifacts);
        BatchedOpenChecks(artifacts);
        SaveFailureChecks(artifacts);
        Write(artifacts / L"result.txt", "PASS: 32/100/256-tab roundtrip, flags/order/active, damaged counts, empty paths, atomic replacement failure, batched file opens/scroll, autosave retry, and close failure choices/reentry.\n");
    } catch (const std::exception& error) {
        std::cerr << "lifetime_probe FAILED: " << error.what() << std::endl; result = 1;
    }
    Gdiplus::GdiplusShutdown(graphics);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

