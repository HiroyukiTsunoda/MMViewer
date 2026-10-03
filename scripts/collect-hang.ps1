param(
    [int]$ProcessId = 0,
    [switch]$NoDump
)

# Run before ending MMViewer in Task Manager. This only reads the process;
# it does not close, restart, reposition, or suspend any application window.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRoot ('artifacts\hang-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $taskOutput | Out-Null

if (-not ('MMViewerHangCapture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class MMViewerHangCapture {
    public delegate bool EnumWindow(IntPtr window, IntPtr data);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    public sealed class Window {
        public long Handle;
        public uint Thread;
        public string Class, Title;
        public bool Visible, Minimized, Enabled, Ready, Closing, OnMonitor;
        public bool RectAvailable, Responding;
        public int ProbeError;
        public long Owner;
        public Rect Bounds;
    }
    public sealed class ThreadWindows {
        public uint Thread;
        public bool Enumerated;
        public int Error;
        public string TargetDesktop, CollectorDesktop;
        public List<Window> Windows = new List<Window>();
    }
    [DllImport("user32.dll", SetLastError=true)]
    static extern bool EnumThreadWindows(uint thread, EnumWindow callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern int GetClassName(IntPtr window, StringBuilder text, int length);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromRect(ref Rect rect, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll", SetLastError=true)]
    static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr w, IntPtr l,
        uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder name,
        uint length, out uint needed);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] static extern void SetLastError(uint error);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, uint process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("dbghelp.dll", SetLastError=true)]
    static extern bool MiniDumpWriteDump(IntPtr process, uint id, IntPtr file, uint type,
        IntPtr exception, IntPtr stream, IntPtr callback);

    static string DesktopName(uint thread) {
        var name = new StringBuilder(256);
        uint needed;
        return GetUserObjectInformation(GetThreadDesktop(thread), 2, name,
            (uint)(name.Capacity * 2), out needed) ? name.ToString() : "unavailable";
    }

    public static ThreadWindows Inspect(uint thread) {
        var result = new ThreadWindows { Thread = thread,
            TargetDesktop = DesktopName(thread), CollectorDesktop = DesktopName(GetCurrentThreadId()) };
        // Compare physical window coordinates with physical monitor bounds.
        var previousDpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
        try {
            SetLastError(0);
            result.Enumerated = EnumThreadWindows(thread, (window, unused) => {
                var name = new StringBuilder(256);
                var title = new StringBuilder(1024);
                GetClassName(window, name, name.Capacity);
                GetWindowText(window, title, title.Capacity);
                Rect bounds;
                bool rectAvailable = GetWindowRect(window, out bounds);
                UIntPtr ignored;
                SetLastError(0);
                // WM_NULL probes responsiveness without changing application state.
                bool responds = SendMessageTimeout(window, 0, UIntPtr.Zero, IntPtr.Zero,
                    0x22, 500, out ignored) != IntPtr.Zero;
                int probeError = responds ? 0 : Marshal.GetLastWin32Error();
                result.Windows.Add(new Window {
                    Handle = window.ToInt64(), Thread = thread,
                    Class = name.ToString(), Title = title.ToString(),
                    Visible = IsWindowVisible(window), Minimized = IsIconic(window),
                    Enabled = IsWindowEnabled(window), Owner = GetWindow(window, 4).ToInt64(),
                    Ready = GetProp(window, "MMViewer.Ready") != IntPtr.Zero,
                    Closing = GetProp(window, "MMViewer.Closing") != IntPtr.Zero,
                    RectAvailable = rectAvailable, Bounds = bounds,
                    OnMonitor = rectAvailable && MonitorFromRect(ref bounds, 0) != IntPtr.Zero,
                    Responding = responds, ProbeError = probeError
                });
                return true;
            }, IntPtr.Zero);
            result.Error = result.Enumerated ? 0 : Marshal.GetLastWin32Error();
        } finally {
            if (previousDpi != IntPtr.Zero) SetThreadDpiAwarenessContext(previousDpi);
        }
        return result;
    }

    public static void Dump(uint id, string path) {
        var process = OpenProcess(0x410, false, id);
        if (process == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        try {
            using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                // Stacks, thread metadata, and unloaded modules; no full-memory dump.
                if (!MiniDumpWriteDump(process, id, file.SafeFileHandle.DangerousGetHandle(),
                    0x1020, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero))
                    throw new System.ComponentModel.Win32Exception();
            }
        } finally { CloseHandle(process); }
    }
}
'@
}

$taskProcesses = @(Get-Process -Name MMViewer -ErrorAction SilentlyContinue |
    Where-Object { $ProcessId -eq 0 -or $_.Id -eq $ProcessId })
$taskRecords = @()
foreach ($taskProcess in $taskProcesses) {
    $taskRecord = [ordered]@{ id = $taskProcess.Id; capturedAt = (Get-Date).ToString('o') }
    try {
        $taskStarted = $taskProcess.StartTime
        $taskRecord.startedAt = $taskStarted.ToString('o')
        $taskRecord.path = $taskProcess.Path
        $taskRecord.cpuSeconds = $taskProcess.TotalProcessorTime.TotalSeconds
        $taskRecord.privateBytes = $taskProcess.PrivateMemorySize64
        $taskRecord.handles = $taskProcess.HandleCount
        $taskRecord.threads = @($taskProcess.Threads | ForEach-Object {
            $taskThread = $_
            $taskThreadRecord = [ordered]@{ id = $taskThread.Id; state = $taskThread.ThreadState.ToString() }
            if ($taskThread.ThreadState -eq [Diagnostics.ThreadState]::Wait) {
                $taskThreadRecord.waitReason = $taskThread.WaitReason.ToString()
            }
            $taskThreadRecord.windows = [MMViewerHangCapture]::Inspect($taskThread.Id)
            $taskThreadRecord
        })
        # Save window evidence first, even if a later dump cannot be written.
        $taskRecord | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $taskOutput "process-$($taskProcess.Id).json") -Encoding utf8
        if (-not $NoDump) {
            $taskCurrent = Get-Process -Id $taskProcess.Id -ErrorAction Stop
            if ($taskCurrent.ProcessName -ne 'MMViewer' -or $taskCurrent.StartTime -ne $taskStarted) {
                throw '採取中に対象プロセスが終了・交代しました。'
            }
            $taskDump = Join-Path $taskOutput "MMViewer-$($taskProcess.Id).dmp"
            [MMViewerHangCapture]::Dump($taskProcess.Id, $taskDump)
            $taskRecord.dump = $taskDump
        }
    } catch {
        $taskRecord.error = $_.Exception.Message
    }
    $taskRecords += $taskRecord
}

$taskEvidence = [ordered]@{
    capturedAt = (Get-Date).ToString('o')
    collectorSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
    note = '強制終了前の状態。ウィンドウの列挙・応答検査は実行デスクトップや権限に制約される場合があります。'
    processes = $taskRecords
}
$taskEvidence | ConvertTo-Json -Depth 14 |
    Set-Content -LiteralPath (Join-Path $taskOutput 'capture.json') -Encoding utf8

Write-Output "診断資料: $taskOutput"
if (-not $taskProcesses.Count) {
    Write-Output '対象のMMViewerプロセスは見つかりませんでした。'
} else {
    foreach ($taskRecord in $taskRecords) {
        Write-Output ("PID {0}: {1}" -f $taskRecord.id, $(if ($taskRecord.error) { $taskRecord.error } else { '採取完了' }))
    }
}
if (@($taskRecords | Where-Object { $_.error }).Count) { exit 1 }
