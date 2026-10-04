@echo off
rem ===================================================================
rem  Create desktop shortcuts (run ONCE)
rem    DisplaySwitch GUI.lnk     -> DisplaySwitch.exe    graphical UI
rem    DisplaySwitch Toggle.lnk  -> run-toggle.bat       one click toggle
rem  NOTE: keep this file pure ASCII. Chinese text inside a .bat gets
rem        mangled by cmd.exe (it reads the file as the OEM codepage),
rem        which breaks line continuations and command parsing.
rem  You can rename the .lnk files to Chinese afterwards (F2).
rem ===================================================================
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "WD=%~dp0"
if "%WD:~-1%"=="\" set "WD=%WD:~0,-1%"

set "LINK1=DisplaySwitch GUI.lnk"
set "LINK2=DisplaySwitch Toggle.lnk"
set "EXE=%WD%\DisplaySwitch.exe"
set "BAT=%WD%\run-toggle.bat"

echo Creating desktop shortcuts ...
echo   GUI    : %EXE%
echo   Toggle : %BAT%
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$d=[Environment]::GetFolderPath('Desktop'); $w=New-Object -ComObject WScript.Shell; $a=$w.CreateShortcut((Join-Path $d $env:LINK1)); $a.TargetPath=$env:EXE; $a.WorkingDirectory=$env:WD; $a.IconLocation=($env:EXE + ',0'); $a.Description='DisplaySwitch GUI'; $a.Save(); $b=$w.CreateShortcut((Join-Path $d $env:LINK2)); $b.TargetPath=$env:BAT; $b.WorkingDirectory=$env:WD; $b.IconLocation=($env:EXE + ',0'); $b.Description='Toggle stretch / daily'; $b.WindowStyle=7; $b.Save(); Write-Host ('[OK] ' + (Join-Path $d $env:LINK1)); Write-Host ('[OK] ' + (Join-Path $d $env:LINK2))"

if errorlevel 1 (
    echo.
    echo [!] Failed. You can create it by hand instead:
    echo     right click DisplaySwitch.exe -^> Send to -^> Desktop ^(create shortcut^)
    pause
    exit /b 1
)
echo.
echo [OK] Done. Double click the desktop shortcuts to use them.
pause
