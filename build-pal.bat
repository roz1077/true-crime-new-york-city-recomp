@echo off
setlocal
title True Crime NYC - PAL build
cd /d "%~dp0"
set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
set "PRESET=pal"
set "PAL_ASSETS=assets-pal"
set "GAME_EXE=TrueCrimeNYCPAL.exe"
if /i "%~1"=="dev" set "PRESET=pal-dev"
if /i "%~1"=="eu" (
    set "PRESET=pal-eu"
    set "PAL_ASSETS=assets-pal-eu"
    set "GAME_EXE=TrueCrimeNYCPALEU.exe"
)
if /i "%~1"=="eu-dev" (
    set "PRESET=pal-eu-dev"
    set "PAL_ASSETS=assets-pal-eu"
    set "GAME_EXE=TrueCrimeNYCPALEU.exe"
)
set "OUT=%CD%\out\build\%PRESET%"

if not exist "%PAL_ASSETS%\default.xbe" (
    echo   Put the matching European default.xbe in %PAL_ASSETS% first.
    echo     %CD%\%PAL_ASSETS%\default.xbe
    if not defined CI pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo   Visual Studio with the C++ workload was not found.
    if not defined CI pause
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"

cmake --preset %PRESET% || goto :fail
cmake --build "%OUT%" --parallel || goto :fail
echo.
echo   PAL build finished: %OUT%\%GAME_EXE%
if not defined CI pause
exit /b 0

:fail
echo.
echo   PAL BUILD FAILED.
if not defined CI pause
exit /b 1
