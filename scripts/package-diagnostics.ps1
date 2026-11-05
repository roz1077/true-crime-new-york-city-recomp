param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\build\release'),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'),
    [string]$Stamp = (Get-Date -Format 'yyyy-MM-dd')
)

$ErrorActionPreference = 'Stop'
$name = "TrueCrimeNYC-diagnostics-$Stamp"
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($file in 'TrueCrimeNYC.exe', 'TrueCrimeNYC.dll', 'TrueCrimeNYC.pdb', 'TrueCrimeNYC.dll.stamp') {
    Copy-Item -LiteralPath (Join-Path $BuildDir $file) -Destination (Join-Path $stage $file) -Force
}
New-Item -ItemType Directory -Path (Join-Path $stage 'compiler') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDir 'compiler\TrueCrimeNYC.lib') -Destination (Join-Path $stage 'compiler\TrueCrimeNYC.lib') -Force

$readme = @'
True Crime NYC - diagnostic build 6 (2026-11-05)

This build contains the FIX for the crash you reproduced, plus the diagnostics.

WHAT WAS WRONG
The city-cell grid code maps a cell coordinate with (coordinate + 1000) / 3 and
uses only the signed division remainders (expected 0..2) to pick one of nine
16-byte records. A negative coordinate makes the CPU's signed division return a
negative remainder; the index became -1 and the record was written 16 bytes
BEFORE the table - onto the end of cell slot 8 (+0x70/+0x74). The renderer then
followed 0xFFFFFFF6 and crashed. The watch run showed the exact write: index
0xFFFFFFFF and remainder -1.
The fix makes the two index divisions use the mathematical remainder: when the
remainder is negative it adds the divisor (3), so the index is always 0..8. For
every non-negative coordinate this produces exactly the same value as before;
only the previously impossible negative case changes, and it can no longer write
outside the table. Nothing is skipped and no state is guessed.

INSTALL
1. Close the game completely (check Task Manager for TrueCrimeNYC.exe).
2. Back up TrueCrimeNYC.exe, TrueCrimeNYC.dll and TrueCrimeNYC.dll.stamp from your
   True Crime NYC folder.
3. Copy TrueCrimeNYC.exe, TrueCrimeNYC.dll, TrueCrimeNYC.pdb and
   TrueCrimeNYC.dll.stamp from this folder into that folder, replacing the files.
   The .stamp is important: without it the launcher rebuilds the DLL and loses the
   fix and diagnostics.
4. If the log says "Game code: building TrueCrimeNYC.dll" when you start, the
   stamp did not match. In that case also copy compiler\TrueCrimeNYC.lib from this
   folder into the game folder's compiler folder, then start again.
Keep your existing assets and config folders - only those game files are replaced.

PLEASE TEST
Run run-normal.cmd and play the same save/route that crashed (about 2 minutes).
It should no longer crash at that spot. Please send back:
- logs\tcnyc.log
- the newest CrashDumps files if it does crash (.dmp and guest-*.bin)

If you run run-watch.cmd instead, the log shows every write into cell slot 8 - a
"watch: hit" line with a valid value after activation means the slot was written
normally. The other launcher, run-serial.cmd, is no longer needed for this bug.
'@
Set-Content -LiteralPath (Join-Path $stage 'READ-ME-FIRST.txt') -Value $readme -Encoding utf8

$normal = @'
@echo off
start "" "%~dp0TrueCrimeNYC.exe"
'@
Set-Content -LiteralPath (Join-Path $stage 'run-normal.cmd') -Value $normal -Encoding ascii

$serial = @'
@echo off
set XBE_SERIAL_GUEST=1
start "" "%~dp0TrueCrimeNYC.exe"
'@
Set-Content -LiteralPath (Join-Path $stage 'run-serial.cmd') -Value $serial -Encoding ascii

$watch = @'
@echo off
set XBE_EFFECT_WATCH=1
start "" "%~dp0TrueCrimeNYC.exe"
'@
Set-Content -LiteralPath (Join-Path $stage 'run-watch.cmd') -Value $watch -Encoding ascii

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Set-Content -LiteralPath (Join-Path $OutDir "$name.sha256.txt") -Value "$hash  $name.zip" -Encoding ascii
Write-Output "Created $zip"
Write-Output "SHA256 $hash"
