@echo off
rem ---------------------------------------------------------------------------
rem run-controller.bat - start ONLY the Controller bar
rem (vn-mediabus-controller.exe).
rem
rem Normally you start run.bat and launch the Controller from the tray menu.
rem This wrapper is for running the bar by itself, which is what you want when
rem the launcher is not involved. The bar is an HTTP *client*: with no Player
rem running it opens showing OFFLINE, which is a valid state.
rem
rem The Controller owns its own API on :8081. Its log goes to bin\controller.log.
rem
rem Extra arguments are passed through:
rem   run-controller.bat --script controller-example.lua
rem   run-controller.bat --start-offline
rem ---------------------------------------------------------------------------
setlocal
call "%~dp0_bin-dir.bat"
if errorlevel 1 exit /b 1

set "APP_EXE=%BIN%\vn-mediabus-controller.exe"
set "APP_LOG=%BIN%\controller.log"
set "APP_STDERR_ONLY=0"
set "APP_ARGS=%*"

call "%~dp0_start-app.bat"
if errorlevel 1 exit /b 1

echo vn-mediabus-controller is running. API: http://127.0.0.1:8081
echo   log: %APP_LOG%
exit /b 0
