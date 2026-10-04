@echo off
rem EMERGENCY: force restore from rollback.ccd / rollback.mon
rem Use this when the screen looks wrong and you cannot get back.
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0DisplaySwitchCLI.exe" emergency
echo.
pause
