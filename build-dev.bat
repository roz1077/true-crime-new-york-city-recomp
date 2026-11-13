@echo off
rem Developer build: the game with the F9 debug menu, in out\build\dev. Never used for release packages
rem (release.bat packages out\build\release, and the packaging script refuses a developer build).
rem
rem   build-dev.bat                      developer build
rem   build-dev.bat C:\path\to\DLSS      developer build with experimental DLSS (NVIDIA's DLSS SDK checkout)
rem
rem See DEVELOPER.md.
setlocal
set "GAME_EXE=TrueCrimeNYC"
cd /d "%~dp0"
set "OUT=%CD%\out\build\dev"
set "DLSS=%~1"

echo.
echo   Developer build
echo   ================================================
echo.

if not exist config\game.toml (
    echo   Run build.bat once first: it checks the toolchain and writes config\game.toml.
    goto :fail
)
if defined DLSS if not exist "%DLSS%\include\nvsdk_ngx.h" (
    echo   %DLSS% is not an NVIDIA DLSS SDK checkout: include\nvsdk_ngx.h is missing.
    goto :fail
)
if defined DLSS if not exist "%DLSS%\lib\Windows_x86_64\rel\nvngx_dlss.dll" (
    echo   %DLSS%\lib\Windows_x86_64\rel\nvngx_dlss.dll is missing. Clone the SDK with Git LFS installed.
    goto :fail
)

rem the toolchain, as build.bat sets it up
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo   Visual Studio with the C++ workload was not found. Run build.bat first.
    goto :fail
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"

rem an empty TCNYC_DLSS_SDK turns DLSS off again after an earlier DLSS build in the same folder
echo [1/2] Configure
if defined DLSS (
    echo       with experimental DLSS from %DLSS%
    cmake --preset dev "-DTCNYC_DLSS_SDK=%DLSS%" >nul || goto :fail
) else (
    cmake --preset dev "-DTCNYC_DLSS_SDK=" >nul || goto :fail
)

echo [2/2] Build
echo       the first build recompiles the game code, expect 30 to 90 minutes ...
cmake --build "%OUT%" || goto :fail
if defined DLSS copy /y "%DLSS%\lib\Windows_x86_64\rel\nvngx_dlss.dll" "%OUT%\" >nul || goto :fail
if not defined DLSS if exist "%OUT%\nvngx_dlss.dll" del "%OUT%\nvngx_dlss.dll"

echo.
echo   Developer build ready: out\build\dev\%GAME_EXE%.exe
if defined DLSS echo   DLSS: set Render scale to 50, 58, 67 or 100%% and Upscaler to DLSS.
echo   Press F9 in game for the debug menu. Not for release packages.
echo.
exit /b 0

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
exit /b 1
