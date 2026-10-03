#pragma once

#include <atomic>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm {

struct FolderNode {
    std::wstring name;
    std::wstring path;
    bool directory = false;
    // A list keeps every node at a fixed address while siblings are inserted,
    // removed or spliced away, so native tree rows can point at nodes directly.
    std::list<FolderNode> children;
    // Structural equality lets a rescan that found nothing new skip rebuilding
    // the native tree. A case-only rename compares unequal, as it should.
    bool operator==(const FolderNode&) const = default;
};

// Sibling order produced by ScanFolder: directories first, then names ordered
// case-insensitively (ordinal), ties broken by the exact name.
bool FolderNodeBefore(const FolderNode& left, const FolderNode& right);

// Optional bounded history check performed during the existing worker scan.
// Only tracked folders proven absent from fully enumerated locations are
// reported. Skipped/inaccessible locations retain their history. A failed or
// cancelled scan leaves complete false and cannot authorize history removal.
struct FolderScanCoverage {
    std::vector<std::wstring> trackedFolders;
    std::vector<std::wstring> missingFolders;
    bool complete = false;
};

// Runs synchronously on the caller's worker. Cancellation throws SCAN_CANCELLED.
// Descendant reparse points and inaccessible branches are skipped. Excluded
// folders and their descendants are skipped before filesystem access.
FolderNode ScanFolder(const std::wstring& root, const std::atomic_bool* cancel = nullptr,
    const std::vector<std::wstring>& excludedFolders = {}, FolderScanCoverage* coverage = nullptr);

struct Document {
    std::wstring path;
    std::string source;
    std::string html; // Reserved; native rendering consumes source directly.
    std::string encoding;
    std::string hash;
    std::unordered_map<std::string, std::string> references; // Reserved for alternate renderers.
};

inline constexpr unsigned long MaximumDocumentBytes = 10 * 1024 * 1024;

// Invalid UTF-8 without a recognized BOM throws runtime_error("ENCODING_REQUIRED").
// Cancellation throws READ_CANCELLED and interrupts retries, read chunks, and
// UTF-8 validation. x64 uses baseline SSE2 for ASCII spans; unsupported targets
// scan ASCII scalarly. Mixed text retains Windows' strict UTF-8 validation.
Document ReadDocument(const std::wstring& path, bool cp932 = false, const std::atomic_bool* cancel = nullptr);

std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
std::wstring NormalizePath(const std::wstring& path);
bool IsMarkdown(const std::wstring& path);
// Returns a local absolute path, or empty for unsupported/network references.
// Literal # separates the fragment; percent decoding is applied afterward.
std::wstring ResolveReference(const std::wstring& docpath, const std::string& target);
std::string Fragment(const std::string& target);

} // namespace mm
