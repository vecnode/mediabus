# build.ps1 — configure and build the application.
#
# Lives in scripts/ with the other entry points; the repository root is this
# file's parent directory. Invoke it as:
#     pwsh -File scripts/build.ps1
# or double-click scripts\build.bat, which finds PowerShell for you.
#
# PATH is REPLACED, not extended. With the user's normal PATH,
# C:\Strawberry\c\bin\libwinpthread-1.dll shadows MSYS2's and cc1plus.exe dies
# with STATUS_ENTRYPOINT_NOT_FOUND printing nothing at all. See BUILDING.md.

param(
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$Run,
    [switch]$RunController,
    [switch]$RunDashboard,
    # Which GUI to launch with -Run. Player is the default because it is the
    # one that shows something.
    [ValidateSet('Player', 'Controller', 'Dashboard')]
    [string]$App = 'Player'
)

$ErrorActionPreference = 'Stop'

$Msys  = 'C:\msys64'
$Mingw = "$Msys\mingw64"
$env:PATH = "$Mingw\bin;$Msys\usr\bin;$env:SystemRoot\system32;$env:SystemRoot"
$env:TEMP = "$Msys\tmp"
$env:TMP  = "$Msys\tmp"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:CC  = 'gcc'
$env:CXX = 'g++'

# This script lives in scripts/, so the repository root is its parent. Every
# path below is derived from $Repo, never from the caller's working directory.
$Repo = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Repo 'build'

# The three applications this tree produces. They must all exist after a build:
# the Dashboard locates the other two beside itself, so a partial build would
# leave it with buttons that cannot work. The test binary is checked separately
# below, and is the one that proves the rest is sound.
$Exes = [ordered]@{
    Player     = 'bin\vn-mediabus-player.exe'
    Controller = 'bin\vn-mediabus-controller.exe'
    Dashboard  = 'bin\vn-mediabus-dashboard.exe'
}
$TestExe = 'bin\vn-mediabus-tests.exe'

# MSYS2 mingw64 has no cmake package on this machine, so use the native
# Windows CMake and tell it which toolchain to drive. Ninja is in MSYS2.
$Cmake = 'C:\Program Files\CMake\bin\cmake.exe'
if (-not (Test-Path $Cmake)) {
    $found = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $found) { throw "cmake not found (looked in Program Files and PATH)" }
    $Cmake = $found.Source
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host ">>> removing $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# libmpv must exist before configuring: the build fails fast with a clear
# message if it is missing, but a nicer place to say so is here.
if (-not (Test-Path (Join-Path $Repo 'vendor\libmpv\lib\libmpv.dll.a'))) {
    throw "Vendored libmpv missing. Run:  pwsh -File scripts/tools/build-libmpv.ps1"
}

# Dear ImGui is vendored under vendor/imgui and is tracked, so a clone builds
# with no network access. This is only the hint for how to reproduce it.
if (-not (Test-Path (Join-Path $Repo 'vendor\imgui\imgui.cpp'))) {
    throw "Vendored Dear ImGui missing. Run:  pwsh -File scripts/tools/fetch-imgui.ps1"
}

Write-Host ">>> configuring with $Cmake"
# Forward slashes: CMake stores these in a generated .cmake file, and a Windows
# path with backslashes becomes an invalid escape there ('\m' in C:\msys64...).
$MingwFwd = $Mingw -replace '\\', '/'
& $Cmake -S $Repo -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER="$MingwFwd/bin/g++.exe"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed: $LASTEXITCODE" }

if ($ConfigureOnly) { Write-Host "configured only"; exit 0 }

Write-Host "`n>>> building"
& $Cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

$built = @()
foreach ($name in $Exes.Keys) {
    $path = Join-Path $Repo $Exes[$name]
    if (-not (Test-Path $path)) {
        throw "$name binary not found: $path`nThe Dashboard needs all three side by side; a partial build is not usable."
    }
    $built += $path
}
$testPath = Join-Path $Repo $TestExe
if (-not (Test-Path $testPath)) {
    throw "test binary not found: $testPath"
}
$built += $testPath
Write-Host ""
foreach ($path in $built) { Write-Host "built: $path" }

# ---------------------------------------------------------------------------
# Stage the MinGW runtime next to the binaries.
#
# Without this, the executables only run if MSYS2 happens to be on PATH;
# launched any other way (from Explorer, a shortcut, the Dashboard) Windows
# fails to resolve libgcc_s_seh-1, libwinpthread-1, libstdc++-6, glew32 and
# glfw3, and the process dies before main() with 0xC0000135 and no message at
# all. The build should produce something that actually runs.
#
# Every binary is inspected, because each links a different closure: the Player
# pulls libmpv, the Controller pulls Lua, the Dashboard pulls neither. The union
# is what lands in bin\.
#
# libmpv-2.dll is already vendored in bin\; the rest comes from the toolchain.
# These are build outputs, so they are git-ignored.
# ---------------------------------------------------------------------------
Write-Host "`n>>> staging MinGW runtime DLLs into bin\"
$ldd = "$Msys\usr\bin\ldd.exe"
$staged = 0
$seen = @{}
foreach ($path in $built) {
    $lddOutput = & $ldd $path 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "ldd failed on $(Split-Path $path -Leaf); bin\ may not run outside an MSYS2 shell"
        continue
    }
    foreach ($line in $lddOutput) {
        if ($line -notmatch '=>\s+(\S+\.dll)') { continue }
        # ldd reports MSYS-style paths, not Windows ones:
        #   /mingw64/bin/glfw3.dll  ->  C:\msys64\mingw64\bin\glfw3.dll
        $reported = ($Matches[1] -replace '\(0x[0-9a-fA-F]+\)', '').Trim()
        $source = $null
        if ($reported -match '^/mingw64/') {
            $source = Join-Path $Mingw ($reported -replace '^/mingw64/', '' -replace '/', '\')
        } elseif ($reported -match '^/([a-zA-Z])/(.*)$') {
            $source = ($Matches[1] + ':\' + $Matches[2]) -replace '/', '\'
        } elseif ($reported -match '^[a-zA-Z]:[\\/]') {
            $source = $reported
        }
        if ([string]::IsNullOrWhiteSpace($source) -or -not (Test-Path $source)) { continue }
        # Only the toolchain; never Windows system DLLs.
        if ($source -notlike "$Mingw\bin\*") { continue }
        Copy-Item $source (Join-Path $Repo 'bin') -Force
        if (-not $seen.ContainsKey($source)) {
            $seen[$source] = $true
            $staged++
        }
    }
}
Write-Host "  staged $staged runtime DLL(s)"

if ($RunController) { $App = 'Controller' }
if ($RunDashboard) { $App = 'Dashboard' }

if ($Run -or $RunController -or $RunDashboard) {
    $target = Join-Path $Repo $Exes[$App]
    Write-Host "`n>>> running $App (cwd = bin, so bin/data resolves)"
    Push-Location (Join-Path $Repo 'bin')
    try { & $target } finally { Pop-Location }
}
