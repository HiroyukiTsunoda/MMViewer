#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "viewer.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace Gdiplus;
const CLSID PngEncoder{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void WriteImage(const fs::path& path, int width, int height, Color color) {
    // Replace only the probe's own image, after its original decoder has gone
    // out of scope. The viewer retains an independent viewport raster.
    const auto replacement = path.parent_path() / L"image-replacement.png";
    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    {
        Graphics graphics(&bitmap);
        Require(graphics.Clear(color) == Ok, "image generation");
    }
    Require(bitmap.Save(replacement.c_str(), &PngEncoder) == Ok, "image file save");
    Require(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
        "image file replacement (no retained decoder file lock)");
}
void SettlePaint(HWND window) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        RECT before{}, after{};
        GetClientRect(window, &before);
        InvalidateRect(window, nullptr, FALSE);
        SendMessageW(window, WM_PAINT, 0, 0);
        GetClientRect(window, &after);
        if (before.right == after.right && before.bottom == after.bottom) return;
    }
    throw std::runtime_error("viewer geometry failed to settle");
}
struct Pixels {
    std::uint64_t red = 0, green = 0, panel = 0;
    int greenLeft = INT_MAX, greenTop = INT_MAX, greenRight = -1, greenBottom = -1;
    int GreenWidth() const { return greenRight < 0 ? 0 : greenRight - greenLeft + 1; }
    int GreenHeight() const { return greenBottom < 0 ? 0 : greenBottom - greenTop + 1; }
};
Pixels Snapshot(HWND window, const fs::path& path) {
    SettlePaint(window);
    RECT client{};
    GetClientRect(window, &client);
    Bitmap bitmap(client.right, client.bottom, PixelFormat32bppARGB);
    {
        Graphics graphics(&bitmap);
        HDC dc = graphics.GetHDC();
        Require(dc != nullptr, "snapshot device context");
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        graphics.ReleaseHDC(dc);
    }
    Require(bitmap.Save(path.c_str(), &PngEncoder) == Ok, "snapshot file save");
    BitmapData data{};
    Rect area(0, 0, client.right, client.bottom);
    Require(bitmap.LockBits(&area, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok,
        "snapshot pixels");
    Pixels result;
    for (int y = 0; y < client.bottom; ++y) {
        auto row = static_cast<const BYTE*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < client.right; ++x) {
            auto pixel = row + x * 4;
            if (pixel[2] > 180 && pixel[1] < 60 && pixel[0] < 60) ++result.red;
            if (pixel[1] > 150 && pixel[2] < 60 && pixel[0] < 90) {
                ++result.green;
                result.greenLeft = std::min(result.greenLeft, x); result.greenRight = std::max(result.greenRight, x);
                result.greenTop = std::min(result.greenTop, y); result.greenBottom = std::max(result.greenBottom, y);
            }
            if (pixel[2] == 245 && pixel[1] == 247 && pixel[0] == 250) ++result.panel;
        }
    }
    bitmap.UnlockBits(&data);
    std::cout << "snapshot=" << path.filename().string() << " red=" << result.red << " green=" << result.green
        << " green_bounds=" << result.GreenWidth() << 'x' << result.GreenHeight() << " panel=" << result.panel << '\n';
    return result;
}
int ScrollRange(HWND window) {
    SCROLLINFO info{sizeof(info), SIF_RANGE};
    Require(GetScrollInfo(window, SB_VERT, &info) != FALSE, "document scroll range");
    return info.nMax;
}
int Probe(const fs::path& output) {
    fs::create_directories(output);
    const auto image = output / L"changing-image.png";
    const auto document = output / L"image-lifecycle.md";
    std::string markdown = "![lifecycle probe](changing-image.png)\n\n# Following text\n\n";
    for (int i = 0; i < 100; ++i) markdown += "Scroll-away fixture paragraph.\n\n";
    { std::ofstream file(document, std::ios::binary); file << markdown; }
    WriteImage(image, 100, 100, Color(255, 220, 20, 20));
    // No visible window and no application session are needed for this isolated
    // renderer probe. WM_PRINTCLIENT exercises its real GDI+ drawing path.
    HWND parent = CreateWindowExW(0, L"STATIC", L"MMViewer image lifecycle probe", WS_OVERLAPPEDWINDOW,
        0, 0, 800, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(parent != nullptr, "probe parent window");
    Pixels initial, offscreen, resized, changed, fresh, recovered, oversized, missing;
    std::uintmax_t replacementBytes = 0;
    {
        mm::NativeDocumentView view;
        Require(view.Create(parent, 1) != nullptr, "probe viewer window");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 760, 540, SWP_NOZORDER | SWP_NOACTIVATE);
        view.SetDocument(markdown, document.native(), 16, false);
        initial = Snapshot(view.Handle(), output / L"01-initial-100x100-red.png");
        Require(initial.red >= 9000, "initial permitted image is visible");
        const int initialRange = ScrollRange(view.Handle());
        // Even while the old raster is visible, its decoder must no longer hold
        // the original file open. The next raster reload reads this replacement.
        WriteImage(image, 240, 60, Color(255, 20, 200, 40));
        Require(Snapshot(view.Handle(), output / L"01b-visible-raster-remains-cached.png").red >= 9000,
            "ordinary repaint reuses the viewport raster without reopening the changed file");
        view.SetScrollRatio(1);
        offscreen = Snapshot(view.Handle(), output / L"02-image-outside-viewport.png");
        Require(offscreen.red == 0, "image left the viewport before replacement");
        Require(view.ScrollRatio() > 0.95, "viewer reached document end");

        view.SetScrollRatio(0);
        resized = Snapshot(view.Handle(), output / L"03-valid-resize-240x60-green.png");
        // At fractional DPI the image origin can fall between pixel rows. The
        // bicubic edge can lose one row/column from this solid-color threshold.
        Require(resized.green > 13000 && std::abs(resized.GreenWidth() - 240) <= 1 && std::abs(resized.GreenHeight() - 60) <= 1,
            "valid image replacement reflows instead of stretching into the old dimensions");
        Require(ScrollRange(view.Handle()) == initialRange - 40,
            "resized image reflows following text and document scroll range");
        view.SetScrollRatio(1);
        Snapshot(view.Handle(), output / L"04-resized-image-outside-viewport.png");

        // This replacement exceeds the application's 16,384-pixel width limit
        // while using only 20,000 pixels (80 KB uncompressed), not a large file.
        WriteImage(image, 20000, 1, Color(255, 20, 200, 40));
        replacementBytes = fs::file_size(image);
        view.SetScrollRatio(0);
        changed = Snapshot(view.Handle(), output / L"05-same-document-rejects-overwidth.png");
        Require(changed.green == 0 && changed.panel > 1000,
            "same-document reload rejects over-width image and shows failure panel");

        // Invalidate parsing/layout state without changing image bytes. This is
        // the same load path that a newly opened document takes.
        view.InvalidateDocumentCache(document.native());
        view.SetDocument(markdown, document.native(), 16, false);
        view.SetScrollRatio(0);
        fresh = Snapshot(view.Handle(), output / L"06-fresh-parse-rejects-overwidth.png");
        Require(fresh.green == 0, "fresh parse rejects over-width image");

        WriteImage(image, 180, 80, Color(255, 20, 200, 40));
        view.InvalidateDocumentCache(document.native());
        view.SetDocument(markdown, document.native(), 16, false);
        view.SetScrollRatio(0);
        recovered = Snapshot(view.Handle(), output / L"07-valid-image-recovered-180x80.png");
        Require(recovered.green > 13000 && std::abs(recovered.GreenWidth() - 180) <= 1 && std::abs(recovered.GreenHeight() - 80) <= 1,
            "valid replacement after rejection recovers on document reload");
        view.SetScrollRatio(1);
        SettlePaint(view.Handle());
        // Trailing bytes leave this PNG decodable but exceed the input-size
        // limit. Reject it before constructing a fresh decoder.
        fs::resize_file(image, 20u * 1024u * 1024u + 1u);
        view.SetScrollRatio(0);
        oversized = Snapshot(view.Handle(), output / L"08-same-document-rejects-oversized-file.png");
        Require(oversized.green == 0 && oversized.panel > 1000,
            "same-document reload rechecks the 20 MiB file-size limit");

        WriteImage(image, 100, 100, Color(255, 220, 20, 20));
        view.InvalidateDocumentCache(document.native());
        view.SetDocument(markdown, document.native(), 16, false);
        view.SetScrollRatio(0);
        Require(Snapshot(view.Handle(), output / L"09-image-before-deletion.png").red >= 9000,
            "permitted image reloads after oversized replacement");
        view.SetScrollRatio(1);
        SettlePaint(view.Handle());
        Require(fs::remove(image), "image decoder has released its file lock");
        view.SetScrollRatio(0);
        missing = Snapshot(view.Handle(), output / L"10-deleted-image-error-panel.png");
        Require(missing.red == 0 && missing.panel > 1000,
            "deleted image does not retain stale pixels or blank image bounds");
        const auto failureStats = view.GetPerformanceStats();
        for (int i = 0; i < 32; ++i) SettlePaint(view.Handle());
        const auto repaintedStats = view.GetPerformanceStats();
        Require(repaintedStats.layouts == failureStats.layouts &&
            repaintedStats.bufferAllocations == failureStats.bufferAllocations,
            "failed image does not cause repeated layouts or buffer allocation on repaint");
    }
    DestroyWindow(parent);
    std::ofstream report(output / L"result.json");
    report << "{\n"
        << "  \"probe\": \"image-replacement-lifecycle\",\n"
        << "  \"initial_width\": 100,\n"
        << "  \"initial_height\": 100,\n"
        << "  \"replacement_width\": 20000,\n"
        << "  \"replacement_height\": 1,\n"
        << "  \"replacement_bytes\": " << replacementBytes << ",\n"
        << "  \"initial_red_pixels\": " << initial.red << ",\n"
        << "  \"offscreen_red_pixels\": " << offscreen.red << ",\n"
        << "  \"same_document_green_pixels\": " << changed.green << ",\n"
        << "  \"fresh_parse_green_pixels\": " << fresh.green << ",\n"
        << "  \"resized_width\": " << resized.GreenWidth() << ",\n"
        << "  \"resized_height\": " << resized.GreenHeight() << ",\n"
        << "  \"recovered_width\": " << recovered.GreenWidth() << ",\n"
        << "  \"recovered_height\": " << recovered.GreenHeight() << ",\n"
        << "  \"oversized_file_green_pixels\": " << oversized.green << ",\n"
        << "  \"missing_file_red_pixels\": " << missing.red << ",\n"
        << "  \"passed\": true\n}\n";
    Require(bool(report), "probe report output");
    std::cout << "image lifecycle probe: initial red=" << initial.red
        << ", same-document green=" << changed.green << ", fresh-parse green=" << fresh.green
        << ", resized=" << resized.GreenWidth() << 'x' << resized.GreenHeight()
        << ", recovered=" << recovered.GreenWidth() << 'x' << recovered.GreenHeight()
        << ", oversized-file green=" << oversized.green << ", missing-file red=" << missing.red
        << ", PASS\n";
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return 2;
    int result = 1;
    try {
        fs::path output = argc > 1 ? fs::absolute(argv[1]) :
            fs::absolute(fs::path(L"artifacts") / (L"image-lifecycle-" + std::to_wstring(GetCurrentProcessId())));
        result = Probe(output);
    } catch (const std::exception& error) {
        std::cerr << "image lifecycle probe: " << error.what() << '\n';
    }
    GdiplusShutdown(token);
    return result;
}
