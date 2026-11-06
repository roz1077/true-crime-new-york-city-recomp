@echo off
rem Runs the port with the read-only vehicle spawn trace enabled.
rem Reproduce the Copland state, then close the game; the trace is in
rem out\build\release\logs\tcnyc.log.
setlocal
cd /d "%~dp0out\build\release"
if not exist TrueCrimeNYC.exe (
    echo TrueCrimeNYC.exe was not found. Run build.bat first.
    pause
    exit /b 1
)
set "XBE_TRACE_TRAFFIC=1"
start "" "%CD%\TrueCrimeNYC.exe" %*
