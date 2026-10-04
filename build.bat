@echo off
rem ===================================================================
rem  DisplaySwitch build script
rem -------------------------------------------------------------------
rem  Produces two executables:
rem    DisplaySwitch.exe      graphical UI (daily use)
rem    DisplaySwitchCLI.exe   console version (scripts / emergency)
rem
rem  Toolchain: MSYS2 MinGW-w64 g++ / windres (default Y:\msys64\ucrt64\bin)
rem  Sources  : src\core.cpp + src\main_gui.cpp / src\main_cli.cpp
rem  Icon     : src\icon.ico  (regenerate with: python tools\make_icon.py)
rem
rem  NOTE: keep this file pure ASCII. cmd.exe reads .bat as the OEM
rem        codepage; UTF-8 Chinese here breaks command parsing and the
rem        "^" line continuation.
rem ===================================================================
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set GXX=Y:\msys64\ucrt64\bin\g++.exe
if not exist "%GXX%" set GXX=g++
set WINDRES=Y:\msys64\ucrt64\bin\windres.exe
if not exist "%WINDRES%" set WINDRES=windres

set CXXFLAGS=-std=c++17 -O2 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter -static -s
set LIBS=-luser32 -lgdi32 -lshell32 -lsetupapi -lcfgmgr32

if not exist build mkdir build

echo Using compiler: %GXX%
"%GXX%" --version | findstr /r "^g++"
echo.

rem ---------------------------------------------------------------
rem  [1/3] resources (manifest + icon) - GUI only
rem ---------------------------------------------------------------
set RESFILE=
if not exist src\icon.ico (
    echo [i] src\icon.ico missing, trying to generate it ...
    where python >nul 2>nul && python tools\make_icon.py
)
if exist src\icon.ico (
    echo [1/3] compiling resources src\gui.rc ...
    "%WINDRES%" src\gui.rc -O coff -o build\gui.res
    if errorlevel 1 (
        echo [!] resource compile failed, building without resources
    ) else (
        set RESFILE=build\gui.res
    )
) else (
    echo [i] no icon.ico, skipping resources ^(default system icon will be used^)
)

rem ---------------------------------------------------------------
rem  [2/3] graphical UI
rem ---------------------------------------------------------------
echo.
echo [2/3] building DisplaySwitch.exe ^(GUI^) ...
if defined RESFILE (
    "%GXX%" %CXXFLAGS% -mwindows -municode src\core.cpp src\main_gui.cpp %RESFILE% -o DisplaySwitch.exe %LIBS% -lcomctl32
) else (
    "%GXX%" %CXXFLAGS% -mwindows -municode src\core.cpp src\main_gui.cpp -o DisplaySwitch.exe %LIBS% -lcomctl32
)
if errorlevel 1 (
    echo.
    echo [!] GUI build FAILED
    echo     remember: -mwindows AND -municode are both required ^(entry is wWinMain^)
    pause
    exit /b 1
)

rem ---------------------------------------------------------------
rem  [3/3] console version
rem ---------------------------------------------------------------
echo.
echo [3/3] building DisplaySwitchCLI.exe ...
"%GXX%" %CXXFLAGS% src\core.cpp src\main_cli.cpp -o DisplaySwitchCLI.exe %LIBS%
if errorlevel 1 (
    echo.
    echo [!] console build FAILED
    pause
    exit /b 1
)

echo.
echo ===========================================================
echo  [OK] build finished
echo     DisplaySwitch.exe      graphical UI
echo     DisplaySwitchCLI.exe   console version
echo.
echo  Double click DisplaySwitch.exe to start. See README.md.
echo ===========================================================
echo.
pause
