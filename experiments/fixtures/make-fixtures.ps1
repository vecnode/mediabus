# make-fixtures.ps1 — generate local test media for the probe and, later, the app.
#
# The clip is deliberately a sync-bip: a 440 Hz tone in the left channel and
# 880 Hz in the right, so any A/V drift is audible rather than merely measured.
#
# Fixtures are git-ignored build inputs, not sources.

$ErrorActionPreference = 'Stop'

$Msys = 'C:\msys64'
$Mingw = "$Msys\mingw64"
$env:PATH = "$Mingw\bin;$Msys\usr\bin;$env:SystemRoot\system32;$env:SystemRoot"

$Fx = Join-Path (Split-Path -Parent $PSScriptRoot) 'p0\fixtures'
New-Item -ItemType Directory -Force -Path $Fx | Out-Null

$clip = Join-Path $Fx 'sync-bip.mp4'
$srt = Join-Path $Fx 'test.srt'

if (-not (Test-Path $clip)) {
    Write-Host "generating $clip"
    & "$Mingw\bin\ffmpeg.exe" -y -loglevel error `
        -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=6" `
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=6" `
        -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=6" `
        -filter_complex "[1:a][2:a]amerge=inputs=2[a]" -map 0:v -map "[a]" `
        -c:v libx264 -pix_fmt yuv420p -preset veryfast -c:a aac -shortest $clip
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $LASTEXITCODE" }
}

@"
1
00:00:00,500 --> 00:00:05,500
P0 SUBTITLE PROOF
"@ | Set-Content -Path $srt -Encoding ASCII

Get-ChildItem $Fx | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "fixtures ready in $Fx"
