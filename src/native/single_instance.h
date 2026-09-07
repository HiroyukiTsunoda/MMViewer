#pragma once
#include <windows.h>
#include <string>

namespace mm {

inline constexpr wchar_t InstanceMutexName[] = L"Local\\MMViewer.Native.Main.v1.Instance";
inline constexpr wchar_t InstanceReadyProperty[] = L"MMViewer.Ready";
inline constexpr wchar_t InstanceClosingProperty[] = L"MMViewer.Closing";

enum class InstanceLaunch { Primary, Forwarded, Failed };

// The launching thread owns the mutex until all application cleanup is done.
// Retaining only a named handle (or checking ERROR_ALREADY_EXISTS) would not
// recover correctly when a failed owner leaves other launchers waiting.
class SingleInstance {
    HANDLE mutex_ = nullptr;
    bool owned_ = false;

    static HWND FindReceiver(const wchar_t* windowClass, bool requireReady) {
        for (auto window = FindWindowExW(nullptr, nullptr, windowClass, nullptr); window;
             window = FindWindowExW(nullptr, window, windowClass, nullptr)) {
            if (!GetPropW(window, InstanceClosingProperty)
                && (!requireReady || GetPropW(window, InstanceReadyProperty))) return window;
        }
        return nullptr;
    }

    static InstanceLaunch Forward(HWND receiver, const std::wstring& payload, DWORD timeoutMs) {
        DWORD process = 0;
        GetWindowThreadProcessId(receiver, &process);
        if (process) AllowSetForegroundWindow(process);
        COPYDATASTRUCT data{0x4d4d, DWORD(payload.size() * sizeof(wchar_t)),
            const_cast<wchar_t*>(payload.data())};
        DWORD_PTR accepted = 0;
        // Never retry an uncertain delivery: the receiver may have already
        // opened the file. In particular, a timeout must not launch a new UI.
        if (!SendMessageTimeoutW(receiver, WM_COPYDATA, 0, LPARAM(&data),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, timeoutMs, &accepted) || !accepted)
            return InstanceLaunch::Failed;
        if (IsIconic(receiver)) ShowWindowAsync(receiver, SW_RESTORE);
        SetForegroundWindow(receiver);
        return InstanceLaunch::Forwarded;
    }

public:
    explicit SingleInstance(const wchar_t* name = InstanceMutexName)
        : mutex_(CreateMutexW(nullptr, FALSE, name)) {}
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    ~SingleInstance() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }

    InstanceLaunch Route(const wchar_t* windowClass, const std::wstring& payload,
                         DWORD startupTimeoutMs = 10000, DWORD sendTimeoutMs = 2000) {
        if (!mutex_ || owned_ || payload.empty() || payload.back() != L'\0'
            || payload.size() > (1024 * 1024) / sizeof(wchar_t)) return InstanceLaunch::Failed;
        const auto started = GetTickCount64();
        DWORD waitMs = 0;
        for (;;) {
            const auto result = WaitForSingleObject(mutex_, waitMs);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
                owned_ = true;
                // Also reuse an older MMViewer that predates this mutex and
                // readiness property. Its window is already discoverable.
                if (auto receiver = FindReceiver(windowClass, false)) return Forward(receiver, payload, sendTimeoutMs);
                return InstanceLaunch::Primary;
            }
            if (result != WAIT_TIMEOUT) return InstanceLaunch::Failed;
            // Window creation alone is insufficient: Restore/OpenPaths may
            // still change the active tab before startup reaches its loop.
            if (auto receiver = FindReceiver(windowClass, true)) return Forward(receiver, payload, sendTimeoutMs);
            const auto elapsed = GetTickCount64() - started;
            if (elapsed >= startupTimeoutMs) return InstanceLaunch::Failed;
            waitMs = DWORD((startupTimeoutMs - elapsed) < 50 ? startupTimeoutMs - elapsed : 50);
        }
    }
};

} // namespace mm
