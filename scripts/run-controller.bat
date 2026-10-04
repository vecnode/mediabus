@echo off
rem ---------------------------------------------------------------------------
rem run-controller.bat - start ONLY the Controller bar
rem (media-controller-cpp.exe).
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

set "EXE=%BIN%\media-controller-cpp.exe"
set "LOG=%BIN%\controller.log"

if not exist "%EXE%" (
    echo.
    echo Not built yet: "%EXE%"
    echo Run scripts\build.bat first.
    echo.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%EXE%' -ArgumentList '%*' -WorkingDirectory '%BIN%' -RedirectStandardOutput '%LOG%' -RedirectStandardError '%LOG.err'"

timeout /t 2 /nobreak >nul 2>&1
tasklist /fi "imagename eq media-controller-cpp.exe" 2>nul | find /i "media-controller-cpp.exe" >nul
if errorlevel 1 (
    echo.
    echo The Controller did not start. Read "%LOG%" and "%LOG%.err" - they say why.
    echo.
    pause
    exit /b 1
)

echo media-controller-cpp is running. API: http://127.0.0.1:8081
echo   log: %LOG%
exit /b 0
