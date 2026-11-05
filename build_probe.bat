@echo off
rem ===========================================================================
rem  CHZM SiliconFuse - diagnostic module build script
rem  Target : tools/HardwareProbe/main.cpp
rem  Output : build\CHZM_HardwareProbe.exe
rem  Toolchain : MinGW-w64 g++ (C++20, static runtime, no external deps)
rem
rem  Usage:
rem    build_probe.bat              build, run, save docs\HARDWARE_MAP.txt
rem    build_probe.bat --build-only build only
rem ===========================================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "SRC=%ROOT%\tools\HardwareProbe\main.cpp"
set "OUTDIR=%ROOT%\build"
set "OUT=%OUTDIR%\CHZM_HardwareProbe.exe"
set "MAPFILE=%ROOT%\docs\HARDWARE_MAP.txt"

if not exist "%SRC%" (
    echo [FAIL] Source not found: "%SRC%"
    exit /b 1
)

rem --- Locate the compiler ---------------------------------------------------
set "GXX="
where g++ >nul 2>&1 && set "GXX=g++"
if not defined GXX if exist "%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe" set "GXX=%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
if not defined GXX if exist "C:\msys64\mingw64\bin\g++.exe" set "GXX=C:\msys64\mingw64\bin\g++.exe"
if not defined GXX if exist "C:\mingw64\bin\g++.exe" set "GXX=C:\mingw64\bin\g++.exe"

if not defined GXX (
    echo [FAIL] MinGW-w64 g++ not found. Install it or add it to PATH.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [INFO] Compiler : %GXX%
echo [INFO] Source   : %SRC%
echo [INFO] Output   : %OUT%
echo.

"%GXX%" -std=c++20 -O2 -static -Wall -Wextra -Wno-unused-parameter ^
    -o "%OUT%" "%SRC%" -lsetupapi -lcfgmgr32

if errorlevel 1 (
    echo.
    echo [FAIL] Compilation failed.
    exit /b 1
)

echo [ OK ] Build complete: %OUT%
echo.

if /I "%~1"=="--build-only" (
    exit /b 0
)

if not exist "%ROOT%\docs" mkdir "%ROOT%\docs"
echo [INFO] Running probe, capturing %MAPFILE%
"%OUT%" > "%MAPFILE%" 2>&1
set "RC=%ERRORLEVEL%"
type "%MAPFILE%"
echo.
echo [INFO] Probe exit code: %RC%
exit /b %RC%
