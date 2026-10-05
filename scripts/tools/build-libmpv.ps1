# build-libmpv.ps1 — build libmpv from source against the ffmpeg installed here.
#
# WHY THIS EXISTS (do not replace with `pacman -S mingw-w64-x86_64-mpv`):
# the MSYS2 mpv package on this machine fails at load with
#
#     libavcodec: build version 62.28.101 incompatible with runtime version 62.28.100
#
# i.e. it was built against a libavcodec one patch newer than the installed
# ffmpeg. `mpv.exe --version` fails too, so it is not an application bug, and
# reinstalling does not help because the repo no longer carries a matching
# package. Building mpv against the ffmpeg that is actually present removes the
# skew entirely. See BUILDING.md.

$ErrorActionPreference = 'Stop'

# pinned so the build is reproducible
$MpvVersion = '0.41.0'
$MpvSha256  = 'ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209'
$MpvUrl     = "https://github.com/mpv-player/mpv/archive/refs/tags/v$MpvVersion.tar.gz"

$Msys    = 'C:\msys64'
$Mingw   = "$Msys\mingw64"
$MsysBin = "$Msys\usr\bin"

# PATH is REPLACED, not extended: a libwinpthread-1.dll from Strawberry/MinGW
# elsewhere shadows MSYS2's and cc1plus.exe dies silently. See BUILDING.md.
$env:PATH = "$Mingw\bin;$MsysBin;$env:SystemRoot\system32;$env:SystemRoot"
$env:TEMP = "$Msys\tmp"
$env:TMP  = "$Msys\tmp"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:CC  = 'gcc'
$env:CXX = 'g++'

$Repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Cache   = Join-Path $Repo '.cache'
$Work    = Join-Path $Cache "mpv-$MpvVersion"
$Tarball = Join-Path $Cache "mpv-$MpvVersion.tar.gz"
$Build   = Join-Path $Work 'build'
$Src     = Join-Path $Work "mpv-$MpvVersion"

# Forward slashes everywhere we hand a path to meson/ninja/shell tools.
# meson.build() does `conf_data.set_quoted('CONFIGURATION', meson.build_options())`,
# which bakes the option string into a C literal — a Windows path with
# backslashes then becomes an illegal escape sequence in player/command.c
# ("incomplete universal character name \U"). Passing forward-slash paths keeps
# that string free of backslashes and also avoids bsdtar's host:path parsing.
function To-Slash([string]$p) { $p -replace '\\', '/' }

New-Item -ItemType Directory -Force -Path $Cache | Out-Null

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$What)
    Write-Host "`n>>> $What"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit $LASTEXITCODE" }
}

# ---------------------------------------------------------------- fetch
if (-not (Test-Path $Tarball)) {
    Write-Host ">>> downloading mpv $MpvVersion"
    Invoke-WebRequest -Uri $MpvUrl -OutFile $Tarball -UseBasicParsing -TimeoutSec 600
}
$hash = (Get-FileHash $Tarball -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $MpvSha256) {
    throw "mpv tarball hash mismatch:`n  expected $MpvSha256`n  actual   $hash"
}
Write-Host "tarball verified: $hash"

# ---------------------------------------------------------------- extract
# MSYS GNU tar, NOT tar.exe on PATH: bsdtar parses 'C:\...' as host:path.
if (-not (Test-Path (To-Slash (Join-Path $Src 'meson.build')))) {
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    Invoke-Tool "$MsysBin\tar.exe" @(
        '-xzf', (To-Slash $Tarball), '-C', (To-Slash $Work)
    ) "extracting mpv source"
}

# ---------------------------------------------------------------- configure
# Deliberately lean: no vapoursynth / bluray / dvd / cdda / archive / caca and
# no C plugins. Lua + MuJS are kept because scripting is a product feature.
#
# NO --prefix: `meson.build()` does
#     conf_data.set_quoted('CONFIGURATION', meson.build_options())
# and an absolute Windows --prefix lands in that string with UNESCAPED
# backslashes, producing an illegal C literal:
#     #define CONFIGURATION " ... -Dprefix=C:\Users\...  ... "
# which fails as 'incomplete universal character name \U'. We never run
# `ninja install` (artefacts are copied out below), so no prefix is needed.
$buildNinja = Join-Path $Build 'build.ninja'
if (-not (Test-Path $buildNinja)) {
    Invoke-Tool "$Mingw\bin\meson.exe" @(
        'setup', (To-Slash $Build), (To-Slash $Src),
        '--buildtype=release',
        '-Ddefault_library=shared',
        '-Dgpl=true',
        '-Dlibmpv=true',
        '-Dcplayer=false',
        '-Dvapoursynth=disabled',
        '-Djavascript=enabled',
        '-Dlua=enabled',
        '-Dcplugins=disabled',
        '-Duchardet=disabled',
        '-Dzimg=disabled',
        '-Drubberband=disabled',
        '-Dlibarchive=disabled',
        '-Dlibbluray=disabled',
        '-Ddvdnav=disabled',
        '-Dcdda=disabled',
        '-Dcaca=disabled',
        '-Djpeg=disabled',
        '-Dlcms2=disabled',
        '-Dsdl2-gamepad=disabled',
        '-Dsixel=disabled',
        '-Dtests=false',
        '-Dfuzzers=false',
        '-Dmanpage-build=disabled',
        '-Dgl=auto',
        '-Dd3d11=enabled',
        '-Dd3d-hwaccel=enabled',
        '-Dgl-win32=enabled',
        '-Dplain-gl=enabled',
        '-Dwasapi=enabled',
        '-Dwin32-threads=enabled',
        '-Dwin32-subsystem=windows',
        '-Dbuild-date=false'
    ) "configuring mpv with meson"
}
if (-not (Test-Path $buildNinja)) {
    throw "meson setup did not create $buildNinja"
}

# Belt and braces: if any path still reached CONFIGURATION with a backslash,
# neutralise the escapes so the C literal is valid. Harmless when already clean.
foreach ($hdr in @('config.h', 'config.h.meson')) {
    $p = Join-Path $Build $hdr
    if (-not (Test-Path $p)) { continue }
    $text = Get-Content $p -Raw
    $fixed = [regex]::Replace($text, '\\\\(?=[A-Za-z0-9._])', '/')
    if ($fixed -ne $text) {
        Set-Content -Path $p -Value $fixed -Encoding UTF8 -NoNewline
        Write-Host "normalised backslashes in $hdr (meson CONFIGURATION quirk)"
    }
}

# ---------------------------------------------------------------- build
Invoke-Tool "$Mingw\bin\ninja.exe" @('-C', (To-Slash $Build), '-j', '6') "building libmpv"

$builtDll = Join-Path $Build 'libmpv-2.dll'
$builtLib = Join-Path $Build 'libmpv.dll.a'
if (-not (Test-Path $builtDll)) { throw "build did not produce $builtDll" }

# ---------------------------------------------------------------- vendor into repo
New-Item -ItemType Directory -Force -Path (Join-Path $Repo 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Repo 'lib') | Out-Null
Copy-Item $builtDll (Join-Path $Repo 'bin\libmpv-2.dll') -Force
Copy-Item $builtLib (Join-Path $Repo 'lib\libmpv.dll.a')   -Force
Copy-Item "$Mingw\include\mpv" (Join-Path $Repo 'lib') -Recurse -Force

Write-Host "`n=== libmpv installed ==="
Get-ChildItem (Join-Path $Repo 'bin\libmpv-2.dll'),
              (Join-Path $Repo 'lib\libmpv.dll.a'),
              (Join-Path $Repo 'lib\mpv\client.h') | Select-Object FullName, Length | Format-Table -AutoSize
Write-Host "Built against libavcodec $(& "$Mingw\bin\pkg-config.exe" --modversion libavcodec)"
