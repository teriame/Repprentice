@echo off
rem Build the single self-contained Windows release executable: dist\Apprentice-<VERSION>-x64.exe
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1" %*
