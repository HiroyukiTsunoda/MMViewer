// Real modal and asynchronous regression checks use isolated files and session state.
#define main IncludedHostMain
#include "host_tests.cpp"
#undef main

namespace {
struct EncodingProbe {
    std::function<void()> reenter;
    bool callbackRan = false;
    std::string callbackError;
    int answer = IDYES;
    UINT_PTR timer = 0;
    HHOOK hook = nullptr;
};
EncodingProbe* encodingProbe = nullptr;

BOOL CALLBACK OwnDialog(HWND window, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (process == GetCurrentProcessId() && wcscmp(name, L"#32770") == 0) {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

LRESULT CALLBACK HideProbeDialog(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE) {
        HWND dialog = HWND(w);
        wchar_t name[64]{};
        GetClassNameW(dialog, name, 64);
        if (wcscmp(name, L"#32770") == 0) {
            SetWindowPos(dialog, nullptr,
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200,
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 200,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

void CALLBACK EncodingReentry(HWND, UINT, UINT_PTR timer, DWORD) {
    auto* probe = encodingProbe;
    if (!probe || probe->callbackRan) return;
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), OwnDialog, LPARAM(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer);
    ShowWindow(dialog, SW_HIDE);
    probe->callbackRan = true;
    try { probe->reenter(); }
    catch (const std::exception& error) { probe->callbackError = error.what(); }
    PostMessageW(dialog, WM_COMMAND, probe->answer, 0);
}

std::unique_ptr<Loaded> TakeReadCompletion(App& app) {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        if (PeekMessageW(&message, app.hwnd, LoadDone, LoadDone, PM_REMOVE)) {
            auto result = std::unique_ptr<Loaded>(reinterpret_cast<Loaded*>(message.lParam));
            if (result->cancel == app.readCancel) return result;
        }
        Sleep(1);
    }
    throw std::runtime_error("No document read completion in encoding check");
}

void RespondToEncoding(App& app, std::unique_ptr<Loaded> result,
    int answer, std::function<void()> reenter) {
    Check(result->error == "ENCODING_REQUIRED", "Fixture did not reach encoding prompt");
    EncodingProbe state;
    state.reenter = std::move(reenter);
    state.answer = answer;
    state.hook = SetWindowsHookExW(WH_CBT, HideProbeDialog, nullptr, GetCurrentThreadId());
    Check(state.hook != nullptr, "Cannot isolate encoding dialog placement");
    encodingProbe = &state;
    state.timer = SetTimer(nullptr, 0, 50, EncodingReentry);
    if (!state.timer) {
        encodingProbe = nullptr;
        UnhookWindowsHookEx(state.hook);
        throw std::runtime_error("Cannot arm encoding reentry callback");
    }
    app.AcceptLoad(std::move(result));
    KillTimer(nullptr, state.timer);
    UnhookWindowsHookEx(state.hook);
    encodingProbe = nullptr;
    Check(state.callbackRan && state.callbackError.empty(), "Encoding callback did not complete");
    Check(!app.encodingPromptCancel, "Encoding prompt retained its temporary request guard");
}

void StopProbeWatches(App& app) {
    for (UINT_PTR timer : {1, 2, 3, 4, 5}) KillTimer(app.hwnd, timer);
    app.CancelWatchSetup();
    app.RetireWatches(std::move(app.watches));
}

void EncodingReentryChecks(const fs::path& root) {
    const auto invalid = root / L"cp932.md";
    std::vector<std::wstring> paths;
    for (int i = 0; i < 40; ++i) {
        const auto file = root / (L"valid-" + std::to_wstring(i) + L".md");
        Write(file, "# Valid document " + std::to_string(i) + "\n");
        paths.push_back(file.wstring());
    }
    const auto prepare = [&](App& app) {
        Write(invalid, std::string("\x82\xa0", 2));
        app.OpenPaths({invalid.wstring()});
        StopProbeWatches(app);
        return TakeReadCompletion(app);
    };
    for (const auto answer : {IDYES, IDNO}) {
        Host host(root / (answer == IDYES ? L"yes.ini" : L"no.ini"));
        auto& app = host.app;
        auto result = prepare(app);
        const auto request = app.readCancel;
        RespondToEncoding(app, std::move(result), answer, [&] {
            app.CheckActive();
            Check(app.readCancel == request, "Activation superseded the visible encoding prompt");
        });
        if (answer == IDYES) {
            LoadedPath(app, invalid);
            Check(app.tabs[app.active].cp932 && app.currentDoc->source == "\xe3\x81\x82",
                "Yes did not decode the current CP932 document");
        } else {
            Check(!app.tabs[app.active].cp932 && !app.tabs[app.active].status.empty() && !app.loading,
                "No did not retain a stable read error on the current tab");
        }
    }
    for (const auto answer : {IDYES, IDNO}) {
        Host host(root / (answer == IDYES ? L"reentry-yes.ini" : L"reentry-no.ini"));
        auto& app = host.app;
        auto result = prepare(app);
        const auto originalId = app.tabs[app.active].id;
        const auto previousData = app.tabs.data();
        bool moved = false;
        RespondToEncoding(app, std::move(result), answer, [&] {
            // This is the same entry point as another invocation's WM_COPYDATA.
            app.OpenPaths(paths);
            moved = app.tabs.data() != previousData;
        });
        Check(moved, "Actual file opens did not reallocate the tab vector");
        LoadedPath(app, fs::path(paths.back()));
        const auto original = std::find_if(app.tabs.begin(), app.tabs.end(),
            [&](const auto& tab) { return tab.id == originalId; });
        Check(original != app.tabs.end() && !original->cp932 && original->status.empty(),
            "Superseded prompt mutated the original inactive tab");
        Check(!app.tabs[app.active].cp932 && app.tabs[app.active].status.empty()
            && app.currentDoc->source == "# Valid document 39\n",
            "Superseded prompt corrupted the newly opened tab");
    }
    {
        Host host(root / L"close-original.ini");
        auto& app = host.app;
        auto result = prepare(app);
        const auto originalId = app.tabs[app.active].id;
        RespondToEncoding(app, std::move(result), IDYES, [&] {
            app.Close(app.active);
            app.OpenPaths({paths.front()});
        });
        LoadedPath(app, fs::path(paths.front()));
        Check(std::none_of(app.tabs.begin(), app.tabs.end(), [&](const auto& tab) { return tab.id == originalId; })
            && !app.tabs[app.active].cp932 && app.tabs[app.active].status.empty(),
            "Closing the original tab applied its encoding answer to its replacement");
    }
    for (const bool background : {false, true}) {
        Host host(root / (background ? L"same-generation.ini" : L"new-generation.ini"));
        auto& app = host.app;
        auto result = prepare(app);
        const auto generation = app.loadGeneration;
        const auto request = app.readCancel;
        RespondToEncoding(app, std::move(result), background ? IDNO : IDYES, [&] {
            Write(invalid, "# Now valid UTF-8\n");
            app.LoadActive(true, true, background);
            Check(app.readCancel != request && (background ? app.loadGeneration == generation : app.loadGeneration != generation),
                "Replacement read did not exercise its expected generation/token boundary");
        });
        LoadedPath(app, invalid);
        Check(!app.tabs[app.active].cp932 && app.tabs[app.active].status.empty()
            && app.currentDoc->source == "# Now valid UTF-8\n",
            "Stale encoding response changed a newer read of the same tab");
    }
    {
        Host host(root / L"shift-original-index.ini");
        auto& app = host.app;
        app.OpenPaths({paths.front()});
        LoadedPath(app, fs::path(paths.front()));
        auto result = prepare(app);
        const auto originalId = app.tabs[app.active].id;
        RespondToEncoding(app, std::move(result), IDYES, [&] { app.Close(IndexOf(app, fs::path(paths.front()))); });
        LoadedPath(app, invalid);
        Check(app.tabs[app.active].id == originalId && app.tabs[app.active].cp932
            && app.currentDoc->source == "\xe3\x81\x82",
            "Closing an earlier inactive tab lost the current tab's encoding choice");
    }
    Write(root / L"result.txt", "PASS: Yes/No, tab-vector reallocation, active close, inactive index shift, replacement generation and replacement token.\n");
    std::cout << "PASS encoding confirmation remains safe across tab opens/closes and replacement reads\n";
}

void WatchRecreationCheck(const fs::path& root, bool registered) {
    const auto directory = root / L"watched";
    const auto file = directory / L"document.md";
    Write(file, "# Initial body\n");
    Host host(root / L"session.ini");
    auto& app = host.app;
    if (registered) app.OpenRoot(directory.wstring());
    app.OpenPaths({file.wstring()});
    LoadedPath(app, file);
    Until([&] { return !app.watchSetupPending && app.watches.size() == 1 && app.watches.front()->pending
        && (!registered || TreeItem(app, file) != nullptr); }, "Initial watch/tree was not ready");
    PumpFor(800);
    // These exact files were created above in this isolated artifact directory.
    Check(fs::remove(file) && fs::remove(directory), "Cannot remove isolated watched directory");
    Until([&] { return app.watchRetryPending && !app.loading && !app.tabs[app.active].status.empty(); },
        "Deleted parent did not schedule a watch retry and document read error", 6000);
    const auto failedGeneration = app.watchGeneration;
    PumpFor(600);
    Check(app.watchGeneration <= failedGeneration + 1, "Failed watch is spinning through immediate rebuilds");
    Write(file, "# Recreated first body\n");
    Until([&] { return !app.watchSetupPending && !app.watchRetryPending && !app.watchLimited
        && app.watches.size() == 1 && app.watches.front()->pending && !app.loading
        && app.currentDoc && app.currentDoc->source == "# Recreated first body\n"
        && (!registered || TreeItem(app, file) != nullptr); },
        "Recreated parent did not recover its watch, active document, and folder list", 9000);
    Write(file, "# Recreated second body - watch must observe this\n");
    Until([&] { return !app.loading && app.currentDoc
        && app.currentDoc->source == "# Recreated second body - watch must observe this\n"; },
        "Save after parent recreation was not detected");
    const auto healthyGeneration = app.watchGeneration;
    PumpFor(3300);
    Check(!app.watchRetryPending && app.watchGeneration == healthyGeneration,
        "Recovered watch kept rebuilding while idle");
    Check(fs::remove(file) && fs::remove(directory), "Cannot remove isolated directory for cancellation check");
    Until([&] { return app.watchRetryPending; }, "Missing parent did not schedule cancellation fixture retry");
    if (registered) app.RemoveFolderFromList(directory.wstring());
    while (!app.tabs.empty()) app.Close(0);
    Until([&] { return app.watches.empty() && !app.watchRetryPending && !app.watchSetupPending; },
        "Closing registrations retained missing-folder retries");
    const auto closedGeneration = app.watchGeneration;
    Write(file, "# Closed registration must remain closed\n");
    PumpFor(3300);
    Check(app.tabs.empty() && app.watches.empty() && !app.watchRetryPending
        && app.watchGeneration == closedGeneration, "A closed registration restarted watch setup");
    std::cout << "PASS " << (registered ? "registered-root" : "standalone")
              << " parent recreation, subsequent save, idle monitoring, and retry cancellation\n";
}

void WatchCloseCheck(const fs::path& root) {
    const auto directory = root / L"watched";
    const auto file = directory / L"document.md";
    Write(file, "# Closing with a pending retry\n");
    Host host(root / L"session.ini");
    auto& app = host.app;
    app.OpenPaths({file.wstring()});
    LoadedPath(app, file);
    Until([&] { return !app.watchSetupPending && app.watches.size() == 1 && app.watches.front()->pending; },
        "Initial close-check watch was not armed");
    Check(fs::remove(file) && fs::remove(directory), "Cannot remove isolated close-check directory");
    Until([&] { return app.watchRetryPending; }, "Close check did not reach a pending retry");
    SendMessageW(app.hwnd, WM_CLOSE, 0, 0);
    Check(!IsWindow(app.hwnd) && app.closing && app.watches.empty() && !app.watchRetryPending && !app.watchSetupPending,
        "Window close retained a watch, setup, or retry");
    Pump();
    std::cout << "PASS window close cancels pending directory recovery\n";
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR graphics = 0;
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize native controls");
        PruneOldRuns(fs::absolute(fs::path(__FILE__)).parent_path().parent_path() / L"artifacts" / L"async-regression");
        const auto root = fs::absolute(fs::path(__FILE__)).parent_path().parent_path()
            / L"artifacts" / L"async-regression"
            / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::wcout << L"Regression artifacts: " << root.wstring() << std::endl;
        if (argc > 1 && std::string(argv[1]) == "--encoding-reentry") EncodingReentryChecks(root);
        else {
            WatchRecreationCheck(root / L"standalone", false);
            WatchRecreationCheck(root / L"registered", true);
            WatchCloseCheck(root / L"close");
            Write(root / L"result.txt", "PASS: standalone and registered parent recovery, subsequent saves, no idle retry, tab/registration/window close cancellation.\n");
        }
    } catch (const std::exception& error) {
        std::cerr << "async_probe FAILED: " << error.what() << std::endl;
        result = 1;
    }
    Gdiplus::GdiplusShutdown(graphics);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
