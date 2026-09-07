#include "single_instance.h"
#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);DispatchMessageW(&message);
    }
}

template<class Predicate> void Until(Predicate done) {
    const auto deadline = GetTickCount64() + 10000;
    while (!done() && GetTickCount64() < deadline) { Pump();Sleep(1); }
    Check(done(), "Timed out waiting for launchers");
}

struct Receiver {
    std::wstring name = L"MMViewer.InstanceTests." + std::to_wstring(GetCurrentProcessId());
    std::wstring mutexName = L"Local\\" + name;
    HWND window = nullptr;
    bool accept = true;
    std::vector<std::wstring> received;
    Receiver() {
        WNDCLASSW type{};type.lpfnWndProc = Proc;type.hInstance = GetModuleHandleW(nullptr);type.lpszClassName = name.c_str();
        Check(RegisterClassW(&type) != 0, "Cannot register test receiver");
    }
    ~Receiver() {
        if (window) DestroyWindow(window);
        UnregisterClassW(name.c_str(), GetModuleHandleW(nullptr));
    }
    void Create(bool ready) {
        window = CreateWindowExW(0, name.c_str(), L"Hidden instance test", WS_OVERLAPPED,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        Check(window != nullptr, "Cannot create test receiver");
        if (ready) Ready();
    }
    void Ready() { Check(SetPropW(window, mm::InstanceReadyProperty, HANDLE(1)) != FALSE, "Cannot mark test receiver ready"); }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(self));
        }
        if (message == WM_COPYDATA && self) {
            const auto data = reinterpret_cast<const COPYDATASTRUCT*>(l);
            if (!self->accept || data->dwData != 0x4d4d || !data->lpData
                || data->cbData % sizeof(wchar_t)) return FALSE;
            self->received.emplace_back(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            return TRUE;
        }
        return DefWindowProcW(window, message, w, l);
    }
};

std::wstring Payload(int index) {
    auto text = L"C:\\日本語 フォルダ\\資料 " + std::to_wstring(index) + L".md";
    text.push_back(L'\0');text.push_back(L'\0');return text;
}

void ConcurrentLaunches() {
    Receiver receiver;
    constexpr int count = 6;
    std::barrier start(count + 1);
    std::atomic_int primaries = 0, finished = 0;
    std::vector<mm::InstanceLaunch> results(count, mm::InstanceLaunch::Failed);
    std::vector<std::jthread> launchers;
    for (int index = 0; index < count; ++index) launchers.emplace_back([&, index](std::stop_token stop) {
        mm::SingleInstance gate(receiver.mutexName.c_str());
        start.arrive_and_wait();
        results[index] = gate.Route(receiver.name.c_str(), Payload(index));
        if (results[index] == mm::InstanceLaunch::Primary) ++primaries;
        ++finished;
        // Model the primary's full lifetime, including startup before HWND.
        if (results[index] == mm::InstanceLaunch::Primary) while (!stop.stop_requested()) Sleep(1);
    });
    start.arrive_and_wait();
    Until([&] { return primaries.load() != 0; });
    receiver.Create(false);
    const auto beforeReady = GetTickCount64() + 100;
    while (GetTickCount64() < beforeReady) { Pump();Sleep(1); }
    Check(primaries == 1 && finished == 1 && receiver.received.empty(),
        "Simultaneous launches created another primary or forwarded before startup was ready");
    receiver.Ready();
    Until([&] { return finished == count; });
    Check(primaries == 1 && receiver.received.size() == count - 1, "Concurrent requests were lost or launched multiple primaries");
    for (int index = 0; index < count; ++index) if (results[index] != mm::InstanceLaunch::Primary) {
        Check(results[index] == mm::InstanceLaunch::Forwarded, "Secondary launcher failed to forward");
        size_t copies = 0;for (const auto& payload : receiver.received) if (payload == Payload(index)) ++copies;
        Check(copies == 1, "Unicode file path was changed, duplicated or lost");
    }
    Check(!IsWindowVisible(receiver.window), "Test receiver must remain hidden");
    // jthread destruction requests stop and joins, releasing the primary.
}

void FailureAndRecovery() {
    Receiver receiver;
    {
        mm::SingleInstance primary(receiver.mutexName.c_str());
        Check(primary.Route(receiver.name.c_str(), Payload(0)) == mm::InstanceLaunch::Primary, "Cannot acquire primary");
        mm::InstanceLaunch outcome = mm::InstanceLaunch::Primary;
        std::jthread waiting([&] {
            mm::SingleInstance secondary(receiver.mutexName.c_str());
            outcome = secondary.Route(receiver.name.c_str(), Payload(1), 60);
        });
        waiting.join();
        Check(outcome == mm::InstanceLaunch::Failed, "Startup timeout launched a second primary");
        receiver.Create(true);
        // The UI deliberately does not pump until this sender times out.
        std::jthread hung([&] {
            mm::SingleInstance secondary(receiver.mutexName.c_str());
            outcome = secondary.Route(receiver.name.c_str(), Payload(2), 1000, 60);
        });
        hung.join();
        Check(outcome == mm::InstanceLaunch::Failed, "Unresponsive receiver was treated as a successful delivery");
        Pump();
        receiver.accept = false;
        std::atomic_bool finished = false;
        std::jthread rejected([&] {
            mm::SingleInstance secondary(receiver.mutexName.c_str());
            outcome = secondary.Route(receiver.name.c_str(), Payload(3));finished = true;
        });
        Until([&] { return finished.load(); });rejected.join();
        Check(outcome == mm::InstanceLaunch::Failed, "Receiver rejection was ignored");
        Check(SetPropW(receiver.window, mm::InstanceClosingProperty, HANDLE(1)) != FALSE, "Cannot mark closing");
        finished = false;
        std::jthread closing([&] {
            mm::SingleInstance secondary(receiver.mutexName.c_str());
            outcome = secondary.Route(receiver.name.c_str(), Payload(4), 60);finished = true;
        });
        Until([&] { return finished.load(); });closing.join();
        Check(outcome == mm::InstanceLaunch::Failed, "A closing primary lost exclusion or accepted another request");
    }
    // The hidden closing HWND may outlive App cleanup; it must not prevent
    // a new primary after the lifetime mutex is released.
    {
        mm::SingleInstance restarted(receiver.mutexName.c_str());
        Check(restarted.Route(receiver.name.c_str(), Payload(5)) == mm::InstanceLaunch::Primary,
            "Normal shutdown left the instance locked");
    }
    HANDLE abandoned = nullptr;
    std::jthread crashed([&] { abandoned = CreateMutexW(nullptr, TRUE, receiver.mutexName.c_str()); });
    crashed.join();
    Check(abandoned != nullptr, "Cannot create abandoned-mutex fixture");
    {
        mm::SingleInstance recovered(receiver.mutexName.c_str());
        const auto result = recovered.Route(receiver.name.c_str(), Payload(6));
        CloseHandle(abandoned);
        Check(result == mm::InstanceLaunch::Primary, "Abandoned owner prevented recovery");
    }
}

void LegacyReceiver() {
    Receiver receiver;receiver.Create(false);
    std::atomic_bool finished = false;
    mm::InstanceLaunch outcome = mm::InstanceLaunch::Failed;
    std::jthread launcher([&] {
        mm::SingleInstance gate(receiver.mutexName.c_str());
        outcome = gate.Route(receiver.name.c_str(), Payload(0));finished = true;
    });
    Until([&] { return finished.load(); });launcher.join();
    Check(outcome == mm::InstanceLaunch::Forwarded && receiver.received.size() == 1,
        "An already running older MMViewer was not reused");
}
} // namespace

int main() {
    try {
        ConcurrentLaunches();FailureAndRecovery();LegacyReceiver();
        std::cout << "PASS single instance: concurrent startup, readiness, Unicode forwarding, timeout/rejection, shutdown, abandonment, legacy receiver\n";
    } catch (const std::exception& error) {
        std::cerr << "single_instance_tests FAILED: " << error.what() << '\n';return 1;
    }
    return 0;
}
