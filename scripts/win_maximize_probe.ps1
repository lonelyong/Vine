#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Maximize a running Vine window, so the resize path can be observed on Windows.

.DESCRIPTION
    The Windows counterpart of the X11 pixel probe (scripts/xwin2ppm.py): it gives a repeatable way to
    trigger "the window grew" without a human dragging the frame. It only does what a user does when
    clicking the maximize button -- ShowWindow(SW_MAXIMIZE) on the process' main window -- so anything
    the application does in response is the application's own behaviour.

    Used to measure the resize transient described in .ai/bugs/vsg-maximize-black-band.md: run the app
    with its log redirected, call this script, and compare the timestamps of the window/graph/slot
    instrumentation with the moment the window changed size.

.EXAMPLE
    cd build/bin/Debug
    cmd /c "Vine.exe > trace.txt 2>&1"      # in another shell, or with Start-Process
    pwsh scripts/win_maximize_probe.ps1 -WaitSeconds 8
#>
param(
    [string]$ProcessName = 'Vine',
    # How long to wait for the application to start (and build its first frames) before maximizing.
    [int]$WaitSeconds = 8
)

Add-Type -Namespace VW -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@

Start-Sleep -Seconds $WaitSeconds
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $proc) {
    Write-Output "no process '$ProcessName'"
    exit 1
}
$handle = $proc.MainWindowHandle
Write-Output ("hwnd={0}" -f $handle)
if ($handle -eq 0) {
    Write-Output 'the process has no main window handle'
    exit 1
}
[VW.Win]::ShowWindow($handle, 3) | Out-Null   # SW_MAXIMIZE
Write-Output 'maximized'
