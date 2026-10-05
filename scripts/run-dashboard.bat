@echo off
rem ---------------------------------------------------------------------------
rem run-dashboard.bat - start ONLY the launcher window (vn-mediabus-dashboard.exe).
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
if errorlevel 1 exit /b 1

set "APP_EXE=%BIN%\vn-mediabus-dashboard.exe"
set "APP_LOG=%BIN%\dashboard.log"
rem The launcher is a Windows-subsystem binary: it has no stdout to redirect.
set "APP_STDERR_ONLY=1"
set "APP_ARGS=%*"

call "%~dp0_start-app.bat"
if errorlevel 1 exit /b 1

echo vn-mediabus-dashboard is running, window shown.
echo   log: %APP_LOG%
exit /b 0
