@echo off
rem ---------------------------------------------------------------------------
rem run.bat - start the launcher (the "mother app") in the Windows tray.
rem
rem This is the only entry point an operator needs. The launcher stays in the
rem notification area until QUIT is chosen there, so the Player and the
rem Controller can be closed and reopened without restarting anything:
rem
rem   right-click the tray icon  ->  Launch Player / Launch Controller / Quit
rem   left-click the tray icon   ->  show or hide the launcher window
rem
rem The window starts hidden (--tray). Closing it later hides it again rather
rem than exiting. There is one launcher per session: a second run.bat finds the
rem first one and does nothing.
rem
rem Build first if bin\ is empty:  build.bat
rem ---------------------------------------------------------------------------
setlocal

set "BIN=%~dp0bin"
set "EXE=%BIN%\media-dashboard-cpp.exe"
set "LOG=%BIN%\dashboard.log"

if not exist "%EXE%" (
    echo.
    echo Not built yet: "%EXE%"
    echo Run build.bat first.
    echo.
    pause
    exit /b 1
)

rem Start it detached, with its output in bin\dashboard.log.
rem
rem This goes through PowerShell's Start-Process instead of `start`, for two
rem measured reasons: `start` does not pass a redirection on to its child, so the
rem log came out empty, and a launcher whose log is empty is undiagnosable when
rem it refuses to appear. The launcher is a Windows subsystem binary, so no
rem console window appears next to the tray icon either way.
rem
rem Extra arguments are passed through after --tray.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%EXE%' -ArgumentList '--tray %*' -WorkingDirectory '%BIN%' -RedirectStandardError '%LOG%'"

rem Confirm it actually came up. A launcher missing a runtime DLL dies before
rem main() and prints nothing at all, which would otherwise look like nothing
rem happened; this is also how a double-click reports success or failure.
timeout /t 2 /nobreak >nul 2>&1
tasklist /fi "imagename eq media-dashboard-cpp.exe" 2>nul | find /i "media-dashboard-cpp.exe" >nul
if errorlevel 1 (
    echo.
    echo The launcher did not start.
    echo Check that bin\ has been built with build.bat, then read
    echo "%LOG%" - it says why.
    echo.
    pause
    exit /b 1
)

echo mediaplayer-app launcher is running.
echo   right-click its tray icon to launch the Player or the Controller, or to quit.
echo   no tray icon? The launcher window is shown instead - see bin\dashboard.log.
exit /b 0
