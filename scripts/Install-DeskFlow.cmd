@echo off
setlocal
if not exist "%~dp0DeskSetup.exe" (
  echo DeskSetup.exe is missing. Extract the complete setup archive.
  pause
  exit /b 1
)
start "" "%~dp0DeskSetup.exe" %*
