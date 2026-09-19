#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Maximize a running Vine window, so the resize path can be observed on Windows.

.DESCRIPTION
    The Windows counterpart of the X11 pixel probe (scripts/xwin2ppm.py): it gives a repeatable way to
    trigger "the window grew" (or shrank) without a human dragging the frame. It only does what a user
    does when clicking the window's buttons -- ShowWindow(SW_MAXIMIZE) / (SW_RESTORE) on the process'
    toplevel window -- so anything the application does in response is the application's own behaviour.

    Used to measure the resize transient described in .ai/bugs/vsg-maximize-black-band.md and the
    in-place resize in .ai/design/vsg-target-resize-in-place.md: run the app with its log redirected,
    call this script, and compare the timestamps of the window/target/slot instrumentation (the
    "build profile" line) with the moment the window changed size.

    The window is found by ENUMERATING the process' visible toplevel windows and taking the largest:
    this machine's Vine process reports MainWindowHandle == 0, which is what this script used to rely
    on, so it exited with "the process has no main window handle" instead of maximizing anything.

.EXAMPLE
    cd build/bin/Debug
    cmd /c "Vine.exe > trace.txt 2>&1"      # in another shell, or with Start-Process
    pwsh scripts/win_maximize_probe.ps1 -WaitSeconds 8
    pwsh scripts/win_maximize_probe.ps1 -ShowCommand 9      # back to the restored size
    pwsh scripts/win_maximize_probe.ps1 -ShotPath shot.png  # and capture the client area
#>
param(
    [string]$ProcessName = 'Vine',
    # How long to wait for the application to start (and build its first frames) before maximizing.
    [int]$WaitSeconds = 8,
    # 3 = SW_MAXIMIZE, 9 = SW_RESTORE (the two directions a resize is measured in).
    [int]$ShowCommand = 3,
    # When set, capture the window to this PNG (see -ShotDelayMs).
    [string]$ShotPath = '',
    # How long to wait after the show command before capturing: short enough to catch the transient.
    [int]$ShotDelayMs = 250
)

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class VineWinProbe
{
    private delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    /// <summary>Largest toplevel visible window of the process, or IntPtr.Zero.</summary>
    ///
    /// The process' MainWindowHandle is 0 on this machine (the app's toplevel is not the one Win32
    /// picks), so the window is found by enumeration instead: the largest VISIBLE toplevel that the
    /// process owns is the one a user would drag, which is what maximising has to act on.
    public static IntPtr FindLargest(int pid)
    {
        IntPtr best = IntPtr.Zero;
        long best_area = 0;
        EnumWindows((hWnd, _) =>
        {
            uint owner;
            GetWindowThreadProcessId(hWnd, out owner);
            if (owner != (uint)pid || !IsWindowVisible(hWnd) || GetParent(hWnd) != IntPtr.Zero) return true;
            RECT rect;
            if (!GetClientRect(hWnd, out rect)) return true;
            long area = (long)(rect.Right - rect.Left) * (rect.Bottom - rect.Top);
            if (area > best_area) { best_area = area; best = hWnd; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static string Describe(IntPtr hWnd)
    {
        RECT rect;
        GetClientRect(hWnd, out rect);
        StringBuilder title = new StringBuilder(256);
        GetWindowText(hWnd, title, title.Capacity);
        return string.Format("hwnd=0x{0:X} client={1}x{2} title='{3}'", (long)hWnd,
                             rect.Right - rect.Left, rect.Bottom - rect.Top, title);
    }

    public static bool Show(IntPtr hWnd, int cmd) { return ShowWindow(hWnd, cmd); }

    /// <summary>Screen rectangle of the window including its frame (the capture region).</summary>
    public static int[] ScreenRect(IntPtr hWnd)
    {
        RECT rect;
        GetWindowRect(hWnd, out rect);
        return new int[] { rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top };
    }
}
'@

Start-Sleep -Seconds $WaitSeconds
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $proc) {
    Write-Output "no process '$ProcessName'"
    exit 1
}
$handle = [VineWinProbe]::FindLargest($proc.Id)
if ($handle -eq [IntPtr]::Zero) {
    Write-Output 'the process has no visible toplevel window'
    exit 1
}
Write-Output ([VineWinProbe]::Describe($handle))
[VineWinProbe]::Show($handle, $ShowCommand) | Out-Null
Write-Output ("shown with command {0}" -f $ShowCommand)

if ($ShotPath -ne '') {
    # A note on what a capture is worth here: a Vulkan window's content is not in the GDI surface, so
    # CopyFromScreen may show the desktop instead of the frame. It is kept because the black-band bug it
    # was written for (.ai/bugs/vsg-maximize-black-band.md) is about the CLIENT AREA, which this does
    # show; pixel-level proof lives in the self-test (vsg_backend_selftest, see selftest_resize.cpp).
    Start-Sleep -Milliseconds $ShotDelayMs
    Add-Type -AssemblyName System.Drawing
    $r = [VineWinProbe]::ScreenRect($handle)
    $bmp = New-Object System.Drawing.Bitmap($r[2], $r[3])
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    $graphics.CopyFromScreen($r[0], $r[1], 0, 0, $bmp.Size)
    $bmp.Save($ShotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bmp.Dispose()
    Write-Output ("captured {0} ({1}x{2}) after {3} ms" -f $ShotPath, $r[2], $r[3], $ShotDelayMs)
}

Start-Sleep -Milliseconds 1500
Write-Output ([VineWinProbe]::Describe($handle))
