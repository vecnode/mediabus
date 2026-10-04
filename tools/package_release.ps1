# package_release.ps1 - assemble a runnable, self-contained release folder.
#
# WHY THIS IS NOT JUST "copy the exe": the app links libmpv, which in turn pulls
# in a large transitive set (FFmpeg's avcodec/avformat/swscale/swresample,
# libass, libplacebo, Lua, MuJS, and the MinGW runtime). On a machine without
# MSYS2 none of those resolve, and the app dies before main() with no message.
# This resolves the closure from the executable with ldd and copies the DLLs.
#
# The result is verified by actually running it with a PATH that excludes MSYS2,
# which is the only way to know the bundle is complete.

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
$Exe = Join-Path $BinDir 'media-player-cpp.exe'

if (-not (Test-Path $Exe)) {
    throw "not built: $Exe (run build.ps1 first)"
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $Repo 'dist\media-player-cpp'
}

if (Test-Path $OutDir) {
    Remove-Item -Recurse -Force $OutDir
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------- payload
Write-Host ">>> staging into $OutDir"
Copy-Item $Exe (Join-Path $OutDir 'media-player-cpp.exe') -Force

# bin/data holds runtime media and the scripts directory; ship the structure,
# not whatever test media happens to be lying around.
$dataSrc = Join-Path $BinDir 'data'
$dataDst = Join-Path $OutDir 'data'
New-Item -ItemType Directory -Force -Path $dataDst | Out-Null
if (Test-Path (Join-Path $dataSrc 'scripts')) {
    Copy-Item (Join-Path $dataSrc 'scripts') (Join-Path $dataDst 'scripts') -Recurse -Force
} else {
    New-Item -ItemType Directory -Force -Path (Join-Path $dataDst 'scripts') | Out-Null
}
Copy-Item (Join-Path $Repo 'README.md') $OutDir -Force
Copy-Item (Join-Path $Repo 'LICENSE')   $OutDir -Force

# ---------------------------------------------------------------- dependencies
# Resolve the full DLL closure. libmpv-2.dll is next to the exe already; ldd
# needs it on PATH to walk through to FFmpeg and friends.
$env:PATH = "$BinDir;$env:PATH"
$ldd = Join-Path $Msys 'usr\bin\ldd.exe'

Write-Host ">>> resolving DLL closure with ldd"
$raw = & $ldd $Exe 2>&1
if ($LASTEXITCODE -ne 0) { throw "ldd failed: $raw" }

$copied = 0
$missing = @()
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

    # Never redistribute Windows system DLLs; only the MSYS2 toolchain and the
    # vendored libmpv next to the exe belong in the bundle.
    $isToolchain = $source -like "$Mingw\bin\*"
    $isVendored = $source -like "$BinDir\*"
    if (-not ($isToolchain -or $isVendored)) {
        continue
    }

    $name = Split-Path $source -Leaf
    Copy-Item $source (Join-Path $OutDir $name) -Force
    $copied++
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
# The only meaningful check: run it with MSYS2 removed from PATH. If a DLL is
# missing the process dies before printing anything, which is the failure mode
# this whole script exists to prevent.
if (-not $SkipVerify) {
    Write-Host ">>> verifying the bundle runs without MSYS2 on PATH"
    $savedPath = $env:PATH
    $savedTemp = $env:TEMP
    $savedTmp  = $env:TMP
    try {
        $env:PATH = "$env:SystemRoot\system32;$env:SystemRoot"
        $env:TEMP = "$env:SystemRoot\Temp"
        $env:TMP  = "$env:SystemRoot\Temp"
        $proc = Start-Process -FilePath (Join-Path $OutDir 'media-player-cpp.exe') `
            -WorkingDirectory $OutDir `
            -RedirectStandardError (Join-Path $OutDir 'verify.err') `
            -PassThru -NoNewWindow
        Start-Sleep -Seconds 3
        $alive = -not $proc.HasExited
        if ($alive) { $proc.Kill(); $proc.WaitForExit() }

        $err = Get-Content (Join-Path $OutDir 'verify.err') -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $OutDir 'verify.err') -ErrorAction SilentlyContinue

        if (-not $alive) {
            Write-Host "`n*** BUNDLE FAILED: process exited immediately ***"
            $err | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" }
            throw "release bundle is not self-contained"
        }
        Write-Host "  ok: process stayed alive with no MSYS2 on PATH"
        $err | Select-Object -First 4 | ForEach-Object { Write-Host "  $_" }
    } finally {
        $env:PATH = $savedPath
        $env:TEMP = $savedTemp
        $env:TMP  = $savedTmp
    }
}

Write-Host "`nrelease ready: $OutDir"
