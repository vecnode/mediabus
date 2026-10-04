# build.ps1 - build the standalone audio probe.
#
# This is the one probe worth keeping: it creates its own libmpv instance with
# no GL context and reports current-ao / audio-pts, which is how the audio path
# is verified independently of the player UI. The probes that used to live here
# (p0_probe, option_probe) each answered a one-off question that the app itself
# or BUILDING.md now covers.
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
$Src = Join-Path $PSScriptRoot 'audio_probe.cpp'
$Exe = Join-Path $PSScriptRoot 'audio_probe.exe'

Write-Host "=== compiling audio_probe ==="
# Quote every -I/-L argument: PowerShell mangles C:/... paths otherwise and the
# linker then resolves nothing while blaming the symbols.
& "$Bin\g++.exe" -std=c++17 -O1 $Src (Join-Path $Repo 'src\core\Log.cpp') -o $Exe `
    "-I$Repo\lib" "-I$Repo\src" "-I$Repo\third_party" `
    "$Repo\lib\libmpv.dll.a" -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw "compile failed with exit $LASTEXITCODE" }
Write-Host "built: $Exe"

if ($args.Count -ge 1) {
    Write-Host "`n=== running against $($args[0]) ==="
    # libmpv must be findable; bin\ holds the vendored copy.
    $env:PATH = "$Repo\bin;$Bin;$env:SystemRoot\system32;$env:SystemRoot"
    & $Exe $args[0]
    Write-Host "`nprobe exit: $LASTEXITCODE"
} else {
    Write-Host "`nrun it against a file:  .\audio_probe.exe ..\bin\data\clip.mp4"
}
