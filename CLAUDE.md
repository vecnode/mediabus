# CLAUDE.md

Guidance for Claude Code in this repository. The full agent/contributor guide is
in `AGENTS.md` — read it first.

@AGENTS.md

## Claude-specific quick reference

- **Stack:** GLFW 3 + OpenGL 3.3 core + libmpv (render API). Scriptable via mpv
  Lua/JS. No openFrameworks, no widget toolkit — this repo used to be an
  openFrameworks project and that tree is gone.
- **The `RenderDevice` rule is the load-bearing convention:** nothing above
  `src/app/render/` may name a graphics API. `MPVSurface` is the single
  sanctioned exception, because it attaches mpv's output to a GL texture.
- **Build:** `pwsh -File build.ps1`. It must run with PATH confined to MSYS2 or
  `cc1plus.exe` dies silently printing nothing. Never call the toolchain by hand
  without reading `BUILDING.md`.
- **libmpv is built from source** by `tools/build-libmpv.ps1` because the MSYS2
  package has a libavcodec version mismatch (`62.28.101` vs `62.28.100`). Do not
  swap it for pacman's.
- **Threading:** the decoder is main-thread only; HTTP workers submit closures
  and `HttpControlServer::poll()` drains them each frame. Never touch the
  controller from a worker thread.
- **Scripts** load only from `bin/data/scripts`, only before
  `mpv_initialize()`, and cannot be unloaded at runtime — reload means restart.
  The option is `scripts` (plural, a path list), not `script`.
- **Status contract:** the first eight keys of `/api/status` are frozen; add
  fields, never remove or rename them.
- **Security defaults stay off:** `ytdl`, `load-scripts`, `config`,
  `input-conf`, `access-references`, `autoload-files`.
- **Verify with evidence.** `bin/media_tests.exe` covers the API and library
  logic without a GL context; `p0/audio_probe.exe` proves the audio path; prefer
  screenshots over assertions for anything visual.
