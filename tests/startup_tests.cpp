#include "startup.h"
#include "core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring NativePath(const fs::path& path) {
    auto value = path.native();
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

struct TemporaryFolder {
    fs::path path;
    fs::path base;
    TemporaryFolder() {
        wchar_t buffer[MAX_PATH + 1]{};
        DWORD count = GetTempPathW(MAX_PATH, buffer);
        Require(count > 0 && count < MAX_PATH, "GetTempPath failed");
        base = mm::NormalizePath(buffer);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path = base / (L"MMviewer-Startup-Tests-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(NativePath(path).c_str(), nullptr)) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("Cannot create startup test folder");
        }
        throw std::runtime_error("Cannot create unique startup test folder");
    }
    ~TemporaryFolder() {
        // Remove only the unique directory created by this object, after
        // verifying its resolved parent and fixed filename prefix.
        const auto absolute = fs::absolute(path).lexically_normal();
        if (absolute.parent_path() == base && absolute.filename().native().rfind(L"MMviewer-Startup-Tests-", 0) == 0) {
            std::error_code error;
            fs::remove_all(fs::path(NativePath(absolute)), error);
        }
    }
};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE handle) : value(handle) {}
};

void CreateFileFixture(const fs::path& path) {
    Handle file(CreateFileW(NativePath(path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(file.value != INVALID_HANDLE_VALUE, "Cannot create startup fixture");
    const char text[] = "# startup fixture\n";
    DWORD written = 0;
    Require(WriteFile(file.value, text, sizeof(text) - 1, &written, nullptr) && written == sizeof(text) - 1,
        "Cannot write startup fixture");
}

void ExistingMissingAndWrongType(const fs::path& root) {
    const auto directory = root / L"登録済み フォルダー";
    Require(CreateDirectoryW(NativePath(directory).c_str(), nullptr) != 0, "Cannot create Japanese directory");
    const auto file = directory / L"日本語 (読書) #1.md";
    CreateFileFixture(file);
    const auto missingFile = directory / L"削除された.md";
    const auto missingDirectory = root / L"ないフォルダー" / L"下の階層";
    const auto normalizedFile = directory / L"unused" / L".." / file.filename();
    const auto checks = mm::CheckRegisteredPaths({
        {root.native(), true},
        {directory.native(), true},
        {normalizedFile.native(), false},
        {missingFile.native(), false},
        {missingDirectory.native(), true},
        {file.native(), true},
        {directory.native(), false},
        {NativePath(file), false}
    });
    Require(checks.size() == 8, "Results must preserve the input count and order");
    for (size_t i : {size_t(0), size_t(1), size_t(2), size_t(7)})
        Require(checks[i].presence == mm::PathPresence::Exists && checks[i].error == 0, "Existing entry not detected");
    Require(checks[2].path == mm::NormalizePath(file.native()), "Lexical path normalization failed");
    Require(checks[2].path == checks[7].path, "Extended Unicode path normalization failed");
    Require(checks[0].directory && !checks[2].directory, "Expected path types were not retained");
    Require(checks[3].presence == mm::PathPresence::Missing && checks[3].error == ERROR_FILE_NOT_FOUND,
        "Missing file was not classified definitively");
    Require(checks[4].presence == mm::PathPresence::Missing && checks[4].error == ERROR_PATH_NOT_FOUND,
        "Missing parent folder was not classified definitively");
    Require(checks[5].presence == mm::PathPresence::WrongType && checks[5].error == 0,
        "File registered as folder was not detected");
    Require(checks[6].presence == mm::PathPresence::WrongType && checks[6].error == 0,
        "Folder registered as file was not detected");
    std::cout << "PASS existing root/file, Japanese paths, normalization, missing entries, wrong types\n";
}

void UnavailableAndCancellation(const fs::path& root) {
    auto withNull = root.native(); withNull += L'\0'; withNull += L"ignored.md";
    const auto malformed = root / L"not?valid.md";
    const auto checks = mm::CheckRegisteredPaths({
        {L"", true}, {withNull, false}, {malformed.native(), false}, {root.native(), true}
    });
    for (size_t i = 0; i < 3; ++i)
        Require(checks[i].presence == mm::PathPresence::Unavailable && checks[i].error == ERROR_INVALID_NAME,
            "Invalid/unqueryable entry should remain unavailable, not missing");
    Require(checks[3].presence == mm::PathPresence::Exists, "Bad registration must not stop later checks");
    std::atomic_bool cancelled = true;
    bool threw = false;
    try { (void)mm::CheckRegisteredPaths({{(root / L"does-not-exist" / L"never-read.md").native(), false}}, &cancelled); }
    catch (const std::runtime_error& error) { threw = std::string(error.what()) == "STARTUP_CHECK_CANCELLED"; }
    Require(threw, "Pre-cancelled startup check did not throw its cancellation marker");
    threw = false;
    try { (void)mm::CheckRegisteredPaths({}, &cancelled); }
    catch (const std::runtime_error& error) { threw = std::string(error.what()) == "STARTUP_CHECK_CANCELLED"; }
    Require(threw, "Empty pre-cancelled batch should still honor cancellation");
    std::cout << "PASS unavailable registrations and cancellation before missing-path I/O\n";
}

void LockedFileAndLongPath(const fs::path& root) {
    const auto locked = root / L"exclusive.md";
    CreateFileFixture(locked);
    Handle lock(CreateFileW(NativePath(locked).c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(lock.value != INVALID_HANDLE_VALUE, "Cannot lock fixture exclusively");
    const auto checked = mm::CheckRegisteredPaths({{locked.native(), false}});
    Require(checked[0].presence == mm::PathPresence::Exists && checked[0].error == 0,
        "Existence metadata must not require opening locked document contents");

    auto directory = root;
    while (directory.native().size() < MAX_PATH + 40) {
        directory /= L"長い登録済みフォルダー名_0123456789";
        Require(CreateDirectoryW(NativePath(directory).c_str(), nullptr) != 0, "Cannot create long-path fixture directory");
    }
    const auto file = directory / L"長いパスの文書.md";
    CreateFileFixture(file);
    const auto longChecks = mm::CheckRegisteredPaths({{directory.native(), true}, {file.native(), false}});
    Require(longChecks[0].presence == mm::PathPresence::Exists && longChecks[1].presence == mm::PathPresence::Exists,
        "Long Unicode paths were not checked successfully");
    std::cout << "PASS exclusive content lock and paths longer than MAX_PATH\n";
}

} // namespace

int main() {
    try {
        TemporaryFolder temporary;
        ExistingMissingAndWrongType(temporary.path);
        UnavailableAndCancellation(temporary.path);
        LockedFileAndLongPath(temporary.path);
        std::cout << "All startup registration tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
