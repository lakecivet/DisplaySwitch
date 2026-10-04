@echo off
rem Force STRETCH mode (1280x882)
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0DisplaySwitchCLI.exe" stretch
set RC=%ERRORLEVEL%
if not "%RC%"=="0" ( echo. & echo [!] exit code %RC% & pause )
