@echo off
echo ========================================================
echo Deploying Bigscreen Virtual Desktop Driver to SteamVR...
echo ========================================================
copy /Y "%~dp0..\build\msvc\Release\driver_bigscreen_desktop.dll" "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\driver_bigscreen_desktop\bin\win64\driver_bigscreen_desktop.dll"
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] driver_bigscreen_desktop.dll deployed successfully to SteamVR!
) else (
    echo.
    echo [ERROR] Access denied. Right-click this file and choose "Run as administrator".
)
echo.
pause
