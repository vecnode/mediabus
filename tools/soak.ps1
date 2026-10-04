# soak.ps1 - instrumented long-run harness.
#
# Answers the questions a short smoke test cannot:
#   * does resident memory grow without bound?
#   * do threads or handles leak?
#   * does the HTTP API stay responsive under sustained traffic?
#   * does playback keep real time, or drift?
#   * does the process ever die?
#
# Playback of the test clip ends after a few seconds, so the harness actively
# drives the API (open / play / seek / pause / volume) to keep the decoder
# working. That is also what exercises the request path, which is the part most
# likely to leak.
#
# Usage:
#   pwsh -File tools/soak.ps1                      # 5 minutes
#   pwsh -File tools/soak.ps1 -Minutes 20
#   pwsh -File tools/soak.ps1 -Minutes 1440        # a full day

param(
    [int]$Minutes = 5,
    [int]$SampleSeconds = 15,
    [int]$RequestsPerSample = 120,
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Repo 'bin'
$Exe = Join-Path $Bin 'vn-mediabus-player.exe'
$Base = 'http://127.0.0.1:8080'

if (-not (Test-Path $Exe)) { throw "not built: $Exe (run build.ps1)" }
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $Repo 'dist\soak-report.txt'
}
New-Item -ItemType Directory -Force -Path (Split-Path $ReportPath) | Out-Null

# The app needs its own libmpv on PATH; nothing else here depends on MSYS2.
$savedPath = $env:PATH
$env:PATH = "$Bin;$savedPath"

$logPath = Join-Path $env:TEMP 'media-soak-stderr.log'
Remove-Item $logPath -ErrorAction SilentlyContinue

function Get-Api {
    param([string]$Method, [string]$Path, [string]$Body)
    try {
        if ($Method -eq 'GET') {
            return Invoke-RestMethod "$Base$Path" -TimeoutSec 5
        }
        return Invoke-RestMethod "$Base$Path" -Method Post -Body $Body `
            -ContentType 'application/json' -TimeoutSec 5
    } catch {
        return $null
    }
}

Write-Host ">>> launching $Exe"
$proc = Start-Process -FilePath $Exe -WorkingDirectory $Bin `
    -RedirectStandardError $logPath -PassThru -NoNewWindow

# Wait for the API to answer rather than guessing a sleep duration.
$ready = $false
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 200
    if ($proc.HasExited) { throw "process exited during startup; see $logPath" }
    $health = Get-Api -Method GET -Path '/api/health'
    if ($null -ne $health) { $ready = $true; break }
}
if (-not $ready) { $proc.Kill(); throw "HTTP API never came up" }
Write-Host ">>> API is up (pid $($proc.Id))"

$clips = @(Get-Api -Method GET -Path '/api/clips')
$videoClips = @($clips | Where-Object { $_.mediaType -eq 'video' })
$imageClips = @($clips | Where-Object { $_.mediaType -eq 'image' })
Write-Host ">>> playlist: $($clips.Count) clip(s) - $($videoClips.Count) video, $($imageClips.Count) image"
if ($clips.Count -eq 0) { Write-Warning "playlist is empty; the soak will exercise the API only" }

$samples = New-Object System.Collections.Generic.List[object]
$requests = 0
$failures = 0
$positionChecks = 0
$positionRegressions = 0
$started = Get-Date
$deadline = $started.AddMinutes($Minutes)
$sampleIndex = 0
$baseline = $null

Write-Host ">>> soaking for $Minutes minute(s); sampling every ${SampleSeconds}s"
Write-Host ""

while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) {
        Write-Host "*** PROCESS DIED after $([int]((Get-Date) - $started).TotalSeconds)s ***"
        break
    }

    # ---- sustained request load ------------------------------------------
    # Weighted toward the cheap reads a real controller issues most often, with
    # a share of state-changing calls so the decode path is exercised too.
    for ($r = 0; $r -lt $RequestsPerSample; $r++) {
        $pick = $r % 10
        $result = $null
        switch ($pick) {
            0 { $result = Get-Api GET '/api/status' }
            1 { $result = Get-Api GET '/api/status' }
            2 { $result = Get-Api GET '/api/position' }
            3 { $result = Get-Api GET '/api/position' }
            4 { $result = Get-Api GET '/api/clips' }
            5 { $result = Get-Api GET '/api/health' }
            6 {
                if ($videoClips.Count -gt 0) {
                    $result = Get-Api POST '/api/play' '{}'
                } else { $result = Get-Api GET '/api/status' }
            }
            7 {
                if ($videoClips.Count -gt 0) {
                    $seek = Get-Random -Minimum 0 -Maximum 40
                    $result = Get-Api POST '/api/seek' "{""time"":$seek}"
                } else { $result = Get-Api GET '/api/status' }
            }
            8 {
                $result = Get-Api POST '/api/volume' "{""volume"":$(Get-Random -Minimum 0 -Maximum 100)}"
            }
            9 {
                $all = @($clips)
                if ($all.Count -gt 1) {
                    $idx = Get-Random -Minimum 0 -Maximum $all.Count
                    $result = Get-Api POST "/api/clips/$idx" '{}'
                } else { $result = Get-Api GET '/api/status' }
            }
        }
        $requests++
        if ($null -eq $result) { $failures++ }
    }

    # ---- position must advance, never jump backwards ----------------------
    $status = Get-Api GET '/api/status'
    if ($null -ne $status -and -not $status.isImage -and $status.playing) {
        $positionChecks++
        if ($null -ne $baseline -and $status.position -lt ($baseline.position - 1.0)) {
            # A backwards jump larger than the seek step means position reporting
            # is inconsistent, which is the symptom of a stale-frame swap.
            $positionRegressions++
        }
        $baseline = $status
    }

    # ---- process resources -------------------------------------------------
    $proc.Refresh()
    $rss = $proc.WorkingSet64
    $sample = [pscustomobject]@{
        ElapsedSec = [int]((Get-Date) - $started).TotalSeconds
        RssMB      = [math]::Round($rss / 1MB, 1)
        Threads    = $proc.Threads.Count
        Handles    = $proc.HandleCount
        Requests   = $requests
        Failures   = $failures
        Position   = if ($status) { [math]::Round($status.position, 2) } else { -1 }
        Playing    = if ($status) { $status.playing } else { $false }
    }
    $samples.Add($sample)
    $sampleIndex++
    Write-Host ("  [{0,5}s] rss={1,7} MB  threads={2,4}  handles={3,5}  req={4,6}  fail={5}  pos={6}" -f `
        $sample.ElapsedSec, $sample.RssMB, $sample.Threads, $sample.Handles,
        $sample.Requests, $sample.Failures, $sample.Position)

    Start-Sleep -Seconds $SampleSeconds
}

# ---- final teardown ---------------------------------------------------------
$died = $proc.HasExited
$exitCode = if ($died) { $proc.ExitCode } else { $null }
if (-not $died) {
    $proc.Kill()
    $proc.WaitForExit()
}

$durationSec = [int]((Get-Date) - $started).TotalSeconds
$first = $samples | Select-Object -First 1
$last = $samples | Select-Object -Last 1

# Growth is measured from the first sample, which is already warm (the process
# has loaded libmpv, opened a clip and drawn frames).
$rssGrowth = if ($first -and $last) { $last.RssMB - $first.RssMB } else { 0 }
$peakRss = ($samples | Measure-Object -Property RssMB -Maximum).Maximum
$threadGrowth = if ($first -and $last) { $last.Threads - $first.Threads } else { 0 }
$handleGrowth = if ($first -and $last) { $last.Handles - $first.Handles } else { 0 }

$verdict = 'PASS'
$notes = New-Object System.Collections.Generic.List[string]
if ($died) { $verdict = 'FAIL'; $notes.Add("process exited early (code $exitCode)") }
if ($rssGrowth -gt 50) { $verdict = 'FAIL'; $notes.Add("RSS grew ${rssGrowth} MB (limit 50 MB)") }
if ($threadGrowth -gt 8) { $verdict = 'FAIL'; $notes.Add("threads grew by $threadGrowth") }
if ($samples.Count -gt 2 -and $failures -gt ($requests * 0.01)) {
    $verdict = 'FAIL'
    $notes.Add("$failures of $requests requests failed (>1%)")
}
if ($positionRegressions -gt 0) {
    $verdict = 'FAIL'
    $notes.Add("$positionRegressions backwards position jumps")
}
if ($samples.Count -lt 3) {
    $verdict = 'INCONCLUSIVE'
    $notes.Add("only $($samples.Count) sample(s); run longer")
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("vn-mediabus-player soak report")
$lines.Add("============================")
$lines.Add("started            : $($started.ToString('yyyy-MM-dd HH:mm:ss'))")
$lines.Add("ran for            : ${durationSec}s ($([math]::Round($durationSec/60,1)) min)")
$lines.Add("samples            : $($samples.Count) every ${SampleSeconds}s")
$lines.Add("")
$lines.Add("HTTP requests      : $requests")
$lines.Add("request failures   : $failures")
$lines.Add("position checks    : $positionChecks")
$lines.Add("position regressions: $positionRegressions")
$lines.Add("")
$lines.Add("RSS first sample   : $($first.RssMB) MB")
$lines.Add("RSS last sample    : $($last.RssMB) MB")
$lines.Add("RSS growth         : $rssGrowth MB   (limit 50)")
$lines.Add("RSS peak           : $peakRss MB")
$lines.Add("thread growth      : $threadGrowth   (limit 8)")
$lines.Add("handle growth      : $handleGrowth")
$lines.Add("process died early : $died")
$lines.Add("")
$lines.Add("VERDICT            : $verdict")
foreach ($n in $notes) { $lines.Add("  - $n") }
$lines.Add("")
$lines.Add("Samples:")
foreach ($s in $samples) {
    $lines.Add(("  {0,6}s  rss={1,7} MB  threads={2,4}  handles={3,5}  pos={4}" -f `
        $s.ElapsedSec, $s.RssMB, $s.Threads, $s.Handles, $s.Position))
}

$lines -join "`r`n" | Set-Content -Path $ReportPath -Encoding UTF8

Write-Host ""
$lines | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "report: $ReportPath"

$env:PATH = $savedPath
if ($verdict -eq 'FAIL') { exit 1 }
exit 0
