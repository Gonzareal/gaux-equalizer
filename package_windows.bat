@echo off
REM Gaux Equalizer - crear el instalador para Windows (doble clic)
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_windows.ps1"
pause
