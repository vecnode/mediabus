# AGENTS.md — media-player-cpp

Guidance for AI coding agents (and humans) working in this repository. This file
is the canonical agent guide; `CLAUDE.md` defers to it.

## What this repository is

A **C++ video player with a localhost HTTP control API**, built on:

- **GLFW 3** for the window and input,
- **OpenGL 3.3 core** for rendering,
- **libmpv** (render API) for video, audio, A/V sync and subtitles.

It is also **scriptable**: operator-installed Lua or JS scripts extend player
behaviour without recompiling.

There is **no widget toolkit** — no ImGui, no Qt, no GTK. The on-screen HUD is
drawn as quads through the same `RenderDevice` seam that composites video.

This was formerly an openFrameworks project. The openFrameworks tree, its
addons, the OCR/corpus layer and the `ytdl` integration were all removed; see
"History" below.

### Role in the larger system

This is **app #3 of a three-app system**, controlled by **metaagent** (the C++
agent controller, repo `vecnode/metaagent`). metaagent drives playback over this
app's HTTP API on `:8080` and can build/run the process for centralised control.

**The HTTP API is the contract that matters.** Keep existing routes and response
shapes stable and additive — metaagent's `/api/media/*` proxy depends on them.

## Build & run

The build must use a **PATH confined to MSYS2**. With a normal PATH,
`C:\Strawberry\c\bin\libwinpthread-1.dll` shadows MSYS2's and `cc1plus.exe`
dies with `STATUS_ENTRYPOINT_NOT_FOUND`, printing nothing at all. The scripts
handle this; do not invoke the toolchain by hand without reading BUILDING.md.

```powershell
pwsh -File tools/build-libmpv.ps1   # one-time: libmpv against the local ffmpeg
pwsh -File build.ps1                # configure + build
pwsh -File build.ps1 -Run           # build and launch
```

- Binary: `bin/media-player-cpp.exe`, plus `bin/libmpv-2.dll` (vendored).
- Tests: `bin/media_tests.exe` (run with `bin/` as the working directory).
- The app resolves `bin/data/` **relative to the executable**, so the working
  directory does not matter for media lookup.

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
src/core/       Log, Platform (exe dir, data dir, scripts dir, extensions)
src/media/      IClipSource, MediaClipLibrary, MediaPlayerController, ScriptHost
src/app/        HttpControlServer, render/{RenderDevice,GlRenderDevice}, hud/
src/backends/   mpv/MPVSurface
src/main.cpp    window, frame loop, wiring
```

**The `RenderDevice` rule (hard):** nothing above `src/app/render/` may name a
graphics API. `RenderDevice.h` exposes textured quads, solid/outline rects,
scissor clipping, a 5x7 text draw and a texture pool. `GlRenderDevice.cpp` is
the only translation unit that includes GL headers. A Vulkan or D3D12 backend
is a new implementation of that header, not a rewrite. `MPVSurface` is the one
sanctioned exception, because it must attach mpv's output to a GL texture.

**Threading:**

- The main thread owns the GL context, the frame loop and the decoder. libmpv's
  render API requires the GL context to be current in the calling thread and to
  be the same context the render context was created with; rendering on the
  main thread satisfies that by construction.
- HTTP worker threads **never** touch the controller or the decoder. Each
  handler submits a closure to a queue and waits; `HttpControlServer::poll()`
  runs it on the main thread once per frame. Preserve that.
- `MPVSurface::onRenderUpdate` runs on an mpv thread and only sets an atomic
  flag. Never call GL or another mpv function from it.

**Scripts** (see `ScriptHost.h`) are discovered only under `bin/data/scripts`
and attached **before** `mpv_initialize()`, because mpv only reads the option
then. mpv cannot attach or detach a script at runtime, so reload means restart —
the API says so rather than pretending otherwise.

## HTTP API

`http://127.0.0.1:8080`, localhost only.

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
| Open clip | POST | `/api/clips/{index}` |
| Rescan media | POST | `/api/clips/rescan` |
| Rescan scripts | POST | `/api/scripts/rescan` |

The first eight keys of `/api/status` (`loaded`, `playing`, `isImage`,
`clipIndex`, `clipCount`, `clipName`, `subtitlesEnabled`, `subtitleText`) are a
**frozen contract**. Everything else on that object is additive.

## Conventions & guardrails

- **Match the surrounding C++ style:** tabs for indentation, `media::` namespace,
  `LOG_NOTICE("Category") << ...` for logging, `#pragma once`.
- **Security posture is deliberate.** These stay off: `ytdl` (the default spawns
  an external downloader subprocess), `load-scripts` (would auto-run anything in
  the user's mpv config dir), `config`, `input-conf`, `access-references`,
  `autoload-files`, `load-unsafe-playlists`. Only `bin/data/scripts` is ever
  scanned, and any route taking a path accepts files inside the data directory
  only. See `MPVSurface::applyOptions` and `HttpControlServer::addClipPath`.
- **mpv option names differ from the CLI.** libmpv's option table has `scripts`
  (a path list), not `script`. `p0/option_probe.cpp` answers this kind of
  question in seconds; use it rather than guessing.
- **MSYS-style paths from PowerShell must be quoted** (`"-IC:/msys64/..."`), or
  the linker resolves nothing. A Windows path with backslashes becomes an
  invalid escape in CMake and meson generated files. Forward slashes everywhere.
- **Lua scripts must be pure ASCII with no BOM** — Lua 5.1 treats a byte-order
  mark as a syntax error, and a mangled em-dash breaks the chunk.
- **Tests are the contract.** Add a check to `tests/test_main.cpp` for any API
  or library behaviour you change; it runs without a GL context.
- **Do not commit build output, media, or the mpv build cache.** `bin/*` (except
  `bin/data/`), `build/`, `.cache/`, `obj/`, binaries and `*.mp4`/`*.png` test
  media are git-ignored. `bin/libmpv-2.dll`, `lib/libmpv.dll.a` and `lib/mpv/*.h`
  are intentionally tracked so a clone runs without a source build.

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
