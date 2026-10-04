# mediaplayer-cpp

![Language](https://img.shields.io/badge/language-C%2B%2B-blue.svg)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)

A scriptable video player with a localhost HTTP control API, built on
**libmpv** for playback and **GLFW + OpenGL 3.3** for display.

> **Status:** video playback with audio and scripting are working and verified on
> this machine. The openFrameworks tree, the OCR/corpus layer and the `ytdl`
> integration are **removed**. Images and release packaging are still to come.

## Verified playback

Numbers below are from this machine (Windows, RTX 3090, MSYS2 MinGW64):

| Check | Result |
|---|---|
| Decode | H.264 1280x720, hardware (`d3d11va-copy`) |
| Playback rate | position advanced 1.73s -> 3.77s over 2s of wall clock |
| Seek | `{"time": 4.5}` landed on exactly 4.5 |
| Pause / play | `paused` / `playing` reflected over HTTP |
| Audio decoded | AAC, 2ch @ 48 kHz (Opus / AC-3 / FLAC / PCM also come via libavcodec) |
| Audio output | `current-ao = wasapi`, `ao-volume = 100` |
| A/V clock | `time-pos 2.367` vs `audio-pts 2.337` |
| Loop | end-of-file reloads the clip |

### Format matrix

Generated with ffmpeg and played back through the HTTP API on this machine.
Every entry advanced its position with no decode error logged:

| File | Codec | Audio | Decoder used |
|---|---|---|---|
| `h264.mp4` | H.264 | AAC | `d3d11va-copy` (hardware) |
| `hevc.mkv` | HEVC | Opus | `d3d11va-copy` (hardware) |
| `vp9.webm` | VP9 | Opus | `d3d11va-copy` (hardware) |
| `mpeg4.avi` | MPEG-4 Part 2 | MP3 | `MPEG-4 part 2` (software) |
| `flac.mkv` | H.264 | FLAC | `d3d11va-copy` (hardware) |
| `page-portrait.png` | PNG still | - | `image2`, held |
| `wide-photo.jpg` | JPEG still | - | `image2` (MJPEG), held |

Hardware decode is attempted first (`hwdec=auto-safe`) and falls back to
software per codec, which is why `mpeg4.avi` reports a software decoder.

Stills and video take the same path: mpv decodes both and draws into the app's
FBO, with `image-display-duration=inf` holding a still on screen. An image
therefore has no timeline — `seekable` is false, `playing` is false and
`duration` is 0, whatever transport a host sends.

Reproduce with the standalone audio probe (`p0/build.ps1 <file>`, which reports
`current-ao` and `audio-pts` without needing a window) and
`bin/media_tests.exe` for the control plane.

## What it does

- Plays video with audio, hardware decoding and correct A/V sync.
- Plays still images from the same playlist, width-fit and vertically centred.
- Renders subtitles through mpv's own subtitle engine.
- Loads Lua/JS scripts from `bin/data/scripts/` to extend player behaviour.
- Is driven entirely over a localhost HTTP API.
- Draws a status HUD on the GL canvas, with no widget toolkit.

## Scripting

Drop a `.lua` or `.js` file into `bin/data/scripts/` and restart. Scripts run
inside mpv's own engine and get the full authority of the player — that is the
point of them — but only that one directory is ever scanned, and mpv's
`load-scripts` option stays off so nothing in your personal mpv config can run.

See [scripts/media-player.lua](scripts/media-player.lua) for a reference script
that observes playback, acts on the player, and exchanges messages with the host.

Two limits worth knowing up front: mpv cannot attach a script once it has
started, so **loading changes requires a restart** (`GET /api/scripts` reports
what loaded, and says so honestly rather than pretending to hot-reload). And a
Lua script must be pure ASCII with no byte-order mark — Lua 5.1 treats a BOM as
a syntax error.

## Release bundle

```powershell
pwsh -File tools/package_release.ps1
```

Assembles `dist/media-player-cpp/` and then **verifies it** by running the
executable with MSYS2 removed from `PATH`. That check matters: the app links
libmpv, which drags in a large transitive set (FFmpeg, libass, libplacebo, Lua,
MuJS, the MinGW runtime), and a missing DLL kills the process before `main()`
with no message at all. Copying "the exe and libmpv" is not enough — the
resolved closure is ~115 DLLs.

`build.ps1` stages the same runtime DLLs into `bin/`, so a locally built binary
also runs without MSYS2 on `PATH`. That was not true until the soak harness
caught it: launching `bin/` from Explorer died with `0xC0000135` and no output.

## Open issues

**An unexplained crash under sustained load.** A 10-minute instrumented soak
(`tools/soak.ps1`, 1560 HTTP requests) died at 443s with no error output, the
last log line being `VO: [libmpv] 1400x2000 rgba`. What is known:

- 0 request failures across all 1560 calls before death.
- Thread count fell 55 -> 49 and handles 1379 -> 1370, so no thread/handle leak.
- RSS crept 188.8 -> 194.6 MB over 7.4 minutes (~0.8 MB/min). It plateaus rather
  than running away, but the trend is not flat.

It is **not reproducible on demand**: `tools/stress-switch.ps1` survived 600
rapid clip switches. Note that an earlier version of the harness was silently
broken — PowerShell unwraps a collection returned from a function, so an
8-element clip list arrived as one object and every "switch" iteration reopened
clip 0. That is fixed (the comma operator preserves the array), but the soak has
not been re-run since, so the crash's trigger is still unidentified.

Next step if it recurs: capture a dump (`procdump` or Windows Error Reporting)
and read the faulting module — that distinguishes an mpv bug from ours.

## Not yet implemented

- **A long soak run.** Short runs and the format matrix above pass. The soak
  above ran 7.4 minutes before crashing; no clean 24-hour run exists yet.
- **The neighbour mosaic.** The original app tiled the previous/next clips
  around the current one. That layout was dropped with the OCR/region work; only
  the single width-fit frame is drawn today.
- **Vulkan Video.** Not reachable: `MPV_RENDER_API_TYPE_OPENGL` is the only
  hardware-backed type mpv's render API defines, and `vulkan` hwdec needs
  `vo=gpu-next`. See `BUILDING.md`.

## Stack

| Concern | Library |
|---------|---------|
| Window / input | GLFW 3.4 |
| Rendering | OpenGL 3.3 core |
| Video + audio + A/V clock | libmpv, `vo=libmpv` render API |
| Images | stb_image |
| On-screen HUD | none — drawn through `RenderDevice` on the GL canvas |
| HTTP API | cpp-httplib |
| JSON | nlohmann/json |

There is deliberately **no widget toolkit** (no ImGui, Qt or GTK). The player's
controls are the HTTP API, and the only on-screen furniture is a status HUD and
a subtitle overlay drawn as textured quads through the same `RenderDevice` seam
that composites video — so the dependency list stays at windowing, GL and libmpv.

libmpv is built from source against the ffmpeg installed on the machine — see
[BUILDING.md](BUILDING.md) for why that is required rather than optional.

## Build

```powershell
# 1. one-time: build libmpv against the installed ffmpeg
pwsh -File tools/build-libmpv.ps1

# 2. build the app
pwsh -File build.ps1
```

The build must run with `PATH` confined to MSYS2. A `libwinpthread-1.dll` from
Strawberry Perl or MinGW elsewhere on the machine shadows MSYS2's and makes
`cc1plus.exe` die with no diagnostic. The build scripts handle this; see
[BUILDING.md](BUILDING.md) before building by hand.

## HTTP API

`http://127.0.0.1:8080` (localhost only)

| Action | Method | Endpoint |
|--------|--------|----------|
| Status | GET | `/api/status` |
| Playlist | GET | `/api/clips` |
| Health | GET | `/api/health` |
| Next | POST | `/api/next` |
| Previous | POST | `/api/previous` |
| Play | POST | `/api/play` |
| Stop | POST | `/api/stop` |
| Subtitles | POST | `/api/subtitles` + `{"enabled": true}` |
| Open clip | POST | `/api/clips/{index}` |
| Seek | POST | `/api/seek` + `{"time": 12.5}` / `{"relative": -5}` / `{"percent": 50}` |
| Position | GET | `/api/position` |
| Pause | POST | `/api/pause` + `{"paused": true}` |
| Speed | POST | `/api/speed` + `{"speed": 1.0}` |
| Volume | POST | `/api/volume` + `{"volume": 0..100}` |
| Subtitle track | GET/POST | `/api/subtitles/track` |
| Scripts | GET/POST | `/api/scripts` |
| Rescan media | POST | `/api/clips/rescan` |

Routes are additive: existing responses keep their exact shape so a controller
speaking the older surface continues to work. Every route is localhost-only, and
any route taking a path accepts files inside the data directory only.

Threading: HTTP workers never touch the player. Each handler submits a closure
to a queue and the main thread executes it during `poll()`, so decoding and
compositing stay single-threaded while the API answers concurrently.

## Security posture

Player behaviour is deliberately constrained:

- `ytdl=no` — never spawns an external `youtube-dl`/`yt-dlp` subprocess.
- `load-scripts=no` — the user's `~/.config/mpv/scripts/` is never auto-loaded.
- `config=no`, no `input-conf` — user config and key bindings are ignored.
- `access-references=no`, `autoload-files=no`, `load-unsafe-playlists=no`.
- Local files only; no network input unless explicitly enabled.
- The HTTP API binds localhost only and caps request size.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

The libmpv **client API** headers are ISC-licensed (they exist to allow external
wrappers), while the mpv core is GPLv2+. Because this application links the GPL
build of libmpv, the combined work is distributed under the GPL. The previous
Apache-2.0 licence text is kept in
[LICENSE-APACHE-2.0](LICENSE-APACHE-2.0) for the record.

Copyright (c) vecnode 2026
