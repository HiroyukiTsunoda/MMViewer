#include "core.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cassert>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct TemporaryFolder {
    fs::path path;
    fs::path base;
    TemporaryFolder() {
        wchar_t temporary[MAX_PATH + 1]{};
        const auto length = GetTempPathW(MAX_PATH, temporary);
        if (!length || length >= MAX_PATH) throw std::runtime_error("GetTempPath failed");
        base = mm::NormalizePath(temporary);
        path = base / (L"MMviewer-Core-Tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!fs::create_directory(path)) throw std::runtime_error("Cannot create unique test folder");
    }
    ~TemporaryFolder() {
        // Only remove the exact directory created above, verified inside TEMP.
        const auto absolute = fs::absolute(path).lexically_normal();
        if (absolute.parent_path() == base && absolute.filename().wstring().rfind(L"MMviewer-Core-Tests-", 0) == 0) {
            std::error_code error;
            fs::remove_all(absolute, error);
        }
    }
};

void Write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) throw std::runtime_error("Cannot create fixture");
}

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void ExpectError(const std::function<void()>& action, const char* expected) {
    try { action(); }
    catch (const std::exception& error) {
        Check(std::string(error.what()).find(expected) != std::string::npos, "Unexpected error text");
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

void FolderFiltering(const fs::path& base) {
    const auto root = base / L"folder-filter";
    Write(root / L"z.MD", "# z");
    Write(root / L"a.md", "# a");
    Write(root / L"exclude.markdown", "# explicitly openable but absent from tree");
    Write(root / L"readme.md.bak", "not Markdown");
    Write(root / L".md", "a name that is only the suffix has no extension");
    Write(root / L"B-parent" / L"deep" / L"日本語.md", "# deep");
    Write(root / L"A-parent" / L"direct.md", "# direct");
    Write(root / L"irrelevant" / L"nested" / L"file.txt", "text");
    fs::create_directories(root / L"empty");
    const auto tree = mm::ScanFolder(root.wstring());
    const auto child = [](const mm::FolderNode& node, size_t index) -> const mm::FolderNode& {
        if (index >= node.children.size()) throw std::runtime_error("Child index out of range");
        return *std::next(node.children.begin(), static_cast<std::ptrdiff_t>(index));
    };
    Check(tree.directory && tree.children.size() == 4, "Folder filtering failed");
    Check(child(tree, 0).name == L"A-parent" && child(tree, 0).directory, "Directories should precede files and sort alphabetically");
    Check(child(tree, 1).name == L"B-parent", "Ancestor of nested Markdown was pruned");
    Check(child(child(child(tree, 1), 0), 0).name == L"日本語.md", "Nested Markdown missing");
    Check(child(tree, 2).name == L"a.md" && child(tree, 3).name == L"z.MD", "Markdown extension/sort failed");
    Check(mm::IsMarkdown(L"a.MARKDOWN") && mm::IsMarkdown(L"b.Md") && !mm::IsMarkdown(L"c.md.txt"), "File-open extension handling failed");
    std::atomic_bool cancelled = true;
    ExpectError([&] { (void)mm::ScanFolder(root.wstring(), &cancelled); }, "SCAN_CANCELLED");
    ExpectError([&] { (void)mm::ScanFolder((root / L"missing").wstring()); }, "Folder does not exist");
    std::cout << "PASS folder pruning, ancestors, extension case, sorting, cancellation\n";
}

void ExcludedFolderScanning(const fs::path& base) {
    const auto root = base / L"excluded-folders";
    const auto hidden = root / L"Hidden";
    Write(hidden / L"deep" / L"hidden.md", "# Hidden\n");
    Write(root / L"Hidden-other" / L"keep.md", "# Keep sibling\n");
    Write(root / L"parent" / L"only-child" / L"hidden.md", "# Hidden child\n");
    Write(root / L"keep.md", "# Keep root file\n");
    auto upperCase = hidden.wstring();
    CharUpperBuffW(upperCase.data(), static_cast<DWORD>(upperCase.size()));
    const auto tree = mm::ScanFolder(root.wstring(), nullptr,
        {upperCase + L"\\", (root / L"parent" / L"unused" / L".." / L"only-child").wstring()});
    Check(tree.children.size() == 2 && tree.children.front().name == L"Hidden-other"
        && tree.children.front().children.size() == 1 && tree.children.front().children.front().name == L"keep.md"
        && tree.children.back().name == L"keep.md", "Scan exclusions lost sibling or failed to prune empty ancestor");
    Check(fs::exists(hidden / L"deep" / L"hidden.md"), "Scan exclusion modified filesystem contents");
    const auto descendant = mm::ScanFolder((hidden / L"deep").wstring(), nullptr, {hidden.wstring()});
    Check(descendant.directory && descendant.children.empty(), "Exclusion did not cover a descendant scan root");

    // Unlike output-only filtering, exclusion before filesystem access also
    // succeeds for a path that does not exist. Normal scanning must reject it.
    const auto missing = root / L"missing" / L"nested";
    ExpectError([&] { (void)mm::ScanFolder(missing.wstring()); }, "Folder does not exist");
    const auto skipped = mm::ScanFolder(missing.wstring(), nullptr, {(root / L"missing").wstring()});
    Check(skipped.directory && skipped.path == mm::NormalizePath(missing.wstring()) && skipped.children.empty(),
        "Excluded root accessed the filesystem instead of returning an empty node");
    const auto missingSibling = root / L"missing-other";
    ExpectError([&] { (void)mm::ScanFolder(missingSibling.wstring(), nullptr, {(root / L"missing").wstring()}); },
        "Folder does not exist");
    std::atomic_bool cancelled = true;
    ExpectError([&] { (void)mm::ScanFolder(missing.wstring(), &cancelled, {root.wstring()}); }, "SCAN_CANCELLED");
    std::cout << "PASS scan exclusions before I/O, case/path boundaries, descendant roots, ancestor pruning\n";
}

void ReparsePoints(const fs::path& base) {
    const auto root = base / L"reparse";
    Write(root / L"kept.md", "ok");
    const auto link = root / L"loop";
    if (CreateSymbolicLinkW(link.c_str(), root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto tree = mm::ScanFolder(root.wstring());
        Check(tree.children.size() == 1, "Reparse loop was not excluded");
        RemoveDirectoryW(link.c_str());
        std::cout << "PASS reparse loop exclusion\n";
    } else {
        std::cout << "SKIP symlink creation unavailable (Windows error " << GetLastError() << ")\n";
    }
}

void DocumentEncoding(const fs::path& base) {
    const auto file = base / L"文字 # (1).md";
    const auto text = mm::Utf8(L"# 日本語\n本文 \U0001f600\n");
    Write(file, text);
    const auto utf8 = mm::ReadDocument(file.wstring());
    Check(utf8.source == text && utf8.encoding == "UTF-8", "UTF-8 decoding failed");
    Check(utf8.path == mm::NormalizePath(file.wstring()), "Document path was not normalized");
    Check(utf8.hash.size() == 64 && utf8.hash == mm::ReadDocument(file.wstring()).hash, "Stable SHA256 failed");
    Check(utf8.html.empty(), "Native document must not allocate redundant HTML");
    Write(file, std::string("\xef\xbb\xbf") + text);
    const auto bom = mm::ReadDocument(file.wstring());
    Check(bom.source == text && bom.encoding == "UTF-8 BOM", "UTF-8 BOM decoding failed");
    Check(bom.hash != utf8.hash, "Hash must identify raw file changes");
    const auto wide = mm::Wide(text);
    for (bool little : {true, false}) {
        std::string bytes = little ? std::string("\xff\xfe", 2) : std::string("\xfe\xff", 2);
        for (wchar_t ch : wide) {
            bytes += static_cast<char>(little ? (ch & 255) : (ch >> 8));
            bytes += static_cast<char>(little ? (ch >> 8) : (ch & 255));
        }
        Write(file, bytes);
        const auto document = mm::ReadDocument(file.wstring());
        Check(document.source == text, "UTF-16 decoding failed");
        Check(document.encoding == (little ? "UTF-16 LE" : "UTF-16 BE"), "UTF-16 encoding label failed");
    }
    Write(file, std::string("\x93\xfa\x96\x7b\x8c\xea", 6));
    ExpectError([&] { (void)mm::ReadDocument(file.wstring()); }, "ENCODING_REQUIRED");
    const auto cp932 = mm::ReadDocument(file.wstring(), true);
    Check(cp932.source == mm::Utf8(L"日本語") && cp932.encoding == "CP932", "Explicit CP932 decoding failed");
    Write(file, std::string("\xff\xfe\x00\xd8", 4));
    ExpectError([&] { (void)mm::ReadDocument(file.wstring()); }, "INVALID_UTF16");
    Write(file, std::string("\xff\xfe\x41", 3));
    ExpectError([&] { (void)mm::ReadDocument(file.wstring()); }, "INVALID_UTF16");
    Write(file, std::string("\x81", 1));
    ExpectError([&] { (void)mm::ReadDocument(file.wstring(), true); }, "INVALID_CP932");
    Write(file, "");
    const auto empty = mm::ReadDocument(file.wstring());
    Check(empty.source.empty() && empty.hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "Empty file handling or SHA256 is incorrect");
    Write(file, std::string(mm::MaximumDocumentBytes + 1, 'x'));
    ExpectError([&] { (void)mm::ReadDocument(file.wstring()); }, "DOCUMENT_TOO_LARGE");
    std::cout << "PASS UTF-8/BOM/UTF-16/CP932, strict errors, SHA256, 10 MiB limit\n";
}

void DocumentCancellation(const fs::path& base) {
    std::atomic_bool cancelled = true;
    ExpectError([&] { (void)mm::ReadDocument((base / L"missing-document.md").wstring(), false, &cancelled); },
        "READ_CANCELLED");
    const auto file = base / L"cancel-document.md";
    Write(file, "# Cancellation fixture\n");
    ExpectError([&] { (void)mm::ReadDocument(file.wstring(), false, &cancelled); }, "READ_CANCELLED");
    cancelled = false;
    Check(mm::ReadDocument(file.wstring(), false, &cancelled).source == "# Cancellation fixture\n",
        "Uncancelled read with token did not return the document");

    struct ExclusiveFile {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~ExclusiveFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } exclusive;
    exclusive.value = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(exclusive.value != INVALID_HANDLE_VALUE, "Cannot lock cancellation test document");
    // A sharing violation puts the reader in retry backoff. Cancellation must
    // finish that wait with READ_CANCELLED, rather than retrying the I/O error.
    std::jthread cancelWhileRetrying([&] { Sleep(20); cancelled = true; });
    ExpectError([&] { (void)mm::ReadDocument(file.wstring(), false, &cancelled); }, "READ_CANCELLED");
    std::cout << "PASS read cancellation before opening and during sharing-violation retry\n";
}

void DocumentUtf8Boundaries(const fs::path& base) {
    const auto file = base / L"utf8-boundaries.md";
    // Exercise each byte position in an SSE2 lane and both sides of the
    // cancellation/read chunk boundary. These are Unicode range endpoints,
    // including noncharacters, which are valid UTF-8 and must stay accepted.
    const std::array<std::string, 13> valid = {
        std::string("\0", 1), "\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80",
        "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbe", "\xef\xbf\xbf",
        "\xf0\x90\x80\x80", "\xf3\xbf\xbf\xbf", "\xf4\x80\x80\x80", "\xf4\x8f\xbf\xbf"
    };
    const std::array<std::string, 26> invalid = {
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xdf\x7f", "\xdf\xc0",
        "\xe0\x9f\xbf", "\xe1", "\xe1\x80", "\xe1\x80\x7f", "\xe1\xc0\x80",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xef\xbf", "\xf0\x8f\xbf\xbf",
        "\xf1", "\xf1\x80", "\xf1\x80\x80", "\xf1\x80\x7f\x80",
        "\xf1\x80\x80\xc0", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xfe", "\xff",
        "\xf0\x90\x80\x80\x80"
    };
    std::vector<size_t> offsets;
    for (size_t offset = 0; offset < 32; ++offset) offsets.push_back(offset);
    for (size_t offset = 65532; offset <= 65537; ++offset) offsets.push_back(offset);
    for (const size_t offset : offsets) {
        const std::string prefix(offset, 'a');
        for (const auto& sequence : valid) {
            // Test an exact ending (no trailing input) and an ASCII suffix
            // that returns validation to its vectorized path.
            for (const bool suffix : {false, true}) {
                const auto text = prefix + sequence + (suffix ? std::string(33, 'b') : std::string());
                Write(file, text);
                const auto document = mm::ReadDocument(file.wstring());
                Check(document.source == text && document.encoding == "UTF-8", "Valid UTF-8 boundary sequence changed or rejected");
            }
        }
        for (const auto& sequence : invalid) {
            Write(file, prefix + sequence);
            ExpectError([&] { (void)mm::ReadDocument(file.wstring()); }, "ENCODING_REQUIRED");
            // A BOM must not bypass validation, including when CP932 was
            // requested: a recognized BOM continues to take precedence.
            Write(file, std::string("\xef\xbb\xbf") + prefix + sequence + std::string(33, 'b'));
            ExpectError([&] { (void)mm::ReadDocument(file.wstring(), true); }, "ENCODING_REQUIRED");
        }
    }
    // Long pure ASCII, high ASCII density, and dense multibyte documents cover
    // all fast/scalar transitions rather than testing only short codepoints.
    for (const size_t size : {size_t(15), size_t(16), size_t(17), size_t(65535), size_t(65536), size_t(65537), size_t(mm::MaximumDocumentBytes)}) {
        const std::string text(size, 'x');
        Write(file, text);
        Check(mm::ReadDocument(file.wstring()).source == text, "ASCII read/validation size boundary failed");
    }
    std::string dense;
    for (size_t repeat = 0; repeat < 2000; ++repeat)
        for (const auto& sequence : valid) dense += sequence;
    Write(file, dense);
    Check(mm::ReadDocument(file.wstring()).source == dense, "Dense mixed UTF-8 document changed");
    Write(file, "\xef\xbb\xbf");
    const auto emptyBom = mm::ReadDocument(file.wstring());
    Check(emptyBom.source.empty() && emptyBom.encoding == "UTF-8 BOM", "Empty BOM document failed");
    std::cout << "PASS UTF-8 SIMD/tail/chunk boundaries, strict ranges, BOM precedence, exact 10 MiB read\n";
}

void DocumentReadTiming(const fs::path& base) {
    const auto file = base / L"read-timing.md";
    constexpr size_t size = 1024 * 1024;
    const auto japaneseUnit = mm::Utf8(L"日本語の本文です。\n");
    for (const bool japanese : {false, true}) {
        std::string text;
        if (japanese) {
            text.reserve(size);
            while (size - text.size() >= japaneseUnit.size()) text += japaneseUnit;
            text.append(size - text.size(), ' ');
        } else {
            text.assign(size, 'a');
        }
        Write(file, text);
        Check(mm::ReadDocument(file.wstring()).source == text, "Read timing fixture changed");
        constexpr unsigned iterations = 8;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned iteration = 0; iteration < iterations; ++iteration) {
            const auto document = mm::ReadDocument(file.wstring());
            Check(document.source.size() == size && document.hash.size() == 64, "Read timing iteration failed");
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Informational only: includes allocation, file I/O, hashing, and
        // decoding. Host load is variable, so this is not a pass/fail budget.
        std::cout << "INFO ReadDocument warm-cache " << (japanese ? "Japanese" : "ASCII")
            << " 1 MiB: " << elapsed / iterations << " ms/read (" << iterations << " reads)\n";
    }
}

void References(const fs::path& base) {
    const auto document = base / L"docs" / L"current.md";
    const auto expected = base / L"images" / L"日本語 # (1).png";
    const std::string target = "../images/" + mm::Utf8(L"日本語") + "%20%23%20(1).png#" + mm::Utf8(L"見出し");
    Check(mm::ResolveReference(document.wstring(), target) == mm::NormalizePath(expected.wstring()), "Japanese/encoded hash relative path failed");
    Check(mm::Fragment(target) == mm::Utf8(L"見出し"), "Fragment extraction failed");
    Check(mm::Fragment("a.md#part%20one") == "part one", "Encoded fragment failed");
    Check(mm::ResolveReference(document.wstring(), "#heading") == mm::NormalizePath(document.wstring()), "In-document reference failed");
    Check(mm::ResolveReference(document.wstring(), "other.md?view=1#heading") == mm::NormalizePath((document.parent_path() / L"other.md").wstring()), "Query/fragment stripping failed");
    // ResolveReference receives Markdown link targets, so a literal filename '#'
    // is percent-encoded. Raw filesystem/command-line paths use NormalizePath.
    auto absoluteTarget = mm::Utf8(expected.wstring());
    absoluteTarget.replace(absoluteTarget.find('#'), 1, "%23");
    Check(mm::ResolveReference(document.wstring(), absoluteTarget) == mm::NormalizePath(expected.wstring()), "Absolute encoded local target failed");
    const auto plainAbsolute = base / L"absolute.png";
    Check(mm::ResolveReference(document.wstring(), mm::Utf8(plainAbsolute.wstring())) == mm::NormalizePath(plainAbsolute.wstring()), "Absolute local path failed");
    for (const auto* unsupported : {"https://example.com/a.md", "javascript:alert(1)", "file:///C:/a.md", "//server/share/a.md", "\\\\server\\share\\a.md", "x.md:stream", "bad%00.md", "C:/x.md:stream"})
        Check(mm::ResolveReference(document.wstring(), unsupported).empty(), "Unsupported reference accepted");
    std::cout << "PASS relative paths, encoded hash, Japanese names, fragments, reference restrictions\n";
}

} // namespace

int main() {
    try {
        TemporaryFolder temporary;
        FolderFiltering(temporary.path);
        ExcludedFolderScanning(temporary.path);
        ReparsePoints(temporary.path);
        DocumentEncoding(temporary.path);
        DocumentUtf8Boundaries(temporary.path);
        DocumentCancellation(temporary.path);
        DocumentReadTiming(temporary.path);
        References(temporary.path);
        std::cout << "All core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
