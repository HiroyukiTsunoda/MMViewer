// Diagnostic: time incremental tree updates against a real folder tree.
// Usage: tree_probe <folder> [expanded-subfolder ...]
#define main IncludedHostTestsMain
#include "host_tests.cpp"
#undef main

namespace {

size_t CountNodes(const mm::FolderNode& node) {
    size_t count = 1;
    for (const auto& child : node.children) count += CountNodes(child);
    return count;
}

double Milliseconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

void Report(const App& app, const char* label, double callMs) {
    const auto& stats = app.treeStats;
    std::cout << label << ": call=" << callMs << " ms, rows inserted=" << stats.rowsInserted << " deleted=" << stats.rowsDeleted
              << ", slices=" << stats.slices << " max slice=" << stats.maxSliceMs << " ms, total=" << stats.totalMs
              << " ms, native rows=" << TreeView_GetCount(app.treeH) << std::endl;
}

// Let queued row work run through the real timer path and report its slices.
void PumpUntilIdle(App& app) {
    Until([&] { return !app.TreeTasksPending(); }, "Queued tree work did not finish", 120000);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR graphics = 0;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize controls");
        if (argc < 2) throw std::runtime_error("Usage: tree_probe <folder> [expanded-subfolder ...]");
        const auto root = mm::NormalizePath(argv[1]);
        wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
        const auto session = fs::path(temp) / L"MMViewer-tree-probe" / (L"run-" + std::to_wstring(GetCurrentProcessId()));
        Host host(session / L"session.ini");
        auto& app = host.app;
        auto folder = std::make_unique<FolderRegistration>();
        folder->id = app.nextFolderId++;
        folder->path = root;
        folder->pending = {fs::path(root).filename().wstring(), root, true, {}};
        auto& registration = *folder;
        app.folders.push_back(std::move(folder));
        app.expanded.insert(root);
        for (int i = 2; i < argc; ++i) app.expanded.insert(mm::NormalizePath(argv[i]));
        app.RebuildTree();
        SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        SetWindowPos(app.hwnd, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - app.D(2000), GetSystemMetrics(SM_YVIRTUALSCREEN),
            app.D(1100), app.D(800), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(app.hwnd);

        auto scanStart = std::chrono::steady_clock::now();
        auto tree = std::make_unique<mm::FolderNode>(mm::ScanFolder(root));
        std::cout << "scan: nodes=" << CountNodes(*tree) << " in " << Milliseconds(scanStart) << " ms" << std::endl;

        // Initial population runs through timer slices like the real application.
        auto start = std::chrono::steady_clock::now();
        app.ReplaceFolderTree(registration, std::move(tree));
        const double populateCall = Milliseconds(start);
        PumpUntilIdle(app);
        Report(app, "placeholder -> full tree (sliced)", populateCall);

        auto identical = std::make_unique<Scanned>();
        identical->folderId = registration.id; identical->generation = registration.generation;
        identical->tree = std::make_unique<mm::FolderNode>(*registration.tree);
        start = std::chrono::steady_clock::now();
        app.AcceptTree(std::move(identical));
        std::cout << "identical AcceptTree: " << Milliseconds(start) << " ms" << std::endl;

        auto modified = std::make_unique<mm::FolderNode>(*registration.tree);
        modified->children.push_back({L"probe-added.md", (fs::path(root) / L"probe-added.md").wstring(), false, {}});
        modified->children.sort(mm::FolderNodeBefore); // Hand-made trees must use scan order.
        start = std::chrono::steady_clock::now();
        app.ReplaceFolderTree(registration, std::move(modified));
        const double addCall = Milliseconds(start);
        PumpUntilIdle(app);
        Report(app, "one file added", addCall);

        auto restored = std::make_unique<mm::FolderNode>(*registration.tree);
        std::erase_if(restored->children, [](const auto& node) { return node.name == L"probe-added.md"; });
        start = std::chrono::steady_clock::now();
        app.ReplaceFolderTree(registration, std::move(restored));
        const double removeCall = Milliseconds(start);
        PumpUntilIdle(app);
        Report(app, "one file removed", removeCall);

        // A folder subtree removed and re-added: deletion and insertion of many rows, sliced.
        auto pruned = std::make_unique<mm::FolderNode>(*registration.tree);
        auto largest = pruned->children.end();
        size_t largestSize = 0;
        for (auto it = pruned->children.begin(); it != pruned->children.end(); ++it)
            if (it->directory && CountNodes(*it) > largestSize) { largestSize = CountNodes(*it); largest = it; }
        if (largest != pruned->children.end()) {
            const auto name = largest->name;
            auto keep = std::make_unique<mm::FolderNode>(*registration.tree);
            pruned->children.erase(largest);
            start = std::chrono::steady_clock::now();
            app.ReplaceFolderTree(registration, std::move(pruned));
            const double dropCall = Milliseconds(start);
            PumpUntilIdle(app);
            Report(app, ("subtree removed: " + mm::Utf8(name) + " (" + std::to_string(largestSize) + " nodes)").c_str(), dropCall);
            start = std::chrono::steady_clock::now();
            app.ReplaceFolderTree(registration, std::move(keep));
            const double backCall = Milliseconds(start);
            PumpUntilIdle(app);
            Report(app, "subtree restored", backCall);
        }

        // Files added directly under the root: one queued run of new siblings.
        {
            auto bulk = std::make_unique<mm::FolderNode>(*registration.tree);
            for (int i = 0; i < 3000; ++i) {
                const auto name = L"probe-bulk-" + std::to_wstring(i) + L".md";
                bulk->children.push_back({name, (fs::path(root) / name).wstring(), false, {}});
            }
            bulk->children.sort(mm::FolderNodeBefore);
            auto shrink = std::make_unique<mm::FolderNode>(*registration.tree);
            start = std::chrono::steady_clock::now();
            app.ReplaceFolderTree(registration, std::move(bulk));
            const double bulkCall = Milliseconds(start);
            PumpUntilIdle(app);
            Report(app, "3,000 files added under the root", bulkCall);
            start = std::chrono::steady_clock::now();
            app.ReplaceFolderTree(registration, std::move(shrink));
            const double shrinkCall = Milliseconds(start);
            PumpUntilIdle(app);
            Report(app, "3,000 files removed again", shrinkCall);
        }

        // Cost of pointing a row at a node without changing anything else.
        {
            std::vector<std::pair<HTREEITEM, mm::FolderNode*>> rows;
            app.WalkItems(TreeView_GetRoot(app.treeH), [&](HTREEITEM item, mm::FolderNode* node) { rows.emplace_back(item, node); });
            const auto top = TreeView_GetFirstVisible(app.treeH);
            start = std::chrono::steady_clock::now();
            for (const auto& [item, node] : rows) if (node) app.SetRowNode(item, *node);
            std::cout << "TVM_SETITEM(TVIF_PARAM) x " << rows.size() << ": " << Milliseconds(start) << " ms, first visible "
                      << (TreeView_GetFirstVisible(app.treeH) == top ? "unchanged" : "MOVED") << std::endl;
        }
        ShowWindow(app.hwnd, SW_HIDE);
        std::cout << "tree_probe: done" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "tree_probe FAILED: " << error.what() << '\n'; result = 1;
    }
    Gdiplus::GdiplusShutdown(graphics);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
