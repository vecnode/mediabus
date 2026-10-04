# build.ps1 â€” build and run the P0 probe with the MSYS2 MinGW64 toolchain.
#
# CRITICAL: PATH is replaced, not extended. With the user's normal PATH,
# C:\Strawberry\c\bin\libwinpthread-1.dll shadows MSYS2's and cc1plus.exe
# terminates with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) printing nothing.
# This is a real environment defect, independent of any code in this repo.

$ErrorActionPreference = 'Stop'

$Msys = 'C:\msys64'
$Mingw = "$Msys\mingw64"
$Bin = "$Mingw\bin"

$env:PATH = "$Bin;$Msys\usr\bin;$env:SystemRoot\system32;$env:SystemRoot"
$env:TEMP = "$Msys\tmp"
$env:TMP = "$Msys\tmp"

$Repo = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $PSScriptRoot 'p0_probe.cpp'
$Exe = Join-Path $PSScriptRoot 'p0_probe.exe'

$pkgconfig = "$Bin\pkg-config.exe"
$cflags = (& $pkgconfig --cflags mpv) -join ' '
$libs = (& $pkgconfig --libs glfw3 mpv) -join ' '

Write-Host "pkg-config cflags: $cflags"
Write-Host "pkg-config libs  : $libs"

# pkg-config emits -LC:/... -IC:/... with a drive letter. PowerShell only passes
# these through intact when each is QUOTED; unquoted, the linker silently
# resolves nothing and every symbol becomes an undefined reference.
$inc = "-I$Mingw\include"
$libdir = "-L$Mingw\lib"

Write-Host "`n=== compiling ==="
& "$Bin\g++.exe" -std=c++17 -O1 -g $Src -o $Exe $inc $libdir `
    -lmpv -lglfw3 -lglew32 -lopengl32 -lgdi32 -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw "compile failed with exit $LASTEXITCODE" }
Write-Host "built: $Exe"

Write-Host "`n=== running probe ==="
Push-Location $PSScriptRoot
try {
    # Fixtures live beside the probe (repo folder) so they survive temp cleanup
    # and are easy to find. Regenerate with make-fixtures.ps1 if missing.
    $clip = Join-Path $PSScriptRoot 'fixtures\sync-bip.mp4'
    $srt = Join-Path $PSScriptRoot 'fixtures\test.srt'
    if (-not (Test-Path $clip)) { throw "missing fixture: $clip (run make-fixtures.ps1 first)" }
    & $Exe $clip $srt
    Write-Host "`nprobe exit: $LASTEXITCODE"
} finally {
    Pop-Location
}
