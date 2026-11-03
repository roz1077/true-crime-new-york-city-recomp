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
rem also save the image every 25 draws inside a captured frame (makes F3 slower, helps find a bad draw)
set "XBE_CAPTURE_EVERY=25"
rem record controller 1 (buttons, triggers, sticks, timed by presented pictures) to pad_record.txt; replay with XBE_PAD_REPLAY=pad_record.txt
set "XBE_PAD_RECORD=pad_record.txt"
start "" "%CD%\TrueCrimeNYC.exe" %*
