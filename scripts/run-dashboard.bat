@echo off
rem ---------------------------------------------------------------------------
rem run-dashboard.bat - start ONLY the launcher window (media-dashboard-cpp.exe).
rem
rem run.bat is the same launcher started hidden in the tray, which is the normal
rem way to use it. This wrapper is for looking at it: the window opens on screen
rem so you can see the rows, the media corpus folder and the LAUNCH/STOP buttons.
rem
rem Closing the window still hides it and the tray icon is still the only exit,
rem so pass --no-tray to get an ordinary window that really closes:
rem   run-dashboard.bat --no-tray
rem
rem Extra arguments are passed through:
rem   run-dashboard.bat --width 900 --height 420
rem ---------------------------------------------------------------------------
setlocal
call "%~dp0_bin-dir.bat"

set "EXE=%BIN%\media-dashboard-cpp.exe"
set "LOG=%BIN%\dashboard.log"

if not exist "%EXE%" (
    echo.
    echo Not built yet: "%EXE%"
    echo Run scripts\build.bat first.
    echo.
    pause
    exit /b 1
)

rem The launcher is a Windows-subsystem binary (no console is ever created), so
rem this runs it detached with stderr in bin\dashboard.log. Note the absence of
rem --tray: the window is meant to be seen here.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%EXE%' -ArgumentList '%*' -WorkingDirectory '%BIN%' -RedirectStandardError '%LOG%'"

timeout /t 2 /nobreak >nul 2>&1
tasklist /fi "imagename eq media-dashboard-cpp.exe" 2>nul | find /i "media-dashboard-cpp.exe" >nul
if errorlevel 1 (
    echo.
    echo The launcher did not start. Read "%LOG%" - it says why.
    echo.
    pause
    exit /b 1
)

echo media-dashboard-cpp is running, window shown.
echo   log: %LOG%
exit /b 0
