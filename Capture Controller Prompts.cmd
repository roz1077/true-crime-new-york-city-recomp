@echo off
setlocal
cd /d "%~dp0TrueCrimeNYC"
set "XBE_CAPTURE_DIR=prompt-captures"
set "XBE_CAPTURE_EVERY=25"
start "" "%CD%\TrueCrimeNYC.exe"
