# stress-switch.ps1 - reproduce the crash found by the 10-minute soak.
#
# The soak died ~443s in with no message, right after loading a large still
# (1400x2000 PNG) and initialising the libmpv VO. Its traffic pattern mixed
# cheap status reads with rapid clip switching, seeks and volume changes. This
# harness isolates those, so the culprit is identified rather than guessed at.
#
# Usage: pwsh -File tools/stress-switch.ps1 -Mode switch|seek|volume|play -Iterations 400

param(
    [ValidateSet('switch', 'seek', 'volume', 'play', 'status')]
    [string]$Mode = 'switch',
    [int]$Iterations = 400,
    [int]$ClipSwitchEveryMs = 120
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Repo 'bin'
$Exe = Join-Path $Bin 'media-player-cpp.exe'
$Base = 'http://127.0.0.1:8080'

$env:PATH = "$Bin;$env:PATH"
$logPath = Join-Path $env:TEMP "stress-$Mode.log"
Remove-Item $logPath -ErrorAction SilentlyContinue

function Api {
    param([string]$Method, [string]$Path, [string]$Body)
    try {
        # The leading comma is load-bearing. PowerShell unrolls a collection
        # returned from a function down to its elements, so an 8-element array
        # arrives at the caller as EIGHT separate values; assigning that to one
        # variable then yields the FIRST element and .Count reads 1. That bug
        # silently turned "switch clips" into "open clip 0 forever", and made a
        # soak that never switched look like one that did.
        if ($Method -eq 'GET') {
            return ,(Invoke-RestMethod "$Base$Path" -TimeoutSec 5)
        }
        return ,(Invoke-RestMethod "$Base$Path" -Method Post -Body $Body `
            -ContentType 'application/json' -TimeoutSec 5)
    } catch { return $null }
}

$proc = Start-Process -FilePath $Exe -WorkingDirectory $Bin -RedirectStandardError $logPath -PassThru -NoNewWindow
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 200
    if ($proc.HasExited) { throw "died at startup; see $logPath" }
    if ($null -ne (Api GET '/api/health')) { break }
}

$clips = [object[]]@(Api GET '/api/clips')
Write-Host ">>> mode=$Mode iterations=$Iterations  clips=$($clips.Count)  pid=$($proc.Id)"

$failures = 0
$diedAt = -1
for ($i = 1; $i -le $Iterations; $i++) {
    if ($proc.HasExited) { $diedAt = $i; break }

    $r = $null
    switch ($Mode) {
        'switch' {
            $idx = $i % [Math]::Max($clips.Count, 1)
            $r = Api POST "/api/clips/$idx" '{}'
            Start-Sleep -Milliseconds $ClipSwitchEveryMs
        }
        'seek' {
            $r = Api POST '/api/seek' "{""time"":$(Get-Random -Minimum 0 -Maximum 5)}"
        }
        'volume' {
            $r = Api POST '/api/volume' "{""volume"":$(Get-Random -Minimum 0 -Maximum 100)}"
        }
        'play' {
            $r = Api POST '/api/play' '{}'
            $r = Api POST '/api/pause' '{"paused":true}'
        }
        'status' {
            $r = Api GET '/api/status'
        }
    }
    if ($null -eq $r) { $failures++ }

    if ($i % 50 -eq 0) {
        $proc.Refresh()
        Write-Host ("  {0,5}/{1}  rss={2,7} MB  threads={3}" -f $i, $Iterations,
            [math]::Round($proc.WorkingSet64 / 1MB, 1), $proc.Threads.Count)
    }
}

$alive = -not $proc.HasExited
$exit = if ($alive) { $null } else { $proc.ExitCode }
if ($alive) { $proc.Kill(); $proc.WaitForExit() }

Write-Host ""
if ($diedAt -gt 0) {
    Write-Host "*** CRASHED at iteration $diedAt (mode=$Mode) ***"
    Write-Host "exit code: $exit"
    Write-Host "--- last 12 log lines ---"
    Get-Content $logPath -ErrorAction SilentlyContinue | Select-Object -Last 12 | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host "survived $Iterations iterations of mode=$Mode (failures=$failures)"
Write-Host "--- last 4 log lines ---"
Get-Content $logPath -ErrorAction SilentlyContinue | Select-Object -Last 4 | ForEach-Object { Write-Host "  $_" }
exit 0
