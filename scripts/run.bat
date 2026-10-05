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
rem The window starts hidden. Closing it later hides it again rather than
rem exiting. There is one launcher per session: a second run.bat finds the
rem first one and does nothing.
rem
rem   run.bat --show     start with the launcher window visible
rem
rem Build first if bin\ is empty:  build.bat
rem
rem The other three wrappers start one application each, without the launcher:
rem   run-player.bat  run-controller.bat  run-dashboard.bat
rem ---------------------------------------------------------------------------
setlocal
call "%~dp0_bin-dir.bat"
if errorlevel 1 exit /b 1

set "APP_EXE=%BIN%\vn-mediabus-dashboard.exe"
set "APP_LOG=%BIN%\dashboard.log"
rem The launcher is a Windows-subsystem binary: it has no stdout to redirect.
set "APP_STDERR_ONLY=1"

rem --show is this script's own switch, consumed here; everything else is handed
rem to the launcher. `%*` is deliberately NOT used for the pass-through: `shift`
rem does not touch `%*`, so the switch would be forwarded as well and arrive at
rem the launcher as an unknown argument. Rebuilding the list is the only way to
rem actually drop it. (An argument containing a double quote is still not
rem supported here, which is a batch limitation rather than a decision.)
set "APP_ARGS=--tray"
if /i "%~1"=="--show" (
	set "APP_ARGS="
	shift
)
:collect
if "%~1"=="" goto collected
set "APP_ARGS=%APP_ARGS% %~1"
shift
goto collect
:collected

call "%~dp0_start-app.bat"
if errorlevel 1 exit /b 1

echo vn-mediabus launcher is running.
echo   right-click its tray icon to launch the Player or the Controller, or to quit.
echo   no tray icon? The launcher window is shown instead - see bin\dashboard.log.
exit /b 0
