#include "core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define MM_HAS_SSE2 1
#endif

#pragma comment(lib, "bcrypt.lib")

namespace mm {
namespace {

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct FindHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};

std::wstring NativePath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::runtime_error IoError(const char* operation, DWORD code = GetLastError()) {
    return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(code) + ")");
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return value;
}

// The tree lists ".md" files. A name that is only the suffix has no extension,
// as std::filesystem::path sees it, so it is not listed either.
bool IsTreeMarkdown(std::wstring_view name) {
    return name.size() > 3 && _wcsnicmp(name.data() + name.size() - 3, L".md", 3) == 0;
}

// Child paths are built by appending, which matches std::filesystem::path's
// operator/ for the normalized paths scans use, without its allocations.
std::wstring JoinPath(const std::wstring& folder, std::wstring_view name) {
    std::wstring result;
    result.reserve(folder.size() + 1 + name.size());
    result += folder;
    if (result.empty() || (result.back() != L'\\' && result.back() != L'/')) result += L'\\';
    result.append(name);
    return result;
}

void CheckCancellation(const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("SCAN_CANCELLED");
}

void CheckReadCancellation(const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("READ_CANCELLED");
}

bool IsWithinFolder(const std::wstring& path, const std::wstring& folder) {
    return !folder.empty() && path.size() >= folder.size()
        && _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0
        && (path.size() == folder.size() || folder.back() == L'\\' || path[folder.size()] == L'\\');
}

bool IsExcludedFolder(const std::wstring& path, const std::vector<std::wstring>& excludedFolders) {
    return std::any_of(excludedFolders.begin(), excludedFolders.end(), [&](const auto& excluded) {
        return IsWithinFolder(path, excluded);
    });
}

FolderNode EmptyFolder(const std::wstring& path) {
    auto name = std::filesystem::path(path).filename().wstring();
    return {name.empty() ? path : name, path, true, {}};
}

struct ScanFrame {
    FolderNode node;
    std::vector<std::wstring> folders;
    size_t next = 0;
};

class ScanHistory {
    struct PathLess {
        bool operator()(const std::wstring& left, const std::wstring& right) const {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        }
    };
    std::set<std::wstring, PathLess> missing;
    FolderScanCoverage* coverage;
public:
    ScanHistory(const std::wstring& root, const std::vector<std::wstring>& excluded, FolderScanCoverage* output)
        : coverage(output) {
        if (!coverage) return;
        for (const auto& tracked : coverage->trackedFolders) {
            if (tracked.empty()) continue;
            const auto path = NormalizePath(tracked);
            if (IsWithinFolder(path, root) && !IsExcludedFolder(path, excluded)) missing.insert(path);
        }
    }
    void Exists(const std::wstring& path) { missing.erase(path); }
    void Uncertain(const std::wstring& path) {
        if (missing.empty()) return;
        // The ordered lookup bounds work to tracked paths in this location;
        // it does not retain every directory visited in a large repository.
        missing.erase(path);
        const auto prefix = path.back() == L'\\' ? path : path + L'\\';
        auto first = missing.lower_bound(prefix), last = first;
        while (last != missing.end() && IsWithinFolder(*last, path)) ++last;
        missing.erase(first, last);
    }
    void Complete() {
        if (!coverage) return;
        coverage->missingFolders.assign(missing.begin(), missing.end());
        coverage->complete = true;
    }
};

ScanFrame EnumerateFolder(const std::wstring& path, const std::atomic_bool* cancel, bool isRoot, ScanHistory& history) {
    CheckCancellation(cancel);
    ScanFrame result{EmptyFolder(path), {}, 0};
    // A subfolder was listed by its parent as a directory that is not a reparse
    // point moments ago, so its own enumeration is the only call it needs. If it
    // is gone or unreadable by now, that call fails with anything but "no such
    // file" and the coverage of tracked folders inside stays uncertain, as it
    // did when a separate attribute check failed.
    history.Exists(path);
    WIN32_FIND_DATAW data{};
    FindHandle finder;
    finder.value = FindFirstFileExW(NativePath(JoinPath(path, L"*")).c_str(), FindExInfoBasic,
        &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    CheckCancellation(cancel);
    if (finder.value == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (isRoot && code != ERROR_FILE_NOT_FOUND) throw IoError("Cannot read folder", code);
        if (code != ERROR_FILE_NOT_FOUND) history.Uncertain(path);
        return result;
    }
    do {
        CheckCancellation(cancel);
        const std::wstring_view child = data.cFileName;
        if (child == L"." || child == L"..") continue;
        auto childPath = JoinPath(path, child);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { history.Uncertain(childPath); continue; }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) result.folders.push_back(std::move(childPath));
        else if (IsTreeMarkdown(child)) result.node.children.push_back({std::wstring(child), std::move(childPath), false, {}});
    } while (FindNextFileW(finder.value, &data));
    const auto enumerationError = GetLastError();
    CheckCancellation(cancel);
    if (enumerationError != ERROR_NO_MORE_FILES) history.Uncertain(path);
    return result;
}

std::string ReadBytes(const std::wstring& path, const std::atomic_bool* cancel) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        CheckReadCancellation(cancel);
        try {
            FileHandle file;
            file.value = CreateFileW(NativePath(path).c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            CheckReadCancellation(cancel);
            if (file.value == INVALID_HANDLE_VALUE) throw IoError("Cannot open document");
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.value, &size)) throw IoError("Cannot read document size");
            CheckReadCancellation(cancel);
            if (size.QuadPart > MaximumDocumentBytes) throw std::length_error("DOCUMENT_TOO_LARGE: maximum 10 MiB");
            // Read directly into the final allocation. The usual stable-file
            // path no longer copies every 64 KiB from a stack buffer.
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            size_t used = 0;
            for (;;) {
                CheckReadCancellation(cancel);
                DWORD count = 0;
                if (used == bytes.size()) {
                    // Probe EOF before allocating more space. Files may grow
                    // while open because editors are allowed to keep writing.
                    char next = 0;
                    if (!ReadFile(file.value, &next, 1, &count, nullptr)) throw IoError("Cannot read document");
                    CheckReadCancellation(cancel);
                    if (!count) break;
                    if (used == MaximumDocumentBytes) throw std::length_error("DOCUMENT_TOO_LARGE: maximum 10 MiB");
                    bytes.resize(std::min(static_cast<size_t>(MaximumDocumentBytes), used + 65536));
                    bytes[used++] = next;
                }
                const auto remaining = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - used));
                if (!remaining) continue;
                if (!ReadFile(file.value, bytes.data() + used, remaining, &count, nullptr)) throw IoError("Cannot read document");
                CheckReadCancellation(cancel);
                if (!count) break;
                used += count;
            }
            bytes.resize(used); // Also handles files truncated during the read.
            return bytes;
        } catch (const std::length_error&) {
            CheckReadCancellation(cancel);
            throw;
        } catch (const std::runtime_error&) {
            CheckReadCancellation(cancel);
            if (attempt == 2) throw;
            for (int slice = 0; slice < 10 * (attempt + 1); ++slice) {
                CheckReadCancellation(cancel);
                Sleep(10);
            }
        }
    }
    throw std::runtime_error("Cannot read document");
}

bool IsValidUtf8(std::string_view bytes, const std::atomic_bool* cancel) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t offset = 0;
    while (offset < bytes.size()) {
        CheckReadCancellation(cancel);
        size_t batchEnd = std::min(bytes.size(), offset + 65536);
        if (batchEnd < bytes.size()) {
            // Keep complete codepoints together for native validation. A
            // fourth consecutive continuation byte can never be valid UTF-8.
            unsigned continuationCount = 0;
            while ((data[batchEnd] & 0xc0) == 0x80) {
                if (++continuationCount > 3) return false;
                --batchEnd;
            }
        }
#if defined(MM_HAS_SSE2)
        // SSE2 is a baseline feature on x64. Never read beyond the current
        // batch, including at an allocation's final byte.
        while (batchEnd - offset >= 16) {
            const auto block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset));
            const auto nonAscii = static_cast<unsigned>(_mm_movemask_epi8(block));
            if (nonAscii) {
                offset += std::countr_zero(nonAscii);
                break;
            }
            offset += 16;
        }
#endif
        while (offset < batchEnd && data[offset] < 0x80) ++offset;
        // Preserve Windows' optimized strict validation for mixed/Japanese
        // text instead of replacing its multibyte path with a scalar decoder.
        if (offset < batchEnd && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data() + offset, static_cast<int>(batchEnd - offset), nullptr, 0)) return false;
        offset = batchEnd;
    }
    return true;
}

std::string DecodeBytes(std::string bytes, bool cp932, std::string& encoding, const std::atomic_bool* cancel) {
    CheckReadCancellation(cancel);
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 2 && ((byte(0) == 0xff && byte(1) == 0xfe) || (byte(0) == 0xfe && byte(1) == 0xff))) {
        if (bytes.size() % 2) throw std::runtime_error("INVALID_UTF16: odd byte count");
        const bool little = byte(0) == 0xff;
        std::wstring text;
        text.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i < bytes.size(); i += 2) {
            if ((i & 65535) == 2) CheckReadCancellation(cancel);
            text.push_back(static_cast<wchar_t>(little ? (byte(i) | (byte(i + 1) << 8)) : ((byte(i) << 8) | byte(i + 1))));
        }
        encoding = little ? "UTF-16 LE" : "UTF-16 BE";
        CheckReadCancellation(cancel);
        std::string().swap(bytes); // Release raw input before allocating UTF-8 output.
        return Utf8(text); // WC_ERR_INVALID_CHARS rejects unpaired surrogates.
    }
    if (bytes.size() >= 3 && byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf) {
        if (!IsValidUtf8(std::string_view(bytes).substr(3), cancel))
            throw std::runtime_error("ENCODING_REQUIRED");
        bytes.erase(0, 3);
        encoding = "UTF-8 BOM";
        return bytes;
    }
    if (cp932) {
        encoding = "CP932";
        if (bytes.empty()) return {};
        const int length = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        CheckReadCancellation(cancel);
        if (!length) throw std::runtime_error("INVALID_CP932");
        std::wstring text(static_cast<size_t>(length), L'\0');
        CheckReadCancellation(cancel);
        MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
        CheckReadCancellation(cancel);
        std::string().swap(bytes);
        return Utf8(text);
    }
    if (!IsValidUtf8(bytes, cancel))
        throw std::runtime_error("ENCODING_REQUIRED");
    encoding = "UTF-8";
    return bytes;
}

struct HashProvider {
    BCRYPT_ALG_HANDLE handle = nullptr;
    HashProvider() { if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) handle = nullptr; }
    ~HashProvider() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    HashProvider(const HashProvider&) = delete;
    HashProvider& operator=(const HashProvider&) = delete;
};

std::string Sha256(const std::string& bytes) {
    // Opening the provider dominates a one-shot hash, so keep one for the
    // process. Algorithm handles may be shared between threads; BCryptHash
    // creates its own hash object per call. A failed first open retries per call.
    static const HashProvider shared;
    std::unique_ptr<HashProvider> retry;
    BCRYPT_ALG_HANDLE algorithm = shared.handle;
    if (!algorithm) { retry = std::make_unique<HashProvider>(); algorithm = retry->handle; }
    if (!algorithm) throw std::runtime_error("Cannot initialize document hash");
    unsigned char digest[32]{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    if (status < 0) throw std::runtime_error("Cannot hash document");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}

std::string PercentDecode(std::string_view input) {
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size() && hex(input[i + 1]) >= 0 && hex(input[i + 2]) >= 0) {
            result += static_cast<char>((hex(input[i + 1]) << 4) | hex(input[i + 2])); i += 2;
        } else result += input[i];
    }
    return result;
}

} // namespace

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::length_error("Text is too long");
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("INVALID_UTF16");
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::length_error("Text is too long");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("INVALID_UTF8");
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) throw std::invalid_argument("Invalid path");
    auto normalized = std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().wstring();
    if (normalized.rfind(L"\\\\?\\UNC\\", 0) == 0) normalized = L"\\\\" + normalized.substr(8);
    else if (normalized.rfind(L"\\\\?\\", 0) == 0) normalized.erase(0, 4);
    while (normalized.size() > 3 && normalized.back() == L'\\') normalized.pop_back();
    return normalized;
}

bool IsMarkdown(const std::wstring& path) {
    const auto extension = Lower(std::filesystem::path(path).extension().wstring());
    return extension == L".md" || extension == L".markdown";
}

bool FolderNodeBefore(const FolderNode& left, const FolderNode& right) {
    if (left.directory != right.directory) return left.directory;
    const int comparison = CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
    return comparison == CSTR_LESS_THAN || (comparison == CSTR_EQUAL && left.name < right.name);
}

FolderNode ScanFolder(const std::wstring& root, const std::atomic_bool* cancel, const std::vector<std::wstring>& excludedFolders,
    FolderScanCoverage* coverage) {
    if (coverage) { coverage->complete = false; coverage->missingFolders.clear(); }
    CheckCancellation(cancel);
    const auto path = NormalizePath(root);
    std::vector<std::wstring> excluded;
    excluded.reserve(excludedFolders.size());
    for (const auto& folder : excludedFolders) {
        CheckCancellation(cancel);
        if (!folder.empty()) excluded.push_back(NormalizePath(folder));
    }
    ScanHistory history(path, excluded, coverage);
    if (IsExcludedFolder(path, excluded)) { history.Complete(); return EmptyFolder(path); }
    CheckCancellation(cancel);
    const auto attributes = GetFileAttributesW(NativePath(path).c_str());
    CheckCancellation(cancel);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Folder does not exist");
    std::vector<ScanFrame> stack;
    stack.push_back(EnumerateFolder(path, cancel, true, history));
    for (;;) {
        CheckCancellation(cancel);
        auto& current = stack.back();
        if (current.next < current.folders.size()) {
            auto child = current.folders[current.next++];
            if (IsExcludedFolder(child, excluded)) continue;
            stack.push_back(EnumerateFolder(child, cancel, false, history));
            continue;
        }
        current.node.children.sort(FolderNodeBefore);
        FolderNode completed = std::move(current.node);
        stack.pop_back();
        if (stack.empty()) { history.Complete(); return completed; }
        if (!completed.children.empty()) stack.back().node.children.push_back(std::move(completed));
    }
}

Document ReadDocument(const std::wstring& path, bool cp932, const std::atomic_bool* cancel) {
    CheckReadCancellation(cancel);
    Document document;
    document.path = NormalizePath(path);
    auto bytes = ReadBytes(document.path, cancel);
    CheckReadCancellation(cancel);
    document.hash = Sha256(bytes);
    CheckReadCancellation(cancel);
    document.source = DecodeBytes(std::move(bytes), cp932, document.encoding, cancel);
    CheckReadCancellation(cancel);
    return document;
}

std::wstring ResolveReference(const std::wstring& docpath, const std::string& target) {
    try {
        const auto hash = target.find('#');
        const auto query = target.find('?');
        const auto raw = target.substr(0, std::min(hash, query));
        if (raw.empty()) return NormalizePath(docpath);
        const auto decoded = PercentDecode(raw);
        if (decoded.find('\0') != std::string::npos) return {};
        auto path = Wide(decoded);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (path.rfind(L"\\\\", 0) == 0) return {}; // UNC, protocol-relative and device paths.
        const auto colon = path.find(L':');
        const bool drive = colon == 1 && std::iswalpha(path[0]) && path.size() > 2 && path[2] == L'\\';
        if (colon != std::wstring::npos && !drive) return {};
        if (drive && path.find(L':', 2) != std::wstring::npos) return {}; // Alternate data streams.
        const auto base = std::filesystem::path(NormalizePath(docpath)).parent_path();
        auto resolved = NormalizePath((base / std::filesystem::path(path)).wstring());
        if (resolved.rfind(L"\\\\", 0) == 0) return {};
        return resolved;
    } catch (const std::exception&) { return {}; }
}

std::string Fragment(const std::string& target) {
    const auto hash = target.find('#');
    if (hash == std::string::npos) return {};
    return PercentDecode(std::string_view(target).substr(hash + 1));
}

} // namespace mm
