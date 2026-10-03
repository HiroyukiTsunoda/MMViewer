#include "startup.h"
#include "core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <stdexcept>

namespace mm {
namespace {

void CheckCancellation(const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("STARTUP_CHECK_CANCELLED");
}

std::wstring NativePath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

PathPresence PresenceForError(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return PathPresence::Missing;
    default:
        return PathPresence::Unavailable;
    }
}

} // namespace

std::vector<PathCheck> CheckRegisteredPaths(
    const std::vector<RegisteredPath>& paths,
    const std::atomic_bool* cancel) {
    CheckCancellation(cancel);
    std::vector<PathCheck> results;
    results.reserve(paths.size());
    for (const auto& registered : paths) {
        CheckCancellation(cancel);
        PathCheck result{registered.path, registered.directory, PathPresence::Unavailable, ERROR_SUCCESS};
        try {
            result.path = NormalizePath(registered.path);
        } catch (const std::invalid_argument&) {
            result.error = ERROR_INVALID_NAME;
        } catch (const std::filesystem::filesystem_error& error) {
            result.error = static_cast<unsigned long>(error.code().value());
            if (!result.error) result.error = ERROR_INVALID_NAME;
        }
        CheckCancellation(cancel);
        if (!result.error) {
            const auto native = NativePath(result.path);
            CheckCancellation(cancel);
            const DWORD attributes = GetFileAttributesW(native.c_str());
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            CheckCancellation(cancel);
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                result.error = error;
                result.presence = PresenceForError(error);
            } else {
                const bool actualDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                result.presence = actualDirectory == registered.directory ? PathPresence::Exists : PathPresence::WrongType;
            }
        }
        results.push_back(std::move(result));
    }
    CheckCancellation(cancel);
    return results;
}

} // namespace mm
