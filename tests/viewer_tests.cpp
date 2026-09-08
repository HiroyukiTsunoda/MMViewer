#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <psapi.h>
#include "viewer.h"
#include "diagram.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string Utf8(const std::wstring& value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
double MemoryMiB() {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return memory.PrivateUsage / (1024.0 * 1024.0);
}
double CpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "process CPU measurement");
    ULARGE_INTEGER kernelTicks{}, userTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime; kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime; userTicks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) / 10000.0;
}
void Paint(HWND window) { InvalidateRect(window, nullptr, FALSE); SendMessageW(window, WM_PAINT, 0, 0); }
void SettlePaint(HWND window) {
    // Native scrollbars can change the client width during the first layout.
    // Warm-state counters are sampled only once that geometry has settled.
    for (int attempt = 0; attempt < 4; ++attempt) {
        RECT before{}, after{}; GetClientRect(window, &before);
        Paint(window); GetClientRect(window, &after);
        if (before.right == after.right && before.bottom == after.bottom) return;
    }
    throw std::runtime_error("renderer geometry did not settle");
}
std::uint64_t RenderHash(HWND window) {
    SettlePaint(window);
    RECT client{}; GetClientRect(window, &client);
    Gdiplus::Bitmap bitmap(client.right, client.bottom, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        HDC dc = graphics.GetHDC();
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        graphics.ReleaseHDC(dc);
    }
    Gdiplus::Rect area(0, 0, client.right, client.bottom);
    Gdiplus::BitmapData pixels{};
    Require(bitmap.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &pixels) == Gdiplus::Ok, "snapshot pixel access");
    std::uint64_t hash = 14695981039346656037ULL;
    for (int y = 0; y < client.bottom; ++y) {
        const auto row = static_cast<const BYTE*>(pixels.Scan0) + static_cast<std::ptrdiff_t>(y) * pixels.Stride;
        for (int x = 0; x < client.right; ++x) {
            // The unused RGB pixel byte is unspecified; compare visible RGB only.
            for (int channel = 0; channel < 3; ++channel) { hash ^= row[x * 4 + channel]; hash *= 1099511628211ULL; }
        }
    }
    Require(bitmap.UnlockBits(&pixels) == Gdiplus::Ok, "snapshot pixel release");
    return hash;
}
constexpr size_t RetainedDocumentLimit = 8, RetainedByteLimit = 64 * 1024 * 1024;
void RequireCacheBounded(const mm::NativeDocumentView& view) {
    const auto stats = view.GetPerformanceStats();
    Require(stats.cachedDocuments <= RetainedDocumentLimit, "inactive document count is bounded");
    Require(stats.cachedBytes <= RetainedByteLimit, "inactive document retained bytes are bounded");
}
void CacheRegression(HWND parent) {
    std::cerr << "viewer tests: cache/settings/geometry regression\n";
    mm::NativeDocumentView view;
    Require(view.Create(parent, 2) != nullptr, "cache regression window creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    const std::wstring pathA = L"C:\\MMviewer-tests\\cache-a.md", pathB = L"C:\\MMviewer-tests\\cache-b.md";
    std::string sourceA = "# Alpha\n\n**cachealpha**\n\n", sourceB = "# Beta\n\n**cachebeta**\n\n";
    for (int i = 0; i < 60; ++i) {
        sourceA += Utf8(L"日本語の段落です。\n\n");
        sourceB += Utf8(L"別の本文です。\n\n");
    }
    sourceA += "## destination\n\nAlpha ending.\n";
    sourceB += "## destination\n\nBeta ending.\n";
    view.SetDocument(sourceA, pathA, 16, false);
    const auto hashA = RenderHash(view.Handle());
    view.SetDocument(sourceB, pathB, 23, true);
    const auto hashB = RenderHash(view.Handle());
    Require(hashA != hashB, "distinct document/settings render differently");
    view.Search(L"**cachebeta**", 0);
    Require(view.SearchResult().second == 1, "source-mode document exposes Markdown markers");
    const auto warmed = view.GetPerformanceStats();
    view.SetDocument(sourceA, pathA, 16, false);
    Require(view.SearchResult().second == 0, "cache restore refreshes the current search query");
    Require(view.FontSize() == 16, "cached Alpha restores requested font size");
    view.Search(L"", 0); view.SetScrollRatio(0);
    Require(RenderHash(view.Handle()) == hashA, "cached Alpha exactly restores rendered pixels");
    view.SetDocument(sourceB, pathB, 23, true);
    Require(view.FontSize() == 23, "cached Beta restores requested font size");
    Require(RenderHash(view.Handle()) == hashB, "cached source-mode Beta exactly restores rendered pixels");
    auto stats = view.GetPerformanceStats();
    Require(stats.parses == warmed.parses && stats.layouts == warmed.layouts, "warm font/source switches avoid parsing and layout");
    Require(stats.cacheHits == warmed.cacheHits + 2, "both warm switches restore cached documents");
    Require(stats.bufferAllocations == warmed.bufferAllocations, "same-size tab paints reuse the backbuffer");

    view.SetDocument(sourceA, pathA, 16, false); SettlePaint(view.Handle());
    const auto scrollStart = view.GetPerformanceStats();
    view.SetScrollRatio(0.5); SettlePaint(view.Handle());
    Require(std::abs(view.ScrollRatio() - 0.5) < 0.02, "cached layout applies the requested scroll ratio");
    view.ScrollToAnchor(L"destination"); SettlePaint(view.Handle());
    Require(view.ScrollRatio() > 0.9, "cached layout navigates to the final anchor");
    stats = view.GetPerformanceStats();
    Require(stats.layouts == scrollStart.layouts, "scroll and anchor navigation do not repeat layout");
    Require(stats.bufferAllocations == scrollStart.bufferAllocations, "scroll paints reuse the backbuffer");
    const auto heightStart = stats;
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 430, SWP_NOZORDER | SWP_NOACTIVATE); SettlePaint(view.Handle());
    stats = view.GetPerformanceStats();
    Require(stats.layouts == heightStart.layouts && stats.parses == heightStart.parses, "height-only resize preserves text layout");
    Require(stats.bufferAllocations > heightStart.bufferAllocations, "size change replaces the viewport buffer");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 650, 430, SWP_NOZORDER | SWP_NOACTIVATE); SettlePaint(view.Handle());
    const auto widthChanged = view.GetPerformanceStats();
    Require(widthChanged.layouts > stats.layouts && widthChanged.parses == stats.parses, "width resize reflows without reparsing");
    Paint(view.Handle()); Paint(view.Handle());
    Require(view.GetPerformanceStats().bufferAllocations == widthChanged.bufferAllocations, "repeated same-size paints allocate no buffer");

    std::string updated = sourceA;
    updated.replace(updated.find("cachealpha"), std::string("cachealpha").size(), "updatedalpha");
    const auto updateStart = view.GetPerformanceStats();
    view.SetDocument(updated, pathA, 16, false); SettlePaint(view.Handle());
    Require(view.GetPerformanceStats().parses == updateStart.parses + 1, "changed source at the same path is reparsed");
    view.Search(L"updatedalpha", 0); Require(view.SearchResult().second == 1, "updated source is searchable");
    view.Search(L"cachealpha", 0); Require(view.SearchResult().second == 0, "updated source drops obsolete text");
    const auto refreshStart = view.GetPerformanceStats();
    view.InvalidateDocumentCache(pathA); view.SetDocument(updated, pathA, 16, false); SettlePaint(view.Handle());
    Require(view.GetPerformanceStats().parses == refreshStart.parses + 1, "explicit refresh reparses unchanged active source");
    view.SetDocument(sourceB, pathB, 23, true); SettlePaint(view.Handle());
    view.InvalidateDocumentCache(pathA);
    const auto invalidated = view.GetPerformanceStats();
    view.SetDocument(updated, pathA, 16, false); SettlePaint(view.Handle());
    Require(view.GetPerformanceStats().parses == invalidated.parses + 1, "invalidated inactive document cannot reuse stale parse state");
    RequireCacheBounded(view);
}
void CacheEvictionRegression(HWND parent) {
    std::cerr << "viewer tests: cache count and retained-byte eviction\n";
    {
        mm::NativeDocumentView view;
        Require(view.Create(parent, 3) != nullptr, "cache count regression window creation");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
        std::vector<std::string> sources;
        std::vector<std::wstring> paths;
        std::uint64_t firstHash = 0;
        for (size_t i = 0; i < RetainedDocumentLimit + 2; ++i) {
            sources.push_back("# Entry " + std::to_string(i) + "\n\nUnique-entry-" + std::to_string(i) + "\n\n");
            for (int line = 0; line < 40; ++line) sources.back() += "Body paragraph.\n\n";
            paths.push_back(L"C:\\MMviewer-tests\\count-" + std::to_wstring(i) + L".md");
            view.SetDocument(sources.back(), paths.back(), 16, false); SettlePaint(view.Handle());
            if (i == 0) firstHash = RenderHash(view.Handle());
            RequireCacheBounded(view);
        }
        auto before = view.GetPerformanceStats();
        Require(before.cachedDocuments == RetainedDocumentLimit, "small documents reach the count limit");
        view.SetDocument(sources.front(), paths.front(), 16, false);
        Require(RenderHash(view.Handle()) == firstHash, "count-evicted document is reproduced accurately");
        Require(view.GetPerformanceStats().parses == before.parses + 1, "least-recently-used document is evicted at the count limit");
        view.Search(L"Unique-entry-0", 0); Require(view.SearchResult().second == 1, "count eviction preserves the selected document text");
        RequireCacheBounded(view);
    }
    {
        mm::NativeDocumentView view;
        Require(view.Create(parent, 4) != nullptr, "cache byte regression window creation");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto path = [](size_t i) { return L"C:\\MMviewer-tests\\byte-" + std::to_wstring(i) + L".md"; };
        std::string rules;
        size_t ruleCount = 20000;
        // Calibrate using the implementation's retained-allocation estimate so
        // STL growth policies do not make the byte-limit regression fragile.
        for (int attempt = 0; attempt < 3; ++attempt) {
            rules.clear(); rules.reserve(ruleCount * 5);
            for (size_t i = 0; i < ruleCount; ++i) rules += "---\n\n";
            view.InvalidateDocumentCache(path(0));
            view.SetDocument("# Budget-entry-0\n\n" + rules + "Budget ending.\n", path(0), 16, false); SettlePaint(view.Handle());
            view.SetDocument("Calibration complete.", L"", 16, false); SettlePaint(view.Handle());
            const auto calibrated = view.GetPerformanceStats();
            Require(calibrated.cachedDocuments == 1, "byte-pressure sample fits individually in the cache");
            if (calibrated.cachedBytes > RetainedByteLimit / RetainedDocumentLimit) break;
            Require(attempt < 2, "byte-pressure sample reaches the required retained size");
            ruleCount *= 2;
        }
        const std::string first = "# Budget-entry-0\n\n" + rules + "Budget ending.\n";
        view.SetDocument(first, path(0), 16, false);
        const auto firstHash = RenderHash(view.Handle());
        for (size_t i = 1; i <= RetainedDocumentLimit; ++i) {
            view.SetDocument("# Budget-entry-" + std::to_string(i) + "\n\n" + rules + "Budget ending.\n", path(i), 16, false);
            SettlePaint(view.Handle()); RequireCacheBounded(view);
        }
        const auto pressure = view.GetPerformanceStats();
        Require(pressure.cachedDocuments < RetainedDocumentLimit, "retained-byte limit evicts before the document-count limit");
        view.SetDocument(first, path(0), 16, false);
        Require(RenderHash(view.Handle()) == firstHash, "byte-evicted document is reproduced accurately");
        Require(view.GetPerformanceStats().parses == pressure.parses + 1, "byte pressure evicts the oldest document");
        view.Search(L"Budget ending.", 0);
        Require(view.SearchResult().second == 1 && view.ScrollRatio() > 0.95, "byte-evicted document retains its final searchable paragraph");
        RequireCacheBounded(view);
    }
}
void WarmBenchmark(mm::NativeDocumentView& view, const std::string& large, const std::string& paragraph) {
    std::cerr << "viewer tests: one MiB Japanese warm switch/scroll benchmark\n";
    const std::wstring pathA = L"C:\\MMviewer-tests\\large.md", pathB = L"C:\\MMviewer-tests\\large-b.md";
    std::string second = "# Second Japanese benchmark\n\n";
    while (second.size() < 1024 * 1024) second += paragraph;
    second += Utf8(L"\n\n別文書の測定終点\n");
    view.Search(L"", 0);
    view.SetDocument(large, pathA, 16, false); view.SetScrollRatio(0); SettlePaint(view.Handle());
    view.SetDocument(second, pathB, 16, false); SettlePaint(view.Handle());
    view.SetDocument(large, pathA, 16, false); SettlePaint(view.Handle());
    const auto warmed = view.GetPerformanceStats();
    const auto report = [](const char* label, auto begin, double cpuStart, double memoryStart) {
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::cout << label << ": wall=" << elapsed << " ms, CPU=" << CpuMilliseconds() - cpuStart
                  << " ms, private delta=" << MemoryMiB() - memoryStart << " MiB, process private=" << MemoryMiB() << " MiB\n";
    };
    auto begin = std::chrono::steady_clock::now();
    double cpuStart = CpuMilliseconds(), memoryStart = MemoryMiB();
    for (int step = 0; step < 32; ++step) {
        const bool selectB = step % 2 == 0;
        view.SetDocument(selectB ? second : large, selectB ? pathB : pathA, 16, false);
        view.SetScrollRatio(0); Paint(view.Handle());
    }
    report("1 MiB A/B, 32 warm switches", begin, cpuStart, memoryStart);
    auto switched = view.GetPerformanceStats();
    Require(switched.parses == warmed.parses && switched.layouts == warmed.layouts, "one MiB warm switches avoid reparsing and relayout");
    Require(switched.cacheHits == warmed.cacheHits + 32, "all benchmark switches hit the document cache");
    Require(switched.bufferAllocations == warmed.bufferAllocations, "warm switch benchmark reuses viewport memory");
    begin = std::chrono::steady_clock::now(); cpuStart = CpuMilliseconds(); memoryStart = MemoryMiB();
    for (int step = 0; step < 32; ++step) { view.SetScrollRatio(step / 31.0); Paint(view.Handle()); }
    report("1 MiB, 32 scroll positions", begin, cpuStart, memoryStart);
    const auto scrolled = view.GetPerformanceStats();
    Require(scrolled.parses == switched.parses && scrolled.layouts == switched.layouts, "32-position scroll avoids parsing and layout");
    Require(scrolled.bufferAllocations == switched.bufferAllocations, "32-position scroll reuses viewport memory");
    Require(view.ScrollRatio() > 0.99, "scroll benchmark reaches the document end");
    view.Search(L"性能測定の終点", 0); Require(view.SearchResult().second == 1, "warm benchmark finishes on document Alpha");
    view.SetDocument(second, pathB, 16, false); view.Search(L"別文書の測定終点", 0);
    Require(view.SearchResult().second == 1 && view.ScrollRatio() > 0.95, "warm document Beta retains its ending");
    RequireCacheBounded(view);
}
void SearchCacheRegression(HWND parent) {
    std::cerr << "viewer tests: lowercase search cache invalidation\n";
    mm::NativeDocumentView view;
    Require(view.Create(parent, 5) != nullptr, "search cache window creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    // Same body length, different text: a stale lowercase copy would still match.
    const std::wstring pathA = L"C:\\MMviewer-tests\\same-length-a.md", pathB = L"C:\\MMviewer-tests\\same-length-b.md";
    view.SetDocument("Alpha one two", pathA, 16, false);
    view.Search(L"ALPHA", 0); Require(view.SearchResult().second == 1, "case-insensitive search finds the first document");
    view.Search(L"alpha", 0); view.Search(L"one", 0);
    Require(view.SearchResult().second == 1, "repeated searches reuse the lowercase body");
    view.SetDocument("Bravo one two", pathB, 16, false);
    Require(view.SearchResult().second == 1, "replacement re-evaluates the query against the new body");
    view.Search(L"alpha", 0); Require(view.SearchResult().second == 0, "same-length replacement drops the old text");
    view.Search(L"bravo", 0); Require(view.SearchResult().second == 1, "same-length replacement is searchable");
    view.SetDocument("Alpha one two", pathA, 16, false);
    Require(view.SearchResult().second == 0, "restored document does not reuse the other document's lowercase copy");
    view.Search(L"alpha", 0); Require(view.SearchResult().second == 1, "restored document searches its own text");
    view.SetSourceMode(true); view.Search(L"alpha", 0);
    Require(view.SearchResult().second == 1, "source mode rebuilds the lowercase body");
    view.Search(L"", 0);
}
void SelectionRegression(HWND parent) {
    std::cerr << "viewer tests: mouse selection endpoints\n";
    mm::NativeDocumentView view;
    Require(view.Create(parent, 6) != nullptr, "selection window creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    std::string body = "# Selection\n\nFirst line of text.\n\n";
    for (int i = 0; i < 12; ++i) body += "Paragraph " + std::to_string(i) + " keeps the layout tall enough to scroll.\n\n";
    body += "Last words.";
    view.SetDocument(body, L"C:\\MMviewer-tests\\selection.md", 16, false);
    SettlePaint(view.Handle());
    RECT client{}; GetClientRect(view.Handle(), &client);
    auto drag = [&](int x1, int y1, int x2, int y2) {
        SendMessageW(view.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
        SendMessageW(view.Handle(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x2, y2));
        SendMessageW(view.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
    };
    // From above the first piece to far below the last: both ends resolve to
    // the nearest block in that direction, the start and the end of the text.
    drag(0, 0, client.right - 1, client.bottom - 1);
    const auto everything = view.SelectedText();
    Require(everything.starts_with(L"Selection\nFirst line of text."), "selection starts at the first piece");
    view.SetScrollRatio(1); SettlePaint(view.Handle());
    drag(0, 0, client.right - 1, client.bottom - 1);
    const auto tail = view.SelectedText();
    Require(tail.ends_with(L"Last words."), "selection ends at the last piece");
    Require(!tail.starts_with(L"Selection"), "scrolled selection starts on a later block");
    drag(5, 5, 5, 5);
    Require(view.SelectedText().empty(), "click without drag selects nothing");
    // A drag inside the text selects a bounded range that other pieces do not extend.
    view.SetScrollRatio(0); SettlePaint(view.Handle());
    drag(client.right / 2, client.bottom / 2, client.right / 2, client.bottom / 2 + 60);
    const auto middle = view.SelectedText();
    Require(!middle.empty() && !middle.starts_with(L"Selection") && !middle.ends_with(L"Last words."),
        "mid-document drag selects a bounded range");
}
void ZoomRoundTripRegression(HWND parent) {
    // Font and width caches now survive a zoom; returning to the original size
    // must reproduce the original pixels from those caches.
    mm::NativeDocumentView view;
    Require(view.Create(parent, 7) != nullptr, "zoom regression window creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    std::string source = "# Zoom\n\n";
    for (int i = 0; i < 40; ++i) source += "Paragraph " + std::to_string(i) + " with **bold**, `code`, and a [link](other.md) among repeated words words words.\n\n";
    source += "| Column A | Column B |\n| --- | --- |\n| cell text | more cell text |\n";
    view.SetDocument(source, L"C:\\MMviewer-tests\\zoom.md", 16, false); SettlePaint(view.Handle());
    const auto before = RenderHash(view.Handle());
    view.SetFontSize(20); SettlePaint(view.Handle());
    Require(RenderHash(view.Handle()) != before, "zoom changes the rendering");
    view.SetFontSize(16); SettlePaint(view.Handle());
    Require(RenderHash(view.Handle()) == before, "zooming back reproduces the original rendering");
    std::cerr << "viewer tests: zoom round trip\n";
}
void LargeTableRegression(HWND parent) {
    // Only the table rows crossing the client area are painted. Scrolling must
    // still show the rows at each position, and the text stays searchable.
    mm::NativeDocumentView view;
    Require(view.Create(parent, 8) != nullptr, "table regression window creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    std::string source = "# Table\n\n| Index | Name | Value |\n| --- | --- | --- |\n";
    for (int i = 0; i < 5000; ++i) source += "| " + std::to_string(i) + " | row-" + std::to_string(i) + " | " + std::to_string(i * 7) + " |\n";
    view.SetDocument(source, L"C:\\MMviewer-tests\\table.md", 16, false); SettlePaint(view.Handle());
    const auto top = RenderHash(view.Handle());
    view.SetScrollRatio(0.5); SettlePaint(view.Handle());
    const auto middle = RenderHash(view.Handle());
    view.SetScrollRatio(1); SettlePaint(view.Handle());
    const auto bottom = RenderHash(view.Handle());
    Require(top != middle && middle != bottom && top != bottom, "scrolling a long table shows different rows");
    view.Search(L"row-2500", 0); Require(view.SearchResult().second == 1, "table text remains searchable");
    SettlePaint(view.Handle());
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) Paint(view.Handle());
    const double perPaint = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / 20;
    std::cerr << "viewer tests: 5,000-row table paint " << perPaint << " ms per frame\n";
}
void SaveSnapshot(HWND window, const std::filesystem::path& destination) {
    RECT client{}; GetClientRect(window, &client);
    Gdiplus::Bitmap output(client.right, client.bottom, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics graphics(&output);
        HDC dc = graphics.GetHDC();
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        graphics.ReleaseHDC(dc);
    }
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes);
    auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    Gdiplus::GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            Require(output.Save(destination.c_str(), &encoders[i].Clsid, nullptr) == Gdiplus::Ok, "native snapshot PNG save");
            return;
        }
    }
    throw std::runtime_error("PNG encoder missing");
}
void CenteringRegression(HWND parent, const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    mm::NativeDocumentView view;
    Require(view.Create(parent, 7) != nullptr, "centering window creation");
    view.SetTheme(true);
    const float dpi = GetDpiForWindow(view.Handle()) / 96.0f;
    auto resize = [&](float width) {
        SetWindowPos(view.Handle(), nullptr, 0, 0, int(std::ceil(width * dpi)), int(900 * dpi), SWP_NOZORDER | SWP_NOACTIVATE);
        SettlePaint(view.Handle());
    };
    auto horizontalMaximum = [&] {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(view.Handle(), SB_HORZ, &info);
        return std::max(0, info.nMax - int(info.nPage) + 1);
    };
    // Read actual panel pixels, independently of the renderer's private geometry.
    auto panelMargins = [&] {
        SettlePaint(view.Handle());
        RECT client{}; GetClientRect(view.Handle(), &client);
        Gdiplus::Bitmap bitmap(client.right, client.bottom, PixelFormat32bppRGB);
        { Gdiplus::Graphics g(&bitmap); HDC dc = g.GetHDC();
          SendMessageW(view.Handle(), WM_PRINTCLIENT, WPARAM(dc), PRF_CLIENT); g.ReleaseHDC(dc); }
        int left = client.right, right = -1;
        for (int x = 0; x < client.right; ++x) {
            Gdiplus::Color pixel; bitmap.GetPixel(x, int(32 * dpi), &pixel);
            if (pixel.GetR() == 31 && pixel.GetG() == 37 && pixel.GetB() == 47) { left = std::min(left, x); right = x; }
        }
        Require(right > left, "centering fixture has a visible panel");
        return std::pair<int,int>{left, client.right - 1 - right};
    };
    auto centered = [&](const char* message) {
        auto [left, right] = panelMargins();
        std::cout << message << ": left=" << left << " right=" << right << " horizontal=" << horizontalMaximum() << '\n';
        Require(left > 28 * dpi && std::abs(left - right) <= 2, message);
        Require(horizontalMaximum() == 0, "fitting content does not create horizontal overflow");
    };
    const std::string graph = "flowchart LR\nA[First node with a descriptive label] --> B[Second node with a descriptive label]\n"
                              "B --> C[Third node with a descriptive label]\nC --> D[Fourth node with a descriptive label]\n";
    const auto diagram = mm::ParseDiagram(graph);
    Require(diagram && diagram->valid && diagram->width > 1100, "wide Mermaid fixture exceeds the normal text column");
    const std::string markdown = "```mermaid\n" + graph + "```\n\n[centered link](other.md)\n";
    const std::wstring path = L"C:\\MMviewer-tests\\centered-diagram.md";
    resize(diagram->width + 650);
    view.SetDocument(markdown, path, 16, false); SettlePaint(view.Handle());
    SaveSnapshot(view.Handle(), output / L"diagram-fitting.png");
    centered("Mermaid wider than text is centered using its real width");
    const auto firstHash = RenderHash(view.Handle());
    const auto painted = view.GetPerformanceStats();
    for (int i = 0; i < 6; ++i) Require(RenderHash(view.Handle()) == firstHash, "repeat paint does not accumulate centering shifts");
    Require(view.GetPerformanceStats().layouts == painted.layouts, "repeat paint does not repeat layout");
    view.SetDocument("A different tab.", L"C:\\MMviewer-tests\\centering-other.md", 16, false); SettlePaint(view.Handle());
    const auto beforeReturn = view.GetPerformanceStats();
    view.SetDocument(markdown, path, 16, false);
    Require(RenderHash(view.Handle()) == firstHash, "cached centering restores identical pixels");
    Require(view.GetPerformanceStats().layouts == beforeReturn.layouts, "cached centering does not repeat layout");

    // A click on a rendered link must follow its shifted Piece coordinates.
    RECT client{}; GetClientRect(view.Handle(), &client);
    Gdiplus::Bitmap bitmap(client.right, client.bottom, PixelFormat32bppRGB);
    { Gdiplus::Graphics g(&bitmap); HDC dc = g.GetHDC();
      SendMessageW(view.Handle(), WM_PRINTCLIENT, WPARAM(dc), PRF_CLIENT); g.ReleaseHDC(dc); }
    POINT link{-1,-1};
    const int textTop = int((diagram->height + 28 + 24 + 15) * dpi);
    for (int y = textTop; y < std::min<int>(client.bottom, textTop + int(40 * dpi)); ++y)
        for (int x = 0; x < client.right; ++x) {
            Gdiplus::Color pixel; bitmap.GetPixel(x, y, &pixel);
            if (pixel.GetR() == 112 && pixel.GetG() == 174 && pixel.GetB() == 255 && (link.x < 0 || x < link.x)) link = {x,y};
        }
    Require(link.x >= 0, "shifted link is rendered below the diagram");
    Require(std::abs(link.x - panelMargins().first) <= 8 * dpi, "shifted text remains aligned with the diagram panel");
    std::wstring followed;
    view.onLink = [&](const std::wstring& target) { followed = target; };
    SendMessageW(view.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(link.x, link.y));
    SendMessageW(view.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(link.x, link.y));
    Require(followed == L"other.md", "shifted link hit testing matches its rendered position");
    SendMessageW(view.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, link.y));
    SendMessageW(view.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(client.right - 1, link.y));
    Require(view.SelectedText().find(L"centered link") != std::wstring::npos, "selection follows shifted text pieces");
    view.Search(L"centered link", 0); Require(view.SearchResult().second == 1, "shifted text remains searchable");
    view.Search(L"", 0); view.SetScrollRatio(0);

    resize(diagram->width - 200);
    SaveSnapshot(view.Handle(), output / L"diagram-overflow.png");
    auto [left, right] = panelMargins();
    Require(std::abs(left - 28 * dpi) <= 2 && horizontalMaximum() > 0, "oversized Mermaid starts at the normal left margin");
    SendMessageW(view.Handle(), WM_HSCROLL, SB_RIGHT, 0); SettlePaint(view.Handle());
    std::tie(left, right) = panelMargins();
    Require(std::abs(right - 28 * dpi) <= 2, "horizontal scrolling reaches the diagram end without spare centering space");
    SaveSnapshot(view.Handle(), output / L"diagram-right-end.png");
    view.SetFontSize(10); SettlePaint(view.Handle());
    Require(GetScrollPos(view.Handle(), SB_HORZ) == 0 && horizontalMaximum() == 0, "zooming down removes stale horizontal scroll");
    centered("zoomed diagram retains normal text-column centering");
    view.SetFontSize(16); resize(diagram->width + 650);
    centered("resizing wider restores centering");
    view.SetWide(true); SettlePaint(view.Handle());
    std::tie(left, right) = panelMargins();
    Require(std::abs(left - 28 * dpi) <= 2 && std::abs(right - 28 * dpi) <= 2, "full-width mode retains ordinary side margins");
    view.SetWide(false);

    std::string table = "|", separator = "|", row = "|";
    for (int i = 0; i < 12; ++i) { table += " Header |"; separator += " --- |"; row += " value |"; }
    resize(2200); view.SetDocument(table + "\n" + separator + "\n" + row + "\n", L"", 16, false);
    centered("wide table is centered using its real width");
    view.SetDocument("```\n" + std::string(400, 'W') + "\n```\n", L"", 16, false); SettlePaint(view.Handle());
    std::tie(left, right) = panelMargins();
    Require(std::abs(left - 28 * dpi) <= 2 && horizontalMaximum() > 0, "oversized code also starts at the normal margin");
    view.SetDocument("```\nShort code\n```\n", L"", 16, false);
    centered("ordinary text-width panels retain their prior centering");
    std::cout << "PASS centering, overflow endpoints, zoom, resize, full width, cache, text interactions, tables and code\n";
}
}

void KeyboardNavigation(HWND parent) {
    mm::NativeDocumentView view;
    Require(view.Create(parent, 2) != nullptr, "keyboard test view creation");
    SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
    std::string document = "# Keyboard navigation\n\n";
    for (int i = 0; i < 180; ++i) document += "Paragraph " + std::to_string(i) + " with enough text to scroll.\n\n";
    auto key = [&](WPARAM code) { SendMessageW(view.Handle(), WM_KEYDOWN, code, 1);SettlePaint(view.Handle()); };
    auto position = [&] { SCROLLINFO info{sizeof(info), SIF_ALL};Require(GetScrollInfo(view.Handle(), SB_VERT, &info), "keyboard scrollbar state");return info; };
    for (bool source : {false, true}) {
        view.SetDocument(document, L"C:\\MMviewer-tests\\keyboard.md", 16, source);
        // No intervening paint: these keys must override pending restoration.
        view.SetScrollRatio(0.6);key(VK_HOME);
        Require(position().nPos == 0, "Home before paint overrides the restored position");
        view.SetScrollRatio(0.2);key(VK_END);
        auto end = position();
        Require(end.nPos == end.nMax - int(end.nPage) + 1 && end.nPos > 0, "End reaches the actual document bottom");
        key(VK_HOME);auto top = position();
        key(VK_NEXT);auto page = position();
        Require(page.nPos == std::max(20, int(top.nPage) - 48), "Page Down advances one viewport with reading overlap");
        key(VK_PRIOR);Require(position().nPos == 0, "Page Up returns to the prior page");
        key(VK_PRIOR);Require(position().nPos == 0, "Page Up clamps at the beginning");
        view.SetFontSize(24);key(VK_END);end = position();
        Require(end.nPos == end.nMax - int(end.nPage) + 1, "End uses the zoomed layout before paint");
        key(VK_NEXT);Require(position().nPos == end.nPos, "Page Down clamps at the end");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 440, 320, SWP_NOZORDER | SWP_NOACTIVATE);
        key(VK_END);end = position();
        Require(end.nPos == end.nMax - int(end.nPage) + 1, "End uses the resized viewport before paint");
    }
    view.SetDocument("# Short\n", L"C:\\MMviewer-tests\\short-keyboard.md");
    for (WPARAM code : {VK_END, VK_HOME, VK_NEXT, VK_PRIOR}) { key(code);Require(position().nPos == 0, "Short documents stay at the beginning"); }
    std::cout << "PASS Home/End/Page Up/Page Down, pending restoration, source mode, zoom/resize, scroll bounds\n";
}

int wmain(int argc, wchar_t** argv) {
    std::cerr << std::unitbuf;
    std::cout << std::unitbuf;
    std::cerr << "viewer tests: GDI+ startup\n";
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplus = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus, &input, nullptr) != Gdiplus::Ok) return 2;
    int result = 0;
    try {
        HWND parent = CreateWindowExW(0, L"STATIC", L"MMviewer renderer tests", WS_OVERLAPPEDWINDOW, -30000, -30000, 840, 620, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(parent != nullptr, "test parent window creation");
        if (argc == 3 && std::wstring(argv[1]) == L"--centering") {
            CenteringRegression(parent, argv[2]);
            DestroyWindow(parent); Gdiplus::GdiplusShutdown(gdiplus); return 0;
        }
        KeyboardNavigation(parent);
        {
            mm::NativeDocumentView view;
            Require(view.Create(parent, 1) != nullptr, "native document window creation");
            SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
            const auto fixture = Utf8(
                L"# 日本語の見出し\n\n日本語の**強調**と[リンク](other.md#target)を読む。\n\n"
                L"- 項目一\n- [x] 項目二\n\n> 引用の本文\n\n"
                L"| 名前 | 値 |\n| --- | --- |\n| 日本語 | 123 |\n\n"
                L"```cpp\nconst int value = 123; // comment\n```\n\n"
                L"![不明な画像](missing-image.png)\n\n"
                L"```mermaid\nflowchart LR\nA[始点] --> B[有効図ラベル]\n```\n\n"
                L"```mermaid\nunsupportedDiagram\n未対応図のソース\n```\n\n"
                L"## 移動先\n最後の本文\n");
            std::cerr << "viewer tests: fixture parse\n";
            view.SetDocument(fixture, L"C:\\MMviewer-tests\\document.md");
            std::cerr << "viewer tests: first layout/search\n";
            view.Search(L"日本語の強調とリンク", 0);
            Require(view.SearchResult() == std::pair<int, int>{1, 1}, "search crosses bold and link runs");
            view.Search(L"日本語", 0);
            Require(view.SearchResult().second == 3, "search includes headings, body, table");
            view.Search(L"日本語", 1); Require(view.SearchResult().first == 2, "search next");
            view.Search(L"日本語", -1); Require(view.SearchResult().first == 1, "search previous");
            view.Search(L"有効図ラベル", 0); Require(view.SearchResult().second == 0, "rendered diagram source excluded from text search");
            view.Search(L"未対応図のソース", 0); Require(view.SearchResult().second == 1, "invalid diagram source searchable");
            std::cerr << "viewer tests: fixture paint\n";
            Paint(view.Handle());
            if (argc == 3 && std::wstring(argv[1]) == L"--snapshots") {
                std::filesystem::path folder(argv[2]); std::filesystem::create_directories(folder);
                SetWindowPos(view.Handle(), nullptr, 0, 0, 980, 1300, SWP_NOZORDER | SWP_NOACTIVATE);
                view.Search(L"", 0); view.SetScrollRatio(0); view.SetTheme(false);
                SaveSnapshot(view.Handle(), folder / L"viewer-light.png");
                view.SetTheme(true);
                SaveSnapshot(view.Handle(), folder / L"viewer-dark.png");
                SetWindowPos(view.Handle(), nullptr, 0, 0, 780, 540, SWP_NOZORDER | SWP_NOACTIVATE);
                view.SetScrollRatio(0);
                std::cerr << "viewer tests: native light/dark snapshots saved\n";
            }
            std::cerr << "viewer tests: theme/font/source/scroll\n";
            view.SetTheme(true); view.SetFontSize(24); Paint(view.Handle());
            Require(view.FontSize() == 24, "font size retained");
            view.SetFontSize(99); Require(view.FontSize() == 40, "font upper bound");
            view.SetFontSize(-1); Require(view.FontSize() == 10, "font lower bound");
            view.SetFontSize(16); view.SetWide(true);
            view.SetSourceMode(true); view.Search(L"有効図ラベル", 0);
            Require(view.SearchResult().second == 1, "source mode searches original diagram source");
            Paint(view.Handle());
            view.SetSourceMode(false); view.ScrollToAnchor(L"移動先"); Paint(view.Handle());
            Require(view.ScrollRatio() > 0, "heading anchor scroll");
            view.SetScrollRatio(0.5); Paint(view.Handle());
            Require(std::abs(view.ScrollRatio() - 0.5) < 0.02, "scroll ratio restoration");
            view.SetDocument("# Short\n\nDone.", L"C:\\MMviewer-tests\\short.md");
            view.Search(L"Done", 0); Paint(view.Handle());
            Require(view.SearchResult().second == 1, "document replacement updates search index");
            Require(view.ScrollRatio() == 0, "short document has no stale scroll");

            CacheRegression(parent);
            CacheEvictionRegression(parent);
            SearchCacheRegression(parent);
            SelectionRegression(parent);
            ZoomRoundTripRegression(parent);
            LargeTableRegression(parent);
            CenteringRegression(parent, std::filesystem::path(__FILE__).parent_path().parent_path() / L"artifacts" / L"centering-regression");

            std::cerr << "viewer tests: large Japanese benchmark\n";

            std::string paragraph = Utf8(L"日本語の長い文書を低いメモリー使用量で表示します。太字やリンクを含まない通常の文章でも読みやすさを確認します。\n\n");
            std::string large = "# Native layout benchmark\n\n";
            while (large.size() < 1024 * 1024) large += paragraph;
            large += Utf8(L"\n\n性能測定の終点\n");
            const double beforeMemory = MemoryMiB();
            auto begin = std::chrono::steady_clock::now();
            view.SetDocument(large, L"C:\\MMviewer-tests\\large.md");
            std::cerr << "viewer tests: large parse complete\n";
            auto parsed = std::chrono::steady_clock::now();
            view.Search(L"性能測定の終点", 0);
            std::cerr << "viewer tests: large layout complete\n";
            auto laidOut = std::chrono::steady_clock::now();
            Paint(view.Handle());
            auto painted = std::chrono::steady_clock::now();
            Require(view.SearchResult() == std::pair<int, int>{1, 1}, "one MiB Japanese layout reaches last paragraph");
            Require(view.ScrollRatio() > 0.95, "large document search scrolls near end");
            auto milliseconds = [](auto from, auto to) { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
            std::cout << "1 MiB Japanese: parse=" << milliseconds(begin, parsed)
                      << " ms, layout/search=" << milliseconds(parsed, laidOut)
                      << " ms, paint=" << milliseconds(laidOut, painted)
                      << " ms, private delta=" << MemoryMiB() - beforeMemory
                      << " MiB, process private=" << MemoryMiB() << " MiB\n";
            WarmBenchmark(view, large, paragraph);
            std::cout << "Native viewer tests passed.\n";
            std::cerr << "viewer tests: destroy view\n";
        }
        DestroyWindow(parent);
    } catch (const std::exception& error) {
        std::cerr << "Native viewer test failed: " << error.what() << '\n'; result = 1;
    }
    std::cerr << "viewer tests: GDI+ shutdown\n";
    Gdiplus::GdiplusShutdown(gdiplus);
    std::cerr << "viewer tests: process exit\n";
    return result;
}
