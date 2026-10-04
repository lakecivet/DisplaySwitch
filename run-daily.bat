@echo off
rem Back to DAILY mode (2560x1600)
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0DisplaySwitchCLI.exe" daily
set RC=%ERRORLEVEL%
if not "%RC%"=="0" ( echo. & echo [!] exit code %RC% & pause )
