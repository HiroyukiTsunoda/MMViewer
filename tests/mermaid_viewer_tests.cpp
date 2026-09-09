#include "viewer.h"
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Paint(mm::NativeDocumentView& view) {
    InvalidateRect(view.Handle(), nullptr, FALSE);
    SendMessageW(view.Handle(), WM_PAINT, 0, 0);
}
void Wait(mm::NativeDocumentView& view, size_t rendered, size_t failed = 0) {
    auto deadline = GetTickCount64() + 45000;
    do {
        Pump(); Paint(view);
        auto stats = view.GetPerformanceStats();
        if (!stats.pendingDiagrams && stats.renderedDiagrams == rendered && stats.failedDiagrams == failed) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("Native Mermaid display did not complete");
}
void Snapshot(mm::NativeDocumentView& view, const fs::path& path) {
    Paint(view);
    RECT rect{}; GetClientRect(view.Handle(), &rect);
    Gdiplus::Bitmap bitmap(rect.right, rect.bottom, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics graphics(&bitmap); auto dc = graphics.GetHDC();
        SendMessageW(view.Handle(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        graphics.ReleaseHDC(dc);
    }
    CLSID png{}; CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}", &png);
    Check(bitmap.Save(path.c_str(), &png) == Gdiplus::Ok, "Cannot save native Mermaid snapshot");
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    auto parent = CreateWindowW(L"STATIC", L"Mermaid integration", WS_OVERLAPPEDWINDOW,
        0, 0, 1400, 850, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    try {
        auto artifacts = fs::path(__FILE__).parent_path().parent_path() / L"artifacts" / L"official-mermaid";
        fs::create_directories(artifacts);
        const auto profile = artifacts / (L"viewer-profile-" + std::to_wstring(GetCurrentProcessId()));
        mm::NativeDocumentView view;
        Check(view.Create(parent, 1) != nullptr, "Cannot create native viewer");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 1360, 800, SWP_NOZORDER | SWP_NOACTIVATE);
        view.EnableOfficialMermaid(profile.wstring());
        const std::string source = R"(# Mermaid 本文への組み込み

本文の前にある marker-before。

```mermaid
flowchart LR
  subgraph frameN["Frame N"]
    direction LR
    writeN["値を設定する Updater"] -->|アクセサで書き込む| dataN["T 型の FrameData"]
    dataN -->|アクセサで読み取る| readN["値を利用する Updater"]
  end
  subgraph frameNext["Frame N+1"]
    direction LR
    writeNext["値を設定する Updater"] -->|アクセサで書き込む| dataNext["T 型の FrameData"]
    dataNext -->|アクセサで読み取る| readNext["値を利用する Updater"]
  end
```

本文の後にある marker-after。
)";
        auto path = (artifacts / L"native-frame.md").wstring();
        view.SetDocument(source, path, 16, false);
        Check(view.GetPerformanceStats().pendingDiagrams == 1, "Official renderer was not requested");
        Wait(view, 1);
        view.Search(L"marker-after", 0); Check(view.SearchResult().second == 1, "Async layout lost following text");
        view.Search(L"writeNext", 0); Check(view.SearchResult().second == 0, "Rendered diagram leaked into text search");
        view.Search(L"", 0); view.SetScrollRatio(0);
        Snapshot(view, artifacts / L"native-light.png");
        view.SetTheme(true); Paint(view);
        Check(view.GetPerformanceStats().pendingDiagrams == 1, "Theme change did not request a new diagram");
        Wait(view, 1); Snapshot(view, artifacts / L"native-dark.png");
        view.SetSourceMode(true); view.Search(L"writeNext", 0);
        Check(view.SearchResult().second > 0, "Mermaid source is not searchable");
        view.SetSourceMode(false); Wait(view, 1);
        const std::string invalid = "# Error\n\n```mermaid\nlowchart LR\nBroken-->Syntax\n```\n\nmarker-after\n";
        view.SetDocument(invalid, (artifacts / L"invalid.md").wstring()); Wait(view, 0, 1);
        view.Search(L"Broken", 0); Check(view.SearchResult().second == 1, "Syntax error source is not searchable");
        view.Search(L"marker-after", 0); Check(view.SearchResult().second == 1, "Syntax error hid following text");
        const auto cacheHits = view.GetPerformanceStats().cacheHits;
        view.SetTheme(false); view.SetDocument(source, path); Wait(view, 1);
        Check(view.GetPerformanceStats().cacheHits > cacheHits, "Completed diagram tab was not cached");
        view.SetFontSize(24); Paint(view);
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL}; GetScrollInfo(view.Handle(), SB_HORZ, &scroll);
        Check(scroll.nMax > static_cast<int>(scroll.nPage), "Wide diagram cannot scroll horizontally");
        view.SetDocument("```mermaid\nflowchart TB\nAbandoned-->Render\n```", (artifacts / L"pending.md").wstring());
        view.SetDocument("# Replaced\n\nmarker-replaced", (artifacts / L"replaced.md").wstring());
        Wait(view, 0); view.Search(L"marker-replaced", 0);
        Check(view.SearchResult().second == 1, "Pending diagram blocked replacement document");
        auto stats = view.GetPerformanceStats();
        Check(stats.cachedDocuments <= 8 && stats.cachedBytes <= 64 * 1024 * 1024, "Diagram cache exceeded its budget");
        // Destruction with initialization/rendering callbacks in flight must be safe.
        {
            mm::NativeDocumentView closing;
            Check(closing.Create(parent, 2) != nullptr, "Cannot create cancellation viewer");
            closing.EnableOfficialMermaid((artifacts / (L"closing-profile-" + std::to_wstring(GetCurrentProcessId()))).wstring());
            closing.SetDocument(source, path);
        }
        Pump();
        std::cout << "PASS native Mermaid: async layout, themes, source/search, errors, cache, zoom, replacement and close\n";
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; result = 1; }
    DestroyWindow(parent); Pump(); Gdiplus::GdiplusShutdown(token); CoUninitialize(); return result;
}
