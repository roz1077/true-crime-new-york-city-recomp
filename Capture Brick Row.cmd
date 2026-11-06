@echo off
setlocal
cd /d "%~dp0out\build\release"
if not exist TrueCrimeNYC.exe (
    echo TrueCrimeNYC.exe was not found. Run build.bat first.
    pause
    exit /b 1
)
set "XBE_CAPTURE_DIR=captures"
set "XBE_CAPTURE_EVERY=1"
set "XBE_CAPTURE_INPUTS=1"
start "" "%CD%\TrueCrimeNYC.exe" %*
