@echo off
rem ---------------------------------------------------------------------------
rem _bin-dir.bat - locate bin\ and the executables, wherever this repo is.
rem
rem Included (via `call`) by every run*.bat, so the four wrappers cannot
rem disagree about where the applications live. Not meant to be run directly -
rem it has an underscore so it sorts away from the entry points.
rem
rem On success it sets:
rem   BIN   the bin\ directory, absolute and without a trailing backslash
rem   EXE   ... no, deliberately not set here: each wrapper names its own.
rem and returns errorlevel 0. On failure it prints why and returns 1.
rem ---------------------------------------------------------------------------
setlocal
set "SCRIPT_DIR=%~dp0"

rem The applications are built side by side into <repo>\bin. These wrappers live
rem in <repo>\scripts, so the repo root is one level up. The second candidate
rem covers a packaged layout where the wrappers sit next to the executables.
set "BIN=%SCRIPT_DIR%..\bin"
if not exist "%BIN%\media-player-cpp.exe" (
    if not exist "%BIN%\media-controller-cpp.exe" (
        if not exist "%BIN%\media-dashboard-cpp.exe" (
            set "BIN=%SCRIPT_DIR%"
        )
    )
)

rem Normalise "..\bin" into an absolute path so the launcher's
rem -WorkingDirectory and the log path are unambiguous.
for %%I in ("%BIN%") do set "BIN=%%~fI"

endlocal & set "BIN=%BIN%"
exit /b 0
