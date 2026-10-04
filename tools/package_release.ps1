# package_release.ps1 - assemble a runnable, self-contained release folder.
#
# WHY THIS IS NOT JUST "copy the exes": the Player links libmpv, which in turn
# pulls in a large transitive set (FFmpeg's avcodec/avformat/swscale/swresample,
# libass, libplacebo, Lua, MuJS, and the MinGW runtime). On a machine without
# MSYS2 none of those resolve, and the app dies before main() with no message.
# This resolves the closure from the executables with ldd and copies the DLLs.
#
# All three applications ship together and must sit side by side: the Dashboard
# finds its two neighbours by name in its own directory, so a bundle missing one
# of them gives the operator a launcher whose buttons cannot work.
#
# The result is verified by actually running each executable with a PATH that
# excludes MSYS2, which is the only way to know the bundle is complete.

param(
    [string]$OutDir = '',
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'

$Msys  = 'C:\msys64'
$Mingw = "$Msys\mingw64"
$env:PATH = "$Mingw\bin;$Msys\usr\bin;$env:SystemRoot\system32;$env:SystemRoot"
$env:TEMP = "$Msys\tmp"
$env:TMP  = "$Msys\tmp"

$Repo = Split-Path -Parent $PSScriptRoot
$BinDir = Join-Path $Repo 'bin'

# Name -> how it is checked in the verify pass. The Player and the Controller
# each answer on their own port; the Dashboard has no API, so staying alive with
# a GL context is the whole check.
$Apps = [ordered]@{
    'media-player-cpp.exe'     = 'http://127.0.0.1:8080/api/health'
    'media-controller-cpp.exe' = 'http://127.0.0.1:8081/api/controller/status'
    'media-dashboard-cpp.exe'  = ''
}

foreach ($name in $Apps.Keys) {
    $exe = Join-Path $BinDir $name
    if (-not (Test-Path $exe)) {
        throw "not built: $exe (run build.ps1 first)"
    }
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $Repo 'dist\mediaplayer-app'
}

if (Test-Path $OutDir) {
    Remove-Item -Recurse -Force $OutDir
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------- payload
Write-Host ">>> staging into $OutDir"
foreach ($name in $Apps.Keys) {
    Copy-Item (Join-Path $BinDir $name) (Join-Path $OutDir $name) -Force
}

# bin/data holds runtime media and the script directories; ship the structure,
# not whatever test media happens to be lying around.
$dataSrc = Join-Path $BinDir 'data'
$dataDst = Join-Path $OutDir 'data'
New-Item -ItemType Directory -Force -Path $dataDst | Out-Null
foreach ($scripts in 'scripts', 'controller-scripts') {
    $from = Join-Path $dataSrc $scripts
    $to = Join-Path $dataDst $scripts
    if (Test-Path $from) {
        Copy-Item $from $to -Recurse -Force
    } else {
        New-Item -ItemType Directory -Force -Path $to | Out-Null
    }
}
Copy-Item (Join-Path $Repo 'README.md')   $OutDir -Force
Copy-Item (Join-Path $Repo 'LICENSE')     $OutDir -Force
Copy-Item (Join-Path $Repo 'BUILDING.md') $OutDir -Force

# ---------------------------------------------------------------- dependencies
# Resolve the DLL closure of every executable. libmpv-2.dll is next to the exes
# already; ldd needs it on PATH to walk through to FFmpeg and friends.
$env:PATH = "$BinDir;$env:PATH"
$ldd = Join-Path $Msys 'usr\bin\ldd.exe'

Write-Host ">>> resolving DLL closure with ldd"
$copied = 0
$seen = @{}
foreach ($name in $Apps.Keys) {
    $exe = Join-Path $BinDir $name
    $raw = & $ldd $exe 2>&1
    if ($LASTEXITCODE -ne 0) { throw "ldd failed on ${name}: $raw" }

    foreach ($line in $raw) {
        # ldd prints "<name> => <path> (0x...)"; take the path.
        if ($line -notmatch '=>\s+(\S+\.dll)') { continue }
        $reported = $Matches[1]
        # Strip the load address if ldd appended one to the path itself.
        $reported = ($reported -replace '\(0x[0-9a-fA-F]+\)', '').Trim()

        # MSYS2's ldd reports MSYS-style paths, NOT Windows ones:
        #   /mingw64/bin/glfw3.dll            -> C:\msys64\mingw64\bin\glfw3.dll
        #   /c/Users/.../bin/libmpv-2.dll     -> C:\Users\...\bin\libmpv-2.dll
        # A naive check for "mingw64/bin" against a Windows path silently matches
        # nothing, which produces a bundle that looks fine and cannot start.
        $source = $null
        if ($reported -match '^/mingw64/') {
            $source = Join-Path $Mingw ($reported -replace '^/mingw64/', '' -replace '/', '\')
        } elseif ($reported -match '^/([a-zA-Z])/(.*)$') {
            $source = ($Matches[1] + ':\' + $Matches[2]) -replace '/', '\'
        } elseif ($reported -match '^[a-zA-Z]:[\\/]') {
            $source = $reported
        }

        if ([string]::IsNullOrWhiteSpace($source) -or -not (Test-Path $source)) {
            continue
        }

        # Never redistribute Windows system DLLs; only the MSYS2 toolchain and
        # the vendored libmpv next to the executables belong in the bundle.
        $isToolchain = $source -like "$Mingw\bin\*"
        $isVendored = $source -like "$BinDir\*"
        if (-not ($isToolchain -or $isVendored)) {
            continue
        }

        $leaf = Split-Path $source -Leaf
        Copy-Item $source (Join-Path $OutDir $leaf) -Force
        if (-not $seen.ContainsKey($source)) {
            $seen[$source] = $true
            $copied++
        }
    }
}

# libmpv is loaded by the linker as a direct import, so make certain it is here
# even if the ldd walk missed it.
$mpv = Join-Path $BinDir 'libmpv-2.dll'
if (Test-Path $mpv) {
    Copy-Item $mpv (Join-Path $OutDir 'libmpv-2.dll') -Force
}

$sizes = Get-ChildItem $OutDir -File | Measure-Object -Property Length -Sum
Write-Host ("`nstaged {0} files ({1:N1} MB) plus data/" -f ((Get-ChildItem $OutDir -File).Count + 1), ($sizes.Sum / 1MB))
Get-ChildItem $OutDir -File | Where-Object { $_.Name -match '\.(exe|dll)$' } |
    Sort-Object Name | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize

# ---------------------------------------------------------------- verify
# The only meaningful check: run each one with MSYS2 removed from PATH. If a DLL
# is missing the process dies before printing anything, which is the failure
# mode this whole script exists to prevent. The two that have an API are asked
# to answer on it, so "it started" is not mistaken for "it works".
if (-not $SkipVerify) {
    Write-Host ">>> verifying the bundle runs without MSYS2 on PATH"
    $savedPath = $env:PATH
    $savedTemp = $env:TEMP
    $savedTmp  = $env:TMP
    $curl = (Get-Command curl.exe).Source
    $failed = @()
    try {
        $env:PATH = "$env:SystemRoot\system32;$env:SystemRoot"
        $env:TEMP = "$env:SystemRoot\Temp"
        $env:TMP  = "$env:SystemRoot\Temp"

        foreach ($name in $Apps.Keys) {
            $errFile = Join-Path $OutDir "$name.verify.err"
            $proc = Start-Process -FilePath (Join-Path $OutDir $name) `
                -WorkingDirectory $OutDir `
                -RedirectStandardError $errFile `
                -PassThru -NoNewWindow
            Start-Sleep -Seconds 4

            $alive = -not $proc.HasExited
            $answered = $true
            if ($alive -and $Apps[$name]) {
                $reply = & $curl -s --max-time 4 $Apps[$name]
                $answered = -not [string]::IsNullOrEmpty($reply)
            }
            if ($alive) { $proc.Kill(); $proc.WaitForExit() }

            $err = Get-Content $errFile -ErrorAction SilentlyContinue
            Remove-Item $errFile -ErrorAction SilentlyContinue

            if ($alive -and $answered) {
                Write-Host "  ok: $name stayed alive$(if ($Apps[$name]) { ' and answered its API' })"
                $err | Select-Object -First 3 | ForEach-Object { Write-Host "      $_" }
            } else {
                $failed += $name
                Write-Host "`n*** BUNDLE FAILED: $name ***"
                $err | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" }
            }
        }
    } finally {
        $env:PATH = $savedPath
        $env:TEMP = $savedTemp
        $env:TMP  = $savedTmp
    }
    if ($failed.Count -gt 0) {
        throw "release bundle is not self-contained: $($failed -join ', ')"
    }
}

Write-Host "`nrelease ready: $OutDir"
