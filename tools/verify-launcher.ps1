# verify-launcher.ps1 - smoke check for the launcher's tray plumbing.
#
# The launcher is the one application that has to survive being invisible, so
# its failure modes are not visible ones: a stray console window, a process with
# no window and no tray icon, a second copy that manages the same two apps
# again. This checks each of those against the real binary.
#
# It drives the tray window directly (FindWindow + WM_COMMAND) because the
# actions are what matter, not the clicking. That is also why the tray window
# handles WM_COMMAND as well as the popup menu returning its result: one entry
# point per action, reachable from a script.
#
# A NOTE ON THIS MACHINE, because it changes how to read the output: this
# session refuses Shell_NotifyIcon for *any* program. A canonical 20-line probe
# fails with error 5 (ACCESS_DENIED) for every NOTIFYICONDATA size, and it fails
# the same way when run by the Task Scheduler, outside this harness entirely. So
# when the icon cannot be created the script checks the documented fallback
# instead: the window must be shown, and closing it must really exit, so nothing
# is ever stranded with no way to reach it.

$ErrorActionPreference = 'Stop'

# This script lives in tools/, so the repository root is its parent.
$Repo = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $Repo 'bin'
$Exe = Join-Path $Bin 'media-dashboard-cpp.exe'
$Log = Join-Path $Bin 'verify-launcher.err'

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class LauncherProbe {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);

    // Enumeration by pid, not by title: it does not depend on a caption, and it
    // still finds the window when it is hidden.
    public static IntPtr Find(uint pid, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid) return true;
            var c = new StringBuilder(256); GetClassNameW(h, c, 256);
            if (c.ToString() == className) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$MenuToggleWindow = 5
$MenuQuit = 6

$failures = @()
function Check([string]$what, [bool]$ok, [string]$detail) {
    Write-Host ("{0,-46}: {1}{2}" -f $what, $(if ($ok) { 'PASS' } else { 'FAIL' }),
        $(if ($detail) { "  ($detail)" } else { '' }))
    if (-not $ok) { $script:failures += $what }
}

Get-Process -Name 'media-dashboard-cpp' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item $Log -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $Exe -ArgumentList '--tray' -WorkingDirectory $Bin `
    -PassThru -NoNewWindow -RedirectStandardError $Log
Start-Sleep -Seconds 3

$logText = Get-Content $Log -Raw -ErrorAction SilentlyContinue
$hasTray = $logText -match 'launcher icon added to the notification area'
Write-Host "environment: tray icon registration $(if ($hasTray) { 'works' } else { 'is refused here (error 5)' })"
Write-Host ''

Check 'launcher started' (-not $proc.HasExited) "pid=$($proc.Id)"

$tray = [LauncherProbe]::Find([uint32]$proc.Id, 'MediaPlayerAppLauncherTray')
Check 'tray message window exists' ($tray -ne [IntPtr]::Zero) "hwnd=$tray"

$window = [LauncherProbe]::Find([uint32]$proc.Id, 'GLFW30')
Check 'launcher window exists' ($window -ne [IntPtr]::Zero) "hwnd=$window"

if ($hasTray) {
    Check 'starts hidden for --tray' (-not [LauncherProbe]::IsWindowVisible($window)) ''
} else {
    # The safety net: no icon must not mean no way to reach the launcher.
    Check 'no icon -> window shown' ([LauncherProbe]::IsWindowVisible($window)) 'not left invisible'
}

# --- the tray's own actions ----------------------------------------------
[void][LauncherProbe]::PostMessage($tray, $WM_COMMAND, [IntPtr]$MenuToggleWindow, [IntPtr]::Zero)
Start-Sleep -Seconds 2
Check 'tray action hides the window' (-not [LauncherProbe]::IsWindowVisible($window)) ''

[void][LauncherProbe]::PostMessage($tray, $WM_COMMAND, [IntPtr]$MenuToggleWindow, [IntPtr]::Zero)
Start-Sleep -Seconds 2
Check 'tray action shows the window' ([LauncherProbe]::IsWindowVisible($window)) ''

[void][LauncherProbe]::PostMessage($tray, $WM_COMMAND, [IntPtr]$MenuQuit, [IntPtr]::Zero)
$exited = $proc.WaitForExit(8000)
Check 'tray QUIT exits cleanly' $exited ''
if (-not $exited) { $proc.Kill() }

# --- a second launcher must not appear ------------------------------------
Start-Sleep -Milliseconds 500
Remove-Item $Log -ErrorAction SilentlyContinue
$first = Start-Process -FilePath $Exe -ArgumentList '--tray' -WorkingDirectory $Bin `
    -PassThru -NoNewWindow -RedirectStandardError $Log
Start-Sleep -Seconds 3
$second = Start-Process -FilePath $Exe -ArgumentList '--tray' -WorkingDirectory $Bin `
    -PassThru -NoNewWindow -RedirectStandardError (Join-Path $Bin 'verify-launcher2.err')
$second.WaitForExit(10000) | Out-Null
Start-Sleep -Seconds 1
$count = @(Get-Process -Name 'media-dashboard-cpp' -ErrorAction SilentlyContinue).Count
Check 'second launch does not add a copy' ($count -eq 1) "instances=$count"

# --- closing the window, in whichever mode we are in ---------------------
$window2 = [LauncherProbe]::Find([uint32]$first.Id, 'GLFW30')
[void][LauncherProbe]::PostMessage($window2, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 2
if ($hasTray) {
    Check 'closing hides but keeps running' (-not $first.HasExited) 'the always-on requirement'
    $tray2 = [LauncherProbe]::Find([uint32]$first.Id, 'MediaPlayerAppLauncherTray')
    [void][LauncherProbe]::PostMessage($tray2, $WM_COMMAND, [IntPtr]$MenuQuit, [IntPtr]::Zero)
} else {
    Check 'no icon -> closing really exits' $first.HasExited 'documented fallback, nothing stranded'
}
if (-not $first.HasExited) { $first.Kill() }
Remove-Item (Join-Path $Bin 'verify-launcher2.err') -ErrorAction SilentlyContinue

# --- --no-tray: the ordinary-window escape hatch --------------------------
# On a machine where the shell refuses Shell_NotifyIcon for good, --no-tray is
# how a person gets a launcher they can actually use. It must be an ordinary
# window: visible, no tray message window, and closing it exits.
Start-Sleep -Milliseconds 500
$noTrayLog = Join-Path $Bin 'verify-launcher-notray.err'
Remove-Item $noTrayLog -ErrorAction SilentlyContinue
$plain = Start-Process -FilePath $Exe -ArgumentList '--no-tray' -WorkingDirectory $Bin `
    -PassThru -NoNewWindow -RedirectStandardError $noTrayLog
Start-Sleep -Seconds 3

$plainWindow = [LauncherProbe]::Find([uint32]$plain.Id, 'GLFW30')
$plainTray = [LauncherProbe]::Find([uint32]$plain.Id, 'MediaPlayerAppLauncherTray')
Check '--no-tray still makes a window' ($plainWindow -ne [IntPtr]::Zero) ''
Check '--no-tray shows that window' ([LauncherProbe]::IsWindowVisible($plainWindow)) ''
Check '--no-tray creates no tray window' ($plainTray -eq [IntPtr]::Zero) 'asked not to'
Check '--no-tray says why in its log' `
    ((Get-Content $noTrayLog -Raw -ErrorAction SilentlyContinue) -match '--no-tray') ''

[void][LauncherProbe]::PostMessage($plainWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
$plainExited = $plain.WaitForExit(8000)
Check '--no-tray closing really exits' $plainExited ''
if (-not $plainExited) { $plain.Kill() }

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "FAILURES: $($failures -join ', ')"
    Get-Content $Log -ErrorAction SilentlyContinue | Select-Object -Last 12 |
        ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'launcher plumbing verified'
exit 0
