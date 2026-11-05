@echo off
rem ===========================================================================
rem  CHZM SiliconFuse - Phase 2 lease broker + GPU pressure emulator
rem  Outputs: build\CHZM_LeaseBroker.exe, build\CHZM_GpuPressure.exe
rem  Toolchain: MinGW-w64 g++ (C++20, AVX2 where required, static)
rem
rem  Usage:
rem    build_broker.bat              build both binaries
rem    build_broker.bat --build-only same (kept for symmetry with other tools)
rem ===========================================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "SRC_BROKER=%ROOT%\tools\LeaseBroker\main.cpp"
set "SRC_PRESSURE=%ROOT%\tools\LeaseBroker\gpu_pressure.cpp"
set "OUTDIR=%ROOT%\build"
set "BROKER=%OUTDIR%\CHZM_LeaseBroker.exe"
set "PRESSURE=%OUTDIR%\CHZM_GpuPressure.exe"

if not exist "%SRC_BROKER%" (
    echo [FAIL] Source not found: "%SRC_BROKER%"
    exit /b 1
)
if not exist "%SRC_PRESSURE%" (
    echo [FAIL] Source not found: "%SRC_PRESSURE%"
    exit /b 1
)

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
echo.

echo [INFO] Building CHZM_LeaseBroker.exe
"%GXX%" -std=c++20 -O2 -mavx2 -static -Wall -Wextra ^
    -o "%BROKER%" "%SRC_BROKER%" -ld3d12 -ldxgi -lole32
if errorlevel 1 (
    echo [FAIL] Broker build failed.
    exit /b 1
)

echo [INFO] Building CHZM_GpuPressure.exe
"%GXX%" -std=c++20 -O2 -static -Wall -Wextra ^
    -o "%PRESSURE%" "%SRC_PRESSURE%" -ld3d12 -ldxgi -lole32
if errorlevel 1 (
    echo [FAIL] Pressure build failed.
    exit /b 1
)

echo.
echo [ OK ] Build complete:
echo        %BROKER%
echo        %PRESSURE%
exit /b 0
