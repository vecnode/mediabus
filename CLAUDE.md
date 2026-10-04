# CLAUDE.md

Guidance for Claude Code in this repository. The full agent/contributor guide is
in `AGENTS.md` — read it first.

@AGENTS.md

## Claude-specific quick reference

- **This tree builds three applications, not one:** the Player (`src/main.cpp`,
  libmpv + GL, API on `:8080`), the Controller (`src/controller_main.cpp`, an
  HTTP client of the Player with Lua 5.1, API on `:8081`) and the Dashboard
  (`src/dashboard_main.cpp`, a launcher that probes both health endpoints).
  They talk over HTTP only — there is no shared DLL and no shared memory.
- **Stack:** GLFW 3 + OpenGL 3.3 core + libmpv (render API). Scriptable via mpv
  Lua/JS in the Player and via embedded Lua 5.1 in the Controller. No
  openFrameworks, no widget toolkit — this repo used to be an openFrameworks
  project and that tree is gone.
- **The `RenderDevice` rule is the load-bearing convention:** nothing above
  `src/app/render/` may name a graphics API. `MPVSurface` is the single
  sanctioned exception, because it attaches mpv's output to a GL texture.
- **Build:** `pwsh -File build.ps1` (or `powershell -File build.ps1` where
  PowerShell 7 is not installed — the scripts use no PS7 feature). It must run
  with PATH confined to MSYS2 or `cc1plus.exe` dies silently printing nothing.
  Never call the toolchain by hand without reading `BUILDING.md`.
- **libmpv is built from source** by `tools/build-libmpv.ps1` because the MSYS2
  package has a libavcodec version mismatch (`62.28.101` vs `62.28.100`). Do not
  swap it for pacman's.
- **Threading:** the decoder is main-thread only; HTTP workers submit closures
  and `poll()` drains them each frame — on the Player's server and the
  Controller's alike. The Controller's poll thread and the Dashboard's probe
  thread publish snapshots and touch nothing else. Never touch the controller or
  the Lua state from a worker thread.
- **Player scripts** load only from `bin/data/scripts`, only before
  `mpv_initialize()`, and cannot be unloaded at runtime — reload means restart.
  The option is `scripts` (plural, a path list), not `script`. **Controller
  scripts** load from `bin/data/controller-scripts` and do reload at runtime.
- **Status contract:** the first eight keys of `/api/status` are frozen; add
  fields, never remove or rename them. The Controller parses that object.
- **Routes that take a path stay inside `bin/data`**; the Controller's script
  route takes a name resolved by `findControllerScript` and refuses any
  directory component.
- **Security defaults stay off:** `ytdl`, `load-scripts`, `config`,
  `input-conf`, `access-references`, `autoload-files`.
- **Verify with evidence.** `bin/media_tests.exe` covers the API, layout, route
  and script-host logic without a GL context; `tools/verify-live.ps1` starts all
  three applications and checks them against each other; `p0/build.ps1` builds
  and runs the standalone audio probe (it reports `current-ao` / `audio-pts`,
  which is how the audio path is proven); `tools/soak.ps1` measures memory and
  thread growth over a long run. Prefer screenshots over assertions for anything
  visual.
- **When a script starts a child process, use `-NoNewWindow` and redirect
  stdio.** A bare `Start-Process` in a non-interactive session blocks the parent
  forever with no output — that is how `verify-live.ps1` used to hang.
