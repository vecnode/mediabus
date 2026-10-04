@echo off
rem ---------------------------------------------------------------------------
rem build.bat - build everything on Windows.
rem
rem A thin wrapper: the real work is in build.ps1, which is where the MSYS2 PATH
rem confinement, the libmpv prerequisite and the runtime-DLL staging live (see
rem BUILDING.md). This exists so "build everything" has one obvious entry point
rem next to the four run*.bat wrappers, and so a double-click in Explorer works.
rem
rem It builds all three applications plus the test binary:
rem
rem   bin\vn-mediabus-player.exe      the player
rem   bin\vn-mediabus-controller.exe  the control bar
rem   bin\vn-mediabus-dashboard.exe   the launcher  <- what run.bat starts
rem   bin\vn-mediabus-tests.exe           the headless suite
rem
rem Arguments are passed straight through, so build.bat -Clean, -Run and
rem -App Controller work exactly as they do on build.ps1.
rem ---------------------------------------------------------------------------
setlocal

set "SCRIPT=%~dp0build.ps1"

if not exist "%SCRIPT%" (
    echo build.ps1 not found next to build.bat.
    exit /b 1
)

rem PowerShell 7 if it is installed, Windows PowerShell 5.1 otherwise. The build
rem scripts use no PowerShell 7 feature on purpose, so either one works.
where pwsh >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
)

if errorlevel 1 (
    echo.
    echo BUILD FAILED - see the messages above, and BUILDING.md for the traps
    echo that make this toolchain fail without saying why.
    echo.
    pause
    exit /b 1
)

echo.
echo Build finished. Start the launcher with:  scripts\run.bat
exit /b 0
