# mediaplayer-app

![Language: C++17](https://img.shields.io/badge/language-C%2B%2B17-blue.svg)
![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)
![Platform: Windows / MSYS2](https://img.shields.io/badge/platform-Windows%20%2F%20MSYS2-0078d4.svg)
![Build: CMake + Ninja](https://img.shields.io/badge/build-CMake%20%2B%20Ninja-064f8c.svg)
![Playback: libmpv](https://img.shields.io/badge/playback-libmpv-3b5526.svg)
![Render: OpenGL 3.3 core](https://img.shields.io/badge/render-OpenGL%203.3%20core-5586a4.svg)
![HTTP API: localhost only](https://img.shields.io/badge/HTTP%20API-localhost%20only-4b8bbe.svg)
![Tests: 511 checks](https://img.shields.io/badge/tests-511%20checks-brightgreen.svg)

**One repository, three Windows applications that work together:** a libmpv video
player with a localhost HTTP control API, a scriptable control bar that drives
it, and a launcher that starts and supervises both.

> **Status:** all three build from this tree, run side by side, and talk to each
> other over HTTP — verified on this machine by `tools/verify-live.ps1`
> (15/15 checks) and by `bin/media_tests.exe` (511 checks, 0 failures). The
> openFrameworks tree, the OCR/corpus layer and the `ytdl` integration are
> **removed**. See [Known issues](#known-issues) for what is not proven yet.

## The three applications

| Application | Binary | What it is | API |
|---|---|---|---|
| **Player** | `media-player-cpp.exe` | GLFW + OpenGL 3.3 core + libmpv. Plays video, audio and stills, renders subtitles, draws a status HUD, and is scriptable. | `http://127.0.0.1:8080` |
| **Controller** | `media-controller-cpp.exe` | A title-bar-shaped control strip. An HTTP *client* of the Player with an embedded Lua 5.1 host, so a session can be scripted. Links no libmpv and owns no decoder. | `http://127.0.0.1:8081` |
| **Dashboard** | `media-dashboard-cpp.exe` | The launcher, and the one process that stays running: it lives in the notification area and starts and stops the other two. No libmpv, no Lua, no server. | none (launcher) |

There is deliberately **no widget toolkit** anywhere — no ImGui, Qt or GTK. Every
window, including the Dashboard's, draws through the same `RenderDevice` seam
that composites video, so the GUI dependency list stays at windowing, OpenGL and
libmpv.

## How they fit together

```
                       HTTP :8080
   ┌───────────────┐  (status, commands)  ┌──────────────────┐
   │    Player     │◄─────────────────────│   Controller     │
   │  libmpv + GL  │─────────────────────►│  HTTP + Lua 5.1  │
   └───────────────┘   JSON status        └──────────────────┘
           ▲                                        ▲
           │  GET /api/health                       │  GET /api/controller/status
           │                                        │
           └──────────────┬─────────────────────────┘
                          │
                  ┌───────────────┐
                  │   Dashboard   │  spawns both, stops only what it started
                  └───────────────┘
```

Three properties make this a system rather than three programs in a folder:

- **The Player's HTTP API is the contract.** The Controller reads
  `/api/status` and posts commands to it; the Dashboard asks `/api/health` on
  both ports. Nothing else crosses a process boundary — no shared memory, no
  shared DLL, no private window messages.
- **Liveness is an API answer, not process enumeration.** The Dashboard decides
  an application is up when its own health endpoint replies, which needs no
  elevation and answers the question that matters ("is it answering?") rather
  than a proxy for it. A Player launched from Explorer shows as running there
  too.
- **The shared numbers are written down once per side and asserted in the
  tests.** `ControllerHttpServer::kPlayerPort`, `AppProbe::kPlayerPort` and the
  test suite all agree, so the applications cannot drift apart silently.

## Build and run

On Windows, two batch files at the root are the short path:

| File | What it does |
|---|---|
| `build.bat` | builds everything: the three applications and the test binary |
| `run.bat` | starts the launcher in the notification area — the usual entry point |

`run.bat` starts **one** process, the launcher, and everything else is started
from its tray menu. The Player and the Controller can then be closed and
reopened without touching the launcher.

Underneath, both are thin wrappers over the PowerShell scripts below, so the
toolchain notes in [BUILDING.md](BUILDING.md) still apply and arguments pass
straight through (`build.bat -Clean` works):

```powershell
# 1. one-time: build libmpv against the ffmpeg installed on this machine
pwsh -File tools/build-libmpv.ps1

# 2. build all three applications and the test binary
pwsh -File build.ps1

# 3. run any of them
pwsh -File build.ps1 -Run                      # Player
pwsh -File build.ps1 -Run -App Controller      # Controller
pwsh -File build.ps1 -Run -App Dashboard       # the launcher
```

> On a machine with only Windows PowerShell 5.1, `powershell -File build.ps1` is
> equivalent — the scripts use no PowerShell 7 feature. `pwsh` is simply what
> they are written for.

`build.ps1` fails loudly if any of the three binaries is missing afterwards: the
Dashboard finds its neighbours **by name in its own directory**, so a partial
build would produce a launcher whose buttons cannot work. It also stages the
MinGW runtime DLLs into `bin/`, without which the executables only run inside an
MSYS2 shell.

The build must run with `PATH` confined to MSYS2. With a normal `PATH`,
`C:\Strawberry\c\bin\libwinpthread-1.dll` shadows MSYS2's and `cc1plus.exe` dies
with `STATUS_ENTRYPOINT_NOT_FOUND`, printing nothing at all. See
[BUILDING.md](BUILDING.md) before building by hand.

## Verified live

`tools/verify-live.ps1` starts the Player, drives it over its own API, starts the
Controller and checks that its commands actually reach the Player, then starts
the Dashboard. It cleans up whatever it started and exits non-zero if any check
fails. Last run on this machine:

| Check | Evidence |
|---|---|
| Player `/api/health` | `{"ok":true}` |
| Player status contract | all eight frozen keys present |
| Player HUD toggle | `hudVisible` true → false → true over HTTP |
| Playback advances | `sync-bip.mp4` position 0.97 → 4.00 over 3s of wall clock |
| Controller API | `:8081/api/controller/status` answers |
| Controller sees Player | `online: true`, clip and position mirrored |
| Controller → Player: next | player clip 2 → 0 |
| Controller → Player: open | player clip becomes the requested video |
| Controller → Player: seek | 50% of a 6.00s clip landed on `position 3.0` |
| Controller → Player: HUD | player `hudVisible` became `false` |
| Script safety | `while true do end` aborted by the instruction budget; the bar still answers |
| Script by name | `{"path":"controller-example.lua"}` loaded and ran |
| Script containment | `{"path":"..\\..\\..\\Windows\\...\\hosts"}` refused with `no such script` |
| Dashboard | runs, sees both applications, quotes both API ports |

The Player's own playback evidence, from this machine (Windows, RTX 3090,
MSYS2 MinGW64):

| Check | Result |
|---|---|
| Decode | H.264 1280x720, hardware (`d3d11va-copy`) |
| Audio output | `AO: [wasapi] 48000Hz stereo 2ch float` |
| Seek | `{"time": 4.5}` landed on exactly 4.5 |
| Loop | end-of-file reloads the clip |

### Format matrix

Generated with ffmpeg and played back through the HTTP API. Every entry advanced
its position with no decode error logged:

| File | Codec | Audio | Decoder used |
|---|---|---|---|
| `h264.mp4` | H.264 | AAC | `d3d11va-copy` (hardware) |
| `hevc.mkv` | HEVC | Opus | `d3d11va-copy` (hardware) |
| `vp9.webm` | VP9 | Opus | `d3d11va-copy` (hardware) |
| `mpeg4.avi` | MPEG-4 Part 2 | MP3 | `MPEG-4 part 2` (software) |
| `flac.mkv` | H.264 | FLAC | `d3d11va-copy` (hardware) |
| `page-portrait.png` | PNG still | – | `image2`, held |
| `wide-photo.jpg` | JPEG still | – | `image2` (MJPEG), held |

Hardware decode is attempted first (`hwdec=auto-safe`) and falls back to software
per codec, which is why `mpeg4.avi` reports a software decoder.

Stills and video take the same path: mpv decodes both and draws into the app's
FBO, with `image-display-duration=inf` holding a still on screen. An image
therefore has no timeline — `seekable` is false, `playing` is false and
`duration` is 0, whatever transport a host sends.

## The Player

Plays video with audio, hardware decoding and correct A/V sync; plays still
images from the same playlist; renders subtitles through mpv's own engine; loads
Lua/JS scripts to extend behaviour; is driven entirely over a localhost HTTP API;
and draws its status HUD as textured quads on the GL canvas.

```
media-player-cpp.exe [--width N] [--height N] [--fullscreen] [--no-hud] [--port N]
Keys:  H toggle HUD   F11 toggle fullscreen   Esc quit
```

`http://127.0.0.1:8080` (localhost only)

| Action | Method | Endpoint |
|---|---|---|
| Health | GET | `/api/health` |
| Status | GET | `/api/status` |
| Position | GET | `/api/position` |
| Playlist | GET | `/api/clips` |
| Play | POST | `/api/play` |
| Stop | POST | `/api/stop` |
| Next | POST | `/api/next` |
| Previous | POST | `/api/previous` |
| Pause | POST | `/api/pause` + `{"paused": true}` (no body = toggle) |
| Seek | POST | `/api/seek` + `{"time": 12.5}` / `{"relative": -5}` / `{"percent": 50}` |
| Speed | POST | `/api/speed` + `{"speed": 1.0}` |
| Volume | POST | `/api/volume` + `{"volume": 0..100}` |
| Subtitles | POST | `/api/subtitles` + `{"enabled": true}` / `{"text": "..."}` |
| HUD | GET/POST | `/api/hud` + `{"visible": true}` |
| Fullscreen | GET/POST | `/api/fullscreen` + `{"visible": true}` |
| Open clip | POST | `/api/clips/{index}` |
| Rescan media | POST | `/api/clips/rescan` |
| Scripts | GET/POST | `/api/scripts`, `/api/scripts/rescan` |

The first eight keys of `/api/status` — `loaded`, `playing`, `isImage`,
`clipIndex`, `clipCount`, `clipName`, `subtitlesEnabled`, `subtitleText` — are a
**frozen contract**. Everything else on that object is additive, so a client
speaking the older surface keeps working.

Threading: HTTP workers never touch the player. Each handler submits a closure to
a queue and blocks; `HttpControlServer::poll()` runs it on the main thread once
per frame, so decoding and compositing stay single-threaded while the API answers
concurrently.

## The Controller

A control strip that sits beside the Player and drives it over HTTP. It shows the
Player's live state, offers transport buttons and a seek bar, forwards keyboard
shortcuts, and runs Lua scripts that issue real HTTP commands to the Player.

```
media-controller-cpp.exe [--width N] [--height N]
                         [--player-host HOST] [--player-port N] [--api-port N]
                         [--script FILE] [--list-scripts] [--start-offline]
Keys:  Space play/pause   H HUD   F fullscreen   S subtitles   R reload script   Esc quit
```

`http://127.0.0.1:8081` (localhost only)

| Action | Method | Endpoint |
|---|---|---|
| Bar + Player state | GET | `/api/controller/status` |
| Scripts on disk | GET | `/api/controller/scripts` |
| Run a script | POST | `/api/controller/script` + `{"path":"x.lua"}` or `{"source":"..."}` |
| Stop the script | POST | `/api/controller/stop-script` |
| Reload from disk | POST | `/api/controller/reload-script` |
| Drive the Player | POST | `/api/controller/command` + `{"command":"next"}`, `{"open":0}`, `{"seek":50}` |

Scripts are named, not pathed: `{"path": ...}` accepts a bare file name that
discovery found under `<data>/controller-scripts`, so the route cannot open
anything else on disk. A path that escapes the directory is refused with
`no such script`.

## The Dashboard (the launcher)

The one piece of friction in a multi-app layout is knowing which executable to
start in what order. The Dashboard removes it, and it is the one thing that stays
running: it lives in the **notification area**, and the Player and the Controller
come and go from its menu.

```
run.bat
  └─ media-dashboard-cpp.exe --tray        one process, in the tray
       ├─ Launch Player      -> media-player-cpp.exe      :8080
       └─ Launch Controller  -> media-controller-cpp.exe  :8081
```

| Where | What |
|---|---|
| right-click the tray icon | Launch Player / Launch Controller / Stop Player / Stop Controller / Show-Hide Dashboard / **Quit** |
| left-click the tray icon | show or hide the launcher window |
| the launcher window | one row per application with its state, path and port, and a LAUNCH or STOP button |
| the window's close box (or Esc) | **hides** the launcher; it does not exit |
| QUIT in the tray menu | the only exit; it also stops the applications this launcher started |

Three deliberate properties:

- **A Player or Controller started elsewhere is never killed from here.**
  `AppProbe::stop` only acts on a child this process created, which is also why
  the tray's Stop items are greyed out for anything it did not start.
- **One launcher per session.** A second `run.bat` notices the first and exits,
  rather than putting a second icon in the tray and managing the same two
  applications again.
- **No invisible processes.** If the tray icon cannot be created — a locked or
  remote session can refuse `Shell_NotifyIcon`, and this machine's session
  currently refuses it for *any* program — the launcher shows its window and
  closing that window really exits. A launcher with no icon and no window would
  be a process only Task Manager could reach.

Its log goes to `bin/dashboard.log` when `run.bat` starts it, because a tray
application has no console to print to. Launched from a console, it prints there
as usual. Liveness comes from each application's health endpoint, polled on a
background thread — never on the frame loop, which must not wait on a socket.

## Scripting

Two independent script hosts, with deliberately different powers:

| | Player scripts | Controller scripts |
|---|---|---|
| Engine | mpv's own Lua/JS (inside libmpv) | embedded Lua 5.1 |
| Directory | `bin/data/scripts/` | `bin/data/controller-scripts/` |
| Can do | everything the player can: mpv properties, events, tracks | only what the Controller can: HTTP commands to the Player |
| Reload | **restart only** — mpv cannot attach a script after `mpv_initialize()` | real reload: `R`, `POST /api/controller/reload-script`, or just edit the file on disk (picked up in half a second) |
| Reference | [scripts/media-player.lua](scripts/media-player.lua) | [scripts/controller-example.lua](scripts/controller-example.lua) |

Both directories are build outputs: the sources of truth are in
[scripts/](scripts/), and the build installs them into `bin/data/`.

Two limits worth knowing up front. A Lua script must be **pure ASCII with no
byte-order mark** — Lua 5.1 treats a BOM as a syntax error. And a Controller
script cannot freeze the bar: a per-tick VM instruction budget aborts a loop that
never yields (`while true do end` is contained, and the bar keeps answering), and
`controller.Sleep(ms)` charges a wall-clock budget instead of blocking, so a
script reads as a sequence of steps spread across frames.

## Release bundle

```powershell
pwsh -File tools/package_release.ps1
```

Assembles `dist/mediaplayer-app/` with all three executables, the script
directories and the resolved DLL closure, then **verifies it** by running each
executable with MSYS2 removed from `PATH` and asking the two that have an API to
answer on it. That check matters: the Player links libmpv, which drags in a large
transitive set (FFmpeg, libass, libplacebo, Lua, MuJS, the MinGW runtime), and a
missing DLL kills the process before `main()` with no message at all. Copying
"the exe and libmpv" is not enough — the resolved closure is ~115 DLLs.

## Security posture

Behaviour is deliberately constrained:

- `ytdl=no` — never spawns an external `youtube-dl`/`yt-dlp` subprocess.
- `load-scripts=no` — the user's `~/.config/mpv/scripts/` is never auto-loaded.
- `config=no`, no `input-conf` — user config and key bindings are ignored.
- `access-references=no`, `autoload-files=no`, `load-unsafe-playlists=no`.
- Local files only; no network input unless explicitly enabled.
- Every HTTP route is **localhost only** and caps request size. A route that
  takes a path accepts files inside the data directory only — and the
  Controller's script route takes a *name*, not a path, for the same reason.
- A script is a trusted, operator-installed artefact that runs with the full
  authority of the application that loaded it. What is not permitted is
  *implicit* execution.

## Testing

```powershell
# headless: the API contract, playlist logic, Controller request mapping and
# script-host behaviour, with a stub Player and no window
cd bin; .\media_tests.exe

# live: starts real windows and checks the three applications against each other
powershell -File tools\verify-live.ps1

# the launcher's tray plumbing: hide-on-close, one instance, QUIT, and the
# no-icon fallback that keeps the process reachable
powershell -File tools\verify-launcher.ps1
```

`tools/soak.ps1` and `tools/stress-switch.ps1` measure memory and thread growth
over a long run and hammer rapid clip switching; `p0/build.ps1 <file>` builds and
runs the standalone audio probe, which reports `current-ao` and `audio-pts`
without needing a window.

## Repository layout

```
run.bat                       start the launcher in the notification area
build.bat                     build everything on Windows
src/main.cpp                  Player: window, frame loop, wiring
src/controller_main.cpp       Controller: bar window, input, Lua host wiring
src/dashboard_main.cpp        Launcher: window, tray, probe thread, frame loop
src/app/HttpControlServer     the Player's control plane
src/app/control/              Controller: model, view, PlayerClient, Lua host, its API
src/app/dashboard/            Launcher: model, view, AppProbe, TrayIcon
src/app/http/                 shared HTTP/JSON client and the submit-and-poll queue
src/app/render/               RenderDevice + the one OpenGL implementation
src/app/hud/                  bitmap font for the HUD and the bars
src/backends/mpv/             MPVSurface: the only file that attaches mpv to GL
src/core/                     logging, platform paths
src/media/                    playlist, playback controller, script discovery
tests/                        the headless suite
tools/                        build, package, soak, stress and verify scripts
p0/                           standalone libmpv audio probe
scripts/                      reference scripts (installed into bin/data by the build)
```

**The `RenderDevice` rule (hard):** nothing above `src/app/render/` may name a
graphics API. `RenderDevice.h` exposes textured quads, solid/outline rects,
scissor clipping, a 5x7 text draw and a texture pool. `GlRenderDevice.cpp` is
the only translation unit that includes GL headers. `MPVSurface` is the one
sanctioned exception, because it must attach mpv's output to a GL texture.

## Known issues

**An unexplained crash under sustained load.** A 10-minute instrumented soak
(`tools/soak.ps1`, 1560 HTTP requests) died at 443s with no error output, the
last log line being `VO: [libmpv] 1400x2000 rgba`. What is known: 0 request
failures across all 1560 calls; thread count fell 55 → 49 and handles 1379 →
1370, so no thread/handle leak; RSS crept 188.8 → 194.6 MB over 7.4 minutes
(~0.8 MB/min), plateauing rather than running away.

It is **not reproducible on demand**: `tools/stress-switch.ps1` survived 600
rapid clip switches. Next step if it recurs: capture a dump (`procdump` or
Windows Error Reporting) and read the faulting module — that distinguishes an
mpv bug from ours.

### Not yet implemented

- **A long soak run.** Short runs and the format matrix pass. No clean 24-hour
  run exists yet.
- **The neighbour mosaic.** The original app tiled the previous/next clips
  around the current one. That layout was dropped with the OCR/region work; only
  the single width-fit frame is drawn today.
- **Vulkan Video.** Not reachable: `MPV_RENDER_API_TYPE_OPENGL` is the only
  hardware-backed type mpv's render API defines, and `vulkan` hwdec needs
  `vo=gpu-next`. See [BUILDING.md](BUILDING.md).

## Stack

| Concern | Library |
|---|---|
| Window / input | GLFW 3.4 |
| Rendering | OpenGL 3.3 core (GLEW loader) |
| Video + audio + A/V clock | libmpv, `vo=libmpv` render API |
| Images | stb_image |
| On-screen HUD and bars | none — drawn through `RenderDevice` on the GL canvas |
| HTTP server (Player, Controller) | cpp-httplib |
| HTTP client (Controller, Dashboard) | cpp-httplib's own client |
| JSON | nlohmann/json |
| Controller scripting | Lua 5.1 |

libmpv is built from source against the ffmpeg installed on the machine — see
[BUILDING.md](BUILDING.md) for why that is required rather than optional.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

The libmpv **client API** headers are ISC-licensed (they exist to allow external
wrappers), while the mpv core is GPLv2+. Because this application links the GPL
build of libmpv, the combined work is distributed under the GPL. The previous
Apache-2.0 licence text is kept in [LICENSE-APACHE-2.0](LICENSE-APACHE-2.0) for
the record.

Copyright (c) vecnode 2026
