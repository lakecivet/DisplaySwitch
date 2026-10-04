@echo off
rem ===================================================================
rem  DisplaySwitch - one click toggle  (stretch <-> daily)
rem  Console version: no window, exits right away.
rem  For the graphical interface, run DisplaySwitch.exe directly.
rem ===================================================================
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0DisplaySwitchCLI.exe" toggle
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo [!] Failed, exit code %RC%
    echo     See DisplaySwitch.log for details.
    echo.
    pause
)
