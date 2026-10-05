@echo off
rem ---------------------------------------------------------------------------
rem run-player.bat - start ONLY the Player (vn-mediabus-player.exe).
rem
rem Use this when the launcher is already running, or when you want the Player
rem on its own. Normally you start run.bat instead and launch the Player from
rem the tray menu, where a second copy on an already-used port cannot happen.
rem
rem The Player owns the decoder, the window and the control API on :8080.
rem Its log goes to bin\player.log.
rem
rem The Player starts with NO clips: it only scans a folder that has been chosen
rem in the Dashboard or the Controller (recorded in bin\mediabus.ini). With no
rem folder chosen it opens instantly, empty, and says so on its HUD.
rem
rem Extra arguments are passed through:
rem   run-player.bat --fullscreen
rem   run-player.bat --width 1280 --height 720
rem ---------------------------------------------------------------------------
setlocal
call "%~dp0_bin-dir.bat"
if errorlevel 1 exit /b 1

set "APP_EXE=%BIN%\vn-mediabus-player.exe"
set "APP_LOG=%BIN%\player.log"
set "APP_STDERR_ONLY=0"
set "APP_ARGS=%*"

call "%~dp0_start-app.bat"
if errorlevel 1 exit /b 1

echo vn-mediabus-player is running. Control API: http://127.0.0.1:8080
echo   log: %APP_LOG%
exit /b 0
