@echo off
rem Starts the PC port with frame capture enabled: press F3 in game to save a capture of the current frame
rem to out\build\release\captures\<time>. Any extra arguments are passed through.
setlocal
cd /d "%~dp0out\build\release"
if not exist TrueCrimeNYC.exe (
    echo TrueCrimeNYC.exe was not found. Run build.bat first.
    pause
    exit /b 1
)
set "XBE_CAPTURE_DIR=captures"
start "" "%CD%\TrueCrimeNYC.exe" %*
