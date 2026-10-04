cmake_minimum_required(VERSION 3.20)
project(media-player-cpp LANGUAGES CXX)

# ---------------------------------------------------------------------------
# mediaplayer-app — three applications built from one tree.
#
#   media-player-cpp      the player: GLFW + OpenGL 3.3 core + libmpv
#   media-controller-cpp  a control bar: HTTP client + embedded Lua 5.1
#   media-dashboard-cpp   a launcher: starts and stops the other two
#
# No widget toolkit: every window draws through RenderDevice, so the only
# GUI-related dependency is the OpenGL context they already own.
#
# libmpv is NOT taken from MSYS2: that package is built against a libavcodec
# one patch newer than the ffmpeg installed here and fails at load. It is built
# from source by tools/build-libmpv.ps1 and vendored into bin/ + lib/.
# See BUILDING.md.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MEDIA_BUILD_APP "Build the Player (needs GLFW/GLEW/libmpv)" ON)
option(MEDIA_BUILD_CONTROLLER "Build the Controller bar (needs GLFW/GLEW/Lua)" ON)
option(MEDIA_BUILD_DASHBOARD "Build the Dashboard launcher (needs GLFW/GLEW)" ON)
option(MEDIA_BUILD_TESTS "Build the test binaries" ON)

# Keep binaries together with runtime media: the app resolves bin/data relative
# to the executable, and libmpv-2.dll must sit beside the exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Vendored libmpv: always matches the ffmpeg it was built against.
# ---------------------------------------------------------------------------
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/lib/mpv/client.h OR
   NOT EXISTS ${CMAKE_SOURCE_DIR}/lib/libmpv.dll.a)
    message(FATAL_ERROR
        "Vendored libmpv not found.\n"
        "Run:  pwsh -File tools/build-libmpv.ps1\n"
        "It builds mpv against the installed ffmpeg into bin/ and lib/.")
endif()

add_library(mpv_vendored INTERFACE)
target_include_directories(mpv_vendored INTERFACE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(mpv_vendored INTERFACE ${CMAKE_SOURCE_DIR}/lib/libmpv.dll.a)

# ---------------------------------------------------------------------------
# Vendored single-header libraries
# ---------------------------------------------------------------------------
add_library(third_party INTERFACE)
target_include_directories(third_party INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Core: everything that has no graphics API and no windowing dependency.
# Kept as a library so tests can link it without a GL context.
# ---------------------------------------------------------------------------
add_library(media_core STATIC
    src/core/Log.cpp
    src/core/Platform.cpp
    src/media/MediaClipLibrary.cpp
    src/media/MediaPlayerController.cpp
    src/media/ScriptHost.cpp
)
target_include_directories(media_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(media_core PUBLIC third_party Threads::Threads)

if(WIN32)
    target_link_libraries(media_core PUBLIC ole32 uuid)
endif()

# ---------------------------------------------------------------------------
# Control plane (HTTP). Split out so tests can drive the API without GLFW.
# ---------------------------------------------------------------------------
add_library(media_http STATIC
    src/app/HttpControlServer.cpp
)
target_link_libraries(media_http PUBLIC media_core)

# The Controller is an HTTP *client* of that API, so the shared client and the
# route table live beside it rather than in the player.
add_library(media_control STATIC
    src/app/http/HttpJsonClient.cpp
    src/app/control/PlayerClient.cpp
)
target_link_libraries(media_control PUBLIC media_core)

if(WIN32)
    # cpp-httplib uses Winsock directly: ws2_32 supplies socket/select/recv.
    target_link_libraries(media_http PUBLIC ws2_32)
    target_link_libraries(media_control PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Windows applications
#
# All three share the same renderer and font: the RenderDevice rule means the
# window code below is the only OpenGL-aware part of any of them.
# ---------------------------------------------------------------------------
set(MEDIA_RENDER_SOURCES
    src/app/render/RenderDevice.cpp
    src/app/render/GlRenderDevice.cpp
    src/app/hud/BitmapFont.cpp
)

# Lua 5.1 is the same Lua mpv ships, so libmpv's runtime closure already
# contains lua51.dll: the Controller's scripting host adds no new shipped
# dependency. Discovered here rather than inside the Controller block because
# the test target exercises the same host.
pkg_check_modules(LUA51 IMPORTED_TARGET lua5.1)
if(MEDIA_BUILD_CONTROLLER AND NOT LUA51_FOUND)
    message(FATAL_ERROR
        "pkg-config could not find lua5.1, which the Controller's scripting "
        "host needs.\nMSYS2: pacman -S mingw-w64-x86_64-lua\n"
        "Or configure with -DMEDIA_BUILD_CONTROLLER=OFF.")
endif()

# ---------------------------------------------------------------------------
# Player
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_APP)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)

    add_executable(media-player-cpp
        src/main.cpp
        ${MEDIA_RENDER_SOURCES}
        src/backends/mpv/MPVSurface.cpp
    )
    if(WIN32)
        # Attaches assets/icon/icon.ico to the executable so the taskbar and
        # title bar show the app icon instead of the generic Windows one.
        target_sources(media-player-cpp PRIVATE src/app/app.rc)
    endif()
    target_link_libraries(media-player-cpp PRIVATE
        media_http
        mpv_vendored
        PkgConfig::GLFW
        PkgConfig::GLEW
        OpenGL::GL
    )
    if(WIN32)
        target_link_libraries(media-player-cpp PRIVATE gdi32 ole32 uuid)
    endif()

    # Install the shipped reference script next to the runtime data. The copy in
    # bin/data/scripts is a build output, not a second source of truth: keeping
    # both tracked let them drift. scripts/media-player.lua is the original.
    set(MEDIA_REFERENCE_SCRIPT
        ${CMAKE_SOURCE_DIR}/scripts/media-player.lua)
    set(MEDIA_SCRIPT_DEST
        ${CMAKE_SOURCE_DIR}/bin/data/scripts/media-player.lua)
    add_custom_command(TARGET media-player-cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/bin/data/scripts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MEDIA_REFERENCE_SCRIPT} ${MEDIA_SCRIPT_DEST}
        COMMENT "Installing reference script into bin/data/scripts"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Controller - needs GLFW/GLEW for its bar, and Lua for its scripting host.
# It deliberately does NOT link mpv: it is a client of the Player's HTTP API.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_CONTROLLER)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)

    add_executable(media-controller-cpp
        src/controller_main.cpp
        ${MEDIA_RENDER_SOURCES}
        src/app/control/ControllerModel.cpp
        src/app/control/ControllerView.cpp
        src/app/control/LuaControllerScript.cpp
        src/app/control/ControllerHttpServer.cpp
    )
    if(WIN32)
        target_sources(media-controller-cpp PRIVATE src/app/app-controller.rc)
    endif()
    target_link_libraries(media-controller-cpp PRIVATE
        media_control
        PkgConfig::GLFW
        PkgConfig::GLEW
        PkgConfig::LUA51
        OpenGL::GL
    )
    if(WIN32)
        target_link_libraries(media-controller-cpp PRIVATE gdi32 ole32 uuid)
    endif()

    # Same treatment as the Player's reference script: the source of truth is
    # scripts/, and bin/ receives a copy at build time.
    set(MEDIA_CONTROLLER_SCRIPT
        ${CMAKE_SOURCE_DIR}/scripts/controller-example.lua)
    set(MEDIA_CONTROLLER_SCRIPT_DEST
        ${CMAKE_SOURCE_DIR}/bin/data/controller-scripts/controller-example.lua)
    add_custom_command(TARGET media-controller-cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/bin/data/controller-scripts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MEDIA_CONTROLLER_SCRIPT} ${MEDIA_CONTROLLER_SCRIPT_DEST}
        COMMENT "Installing example script into bin/data/controller-scripts"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Dashboard - a launcher. No mpv, no Lua, no HTTP server: it probes the other
# two applications' health endpoints and spawns their executables.
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_DASHBOARD)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)

    add_executable(media-dashboard-cpp
        src/dashboard_main.cpp
        ${MEDIA_RENDER_SOURCES}
        src/app/dashboard/AppLauncher.cpp
        src/app/dashboard/DashboardModel.cpp
        src/app/dashboard/DashboardView.cpp
    )
    if(WIN32)
        target_sources(media-dashboard-cpp PRIVATE src/app/app-dashboard.rc)
    endif()
    target_link_libraries(media-dashboard-cpp PRIVATE
        media_control
        PkgConfig::GLFW
        PkgConfig::GLEW
        OpenGL::GL
    )
    if(WIN32)
        target_link_libraries(media-dashboard-cpp PRIVATE gdi32 ole32 uuid)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_TESTS)
    add_executable(media_tests
        tests/test_main.cpp
        # The Controller and Dashboard logic under test is deliberately free of
        # GL: layout is arithmetic, the HTTP server is queues and JSON, and the
        # only drawing code (the two views) stays out of this target. The one
        # file pulled in from the render layer is the bitmap font, which is
        # metrics and glyph data, so the suite still runs headless.
        src/app/control/ControllerModel.cpp
        src/app/control/LuaControllerScript.cpp
        src/app/control/ControllerHttpServer.cpp
        src/app/dashboard/AppLauncher.cpp
        src/app/dashboard/DashboardModel.cpp
        src/app/hud/BitmapFont.cpp
    )
    target_link_libraries(media_tests PRIVATE media_http media_control)
    if(WIN32)
        # The Dashboard's launcher uses CreateProcess/TerminateProcess. On
        # non-Windows that code path is compiled out, so nothing is needed.
        target_link_libraries(media_tests PRIVATE gdi32 ole32 uuid)
    endif()
    if(LUA51_FOUND)
        target_link_libraries(media_tests PRIVATE PkgConfig::LUA51)
    endif()
endif()

if(MSVC)
    set(MEDIA_WARNINGS /W4)
else()
    set(MEDIA_WARNINGS -Wall -Wextra -Wpedantic)
endif()

foreach(target media_core media_http media_control media-player-cpp
        media-controller-cpp media-dashboard-cpp media_tests)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE ${MEDIA_WARNINGS})
    endif()
endforeach()
