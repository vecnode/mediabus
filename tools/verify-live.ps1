# verify-live.ps1 - live acceptance check for the three applications.
#
# Starts the Player, drives it over its own HTTP API, then starts the
# Controller and checks that it reports the Player as online and that its
# commands actually reach the Player. Finally starts the Dashboard, which is
# what "the three apps work with each other" means end to end.
#
# Not part of the build: this is the "does it actually run" evidence the README
# refers to, kept because it is the only way to check behaviour that needs a GL
# context and a real window.
#
# Two details that are not obvious and were both learned the hard way:
#
#   * Every child is started with -NoNewWindow and redirected stdio. Without
#     them Start-Process attaches the child to a new console, which in a
#     non-interactive session (an agent, a CI runner, a service) blocks the
#     parent forever with no output at all.
#   * JSON bodies are written to a file and handed to curl as @file. Windows
#     PowerShell 5.1 strips the inner double quotes from a native argument, so
#     `-d '{"a":1}'` reaches curl as `{a:1}` -- invalid JSON, and the server
#     answers 400 for a reason that has nothing to do with the server.
#
# Everything it writes goes under bin/, which is build output and git-ignored:
# a check must never dirty the tracked tree.

$ErrorActionPreference = 'Stop'

# This script lives in tools/, so the repository root is its parent.
$Repo = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $Repo 'bin'
$Curl = (Get-Command curl.exe).Source

$PlayerPort = 8080
$ControllerPort = 8081

# Per-app stdio lands here. A window that fails to open says so on stderr, and
# that message is the whole diagnosis, so it is kept rather than discarded.
$RunDir = Join-Path $Bin 'verify-live'
Remove-Item $RunDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

# Every line is also appended to a log file. A run of this script starts three
# windows and takes about a minute; having the evidence on disk means a run that
# dies halfway still says how far it got.
$LogPath = Join-Path $RunDir 'verify-live.log'

function Say([string]$line) {
    Write-Host $line
    Add-Content -Path $LogPath -Value $line
}

$started = @()
$failures = @()

function Start-App([string]$exe, [string[]]$extra) {
    $path = Join-Path $Bin $exe
    if (-not (Test-Path $path)) {
        throw "not built: $path (run build.ps1 first)"
    }
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($exe)
    # Splat only the arguments that exist: Start-Process rejects an empty or
    # null-containing ArgumentList.
    $launch = @{
        FilePath               = $path
        WorkingDirectory       = $Bin
        PassThru               = $true
        NoNewWindow            = $true
        RedirectStandardOutput = (Join-Path $RunDir "$stem.out")
        RedirectStandardError  = (Join-Path $RunDir "$stem.err")
    }
    if ($extra -and $extra.Count -gt 0) {
        $launch['ArgumentList'] = $extra
    }
    $proc = Start-Process @launch
    $script:started += , $proc
    Say "started $exe (pid $($proc.Id))"
    return $proc
}

function Stop-All {
    foreach ($proc in $script:started) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item $script:BodyFile -ErrorAction SilentlyContinue
}

function Get-Json([string]$url) {
    $raw = & $Curl -s --max-time 4 $url
    if (-not $raw) { return $null }
    return $raw | ConvertFrom-Json
}

$script:BodyFile = Join-Path $RunDir 'body.json'

function Post-Json([string]$url, [string]$body) {
    Set-Content -Path $script:BodyFile -Value $body -NoNewline -Encoding ascii
    $raw = & $Curl -s --max-time 4 -X POST -H 'Content-Type: application/json' `
        --data-binary "@$($script:BodyFile)" $url
    if (-not $raw) { return $null }
    return $raw | ConvertFrom-Json
}

# A verdict line that also records the failure, so the exit code at the end
# summarises the run instead of only the last thing that happened.
function Check([string]$label, [bool]$ok, [string]$detail) {
    Say ("{0,-26}: {1}{2}" -f $label, $(if ($ok) { 'PASS' } else { 'FAIL' }), $(if ($detail) { "  ($detail)" } else { '' }))
    if (-not $ok) { $script:failures += $label }
}

try {
    foreach ($port in $PlayerPort, $ControllerPort) {
        if (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) {
            throw "port $port is already in use; close the running app first"
        }
    }

    # --- Player ----------------------------------------------------------
    Start-App 'media-player-cpp.exe'
    Start-Sleep -Seconds 5

    $health = Get-Json "http://127.0.0.1:$PlayerPort/api/health"
    Check 'player /api/health' ($null -ne $health -and $health.ok) "$($health | ConvertTo-Json -Compress)"

    $status = Get-Json "http://127.0.0.1:$PlayerPort/api/status"
    $keys = ($status | Get-Member -MemberType NoteProperty).Name
    Say "player /api/status keys   : $($keys -join ', ')"
    # The eight frozen keys are the contract the Controller parses.
    $frozen = 'loaded', 'playing', 'isImage', 'clipIndex', 'clipCount',
        'clipName', 'subtitlesEnabled', 'subtitleText'
    $missing = @($frozen | Where-Object { $_ -notin $keys })
    Check 'player status contract' ($missing.Count -eq 0) "missing: $($missing -join ', ')"
    Say "player clip               : [$($status.clipIndex)] $($status.clipName)"

    Say "player GET  /api/hud      : $((Get-Json "http://127.0.0.1:$PlayerPort/api/hud") | ConvertTo-Json -Compress)"
    Say "player POST /api/hud      : $((Post-Json "http://127.0.0.1:$PlayerPort/api/hud" '{"visible":false}') | ConvertTo-Json -Compress)"
    $after = Get-Json "http://127.0.0.1:$PlayerPort/api/status"
    Check 'player HUD toggles' ($after.hudVisible -eq $false) "hudVisible=$($after.hudVisible)"
    Say "player POST /api/hud true : $((Post-Json "http://127.0.0.1:$PlayerPort/api/hud" '{"visible":true}') | ConvertTo-Json -Compress)"

    # A video, so the position actually advances: a still image has no timeline
    # by design and would prove nothing about playback. The index is looked up
    # rather than hard-coded, because the playlist is whatever media is in
    # bin/data (the library sorts images before videos, so the video is last
    # here, but nothing guarantees that on another machine).
    $clips = Get-Json "http://127.0.0.1:$PlayerPort/api/clips"
    $video = $clips | Where-Object { $_.mediaType -eq 'video' } | Select-Object -First 1
    if ($null -eq $video) {
        Check 'player position advances' $false 'no video clip in bin/data to play'
    } else {
        Say "player open the video     : $((Post-Json "http://127.0.0.1:$PlayerPort/api/clips/$($video.index)" '{}') | ConvertTo-Json -Compress -Depth 2)"
        # Resume explicitly: a freshly opened clip may still be paused, and a
        # paused clip has a position that never moves.
        Post-Json "http://127.0.0.1:$PlayerPort/api/pause" '{"paused":false}' | Out-Null
        Start-Sleep -Seconds 1
        $posBefore = (Get-Json "http://127.0.0.1:$PlayerPort/api/position").position
        Start-Sleep -Seconds 3
        $posAfter = (Get-Json "http://127.0.0.1:$PlayerPort/api/position").position
        Check 'player position advances' ($posAfter -gt $posBefore) "$($video.name): $posBefore -> $posAfter"
    }

    # --- Controller ------------------------------------------------------
    Start-App 'media-controller-cpp.exe'
    Start-Sleep -Seconds 5

    $cstatus = Get-Json "http://127.0.0.1:$ControllerPort/api/controller/status"
    Check 'controller API answers' ($null -ne $cstatus) ''
    Check 'controller sees player' ($cstatus.player.online -eq $true) "$($cstatus.player.lastError)"
    Say "controller sees clip      : [$($cstatus.player.clipIndex)] $($cstatus.player.clipName)"
    Say "controller script dir     : $($cstatus.controller.scriptDirectory)"

    # The point of the Controller: a click, a key or a script on the bar must
    # move the *Player*. Each of these goes bar -> HTTP -> player -> HTTP back.
    $before = (Get-Json "http://127.0.0.1:$PlayerPort/api/status").clipIndex
    Say "controller -> next        : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/command" '{"command":"next"}') | ConvertTo-Json -Compress -Depth 2)"
    Start-Sleep -Seconds 1
    $moved = Get-Json "http://127.0.0.1:$PlayerPort/api/status"
    Check 'controller next reaches' ($moved.clipIndex -ne $before) "clip $before -> $($moved.clipIndex)"

    # The bar can also open a clip by index. Put the video back before seeking:
    # "next" wraps to an image, and a still image has no timeline to seek in, so
    # the seek would prove nothing about the command path.
    if ($null -ne $video) {
        # Built in a variable, not inline: a double-quoted JSON literal nested
        # inside Say's double-quoted string is mangled by the parser, and the
        # server rightly answers 400 for the malformed body that results.
        $openBody = '{"open":' + $video.index + '}'
        Say "controller -> open video  : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/command" $openBody) | ConvertTo-Json -Compress -Depth 2)"
        Start-Sleep -Seconds 1
        $opened = Get-Json "http://127.0.0.1:$PlayerPort/api/status"
        Check 'controller open reaches' ($opened.clipName -eq $video.name) "player clip=$($opened.clipName)"
    }

    Say "controller -> seek 50     : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/command" '{"seek":50}') | ConvertTo-Json -Compress -Depth 2)"
    Start-Sleep -Seconds 1
    $seeked = (Get-Json "http://127.0.0.1:$PlayerPort/api/position")
    Say "player after seek         : $($seeked | ConvertTo-Json -Compress)"
    if ($null -ne $video) {
        # 50% of a clip that reports a duration must land well clear of zero.
        Check 'controller seek reaches' ($seeked.position -gt 0.1) "position=$($seeked.position)"
    }

    Say "controller -> hud off     : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/command" '{"command":"toggle-hud"}') | ConvertTo-Json -Compress -Depth 2)"
    Start-Sleep -Seconds 1
    $hudNow = (Get-Json "http://127.0.0.1:$PlayerPort/api/status").hudVisible
    Check 'controller HUD reaches' ($hudNow -eq $false) "player hudVisible=$hudNow"
    Post-Json "http://127.0.0.1:$ControllerPort/api/controller/command" '{"command":"toggle-hud"}' | Out-Null

    Say "controller scripts        : $((Get-Json "http://127.0.0.1:$ControllerPort/api/controller/scripts") | ConvertTo-Json -Compress -Depth 3)"

    $script = '{"source":"controller.Log(\"api test\") controller.OnTick(function() controller.Log(\"tick\") end)"}'
    Say "controller run script     : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/script" $script) | ConvertTo-Json -Compress)"

    # The instruction budget is what keeps a bad script from freezing the bar.
    $bad = '{"source":"while true do end"}'
    Say "controller bad script     : $((Post-Json "http://127.0.0.1:$ControllerPort/api/controller/script" $bad) | ConvertTo-Json -Compress)"
    Start-Sleep -Seconds 2
    $still = Get-Json "http://127.0.0.1:$ControllerPort/api/controller/status"
    Check 'controller survives spin' ($null -ne $still -and -not [string]::IsNullOrEmpty($still.controller.scriptError)) "scriptError=$($still.controller.scriptError)"

    # A script that drives the Player end to end, through the real API. The
    # name is resolved inside <data>/controller-scripts, which is the whole
    # point of the {"path": ...} form.
    $driver = '{"path":"controller-example.lua"}'
    $driverReply = Post-Json "http://127.0.0.1:$ControllerPort/api/controller/script" $driver
    Say "controller example script : $($driverReply | ConvertTo-Json -Compress)"
    Check 'controller path script' ($driverReply.ok -eq $true) "$($driverReply.error)"
    Start-Sleep -Seconds 4
    $driverStatus = Get-Json "http://127.0.0.1:$ControllerPort/api/controller/status"
    Say "example script running    : $($driverStatus.controller.script)"
    Say "player after script       : $((Get-Json "http://127.0.0.1:$PlayerPort/api/status").clipName)"

    # And the containment rule the resolver exists for: a path that escapes the
    # script directory is refused, not executed.
    $escape = '{"path":"..\\..\\..\\Windows\\System32\\drivers\\etc\\hosts"}'
    $escapeReply = Post-Json "http://127.0.0.1:$ControllerPort/api/controller/script" $escape
    Say "controller path escape    : $($escapeReply | ConvertTo-Json -Compress)"
    Check 'controller refuses escape' ($escapeReply.ok -eq $false) "$($escapeReply.error)"

    # --- Dashboard -------------------------------------------------------
    # The Dashboard's whole job is noticing the other two and starting them. It
    # is started last, with both already up, so what it has to report is exactly
    # the state this run produced.
    Start-App 'media-dashboard-cpp.exe'
    Start-Sleep -Seconds 4
    $dash = Get-Process -Name 'media-dashboard-cpp' -ErrorAction SilentlyContinue
    Check 'dashboard running' ($null -ne $dash) ''

    # The Dashboard decides liveness by probing these two endpoints, so they
    # answering is what it sees; its own log confirms it drew a window.
    $alive = @()
    foreach ($name in 'media-player-cpp', 'media-controller-cpp') {
        if (Get-Process -Name $name -ErrorAction SilentlyContinue) { $alive += $name }
    }
    Check 'dashboard has both apps' ($alive.Count -eq 2) ($alive -join ', ')
    $dashLog = Get-Content (Join-Path $RunDir 'media-dashboard-cpp.err') -ErrorAction SilentlyContinue
    if ($dashLog) {
        $dashLog | Where-Object { $_ -match 'render backend|player API|controller API' } |
            ForEach-Object { Say "dashboard | $_" }
    }
} finally {
    Stop-All
    # A window that died on startup explains itself on stderr; if anything
    # failed, that text is the first thing a reader needs.
    if ($failures.Count -gt 0) {
        Say "FAILURES: $($failures -join ', ')"
        foreach ($stem in 'media-player-cpp', 'media-controller-cpp', 'media-dashboard-cpp') {
            $err = Get-Content (Join-Path $RunDir "$stem.err") -ErrorAction SilentlyContinue
            if ($err) {
                Say "--- $stem stderr (tail) ---"
                $err | Select-Object -Last 10 | ForEach-Object { Say "  $_" }
            }
        }
    } else {
        Say "all checks passed"
    }
    Say "cleaned up"
    Say "log: $LogPath"
}

if ($failures.Count -gt 0) { exit 1 }
