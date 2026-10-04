cmake_minimum_required(VERSION 3.20)
project(vn-mediabus LANGUAGES CXX)

# ---------------------------------------------------------------------------
# vn-mediabus - one tree, three Windows applications, one media bus.
#
#   vn-mediabus-player      GLFW + OpenGL 3.3 core + libmpv, scriptable,
#                           with a localhost HTTP control API on :8080
#   vn-mediabus-controller  a control panel: HTTP client + embedded Lua 5.1,
#                           with its own API on :8081
#   vn-mediabus-dashboard   the launcher: lives in the notification area and
#                           starts and stops the other two
#
# The Controller and the Dashboard draw their interfaces with Dear ImGui on the
# OpenGL context they already own. The Player keeps the original RenderDevice
# HUD: it composites video every frame and has no need of a widget layer.
#
# libmpv is NOT taken from MSYS2: that package is built against a libavcodec one
# patch newer than the ffmpeg installed here and fails at load. It is built from
# source by tools/build-libmpv.ps1 and vendored into vendor/libmpv + bin/.
# See docs/building.md.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MEDIA_BUILD_APP        "Build the Player (needs GLFW/GLEW/libmpv)" ON)
option(MEDIA_BUILD_CONTROLLER "Build the Controller panel (needs GLFW/GLEW/Lua/ImGui)" ON)
option(MEDIA_BUILD_DASHBOARD  "Build the Dashboard launcher (needs GLFW/GLEW/ImGui)" ON)
option(MEDIA_BUILD_TESTS      "Build the headless test binary" ON)

# Keep binaries together with runtime media: the apps resolve bin/data relative
# to the executable, and libmpv-2.dll must sit beside the exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Vendored third-party dependencies.
# ---------------------------------------------------------------------------

# libmpv: always matches the ffmpeg it was built against.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/vendor/libmpv/include/mpv/client.h OR
   NOT EXISTS ${CMAKE_SOURCE_DIR}/vendor/libmpv/lib/libmpv.dll.a)
    message(FATAL_ERROR
        "Vendored libmpv not found.\n"
        "Run:  pwsh -File tools/build-libmpv.ps1\n"
        "It builds mpv against the installed ffmpeg into bin/ and vendor/libmpv/.")
endif()

add_library(mpv_vendored INTERFACE)
target_include_directories(mpv_vendored INTERFACE
    ${CMAKE_SOURCE_DIR}/vendor/libmpv/include)
target_link_libraries(mpv_vendored INTERFACE
    ${CMAKE_SOURCE_DIR}/vendor/libmpv/lib/libmpv.dll.a)
add_library(vn-mediabus::mpv ALIAS mpv_vendored)

# cpp-httplib and nlohmann/json are single headers, as before.
add_library(vendored INTERFACE)
target_include_directories(vendored INTERFACE ${CMAKE_SOURCE_DIR}/vendor)
add_library(vn-mediabus::vendored ALIAS vendored)

# ---------------------------------------------------------------------------
# Libraries. The dependency arrows are deliberate and one-directional:
#
#   core --> media --> control --> {controller, dashboard}
#   core --> net   --> control
#   core --> gfx                  the Player's own renderer and bitmap font
#   core --> win32                TrayIcon, FolderPicker (the only Win32)
#   core --> ui                   ImGui is NOT in core, media, net or control
#   mpv   --> {player}            and nothing else
# ---------------------------------------------------------------------------

add_library(mediabus_core STATIC
    libs/core/AppConfig.cpp
    libs/core/Log.cpp
    libs/core/Platform.cpp
    libs/core/UiScale.cpp
    libs/media/MediaClipLibrary.cpp
    libs/media/MediaPlayerController.cpp
    libs/media/ScriptHost.cpp
)
target_include_directories(mediabus_core PUBLIC ${CMAKE_SOURCE_DIR}/libs)
target_link_libraries(mediabus_core PUBLIC vendored Threads::Threads)
if(WIN32)
    target_link_libraries(mediabus_core PUBLIC ole32 uuid)
endif()
add_library(vn-mediabus::core ALIAS mediabus_core)

# The Player's control plane. Split out so the tests can drive the API with no
# window, no GL and no libmpv.
add_library(mediabus_net STATIC
    libs/net/HttpControlServer.cpp
    libs/net/HttpJsonClient.cpp
)
target_link_libraries(mediabus_net PUBLIC mediabus_core)
if(WIN32)
    # cpp-httplib uses Winsock directly: ws2_32 supplies socket/select/recv.
    target_link_libraries(mediabus_net PUBLIC ws2_32)
endif()
add_library(vn-mediabus::net ALIAS mediabus_net)

# Lua 5.1 is the same Lua mpv ships, so libmpv's runtime closure already
# contains lua51.dll: the Controller's scripting host adds no new shipped
# dependency. Discovered here, before the library that needs its headers, and
# linked PUBLIC so the test target inherits the include path too.
pkg_check_modules(LUA51 IMPORTED_TARGET lua5.1)
if(MEDIA_BUILD_CONTROLLER AND NOT LUA51_FOUND)
    message(FATAL_ERROR
        "pkg-config could not find lua5.1, which the Controller's scripting "
        "host needs.\nMSYS2: pacman -S mingw-w64-x86_64-lua\n"
        "Or configure with -DMEDIA_BUILD_CONTROLLER=OFF.")
endif()

# The Controller is an HTTP *client* of that API, so the shared client and the
# route table live beside it rather than in the player. Nothing here draws.
add_library(mediabus_control STATIC
    libs/control/AppLauncher.cpp
    libs/control/ControllerHttpServer.cpp
    libs/control/ControllerModel.cpp
    libs/control/DashboardModel.cpp
    libs/control/LuaControllerScript.cpp
    libs/control/PlayerClient.cpp
)
target_link_libraries(mediabus_control PUBLIC mediabus_core mediabus_net)
if(LUA51_FOUND)
    target_link_libraries(mediabus_control PUBLIC PkgConfig::LUA51)
endif()
add_library(vn-mediabus::control ALIAS mediabus_control)

# The Player's renderer and its 5x7 bitmap font. The RenderDevice rule lives
# here: GlRenderDevice.cpp is the only translation unit that includes GL.
add_library(mediabus_gfx STATIC
    libs/gfx/BitmapFont.cpp
    libs/gfx/GlRenderDevice.cpp
    libs/gfx/RenderDevice.cpp
    libs/gfx/UiScaleGlfw.cpp
)
target_include_directories(mediabus_gfx PUBLIC ${CMAKE_SOURCE_DIR}/libs)
if(MEDIA_BUILD_APP OR MEDIA_BUILD_CONTROLLER OR MEDIA_BUILD_DASHBOARD)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
    target_link_libraries(mediabus_gfx PUBLIC
        PkgConfig::GLFW PkgConfig::GLEW OpenGL::GL)
endif()
add_library(vn-mediabus::gfx ALIAS mediabus_gfx)

# MPVSurface: the one file that attaches mpv's output to our GL texture. Only
# the Player links it, so only the Player carries the mpv dependency.
if(MEDIA_BUILD_APP)
    add_library(mediabus_mpv_surface STATIC libs/mpv/MPVSurface.cpp)
    target_link_libraries(mediabus_mpv_surface PUBLIC mediabus_gfx mpv_vendored)
    add_library(vn-mediabus::mpv-surface ALIAS mediabus_mpv_surface)
endif()

# Windows shell integration: the tray icon and the folder picker. The only Win32
# in the project, and the only reason shell32 is linked.
if(WIN32 AND (MEDIA_BUILD_CONTROLLER OR MEDIA_BUILD_DASHBOARD))
    add_library(mediabus_win32 STATIC
        libs/win32/FolderPicker.cpp
        libs/win32/TrayIcon.cpp
    )
    target_link_libraries(mediabus_win32 PUBLIC mediabus_core gdi32 ole32 uuid shell32)
    add_library(vn-mediabus::win32 ALIAS mediabus_win32)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui, vendored and pinned (see vendor/imgui/VERSION and docs/architecture.md).
#
# The OpenGL3 backend uses its own bundled gl3w-derived loader, which is
# file-local and does not collide with GLEW. Do NOT define
# IMGUI_IMPL_OPENGL_LOADER_CUSTOM here: Dear ImGui 1.92 still tests that macro
# but no longer includes a custom header for it, so defining it silently skips
# the loader include and every GL type becomes undefined.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_CONTROLLER OR MEDIA_BUILD_DASHBOARD)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui.cpp)
        message(FATAL_ERROR
            "Vendored Dear ImGui not found at vendor/imgui.\n"
            "Run:  pwsh -File tools/fetch-imgui.ps1")
    endif()

    add_library(imgui STATIC
        vendor/imgui/imgui.cpp
        vendor/imgui/imgui_draw.cpp
        vendor/imgui/imgui_tables.cpp
        vendor/imgui/imgui_widgets.cpp
        vendor/imgui/backends/imgui_impl_glfw.cpp
        vendor/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${CMAKE_SOURCE_DIR}/vendor/imgui
        ${CMAKE_SOURCE_DIR}/vendor/imgui/backends)
    target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(imgui PUBLIC PkgConfig::GLFW PkgConfig::GLEW OpenGL::GL)
    add_library(vn-mediabus::imgui ALIAS imgui)

    # The ImGui fence. No application file includes imgui.h: they talk to
    # UiLayer, which advances the frame and submits the draw data.
    add_library(mediabus_ui STATIC
        libs/ui/UiLayer.cpp
    )
    target_include_directories(mediabus_ui PUBLIC ${CMAKE_SOURCE_DIR}/libs)
    target_link_libraries(mediabus_ui PUBLIC mediabus_core imgui)
    add_library(vn-mediabus::ui ALIAS mediabus_ui)
endif()

# ---------------------------------------------------------------------------
# Player - libmpv video, audio and stills, driven over its own HTTP API.
# It deliberately does NOT link ImGui.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_APP)
    add_executable(vn-mediabus-player apps/player/main.cpp)
    if(WIN32)
        target_sources(vn-mediabus-player PRIVATE libs/win32/rc/player.rc)
        # windres resolves a relative ICON path against the build directory, not
        # against the .rc file, so the icon is handed over as an absolute path.
        target_compile_definitions(vn-mediabus-player PRIVATE
            MEDIABUS_ICON="${CMAKE_SOURCE_DIR}/assets/icon/icon.ico")
    endif()
    target_link_libraries(vn-mediabus-player PRIVATE
        mediabus_net mediabus_mpv_surface)
    if(WIN32)
        target_link_libraries(vn-mediabus-player PRIVATE gdi32 ole32 uuid)
    endif()

    # Install the shipped reference script next to the runtime data. The copy in
    # bin/data/scripts is a build output, not a second source of truth: keeping
    # both tracked let them drift. data/media-player.lua is the original.
    add_custom_command(TARGET vn-mediabus-player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/bin/data/scripts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/data/media-player.lua
                ${CMAKE_SOURCE_DIR}/bin/data/scripts/media-player.lua
        COMMENT "Installing reference script into bin/data/scripts"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Controller - a control panel: ImGui bar, Lua host, its own HTTP API.
# It deliberately does NOT link mpv: it is a client of the Player's API.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_CONTROLLER)
    add_executable(vn-mediabus-controller
        apps/controller/main.cpp
        apps/controller/view/ControllerView.cpp
    )
    if(WIN32)
        target_sources(vn-mediabus-controller PRIVATE libs/win32/rc/controller.rc)
        target_compile_definitions(vn-mediabus-controller PRIVATE
            MEDIABUS_ICON="${CMAKE_SOURCE_DIR}/assets/icon/icon.ico")
    endif()
    # The view lives beside the application it draws, so "view/ControllerView.h"
    # resolves without the app having to spell out the path.
    target_include_directories(vn-mediabus-controller PRIVATE
        ${CMAKE_SOURCE_DIR}/apps/controller)
    target_link_libraries(vn-mediabus-controller PRIVATE
        mediabus_control mediabus_gfx mediabus_ui mediabus_win32
        PkgConfig::LUA51)

    # Same treatment as the Player's reference script: the source of truth is
    # data/, and bin/ receives a copy at build time.
    add_custom_command(TARGET vn-mediabus-controller POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/bin/data/controller-scripts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/data/controller-example.lua
                ${CMAKE_SOURCE_DIR}/bin/data/controller-scripts/controller-example.lua
        COMMENT "Installing example script into bin/data/controller-scripts"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Dashboard - the launcher. ImGui interface, tray icon, probe thread. No mpv,
# no Lua, no HTTP server of its own.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_DASHBOARD)
    add_executable(vn-mediabus-dashboard
        apps/dashboard/main.cpp
        apps/dashboard/view/DashboardView.cpp
    )
    if(WIN32)
        target_sources(vn-mediabus-dashboard PRIVATE libs/win32/rc/dashboard.rc)
        target_compile_definitions(vn-mediabus-dashboard PRIVATE
            MEDIABUS_ICON="${CMAKE_SOURCE_DIR}/assets/icon/icon.ico")
        # The launcher is a tray application, so it is linked as a Windows
        # subsystem binary: no console window appears next to the icon. Its log
        # lines still arrive through the ImGui log sink, bin/dashboard.log, or
        # when it is launched from an existing console.
        set_target_properties(vn-mediabus-dashboard PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    target_include_directories(vn-mediabus-dashboard PRIVATE
        ${CMAKE_SOURCE_DIR}/apps/dashboard)
    target_link_libraries(vn-mediabus-dashboard PRIVATE
        mediabus_control mediabus_gfx mediabus_ui mediabus_win32)
endif()

# ---------------------------------------------------------------------------
# Tests - headless. No GL context, no window, no ImGui, no libmpv.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_TESTS)
    add_executable(vn-mediabus-tests
        tests/test_main.cpp
        # The Controller and Dashboard logic under test is deliberately free of
        # GL and of ImGui: layout is arithmetic, the HTTP server is queues and
        # JSON, and the two views stay out of this target entirely. The one file
        # pulled in from the render layer is the bitmap font, which is metrics
        # and glyph data, so the suite still runs headless.
        libs/control/ControllerHttpServer.cpp
        libs/control/ControllerModel.cpp
        libs/control/DashboardModel.cpp
        libs/control/LuaControllerScript.cpp
        libs/gfx/BitmapFont.cpp
    )
    target_include_directories(vn-mediabus-tests PRIVATE ${CMAKE_SOURCE_DIR}/libs)
    target_link_libraries(vn-mediabus-tests PRIVATE
        mediabus_control mediabus_core mediabus_net)
    if(WIN32)
        # AppLauncher uses CreateProcess/TerminateProcess.
        target_link_libraries(vn-mediabus-tests PRIVATE gdi32 ole32 uuid)
    endif()
    if(LUA51_FOUND)
        target_link_libraries(vn-mediabus-tests PRIVATE PkgConfig::LUA51)
    endif()
endif()

if(MSVC)
    set(MEDIA_WARNINGS /W4)
else()
    set(MEDIA_WARNINGS -Wall -Wextra -Wpedantic)
endif()

foreach(target mediabus_core mediabus_net mediabus_control mediabus_gfx
        mediabus_mpv mediabus_win32 mediabus_ui imgui
        vn-mediabus-player vn-mediabus-controller vn-mediabus-dashboard
        vn-mediabus-tests)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE ${MEDIA_WARNINGS})
    endif()
endforeach()

# Vendored code is compiled with the project's warning set suppressed: it is not
# ours to fix, and -Wpedantic on third-party sources is noise that hides ours.
if(TARGET imgui)
    target_compile_options(imgui PRIVATE -w)
endif()
