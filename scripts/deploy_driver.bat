@echo off
echo ========================================================
echo Deploying Bigscreen Virtual Desktop Driver to SteamVR...
echo ========================================================
set "SRC=%~dp0..\build\msvc\Release\driver_bigscreen_desktop.dll"
if not exist "%SRC%" set "SRC=%~dp0..\build\Release\driver_bigscreen_desktop.dll"

echo Source: %SRC%
copy /Y "%SRC%" "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\driver_bigscreen_desktop\bin\win64\driver_bigscreen_desktop.dll"
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] driver_bigscreen_desktop.dll deployed successfully to SteamVR!
    if exist "%~dp0..\build\Debug" (
        copy /Y "%SRC%" "%~dp0..\build\Debug\driver_bigscreen_desktop.dll" >nul 2>&1
    )
) else (
    echo.
    echo [ERROR] Access denied. Right-click this file and choose "Run as administrator".
)
echo.
pause
