#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <psapi.h>
#include "viewer.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
double CpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) throw std::runtime_error("GetProcessTimes");
    auto ticks = [](FILETIME time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10000.0;
}
double PrivateMiB() {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) throw std::runtime_error("GetProcessMemoryInfo");
    return memory.PrivateUsage / (1024.0 * 1024.0);
}
// Uses only the original public viewer API, allowing the exact same workload
// to run against an earlier renderer without diagnostics or cache APIs.
void Measure() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"MMViewer performance", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) throw std::runtime_error("benchmark parent");
    {
        mm::NativeDocumentView view;
        if (!view.Create(parent, 1)) throw std::runtime_error("benchmark view");
        SetWindowPos(view.Handle(), nullptr, 0, 0, 980, 720, SWP_NOZORDER | SWP_NOACTIVATE);
        HDC screen = GetDC(nullptr), target = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 980, 720);
        HGDIOBJ original = SelectObject(target, bitmap);
        ReleaseDC(nullptr, screen);
        if (!target || !bitmap) throw std::runtime_error("benchmark bitmap");
        auto paint = [&] { SendMessageW(view.Handle(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target), PRF_CLIENT); };
        std::string body;
        const std::string paragraph = "日本語の文書を複数タブで切り替え、処理負荷と応答速度を測定します。Markdown viewer benchmark with ordinary text.\n\n";
        while (body.size() < 1024 * 1024) body += paragraph;
        const std::string a = "# Document A\n\n" + body, b = "# Document B\n\n" + body;
        view.SetFontSize(16); view.SetSourceMode(false);
        const auto coldStart = Clock::now(); const auto coldCpu = CpuMilliseconds();
        view.SetDocument(a, L"C:\\MMViewer-benchmark\\a.md"); paint(); paint();
        const auto coldWall = Milliseconds(coldStart), coldCpuTime = CpuMilliseconds() - coldCpu;
        view.SetDocument(b, L"C:\\MMViewer-benchmark\\b.md"); paint(); paint();
        constexpr int switches = 32;
        double prepareMs = 0;
        const auto switchStart = Clock::now(); const auto switchCpu = CpuMilliseconds();
        for (int i = 0; i < switches; ++i) {
            const auto prepareStart = Clock::now();
            view.SetDocument(i % 2 ? b : a, i % 2 ? L"C:\\MMViewer-benchmark\\b.md" : L"C:\\MMViewer-benchmark\\a.md");
            view.SetScrollRatio(0.45);
            prepareMs += Milliseconds(prepareStart);
            paint();
        }
        const auto switchWall = Milliseconds(switchStart), switchCpuTime = CpuMilliseconds() - switchCpu;
        constexpr int scrolls = 64;
        const auto scrollStart = Clock::now(); const auto scrollCpu = CpuMilliseconds();
        for (int i = 0; i < scrolls; ++i) { SendMessageW(view.Handle(), WM_VSCROLL, SB_LINEDOWN, 0); paint(); }
        const auto scrollWall = Milliseconds(scrollStart), scrollCpuTime = CpuMilliseconds() - scrollCpu;
        std::cout << std::fixed << std::setprecision(3)
            << "{\"document_bytes\":" << a.size() << ",\"switches\":" << switches << ",\"scrolls\":" << scrolls
            << ",\"cold_two_paints_wall_ms\":" << coldWall << ",\"cold_cpu_ms\":" << coldCpuTime
            << ",\"switch_prepare_mean_ms\":" << prepareMs / switches
            << ",\"switch_mean_wall_ms\":" << switchWall / switches << ",\"switch_total_cpu_ms\":" << switchCpuTime
            << ",\"scroll_mean_wall_ms\":" << scrollWall / scrolls << ",\"scroll_total_cpu_ms\":" << scrollCpuTime
            << ",\"private_mib\":" << PrivateMiB() << "}\n";
        SelectObject(target, original); DeleteObject(bitmap); DeleteDC(target);
    }
    DestroyWindow(parent);
}
}
int main() {
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 2;
    int result = 0;
    try { Measure(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    Gdiplus::GdiplusShutdown(token); return result;
}
