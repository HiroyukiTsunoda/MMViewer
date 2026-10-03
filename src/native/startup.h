#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace mm {

struct RegisteredPath {
    std::wstring path;
    bool directory = false;
};

enum class PathPresence { Exists, Missing, WrongType, Unavailable };

struct PathCheck {
    std::wstring path;
    bool directory = false;
    PathPresence presence = PathPresence::Unavailable;
    unsigned long error = 0;
};

// Checks only the registered entries, in input order. Paths are normalized
// without filesystem enumeration; document contents and descendants are never
// read. Missing is reserved for a definitively absent path/drive. Other I/O
// failures remain Unavailable so callers do not remove an inaccessible entry.
// error is the Windows error, or zero when attributes were obtained, including
// WrongType. An invalid registration keeps its original path in the result.
// Cancellation is checked before/after each metadata query and throws
// std::runtime_error("STARTUP_CHECK_CANCELLED"). A query already in progress
// may finish before cancellation takes effect.
std::vector<PathCheck> CheckRegisteredPaths(
    const std::vector<RegisteredPath>& paths,
    const std::atomic_bool* cancel = nullptr);

} // namespace mm
