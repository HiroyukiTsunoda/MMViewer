// Exercise scan-driven expansion history without the user's session or files.
#define main IncludedHostTestsMain
#include "host_tests.cpp"
#undef main
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")

namespace {

FolderRegistration& AddExpansionRoot(App& app, const fs::path& path) {
    auto folder = std::make_unique<FolderRegistration>();
    folder->id = app.nextFolderId++;
    folder->path = mm::NormalizePath(path.wstring());
    folder->pending = {path.filename().wstring(), folder->path, true, {}};
    auto& result = *folder;
    app.folders.push_back(std::move(folder));
    app.expanded.insert(result.path);
    return result;
}

std::unique_ptr<Scanned> ExpansionScan(App& app, FolderRegistration& folder) {
    auto result = std::make_unique<Scanned>();
    result->folderId = folder.id;
    result->generation = ++folder.generation;
    for (const auto& path : app.expanded)
        if (app.OwnsExpansion(folder, path)) result->coverage.trackedFolders.push_back(path);
    try {
        result->tree = std::make_unique<mm::FolderNode>(mm::ScanFolder(folder.path, nullptr,
            {app.excludedFolders.begin(), app.excludedFolders.end()}, &result->coverage));
    } catch (const std::exception& error) { result->error = mm::Wide(error.what()); }
    return result;
}

void ExpansionChurn(const fs::path& base) {
    Host host(base / L"churn.ini");
    auto& app = host.app;
    auto& folder = AddExpansionRoot(app, base / L"churn");
    fs::path previous;
    for (int iteration = 0; iteration < 1500; ++iteration) {
        if (!previous.empty()) fs::remove_all(previous);
        const auto current = fs::path(folder.path) / (L"renamed-" + std::to_wstring(iteration));
        Write(current / L"file.md", "# Churn\n");
        app.AcceptTree(ExpansionScan(app, folder));
        const auto item = TreeItem(app, current);
        Check(item != nullptr, "Churn scan did not display current folder");
        TreeView_Expand(app.treeH, item, TVE_EXPAND);
        Check(TreeView_GetCount(app.treeH) == 3 && app.expanded.size() == 2,
            "Expansion history grew while only one child folder exists");
        previous = current;
    }
    app.Save();
    Check(GetPrivateProfileIntW(L"App", L"Expanded", 0, app.ini.c_str()) == 2,
        "Obsolete expansion history was written back to the session");
    std::cout << "PASS 1500 real folder replacements retain only 2 expansion entries\n";
}

void ExpansionScopes(const fs::path& base) {
    Host host(base / L"scopes.ini");
    auto& app = host.app;
    const auto root = base / L"scope";
    Write(root / L"keep" / L"file.md", "# Keep\n");
    Write(root / L"gone" / L"file.md", "# Gone\n");
    fs::create_directories(root / L"empty");
    auto& folder = AddExpansionRoot(app, root);
    app.expanded.insert((root / L"keep").wstring());
    app.expanded.insert((root / L"gone").wstring());
    app.expanded.insert((root / L"empty").wstring());
    auto caseVariant = (root / L"keep").wstring();
    CharUpperBuffW(caseVariant.data(), static_cast<DWORD>(caseVariant.size()));
    const auto beforeCaseInsert = app.expanded.size();
    app.expanded.insert(caseVariant);
    Check(app.expanded.size() == beforeCaseInsert, "Case variants duplicated expansion history");
    auto& neighbor = AddExpansionRoot(app, base / L"scope-other");
    auto& nested = AddExpansionRoot(app, root / L"separate");
    const auto nestedOpen = fs::path(nested.path) / L"temporarily-unavailable";
    app.expanded.insert(nestedOpen.wstring());
    app.AcceptTree(ExpansionScan(app, folder));
    fs::remove_all(root / L"gone");
    // Exercise the actual worker path as well as direct accepted scan results.
    app.RefreshFolder(folder);
    Until([&] { return !folder.scanning; }, "Async expansion history scan did not finish");
    Check(!app.expanded.contains((root / L"gone").wstring()), "Deleted folder expansion remained");
    Check(app.expanded.contains(caseVariant) && app.expanded.contains((root / L"empty").wstring()),
        "Existing or empty folder expansion was discarded");
    Check(app.expanded.contains(neighbor.path) && app.expanded.contains(nested.path)
        && app.expanded.contains(nestedOpen.wstring()), "Another registered root lost its history");

    const auto omitted = root / L"omitted";
    const auto omittedChild = omitted / L"child";
    const auto similarlyNamed = root / L"omitted-other";
    app.excludedFolders.insert(omitted.wstring());
    app.expanded.insert(omittedChild.wstring());
    app.expanded.insert(similarlyNamed.wstring());
    app.AcceptTree(ExpansionScan(app, folder));
    Check(app.expanded.contains(omittedChild.wstring()) && !app.expanded.contains(similarlyNamed.wstring()),
        "Excluded folder protection crossed a path boundary or lost skipped history");
    std::cout << "PASS case-insensitive history, empty folders, exclusions and separate root ownership\n";
}

void ExpansionFailures(const fs::path& base) {
    Host host(base / L"failures.ini");
    auto& app = host.app;
    auto& folder = AddExpansionRoot(app, base / L"unavailable");
    const auto tracked = (fs::path(folder.path) / L"remember").wstring();
    app.expanded.insert(tracked);
    auto failed = ExpansionScan(app, folder);
    Check(!failed->error.empty() && !failed->coverage.complete, "Missing root unexpectedly completed its scan");
    app.AcceptTree(std::move(failed));
    Check(app.expanded.contains(tracked), "Failed root scan removed expansion history");

    mm::FolderScanCoverage cancelled{{tracked}, {tracked}, true};
    std::atomic_bool stop = true;
    bool threw = false;
    try { (void)mm::ScanFolder(folder.path, &stop, {}, &cancelled); }
    catch (const std::exception&) { threw = true; }
    Check(threw && !cancelled.complete && cancelled.missingFolders.empty(),
        "Cancelled scan retained a previous successful coverage result");

    auto rejected = [&] {
        auto result = std::make_unique<Scanned>();
        result->folderId = folder.id;
        result->generation = folder.generation;
        result->tree = std::make_unique<mm::FolderNode>(folder.pending);
        result->coverage.complete = true;
        result->coverage.missingFolders = {tracked};
        return result;
    };
    auto stale = rejected(); ++folder.generation;
    app.AcceptTree(std::move(stale));
    Check(app.expanded.contains(tracked), "Stale scan removed expansion history");
    auto errorResult = rejected(); errorResult->error = L"read failed";
    app.AcceptTree(std::move(errorResult));
    Check(app.expanded.contains(tracked), "Failed partial result removed expansion history");
    auto incomplete = rejected(); incomplete->coverage.complete = false;
    app.AcceptTree(std::move(incomplete));
    Check(app.expanded.contains(tracked), "Incomplete scan removed expansion history");
    std::cout << "PASS failed, cancelled, stale and incomplete scans preserve expansion history\n";
}

struct DenyDirectoryEnumeration {
    std::wstring path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL originalAcl = nullptr;
    bool protectedAcl = false, changed = false;
    explicit DenyDirectoryEnumeration(const fs::path& directory) : path(directory.wstring()) {
        Check(GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &originalAcl, nullptr, &original) == ERROR_SUCCESS, "Cannot read test folder ACL");
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        Check(GetSecurityDescriptorControl(original, &control, &revision) != FALSE, "Cannot read test folder ACL control");
        protectedAcl = (control & SE_DACL_PROTECTED) != 0;
        BYTE sid[SECURITY_MAX_SID_SIZE]{}; DWORD sidSize = sizeof(sid);
        Check(CreateWellKnownSid(WinWorldSid, nullptr, sid, &sidSize) != FALSE, "Cannot create test Everyone SID");
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        PACL restricted = nullptr;
        Check(SetEntriesInAclW(1, &deny, originalAcl, &restricted) == ERROR_SUCCESS, "Cannot construct test folder ACL");
        const auto error = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, restricted, nullptr);
        LocalFree(restricted);
        Check(error == ERROR_SUCCESS, "Cannot deny enumeration on isolated test folder");
        changed = true;
    }
    ~DenyDirectoryEnumeration() {
        if (changed) SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | (protectedAcl ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, originalAcl, nullptr);
        if (original) LocalFree(original);
    }
};

void ExpansionInaccessible(const fs::path& base) {
    Host host(base / L"inaccessible.ini");
    auto& app = host.app;
    const auto root = base / L"inaccessible";
    const auto blocked = root / L"blocked";
    const auto child = blocked / L"child";
    const auto missingSibling = root / L"blocked-other";
    Write(child / L"file.md", "# Blocked\n");
    Write(root / L"keep.md", "# Kept\n");
    auto& folder = AddExpansionRoot(app, root);
    app.expanded.insert(blocked.wstring());
    app.expanded.insert(child.wstring());
    app.expanded.insert(missingSibling.wstring());
    {
        DenyDirectoryEnumeration deny(blocked);
        WIN32_FIND_DATAW entry{};
        const auto finder = FindFirstFileW((blocked / L"*").c_str(), &entry);
        if (finder != INVALID_HANDLE_VALUE) FindClose(finder);
        Check(finder == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
            "Isolated fixture did not deny directory enumeration");
        app.AcceptTree(ExpansionScan(app, folder));
        Check(app.expanded.contains(blocked.wstring()) && app.expanded.contains(child.wstring()),
            "Inaccessible folder history was pruned");
        Check(!app.expanded.contains(missingSibling.wstring()),
            "Inaccessible folder protected unrelated missing sibling history");
    }
    Check(fs::exists(child / L"file.md"), "Test folder ACL was not restored");
    fs::remove_all(child);
    app.AcceptTree(ExpansionScan(app, folder));
    Check(app.expanded.contains(blocked.wstring()) && !app.expanded.contains(child.wstring()),
        "Recovered folder did not prune a subsequently confirmed deletion");
    std::cout << "PASS access-denied subtree retention and pruning after access recovery\n";
}

void ExpansionReparse(const fs::path& base) {
    const auto root = base / L"reparse";
    const auto link = root / L"link";
    Write(root / L"keep.md", "# Kept\n");
    if (!CreateSymbolicLinkW(link.c_str(), root.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        std::cout << "SKIP reparse fixture: symlink creation unavailable (" << GetLastError() << ")\n";
        return;
    }
    struct RemoveLink {
        fs::path path;
        ~RemoveLink() { RemoveDirectoryW(path.c_str()); }
    } remove{link};
    const auto tracked = (link / L"unvisited-child").wstring();
    const auto missing = (root / L"link-other").wstring();
    mm::FolderScanCoverage coverage{{tracked, missing}};
    const auto tree = mm::ScanFolder(root.wstring(), nullptr, {}, &coverage);
    Check(tree.children.size() == 1 && coverage.complete && coverage.missingFolders.size() == 1
        && coverage.missingFolders.front() == missing, "Reparse exclusion lost tracked history or protected a sibling");
    std::cout << "PASS skipped reparse subtree retains history without protecting missing siblings\n";
}

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR graphics = 0;
    if (Gdiplus::GdiplusStartup(&graphics, &startup, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize expansion test controls");
        const auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        PruneOldRuns(sourceRoot / L"artifacts" / L"expansion-tests");
        const auto artifacts = sourceRoot / L"artifacts" / L"expansion-tests"
            / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(artifacts);
        ExpansionScopes(artifacts);
        ExpansionFailures(artifacts);
        ExpansionInaccessible(artifacts);
        ExpansionReparse(artifacts);
        ExpansionChurn(artifacts);
        Write(artifacts / L"result.txt", "PASS: expansion history remains bounded and preserves unverified locations.\n");
        std::wcout << L"Expansion test artifacts: " << artifacts.wstring() << L"\n";
    } catch (const std::exception& error) {
        std::cerr << "expansion_tests FAILED: " << error.what() << '\n'; result = 1;
    }
    Gdiplus::GdiplusShutdown(graphics);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
