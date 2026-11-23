@echo off
echo ========================================================
echo Launching Bigscreen Desktop Live Viewer...
echo ========================================================
taskkill /F /IM BigscreenDesktopViewer.exe 2>nul
timeout /t 1 /nobreak >nul
powershell -ExecutionPolicy Bypass -File "%~dp0prepare_runtime.ps1"
echo Starting BigscreenDesktopViewer.exe...
start "" "%~dp0..\build\native-frida\runtime\BigscreenDesktopViewer.exe"
