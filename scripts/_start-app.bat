@echo off
rem ---------------------------------------------------------------------------
rem _start-app.bat - start one application detached, then confirm it came up.
rem
rem `call`ed by every run*.bat, so the four wrappers cannot disagree about HOW
rem an application is started - the same reason _bin-dir.bat exists for WHERE it
rem lives. A wrapper sets the variables below and calls this; it owns nothing
rem else about the launch.
rem
rem   APP_EXE          absolute path of the executable to start
rem   APP_LOG          absolute path of the log file
rem   APP_STDERR_ONLY  1 = redirect stderr only, 0 = redirect stdout and stderr
rem   APP_ARGS         arguments to pass; may be empty or undefined
rem
rem Returns errorlevel 0 when the process is running afterwards, 1 when it is
rem not - and pauses in that case, because the only reason to see the failure is
rem that a person double-clicked something.
rem
rem WHY THIS FILE EXISTS
rem
rem The obvious version of this is three lines of Start-Process in each wrapper,
rem splicing the caller's arguments in as -ArgumentList '%*'. That is broken for
rem the one case that matters most: PowerShell rejects an EMPTY -ArgumentList as
rem a parameter-binding error ("Cannot validate argument on parameter
rem 'ArgumentList'. The argument is null or empty"), it does not treat it as
rem "no arguments". A double-click passes no arguments, so every wrapper that did
rem that printed a PowerShell error, launched nothing, and then reported "did not
rem start" - which reads exactly like a missing executable.
rem
rem So the argument string is passed through the ENVIRONMENT and only added to
rem the Start-Process call when it is genuinely non-empty. A value that never
rem reaches a shell parser also cannot be quoted wrong, so a path with a space
rem or a quote in it arrives intact.
rem ---------------------------------------------------------------------------
setlocal

if not defined APP_EXE (
	echo _start-app.bat: APP_EXE is not set.
	exit /b 1
)
if not defined APP_LOG (
	echo _start-app.bat: APP_LOG is not set.
	exit /b 1
)
if not exist "%APP_EXE%" (
	echo.
	echo Not built yet: "%APP_EXE%"
	echo Run scripts\build.bat first.
	echo.
	pause
	exit /b 1
)

rem The one-liner below is deliberately on a single logical line: `start` does not
rem pass a redirection on to its child, which is why Start-Process is used, and
rem -NoNewWindow is deliberately NOT used, because it would attach the child to
rem this console and block here until the child exits.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$exe=$env:APP_EXE; $log=$env:APP_LOG; $a=$env:APP_ARGS; $p=@{FilePath=$exe; WorkingDirectory=(Split-Path -Parent $exe)}; if($a -and $a.Trim().Length -gt 0){$p.ArgumentList=$a}; if($env:APP_STDERR_ONLY -eq '1'){Start-Process @p -RedirectStandardError $log}else{Start-Process @p -RedirectStandardOutput $log -RedirectStandardError ($log+'.err')}"

rem Confirm it actually came up. A process missing a runtime DLL dies before
rem main() and prints nothing at all, which would otherwise look like nothing
rem happened.
rem
rem Two details here are load-bearing and both were paid for:
rem
rem   /fo csv /nh   tasklist's default TABLE output TRUNCATES the image name to
rem                 25 characters, so vn-mediabus-controller.exe - 26 characters -
rem                 is printed as "vn-mediabus-controller.ex" and a search for the
rem                 real name never matches. The controller would start perfectly
rem                 and the wrapper would still announce "did not start". The CSV
rem                 form prints the name in full. This is why the length of the
rem                 executable name silently decided whether three wrappers
rem                 appeared to work.
rem
rem   ping, not timeout   timeout refuses to run with no console to read from
rem                 ("Input redirection is not supported") and returns at once, so
rem                 the check would fire before the application had a chance to
rem                 appear and report a failure that is not real.
for %%F in ("%APP_EXE%") do set "APP_NAME=%%~nxF"
ping -n 3 127.0.0.1 >nul 2>&1
tasklist /fi "imagename eq %APP_NAME%" /fo csv /nh 2>nul | find /i "%APP_NAME%" >nul
if errorlevel 1 (
	echo.
	echo %APP_NAME% did not start. Read "%APP_LOG%" - it says why.
	echo.
	pause
	exit /b 1
)

exit /b 0
