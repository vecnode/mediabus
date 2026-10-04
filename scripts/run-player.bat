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
rem Extra arguments are passed through:
rem   run-player.bat --fullscreen
rem   run-player.bat --width 1280 --height 720
rem ---------------------------------------------------------------------------
setlocal
call "%~dp0_bin-dir.bat"

set "EXE=%BIN%\vn-mediabus-player.exe"
set "LOG=%BIN%\player.log"

if not exist "%EXE%" (
    echo.
    echo Not built yet: "%EXE%"
    echo Run scripts\build.bat first.
    echo.
    pause
    exit /b 1
)

rem The Player is a console-subsystem binary that draws its own window, so it is
rem started detached with its diagnostics in a log rather than tying up this
rem console. -NoNewWindow is deliberately NOT used: it would attach the Player to
rem this shell and block until the Player exits.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%EXE%' -ArgumentList '%*' -WorkingDirectory '%BIN%' -RedirectStandardOutput '%LOG%' -RedirectStandardError '%LOG.err'"

timeout /t 2 /nobreak >nul 2>&1
tasklist /fi "imagename eq vn-mediabus-player.exe" 2>nul | find /i "vn-mediabus-player.exe" >nul
if errorlevel 1 (
    echo.
    echo The Player did not start. Read "%LOG%" and "%LOG%.err" - they say why.
    echo.
    pause
    exit /b 1
)

echo vn-mediabus-player is running. Control API: http://127.0.0.1:8080
echo   log: %LOG%
exit /b 0
