# BUILDING.md — toolchain notes

Everything here was discovered the hard way while proving the libmpv stack on
this machine. Two of these are silent-failure traps that cost real time; read
both before building by hand.

## 0. What gets built, and what each target needs

One tree produces three applications plus the test binary. They are separate
targets because their dependencies are genuinely different, and each can be
switched off independently (`-DMEDIA_BUILD_APP=OFF`, `-DMEDIA_BUILD_CONTROLLER`,
`-DMEDIA_BUILD_DASHBOARD`, `-DMEDIA_BUILD_TESTS`):

| Target | Needs |
|---|---|
| `media-player-cpp` | GLFW, GLEW, OpenGL, the vendored libmpv |
| `media-controller-cpp` | GLFW, GLEW, OpenGL, **Lua 5.1** (`lua5.1.pc`) |
| `media-dashboard-cpp` | GLFW, GLEW, OpenGL |
| `media_tests` | nothing graphical — it links the logic only |

Lua 5.1 is the same Lua libmpv already ships, so the Controller adds no new
runtime dependency to a bundle that includes the Player; CMake refuses to
configure the Controller without it and says so. All three must be built
together: the Dashboard locates the other two **by name in its own directory**.

On a machine with only Windows PowerShell 5.1, `powershell -File
scripts/build.ps1` is equivalent to the `pwsh` form used below — the scripts use
no PowerShell 7 feature. There is also `scripts/build.bat` next to it, which
builds everything and picks whichever PowerShell exists; it calls `build.ps1` and
adds nothing of its own. All the entry points live in `scripts/`; `build.ps1`
resolves the repository root as its own parent directory, so it can be invoked
from anywhere.

## 1. `PATH` must be confined to MSYS2 (silent-failure trap)

`g++.exe` reports its version fine, but **every compile exits 1 printing no
diagnostic at all** and produces no object file.

Cause: `cc1plus.exe` terminates with `STATUS_ENTRYPOINT_NOT_FOUND`
(`0xC0000139`) while loading a DLL. On this machine this PATH entry is the
culprit:

```
C:\Strawberry\c\bin\libwinpthread-1.dll
```

It shadows MSYS2's own `libwinpthread-1.dll`. `C:\MinGW\bin` and
`C:\Users\<you>\miniconda3\Library\mingw-w64\bin` are additional hazards.

The fix is to **replace** PATH, not extend it:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\Windows\system32;C:\Windows'
```

Sanity check — this must print `ok`:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\Windows\system32;C:\Windows'
'#include <cstdio>
int main(){ std::printf("ok\n"); }' | Set-Content t.cpp
g++ -std=c++17 t.cpp -o t.exe; .\t.exe
```

### Passing MSYS-style paths from PowerShell

`pkg-config` emits `-IC:/...` and `-LC:/...`. PowerShell only forwards these
intact when each is **quoted**:

```powershell
g++ -std=c++17 main.cpp -o app.exe "-IC:/msys64/mingw64/include" "-LC:/msys64/mingw64/lib" -lmpv
```

Unquoted, the linker resolves nothing and every symbol becomes an undefined
reference — while still complaining about the symbols, not the paths.

## 2. libmpv must be built against the installed ffmpeg (version skew)

The MSYS2 `mingw-w64-x86_64-mpv` package fails at load:

```
libavcodec: build version 62.28.101 incompatible with runtime version 62.28.100
```

The package was built against a libavcodec one patch newer than the ffmpeg
installed here. `mpv.exe --version` itself fails, so this is not an application
bug. Reinstalling does not fix it, and at the time of writing the MSYS2
repository had rolled back to a set that no longer carries the package at all.

The robust fix, and what this repo does, is to build mpv from source against the
ffmpeg that actually exists:

```powershell
pwsh -File scripts/tools/build-libmpv.ps1
```

That produces `bin/libmpv-2.dll` plus `lib/libmpv.dll.a` and `lib/mpv/*.h`,
which are the artefacts the app links and ships against.

## 3. GL entry points need a loader

MSYS2's `GL/gl.h` declares only OpenGL 1.1, so framebuffer objects are not
available without a loader. This project uses **GLEW**:

```cpp
#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>
// after glfwMakeContextCurrent:
glewInit();
```

`glewInit()` must be called after a context is current, or every GL call is a
null pointer.

## 4. libmpv requires `LC_NUMERIC` = `"C"`

`mpv_create()` returns `NULL` if `LC_NUMERIC` is not `"C"`. On a machine whose
locale uses `,` as the decimal separator this is a silent startup failure:

```cpp
std::setlocale(LC_NUMERIC, "C");   // before mpv_create()
```

## 5. `tar.exe` on PATH is bsdtar

It parses `C:\...` and `C:/...` as a remote `host:path` and fails with
`Cannot connect to C`. Use MSYS GNU tar:

```powershell
C:\msys64\usr\bin\tar.exe -xzf /c/msys64/tmp/mpv-0.41.0.tar.gz -C /c/msys64/tmp
```

## 6. `diff` is a PowerShell alias

Meson hard-requires GNU `diff`. In PowerShell, `diff` resolves to
`Compare-Object`. Use the full path: `C:\msys64\usr\bin\diff.exe`.

## 7. Never pass `--prefix` to meson here (meson CONFIGURATION quirk)

`meson.build` line ~1724 does:

```meson
conf_data.set_quoted('CONFIGURATION', meson.build_options())
```

An absolute Windows `--prefix` ends up inside that C string literal with
**unescaped backslashes**:

```c
#define CONFIGURATION "-Dbuildtype=release '-Dprefix=C:\Users\you\.cache/mpv-install' ..."
```

and compilation of `player/command.c` fails with:

```
error: incomplete universal character name \U
```

`scripts/tools/build-libmpv.ps1` therefore passes **no `--prefix`** (artefacts are
copied out of the build tree directly), and additionally normalises any
remaining backslashes in the generated `config.h` as a safety net.

## 8. Render API is OpenGL only — there is no Vulkan backend

`include/mpv/render.h` defines exactly two API types:

```c
#define MPV_RENDER_API_TYPE_OPENGL "opengl"
#define MPV_RENDER_API_TYPE_SW     "sw"
```

There is no Vulkan constant. mpv does have a `vulkan` hwdec, but the manual
requires `--vo=gpu-next` for it, and the render API is `--vo=libmpv` — so
Vulkan Video is unreachable in this architecture. OpenGL 3.3 core with
`d3d11va` (Windows) is the hardware path. Note also that MSYS2's Vulkan headers
were too old here (`VK_VERSION_1_3` not found) and the meson build reported
`vulkan : NO`.

## Required packages

```bash
pacman -S --noconfirm \
  mingw-w64-x86_64-{gcc,cmake,ninja,meson,pkgconf,glfw,glew,ffmpeg,libplacebo,libass,lua51,mujs}
```

## Verified working configuration

| Item | Value |
|------|-------|
| Compiler | g++ 16.1.0 (MSYS2 MinGW64) |
| GL | 3.3.0 NVIDIA 591.86 |
| GPU | NVIDIA GeForce RTX 3090 |
| libmpv | 0.41.0, self-built, client API **2.5** |
| ffmpeg | 8.1 (libavcodec 62.28.100) |
| Render API | `MPV_RENDER_API_TYPE_OPENGL` only — no Vulkan constant in `render.h` |
| hwdec in practice | `d3d11va-copy` |
| Scripting | `lua YES`, `javascript YES` |
