// An optional, bounded endurance workload against the real native host. The
// production implementation and existing host helpers are shared, not mocked.
// Run under an external process timeout: a blocked UI dispatch cannot time out
// itself. Never calls Restore() or touches the user's actual session.ini.
#define main IncludedHostTestsMain
#include "host_tests.cpp"
#undef main

#include <psapi.h>
#include <tlhelp32.h>
#include <iomanip>

namespace {
struct SoakOptions {
    int batches = 6, cycles = 200, idleSeconds = 15;
    fs::path output;
};

struct SoakCounters {
    uint64_t opens = 0, closes = 0, switches = 0, cancellations = 0;
    uint64_t reloads = 0, watchedUpdates = 0, rootChanges = 0, themes = 0;
    uint64_t zooms = 0, resizes = 0, paints = 0;
};

struct SoakSample {
    uint64_t tick = 0, privateBytes = 0, workingSetBytes = 0;
    uint64_t heapAllocatedBytes = 0, heapCommittedBytes = 0, heapReservedBytes = 0;
    double cpuSeconds = 0;
    DWORD handles = 0, gdi = 0, user = 0, threads = 0;
    DWORD heapCount = 0, heapSummaries = 0;
};

SoakSample ReadSoakSample() {
    SoakSample sample;
    sample.tick = GetTickCount64();
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb = sizeof(memory);
    Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE,
        "Cannot read soak process memory");
    sample.privateBytes = memory.PrivateUsage;sample.workingSetBytes = memory.WorkingSetSize;
    Check(GetProcessHandleCount(GetCurrentProcess(), &sample.handles) != FALSE, "Cannot read process handles");
    sample.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    sample.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "Cannot read process CPU time");
    auto ticks = [](FILETIME value) {return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;};
    sample.cpuSeconds = (ticks(kernel) + ticks(user)) / 10000000.0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    Check(snapshot != INVALID_HANDLE_VALUE, "Cannot read thread snapshot");
    THREADENTRY32 entry{};entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++sample.threads;
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    // Read live allocation separately from allocator-retained committed pages.
    // A fixed stack buffer avoids allocating in the heaps being measured. Heap
    // creation/destruction can race enumeration; log successful summaries so
    // partial samples cannot be mistaken for a drop in live allocation.
    HANDLE heaps[512]{};
    sample.heapCount = GetProcessHeaps(DWORD(std::size(heaps)), heaps);
    Check(sample.heapCount != 0, "Cannot enumerate process heaps");
    for (DWORD n = 0; n < std::min(sample.heapCount, DWORD(std::size(heaps))); ++n) {
        HEAP_SUMMARY summary{};summary.cb = sizeof(summary);
        if (HeapSummary(heaps[n], 0, &summary)) {
            ++sample.heapSummaries;
            sample.heapAllocatedBytes += summary.cbAllocated;
            sample.heapCommittedBytes += summary.cbCommitted;
            sample.heapReservedBytes += summary.cbReserved;
        }
    }
    return sample;
}

struct SoakLog {
    std::ofstream file;
    SoakCounters counters;
    SoakSample origin;
    explicit SoakLog(const fs::path& path) : file(path, std::ios::binary | std::ios::trunc), origin(ReadSoakSample()) {
        Check(bool(file), "Cannot create soak metrics log");
        file << std::fixed << std::setprecision(6);
    }
    SoakSample Record(const char* phase, int batch, const App* app = nullptr) {
        auto sample = ReadSoakSample();
        file << "{\"phase\":\"" << phase << "\",\"batch\":" << batch
             << ",\"elapsed_seconds\":" << (sample.tick - origin.tick) / 1000.0
             << ",\"private_bytes\":" << sample.privateBytes << ",\"working_set_bytes\":" << sample.workingSetBytes
             << ",\"heap_allocated_bytes\":" << sample.heapAllocatedBytes
             << ",\"heap_committed_bytes\":" << sample.heapCommittedBytes
             << ",\"heap_reserved_bytes\":" << sample.heapReservedBytes
             << ",\"heap_count\":" << sample.heapCount << ",\"heap_summary_successes\":" << sample.heapSummaries
             << ",\"cpu_seconds\":" << sample.cpuSeconds << ",\"handles\":" << sample.handles
             << ",\"gdi\":" << sample.gdi << ",\"user\":" << sample.user << ",\"threads\":" << sample.threads;
        if (app) {
            auto view = app->view.GetPerformanceStats();
            file << ",\"tabs\":" << app->tabs.size() << ",\"closed\":" << app->closed.size()
                 << ",\"folders\":" << app->folders.size() << ",\"expanded\":" << app->expanded.size()
                 << ",\"excluded\":" << app->excludedFolders.size() << ",\"watches\":" << app->watches.size()
                 << ",\"tree_items\":" << TreeView_GetCount(app->treeH)
                 << ",\"document_cache_count\":" << app->documentCache.size() << ",\"document_cache_bytes\":" << app->documentCacheBytes
                 << ",\"view_cache_count\":" << view.cachedDocuments << ",\"view_cache_bytes\":" << view.cachedBytes
                 << ",\"parses\":" << view.parses << ",\"layouts\":" << view.layouts << ",\"cache_hits\":" << view.cacheHits
                 << ",\"buffer_allocations\":" << view.bufferAllocations;
        }
        file << ",\"opens\":" << counters.opens << ",\"closes\":" << counters.closes << ",\"switches\":" << counters.switches
             << ",\"cancelled_open_attempts\":" << counters.cancellations << ",\"forced_reloads\":" << counters.reloads
             << ",\"watched_updates\":" << counters.watchedUpdates << ",\"root_changes\":" << counters.rootChanges
             << ",\"themes\":" << counters.themes << ",\"zooms\":" << counters.zooms
             << ",\"resizes\":" << counters.resizes << ",\"paints\":" << counters.paints << "}\n";
        file.flush();Check(bool(file), "Cannot flush soak metrics");
        std::cout << phase << " batch=" << batch << " private=" << sample.privateBytes / 1024 / 1024
                  << "MiB handles=" << sample.handles << " GDI=" << sample.gdi << " USER=" << sample.user << std::endl;
        return sample;
    }
};

std::string SoakDocument(int index, int revision = 0) {
    std::string result = "# Endurance document " + std::to_string(index) + "\n\nlayout-marker revision-" + std::to_string(revision) + "\n\n";
    if (index % 4 == 0) {
        result += "![local image](sample.bmp)\n\n```mermaid\nflowchart LR\n  A[Open] --> B[Parse]\n  B --> C[Display]\n```\n\n";
        result += "| Name | Status |\n|---|---|\n| Alpha | Ready |\n| Beta | Done |\n\n";
        result += "```cpp\nfor (int i = 0; i < 100; ++i) { render(i); }\n```\n\n";
    }
    const int sections = index % 4 == 1 ? 150 : (index % 4 == 2 ? 45 : 5);
    for (int n = 0; n < sections; ++n)
        result += "## Section " + std::to_string(n) + "\n\n日本語と English mixed content. **Strong** and *emphasis* with [anchor](#section-0). This paragraph wraps across the available width.\n\n";
    return result;
}

void MakeSoakImage(const fs::path& path) {
    // A small deterministic local image exercises GDI+ decoding without network.
    BITMAPFILEHEADER file{};BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);header.biWidth = 96;header.biHeight = 64;
    header.biPlanes = 1;header.biBitCount = 24;header.biSizeImage = 96 * 64 * 3;
    file.bfType = 0x4d42;file.bfOffBits = sizeof(file) + sizeof(header);file.bfSize = file.bfOffBits + header.biSizeImage;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 96; ++x) {
        char pixel[]{char(x * 2), char(y * 3), char((x + y) % 256)};out.write(pixel, sizeof(pixel));
    }
    Check(bool(out), "Cannot create soak image");
}

void SoakPaint(App& app, SoakCounters& count) {
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    UpdateWindow(app.view.Handle());++count.paints;
}

void CheckSoakBounds(const App& app) {
    Check(app.closed.size() <= 10, "Closed tab history exceeded its bound");
    Check(app.documentCache.size() <= App::DocumentCacheCount && app.documentCacheBytes <= App::DocumentCacheBytes,
        "Raw document cache exceeded its bound");
    auto view = app.view.GetPerformanceStats();
    Check(view.cachedDocuments <= 8 && view.cachedBytes <= 64 * 1024 * 1024, "Viewer cache exceeded its bound");
    Check(app.watches.size() <= 64, "File watcher count exceeded its bound");
}

void SoakSelect(App& app, const fs::path& path, SoakCounters& count) {
    app.Switch(IndexOf(app, path));++count.switches;LoadedPath(app, path);
    Check(app.currentDoc->source.find("layout-marker") != std::string::npos, "Stale or incorrect document content");
}

void SoakReset(App& app, const std::vector<fs::path>& documents, SoakCounters& count) {
    while (app.tabs.size() > 8) {app.Close(int(app.tabs.size()) - 1);++count.closes;}
    app.dark = false;app.ApplyTheme();app.wide = false;app.view.SetWide(false);
    RECT bounds{};GetWindowRect(app.hwnd, &bounds);
    SetWindowPos(app.hwnd, nullptr, bounds.left, bounds.top, 1100, 800, SWP_NOZORDER | SWP_NOACTIVATE);
    for (int n = 0; n < 8; ++n) {
        auto& tab = app.tabs[IndexOf(app, documents[n])];tab.font = 16;tab.source = false;tab.scroll = 0;
        SoakSelect(app, documents[n], count);app.view.SetFontSize(16);app.view.SetSourceMode(false);app.view.SetScrollRatio(0);
        SoakPaint(app, count);
    }
    SoakSelect(app, documents[0], count);SoakPaint(app, count);
    Until([&] {return !app.loading && app.watches.size() == 1 && std::none_of(app.folders.begin(), app.folders.end(),
        [](const auto& folder) {return folder->scanning;});}, "Soak did not reach stable read/watch/scan state", 15000);
    PumpFor(800);CheckSoakBounds(app);
    Check(app.tabs.size() == 8 && app.folders.size() == 1 && app.excludedFolders.size() <= 1,
        "Batch did not restore the fixed tab/folder state");
}

void SoakBatch(App& app, const std::vector<fs::path>& documents, const fs::path& extraRoot,
    int cycles, int batch, SoakLog& log) {
    auto& count = log.counters;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        const auto& extra = documents[8 + cycle % 24];
        app.OpenPaths({extra.wstring()});++count.opens;LoadedPath(app, extra);
        app.view.Search(L"layout-marker", 0);Check(app.view.SearchResult().second == 1, "Loaded document search was incorrect");
        app.view.Search(L"", 0);app.view.SetScrollRatio((cycle % 10) / 10.0);SoakPaint(app, count);
        SoakSelect(app, documents[cycle % 8], count);
        app.Close(IndexOf(app, extra));++count.closes;
        SoakSelect(app, documents[(cycle + 1) % 8], count);SoakPaint(app, count);
        if (cycle % 10 == 0) {
            // No pumping here: exercise cancellation and obsolete posted results.
            for (int n = 0; n < 6; ++n) {app.OpenPaths({documents[16 + n].wstring()});++count.opens;++count.cancellations;}
            while (app.tabs.size() > 8) {app.Close(int(app.tabs.size()) - 1);++count.closes;}
            SoakSelect(app, documents[cycle % 8], count);
        }
        if (cycle % 20 == 0) {
            app.Command(Theme);++count.themes;app.Zoom(1);++count.zooms;
            app.Command(Source);SoakPaint(app, count);app.Command(Source);app.Zoom(999);++count.zooms;
            RECT bounds{};GetWindowRect(app.hwnd, &bounds);
            SetWindowPos(app.hwnd, nullptr, bounds.left, bounds.top, cycle % 40 ? 1100 : 1250, cycle % 40 ? 800 : 900,
                SWP_NOZORDER | SWP_NOACTIVATE);++count.resizes;SoakPaint(app, count);
        }
        if (cycle % 25 == 0) {
            // Alternate completed and cancelled registration scans/watch setup.
            app.OpenRoot(extraRoot.wstring());++count.rootChanges;
            if (cycle % 50 == 0) {
                Until([&] {return FolderTree(app, extraRoot) && WatchCoversFolder(app, extraRoot);},
                    "Extra root scan/watch setup timed out", 15000);
            }
            app.RemoveFolderFromList(extraRoot.wstring());++count.rootChanges;
        }
        if (cycle % 50 == 0) {
            const auto& path = documents[0];SoakSelect(app, path, count);
            const auto revision = 1 + batch * 100000 + cycle;
            auto content = SoakDocument(0, revision);Write(path, content);
            app.Command(Reload);++count.reloads;LoadedPath(app, path);
            Check(app.currentDoc->source == content, "Forced reload displayed stale file bytes");
            content = SoakDocument(0, revision + 1);Write(path, content);
            // Let the actual directory notification/poll/debounce path reload.
            Until([&] {return !app.loading && app.currentDoc && app.currentDoc->source == content;},
                "Automatic watcher refresh displayed stale file bytes", 15000);++count.watchedUpdates;
            SoakPaint(app, count);
        }
        CheckSoakBounds(app);
        if (cycle % 100 == 99) log.Record("progress", batch, &app);
    }
    Write(documents[0], SoakDocument(0));
    SoakSelect(app, documents[0], count);app.Command(Reload);++count.reloads;LoadedPath(app, documents[0]);
    SoakReset(app, documents, count);
}

void AssertSoakResources(const SoakSample& baseline, const SoakSample& current) {
    // Native theme/font and allocator caches may retain a high-water mark.
    // Private bytes are evidence for trend analysis, deliberately not a hard
    // leak verdict. Fixed-state kernel/GDI/USER resources should stay bounded.
    Check(current.handles <= baseline.handles + 16, "Process handles grew beyond the fixed-state allowance");
    Check(current.gdi <= baseline.gdi + 8, "GDI objects grew beyond the fixed-state allowance");
    Check(current.user <= baseline.user + 8, "USER objects grew beyond the fixed-state allowance");
    Check(current.threads <= baseline.threads + 4, "Thread count grew beyond the fixed-state allowance");
}

SoakOptions ParseSoakOptions() {
    SoakOptions options;
    int argc = 0;auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Check(argv != nullptr, "Cannot read soak arguments");
    try {
        for (int n = 1; n < argc; ++n) {
            const std::wstring argument = argv[n];Check(n + 1 < argc, "Soak option is missing its value");
            const std::wstring value = argv[++n];
            if (argument == L"--batches") options.batches = std::stoi(value);
            else if (argument == L"--cycles") options.cycles = std::stoi(value);
            else if (argument == L"--idle-seconds") options.idleSeconds = std::stoi(value);
            else if (argument == L"--output") options.output = value;
            else throw std::runtime_error("Unknown soak option");
        }
    } catch (...) {LocalFree(argv);throw;}
    LocalFree(argv);
    Check(options.batches >= 1 && options.batches <= 1000 && options.cycles >= 1 && options.cycles <= 100000
        && options.idleSeconds >= 2 && options.idleSeconds <= 86400, "Soak option is out of range");
    return options;
}
} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR graphics = 0;Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        auto options = ParseSoakOptions();
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize soak native controls");
        const auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        if (options.output.empty()) {
            PruneOldRuns(sourceRoot / L"artifacts" / L"soak-tests");
            options.output = sourceRoot / L"artifacts" / L"soak-tests"
                / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        }
        options.output = fs::absolute(options.output);fs::create_directories(options.output);
        std::wcout << L"Soak artifacts: " << options.output.wstring() << std::endl;
        const auto root = options.output / L"日本語 # documents";
        const auto extraRoot = options.output / L"transient-root";
        std::vector<fs::path> documents;
        for (int n = 0; n < 32; ++n) {
            documents.push_back(root / (L"document-" + std::to_wstring(n) + L".md"));
            Write(documents.back(), SoakDocument(n));
        }
        MakeSoakImage(root / L"sample.bmp");
        for (int n = 0; n < 24; ++n) Write(extraRoot / L"nested" / (std::to_wstring(n) + L".md"), SoakDocument(n));
        MakeSoakImage(extraRoot / L"nested" / L"sample.bmp");
        SoakLog log(options.output / L"metrics.jsonl");log.Record("before_host", -1);
        {
            Host host(options.output / L"session.ini");auto& app = host.app;
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 1200;
            SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            SetWindowPos(app.hwnd, nullptr, left, GetSystemMetrics(SM_YVIRTUALSCREEN), 1100, 800,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            Check(IsWindowVisible(app.hwnd) != FALSE, "Soak host must exercise visible native control behavior");
            app.OpenRoot(root.wstring());
            Until([&] {return FolderTree(app, root) && WatchCoversFolder(app, root);}, "Soak initial root setup timed out", 15000);
            for (int n = 0; n < 8; ++n) {app.OpenPaths({documents[n].wstring()});++log.counters.opens;LoadedPath(app, documents[n]);SoakPaint(app, log.counters);}
            SoakBatch(app, documents, extraRoot, std::min(options.cycles, 100), 0, log);
            const auto baseline = log.Record("baseline_after_warmup", 0, &app);
            for (int batch = 1; batch <= options.batches; ++batch) {
                SoakBatch(app, documents, extraRoot, options.cycles, batch, log);
                const auto sample = log.Record("batch_stable", batch, &app);AssertSoakResources(baseline, sample);
            }
            const auto idleStart = log.Record("idle_start", options.batches, &app);
            const auto deadline = GetTickCount64() + uint64_t(options.idleSeconds) * 1000;
            while (GetTickCount64() < deadline) {
                const auto nextSample = std::min(deadline, GetTickCount64() + 5000);
                // Match the application's event-driven message loop. A 10 ms
                // test polling interval would itself dominate idle CPU usage.
                while (GetTickCount64() < nextSample) {
                    Pump();const auto now = GetTickCount64();if (now >= nextSample) break;
                    Check(MsgWaitForMultipleObjectsEx(0, nullptr, DWORD(nextSample - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                        != WAIT_FAILED, "Soak idle message wait failed");
                }
                log.Record("idle_sample", options.batches, &app);
            }
            const auto idleEnd = log.Record("idle_end", options.batches, &app);
            AssertSoakResources(baseline, idleEnd);
            const auto idleWall = (idleEnd.tick - idleStart.tick) / 1000.0;
            Check(idleEnd.cpuSeconds - idleStart.cpuSeconds < std::max(1.0, idleWall * 0.10), "Stationary host remained CPU busy");
            log.Record("before_destroy", options.batches, &app);
        }
        PumpFor(1000);log.Record("after_destroy", options.batches);
        Write(options.output / L"result.txt", "PASS: fixed-state resource bounds, content freshness, cancellation, native painting and idle CPU checks passed.\n"
            "Private-memory measurements require trend interpretation; this workload is not proof of leak absence or multi-day operation.\n");
        std::cout << "soak_tests: all checks passed" << std::endl;
    } catch (const std::exception& error) {std::cerr << "soak_tests FAILED: " << error.what() << std::endl;result = 1;}
    Gdiplus::GdiplusShutdown(graphics);if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
