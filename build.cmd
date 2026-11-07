@echo off
rem One-click build (bypasses PowerShell execution policy).
rem NOTE: keep this file ASCII-only; cmd.exe reads .cmd with the OEM codepage.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)
echo.
echo BUILD OK
