cmake_minimum_required(VERSION 3.20)
project(media-player-cpp LANGUAGES CXX)

# ---------------------------------------------------------------------------
# media-player-cpp — scriptable video player driven by a localhost HTTP API.
#
# Stack: GLFW + OpenGL 3.3 core + libmpv (render API) + cpp-httplib.
# No widget toolkit: the on-screen HUD is drawn through RenderDevice, so the
# only GUI-related dependency is the OpenGL context we already own.
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

option(MEDIA_BUILD_APP "Build the GUI application (needs GLFW/GLEW)" ON)
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

if(WIN32)
    # cpp-httplib uses Winsock directly: ws2_32 supplies socket/select/recv.
    target_link_libraries(media_http PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_APP)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)

    add_executable(media-player-cpp
        src/main.cpp
        src/app/render/RenderDevice.cpp
        src/app/render/GlRenderDevice.cpp
        src/app/hud/BitmapFont.cpp
        src/backends/mpv/MPVSurface.cpp
    )
    if(WIN32)
        # Attaches assets/icon/icon.ico to the executable so the taskbar and
        # title bar show the app icon instead of the generic Windows one.
        # The file was committed years ago but never referenced by the build.
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
# Tests
# ---------------------------------------------------------------------------
if(MEDIA_BUILD_TESTS)
    add_executable(media_tests
        tests/test_main.cpp
    )
    target_link_libraries(media_tests PRIVATE media_http)
endif()

if(MSVC)
    set(MEDIA_WARNINGS /W4)
else()
    set(MEDIA_WARNINGS -Wall -Wextra -Wpedantic)
endif()

foreach(target media_core media_http media-player-cpp media_tests)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE ${MEDIA_WARNINGS})
    endif()
endforeach()
