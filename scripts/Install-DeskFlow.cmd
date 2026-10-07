@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-DeskFlow.ps1" -BundleDirectory "%~dp0."
if errorlevel 1 (
  echo Installation failed or Windows administrator confirmation was cancelled.
  pause
  exit /b 1
)
echo Installation finished. DeskFlow can now be opened from Start.
pause
