# screenshot-apps.ps1 - capture the Dashboard and Controller windows as PNGs.
#
# The one thing the headless suite cannot check is whether the interface actually
# looks right, and Dear ImGui gives no way to assert on that from a test. This
# starts each application, finds its window, copies it to a bitmap and writes it
# to a file, so a person (or an agent reading the image) can look at the result.
#
# It is a verification tool, not part of the build. Run it by hand:
#
#     pwsh -File scripts/tools/screenshot-apps.ps1
#     pwsh -File scripts/tools/screenshot-apps.ps1 -Out bin/shots -ControllerOnly
#
# The screenshots land in <repo>/bin/shots by default, which is git-ignored.

param(
    [string]$Out = '',
    # Capture the Controller only. The Dashboard needs the other two running to
    # show anything interesting, so it is the slower of the two.
    [switch]$ControllerOnly,
    [switch]$DashboardOnly,
    # Which Dashboard tab to photograph. The Scripts tab only has content when
    # the Controller is up, so asking for it also starts one.
    [ValidateSet('applications', 'corpus', 'scripts', 'activity')]
    [string]$Tab = 'applications',
    # Press Esc in the Dashboard before capturing, to photograph the Yes/No quit
    # confirmation. A modal is a state no assertion can check, so it gets a
    # picture like everything else here.
    [switch]$Escape,
    # Start a Player too. The Corpus tab reads its playlist from the Player, so
    # without one it can only show the honest "not running" state.
    [switch]$WithPlayer,
    # Folder to hand the Player through /api/media-dir, so the Corpus tab has
    # something to list. Empty leaves whatever mediabus.ini already holds. Note
    # that this PERSISTS, because the Player is the single writer of that setting
    # - send {"path":""} afterwards to leave the machine with nothing chosen.
    [string]$CorpusFolder = '',
    # Wait this long after a window appears before capturing it, so ImGui has
    # laid out and the first frame has been presented.
    [int]$SettleSeconds = 3
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Bin = Join-Path $Repo 'bin'
if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $Bin 'shots'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# Win32 window capture. GetWindowRect gives the frame in screen coordinates;
# CopyFromScreen needs the desktop to be readable, which it is in an interactive
# session and is not over some remote sessions - so this reports that clearly
# rather than writing a black rectangle.
Add-Type -AssemblyName System.Drawing

# Become DPI aware BEFORE anything is measured or captured.
#
# The applications are per-monitor DPI aware, so on a scaled display their windows
# are sized in PHYSICAL pixels: a 1040x760 layout at 150% is a 1040x760 window.
# This script is not aware, so GetClientRect hands it VIRTUALISED coordinates
# (693x506) while PrintWindow fills the bitmap with physical ones - and the capture
# comes out as a crop of the window's top-left corner. That reads exactly like an
# interface bug (a centred dialog appearing clipped off the right edge) which is
# not there, so it is worth the two lines to prevent.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ShotDpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
'@
# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.
$dpiAware = [ShotDpi]::SetProcessDpiAwarenessContext([IntPtr](-4))
if (-not $dpiAware) {
    Write-Warning 'could not become DPI aware; captures on a scaled display will be cropped'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WinCap {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after,
        int X, int Y, int cx, int cy, uint flags);

    // The CLIENT area in screen coordinates: what the application actually drew.
    // Capturing the whole window rect instead would include the title bar and,
    // more importantly, whatever is stacked on top of it - two overlapping apps
    // came out composited into each other until this was used.
    public static int[] ClientRect(IntPtr hWnd) {
        RECT c;
        if (!GetClientRect(hWnd, out c)) { return null; }
        POINT origin = new POINT();
        origin.X = 0; origin.Y = 0;
        if (!ClientToScreen(hWnd, ref origin)) { return null; }
        return new int[] { origin.X, origin.Y, c.Right - c.Left, c.Bottom - c.Top };
    }

    // HWND_TOP, with SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW.
    public static void Raise(IntPtr hWnd) {
        ShowWindow(hWnd, 5);
        SetWindowPos(hWnd, IntPtr.Zero, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);
        BringWindowToTop(hWnd);
        SetForegroundWindow(hWnd);
    }
}
'@

# PrintWindow captures a window's own surface, so what is stacked on top of it
# does not appear in the image. CopyFromScreen cannot do that: with two
# overlapping windows it composites them together, which is what the first
# version of this script produced. In its own Add-Type block because referencing
# System.Drawing from the block above needs the assembly already loaded.
Add-Type @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class WinPrint {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc,
        uint flags);

    // PW_RENDERFULLCONTENT (0x00000002) is what makes this capture a window that
    // draws through a graphics API. Without it an OpenGL window can come back
    // blank, because the window's own WM_PRINT path does not know about the
    // GL surface.
    public static bool Capture(IntPtr hWnd, IntPtr hdc) {
        return PrintWindow(hWnd, hdc, 0x00000002);
    }
}
'@

function Save-WindowShot {
    param(
        [string]$ProcessName,
        [string]$FileName,
        [string]$Label
    )

    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($null -eq $proc) {
        Write-Warning "$Label : no window found for process '$ProcessName'"
        return $false
    }

    [WinCap]::Raise($proc.MainWindowHandle)
    Start-Sleep -Seconds $SettleSeconds

    $rect = [WinCap]::ClientRect($proc.MainWindowHandle)
    if ($null -eq $rect -or $rect[2] -le 0 -or $rect[3] -le 0) {
        Write-Warning "$Label : the window has no usable client rectangle"
        return $false
    }

    $bitmap = New-Object System.Drawing.Bitmap($rect[2], $rect[3])
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $printed = $false
    try {
        $hdc = $graphics.GetHdc()
        try {
            $printed = [WinPrint]::Capture($proc.MainWindowHandle, $hdc)
        } finally {
            $graphics.ReleaseHdc($hdc)
        }
        if (-not $printed) {
            # Some drivers refuse PW_RENDERFULLCONTENT for a GL window. Fall back
            # to reading the screen, which is correct as long as nothing overlaps
            # - and say which path was used, so a composited image is never
            # mistaken for a rendering bug.
            Write-Host "$Label : PrintWindow refused; falling back to a screen capture"
            $graphics.CopyFromScreen($rect[0], $rect[1], 0, 0, $bitmap.Size)
        }
    } finally {
        $graphics.Dispose()
    }

    $path = Join-Path $Out $FileName
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()

    $size = (Get-Item $path).Length
    Write-Host ("{0,-12}: {1}  ({2}x{3}, {4} bytes)" -f $Label, $path, $rect[2], $rect[3], $size)
    return $true
}

# Raise an application's window, press Esc in it and let the modal lay out, so the
# capture that follows photographs a state no assertion can reach. The handle is
# read fresh rather than taken from the object Start-Process returned: a Process
# captured at spawn time can still report a zero MainWindowHandle.
function Send-EscapeTo {
    param([string]$ProcessName, [string]$Label)
    $win = (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Select-Object -First 1).MainWindowHandle
    if (-not $win -or $win -eq [IntPtr]::Zero) {
        Write-Warning "$Label : no window to send Esc to"
        return
    }
    Add-Type -AssemblyName System.Windows.Forms
    [WinCap]::Raise($win) | Out-Null
    Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Start-Sleep -Milliseconds 900
}

# Nothing of ours should already be running, or the capture would grab the wrong
# window and the port checks inside the apps would fight over 8080/8081.
Get-Process -Name 'vn-mediabus-*' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

$started = @()
try {
    # The Controller goes up FIRST when the Scripts tab is the subject: that tab
    # reads its list from the Controller's API, so with no Controller it correctly
    # shows "The Controller is not running" instead of any scripts.
    $needController = (-not $DashboardOnly) -or ($Tab -eq 'scripts')
    if ($needController) {
        $ctrl = Start-Process -FilePath (Join-Path $Bin 'vn-mediabus-controller.exe') `
            -ArgumentList '--start-offline' -WorkingDirectory $Bin `
            -RedirectStandardError (Join-Path $Out 'controller.err') -PassThru
        $started += $ctrl.Id
        Start-Sleep -Seconds 2
    }

    if ($WithPlayer) {
        $player = Start-Process -FilePath (Join-Path $Bin 'vn-mediabus-player.exe') `
            -WorkingDirectory $Bin `
            -RedirectStandardError (Join-Path $Out 'player.err') -PassThru
        $started += $player.Id
        Start-Sleep -Seconds 4

        if ($CorpusFolder) {
            # Through the Player's own API rather than by editing mediabus.ini
            # behind its back: the Player is the single writer of this setting, and
            # a second writer is exactly how the displayed folder and the scanned
            # folder start disagreeing.
            try {
                Invoke-RestMethod -Method Post `
                    -Uri 'http://127.0.0.1:8080/api/media-dir' `
                    -ContentType 'application/json' `
                    -Body (@{ path = $CorpusFolder } | ConvertTo-Json) | Out-Null
                Start-Sleep -Seconds 1
            } catch {
                Write-Warning "could not point the Player at $CorpusFolder : $_"
            }
        }
    }

    if (-not $ControllerOnly) {
        $dash = Start-Process -FilePath (Join-Path $Bin 'vn-mediabus-dashboard.exe') `
            -ArgumentList "--no-tray --tab $Tab" -WorkingDirectory $Bin `
            -RedirectStandardError (Join-Path $Out 'dashboard.err') -PassThru
        $started += $dash.Id
        Start-Sleep -Seconds 3

        if ($Escape) {
            Send-EscapeTo -ProcessName 'vn-mediabus-dashboard' -Label "dashboard/$Tab"
        }
    }

    if (-not $ControllerOnly) {
        $shotName = if ($Escape) { 'dashboard-esc.png' } else { 'dashboard.png' }
        $shotLabel = if ($Escape) { "dashboard/$Tab + Esc" } else { "dashboard/$Tab" }
        Save-WindowShot -ProcessName 'vn-mediabus-dashboard' `
            -FileName $shotName -Label $shotLabel | Out-Null
    }
    if (-not $DashboardOnly) {
        # With --start-offline the Controller shows the offline state, which is
        # the honest picture of a Controller whose Player is not up.
        if ($Escape) {
            Send-EscapeTo -ProcessName 'vn-mediabus-controller' -Label 'controller'
        }
        $ctrlName = if ($Escape) { 'controller-esc.png' } else { 'controller.png' }
        $ctrlLabel = if ($Escape) { 'controller + Esc' } else { 'controller' }
        Save-WindowShot -ProcessName 'vn-mediabus-controller' `
            -FileName $ctrlName -Label $ctrlLabel | Out-Null
    }

    Write-Host ""
    Write-Host "screenshots in $Out"
    foreach ($name in 'dashboard', 'controller') {
        $err = Join-Path $Out "$name.err"
        if (Test-Path $err) {
            $interesting = Select-String -Path $err -Pattern 'imgui-error|warning' -ErrorAction SilentlyContinue
            if ($interesting) {
                Write-Warning "$name reported $($interesting.Count) warning/error line(s); see $err"
            } else {
                Write-Host "$name : no warnings, no ImGui errors"
            }
        }
    }
} finally {
    foreach ($id in $started) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
    Get-Process -Name 'vn-mediabus-*' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
