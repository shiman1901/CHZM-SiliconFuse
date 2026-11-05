@echo off
rem ===========================================================================
rem  CHZM SiliconFuse - ReBAR direct silicon benchmark
rem  Target : tools/ReBarBench/main.cpp
rem  Output : build\CHZM_ReBarBench.exe
rem  Toolchain : MinGW-w64 g++ (C++20, AVX2, static), Direct3D 12 / DXGI
rem
rem  Usage:
rem    build_bench.bat                 build, run, save docs\REBAR_BENCHMARK.txt
rem    build_bench.bat --build-only    build only
rem    build_bench.bat --size 2048     build and run a single 2048 MB suite
rem ===========================================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "SRC=%ROOT%\tools\ReBarBench\main.cpp"
set "OUTDIR=%ROOT%\build"
set "OUT=%OUTDIR%\CHZM_ReBarBench.exe"
set "REPORT=%ROOT%\docs\REBAR_BENCHMARK.txt"

if not exist "%SRC%" (
    echo [FAIL] Source not found: "%SRC%"
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
echo [INFO] Source   : %SRC%
echo [INFO] Output   : %OUT%
echo.

"%GXX%" -std=c++20 -O2 -mavx2 -static -Wall -Wextra ^
    -o "%OUT%" "%SRC%" -ld3d12 -ldxgi

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

set "RUNARGS="
if /I "%~1"=="--size" set "RUNARGS=--size %~2"

echo [INFO] Running benchmark, capturing %REPORT%
"%OUT%" %RUNARGS% > "%REPORT%" 2>&1
set "RC=%ERRORLEVEL%"
type "%REPORT%"
echo.
echo [INFO] Benchmark exit code: %RC%
exit /b %RC%
