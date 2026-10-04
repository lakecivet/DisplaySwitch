@echo off
rem Diagnostics: list displays / resolutions / monitor-device state
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0DisplaySwitchCLI.exe" list
echo.
"%~dp0DisplaySwitchCLI.exe" status
echo.
pause
