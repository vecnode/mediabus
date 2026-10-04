# AGENTS.md — mediaplayer-app

Guidance for AI coding agents (and humans) working in this repository. This file
is the canonical agent guide.

## What this repository is

**Three cooperating Windows applications built from one tree**, all on the same
stack — GLFW 3 for the window and input, OpenGL 3.3 core for rendering, and no
widget toolkit anywhere:

| Target | Source entry point | What it is |
| ------ | ------------------ | ---------- |
| `media-player-cpp` | `src/main.cpp` | The player: libmpv (render API) for video, audio, A/V sync and subtitles, plus a localhost HTTP control API on `:8080`. Scriptable with Lua/JS through mpv. |
| `media-controller-cpp` | `src/controller_main.cpp` | A title-bar-shaped control strip. An HTTP **client** of the player, with an embedded Lua 5.1 host and its own API on `:8081`. Links no libmpv and owns no decoder. |
| `media-dashboard-cpp` | `src/dashboard_main.cpp` | The launcher, and the one process that stays running: it lives in the notification area and starts and stops the other two from there. Probes each health endpoint on a background thread. No libmpv, no Lua, no server. **Windows subsystem binary**: it must never grow a console window. |

There is **no widget toolkit** — no ImGui, no Qt, no GTK. The player's HUD, the
controller bar and the dashboard are all drawn as quads through the same
`RenderDevice` seam that composites video. The launcher's tray icon is shell
integration rather than rendering, and it lives in
`src/app/dashboard/TrayIcon.{h,cpp}` — the only Win32 in the launcher.

`media_tests.exe` is a fourth, headless target: it links the applications' logic
without a GL context.

### The launcher is the entry point, and it is a tray application

`run.bat` starts it; everything else is started from its tray menu. Three
properties are load-bearing and easy to break:

- **Closing its window hides it** (as does Esc) — QUIT in the tray menu is the
  only exit. That is the whole point: the Player and the Controller can be closed
  and reopened without losing the launcher.
- **No invisible processes.** If `Shell_NotifyIcon` fails, `TrayIcon::create`
  returns `Unavailable` and the launcher shows its window, where closing really
  exits. Never let `--tray` leave a process with no icon and no window.
- **One launcher per session**, guarded by a named mutex held for the process's
  lifetime. A second launch exits quietly rather than managing the same two
  applications again. Do not release that mutex on a tray failure.

The tray's actions and the window's buttons both go through the same
`Dashboard::handle`, so the two paths cannot disagree about what is running.
`tools/verify-launcher.ps1` drives all of it through the real binary.

### How the three applications talk to each other

**The player's HTTP API is the contract that matters.** Nothing else crosses a
process boundary:

- the Controller polls `GET /api/status` on `:8080` and posts commands to it;
- the Dashboard asks `GET /api/health` on both `:8080` and `:8081` to decide
  liveness, and finds the executables by name **in its own directory** — a
  side-by-side layout is therefore required, and `build.ps1` fails if any of the
  three binaries is missing;
- the port numbers are declared once per side (`ControllerHttpServer::kPlayerPort`,
  `AppProbe::kPlayerPort`/`kControllerPort`) and asserted by the test suite.

Keep existing routes and response shapes **stable and additive**: external
clients (including the separate `vecnode/metaagent` agent controller) speak the
same surface, and the Controller parses it field by field.

This was formerly an openFrameworks project. The openFrameworks tree, its
addons, the OCR/corpus layer and the `ytdl` integration were all removed; see
"History" below.

## Build & run

The build must use a **PATH confined to MSYS2**. With a normal PATH,
`C:\Strawberry\c\bin\libwinpthread-1.dll` shadows MSYS2's and `cc1plus.exe`
dies with `STATUS_ENTRYPOINT_NOT_FOUND`, printing nothing at all. The scripts
handle this; do not invoke the toolchain by hand without reading BUILDING.md.

```powershell
pwsh -File tools/build-libmpv.ps1   # one-time: libmpv against the local ffmpeg
pwsh -File build.ps1                # configure + build all three apps and the tests
pwsh -File build.ps1 -Run           # build and launch the player
pwsh -File build.ps1 -Run -App Controller   # or -App Dashboard
```

On the root there are also two batch wrappers, which are what a person double
clicks and what the README leads with: `build.bat` (all of the above, with
PowerShell 7 or 5.1 whichever exists) and `run.bat` (start the launcher in the
tray). Keep them thin — `build.ps1` is where the logic belongs.

On a machine that only has Windows PowerShell 5.1, `powershell -File build.ps1`
is equivalent — the scripts use no PowerShell 7 feature. Do not assume `pwsh`
exists.

- Binaries: `bin/media-player-cpp.exe`, `bin/media-controller-cpp.exe`,
  `bin/media-dashboard-cpp.exe`, plus `bin/libmpv-2.dll` (vendored).
- Tests: `bin/media_tests.exe` (run with `bin/` as the working directory).
- Live check: `powershell -File tools/verify-live.ps1` starts all three, drives
  them through each other's APIs and exits non-zero on any failed check.
- Launcher check: `powershell -File tools/verify-launcher.ps1` covers the tray
  behaviours below, which no headless test can reach.
- The apps resolve `bin/data/` **relative to the executable**, so the working
  directory does not matter for media lookup.
- Never launch a child with a bare `Start-Process` in a script that may run
  non-interactively: it attaches a new console and blocks the parent forever
  with no output. Use `-NoNewWindow` and redirect stdio, as `verify-live.ps1`
  does. `run.bat` uses `Start-Process` for the opposite reason — to detach and
  redirect — and deliberately not `start`, which does not pass a redirection on
  to its child.

### Why libmpv is built from source

The MSYS2 `mingw-w64-x86_64-mpv` package fails at load:

```
libavcodec: build version 62.28.101 incompatible with runtime version 62.28.100
```

It was built against a libavcodec one patch newer than the installed ffmpeg, and
`mpv.exe --version` fails too. `tools/build-libmpv.ps1` builds mpv against the
ffmpeg that is actually present and vendors the result into `bin/` and `lib/`.
**Do not replace this with a pacman package.**

## Architecture

```
src/main.cpp                  Player: window, frame loop, wiring
src/controller_main.cpp       Controller: bar window, input, Lua host wiring
src/dashboard_main.cpp        Launcher: window, tray, probe thread, frame loop
src/app/HttpControlServer     the player's control plane
src/app/control/              Controller: ControllerModel/View, PlayerClient,
                              LuaControllerScript, ControllerHttpServer
src/app/dashboard/            Launcher: DashboardModel/View, AppLauncher (AppProbe),
                              TrayIcon (the only Win32 in the launcher)
src/app/http/                 CommandQueue + HttpJsonClient, shared by both sides
src/app/render/               RenderDevice + the one OpenGL implementation
src/app/hud/                  bitmap font, glyph data
src/backends/mpv/MPVSurface   the only file that attaches mpv to GL
src/core/                     Log, Platform (exe dir, data dir, scripts dir, extensions)
src/media/                    IClipSource, MediaClipLibrary, MediaPlayerController, ScriptHost
```

**The `RenderDevice` rule (hard):** nothing above `src/app/render/` may name a
graphics API — and that includes all three `main` files. `RenderDevice.h` exposes
textured quads, solid/outline rects, scissor clipping, a 5x7 text draw and a
texture pool. `GlRenderDevice.cpp` is the only translation unit that includes GL
headers. A Vulkan or D3D12 backend is a new implementation of that header, not a
rewrite. `MPVSurface` is the one sanctioned exception, because it must attach
mpv's output to a GL texture.

**Keep the presentation logic free of GL, HTTP and Lua.** `ControllerModel`,
`DashboardModel` and `ControllerHttpServer::execute` are value-level: that is
what lets the test suite exercise layout, hit tests, the route table and the
script host with no window, no socket and no player.

**Threading:**

- The main thread owns the GL context, the frame loop and the decoder. libmpv's
  render API requires the GL context to be current in the calling thread and to
  be the same context the render context was created with; rendering on the
  main thread satisfies that by construction.
- HTTP worker threads **never** touch the controller, the decoder or the Lua
  state. Each handler submits a closure to a queue and waits; `poll()` runs it
  on the main thread once per frame. Preserve that on both servers.
- The Controller's `PlayerClient` poll thread publishes a snapshot under a mutex
  and does nothing else; the frame loop reads the copy.
- The Dashboard's probe thread does the HTTP requests; the frame loop only reads
  the published snapshot, because it must never wait on a socket.
- `MPVSurface::onRenderUpdate` runs on an mpv thread and only sets an atomic
  flag. Never call GL or another mpv function from it.

## HTTP APIs

The player, `http://127.0.0.1:8080`, localhost only:

| Action | Method | Endpoint |
| ------ | ------ | -------- |
| Status | GET  | `/api/status` |
| Position | GET | `/api/position` |
| Playlist | GET | `/api/clips` |
| Health | GET | `/api/health` |
| Scripts | GET | `/api/scripts` |
| Play | POST | `/api/play` |
| Stop | POST | `/api/stop` |
| Next | POST | `/api/next` |
| Previous | POST | `/api/previous` |
| Pause | POST | `/api/pause` |
| Seek | POST | `/api/seek` |
| Speed | POST | `/api/speed` |
| Volume | POST | `/api/volume` |
| Subtitles | POST | `/api/subtitles` |
| HUD | GET/POST | `/api/hud` |
| Fullscreen | GET/POST | `/api/fullscreen` |
| Open clip | POST | `/api/clips/{index}` |
| Rescan media | POST | `/api/clips/rescan` |
| Rescan scripts | POST | `/api/scripts/rescan` |

The first eight keys of `/api/status` (`loaded`, `playing`, `isImage`,
`clipIndex`, `clipCount`, `clipName`, `subtitlesEnabled`, `subtitleText`) are a
**frozen contract**. Everything else on that object is additive.

The controller, `http://127.0.0.1:8081`, localhost only:

| Action | Method | Endpoint |
| ------ | ------ | -------- |
| Bar + player state | GET | `/api/controller/status` |
| Scripts on disk | GET | `/api/controller/scripts` |
| Run a script | POST | `/api/controller/script` (`{"path":"x.lua"}` or `{"source":"..."}`) |
| Stop the script | POST | `/api/controller/stop-script` |
| Reload from disk | POST | `/api/controller/reload-script` |
| Drive the player | POST | `/api/controller/command` (`{"command":"next"}`, `{"open":0}`, `{"seek":50}`) |

## Conventions & guardrails

- **Match the surrounding C++ style:** tabs for indentation, `media::` namespace,
  `LOG_NOTICE("Category") << ...` for logging, `#pragma once`.
- **Security posture is deliberate.** These stay off: `ytdl` (the default spawns
  an external downloader subprocess), `load-scripts` (would auto-run anything in
  the user's mpv config dir), `config`, `input-conf`, `access-references`,
  `autoload-files`, `load-unsafe-playlists`. Only `bin/data/scripts` and
  `bin/data/controller-scripts` are ever scanned, and **any route taking a path
  accepts files inside the data directory only** — the Controller's script route
  takes a *name*, resolved by `findControllerScript`, and refuses anything with a
  directory component. Keep it that way.
- **mpv option names differ from the CLI.** libmpv's option table has `scripts`
  (a path list), not `script`; setting the wrong name fails with
  `option not found`. `p0/audio_probe.cpp` shows the pattern for asking libmpv
  directly (create, set options, initialize, print properties) rather than
  guessing — copy it when you need to probe another option.
- **MSYS-style paths from PowerShell must be quoted** (`"-IC:/msys64/..."`), or
  the linker resolves nothing. A Windows path with backslashes becomes an
  invalid escape in CMake and meson generated files. Forward slashes everywhere.
- **Lua scripts must be pure ASCII with no BOM** — Lua 5.1 treats a byte-order
  mark as a syntax error, and a mangled em-dash breaks the chunk.
- **JSON bodies in PowerShell must not go through an argument.** Windows
  PowerShell 5.1 strips the inner double quotes from a native argument, so
  `-d '{"a":1}'` reaches curl as `{a:1}`. Write the body to a file and use
  `--data-binary "@file"` (see `verify-live.ps1`) instead of debugging a 400 the
  server was right to send.
- **Tests are the contract.** Add a check to `tests/test_main.cpp` for any API
  or library behaviour you change; it runs without a GL context. For anything
  visual or cross-process, add it to `tools/verify-live.ps1` instead, and prefer
  screenshots over assertions.
- **Do not commit build output, media, or the mpv build cache.** `bin/*` (except
  `bin/data/`), `build/`, `.cache/`, `obj/`, binaries and `*.mp4`/`*.png` test
  media are git-ignored. `bin/libmpv-2.dll`, `lib/libmpv.dll.a` and `lib/mpv/*.h`
  are intentionally tracked so a clone runs without a source build. The installed
  copies under `bin/data/scripts/` and `bin/data/controller-scripts/` are build
  outputs — `scripts/` holds the sources of truth.

## History

Removed in the libmpv migration, and not to be reintroduced:

- the openFrameworks project shell (`Makefile`, `config.make`, `addons.make`,
  `make`-based tooling, the `ofApp` lifecycle),
- `PlatformVideo` and the Media Foundation backend that `#include`d
  openFrameworks' own `.cpp` files,
- the entire OCR/corpus layer (`MediaCorpusProvider`, `src/metaagent/**`,
  `PDF_TEXT.md`/`OBJS_TEXT.md`, region framing and its debug box),
- `ytdl` and the external-downloader subprocess path,
- the `SubtitlesOverlay` text renderer (mpv renders subtitles itself).

`LICENSE-APACHE-2.0` records the licence this project used before the move to
GPL-2.0-or-later, which the GPL libmpv makes necessary.
