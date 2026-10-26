@echo off
setlocal
cd /d "%~dp0Run"
if not exist "AudioSim.exe" (
  echo Run tools\Build-AudioSim.ps1 first.
  pause
  exit /b 1
)
start "AudioSim" "AudioSim.exe"
