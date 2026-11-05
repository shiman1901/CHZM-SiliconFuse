@echo off
rem ===========================================================================
rem  CHZM SiliconFuse - Phase 3 master binary
rem  Output: build\CHZM_SiliconFuse.exe
rem  Toolchain: MinGW-w64 g++ (C++20, AVX2, static, Windows subsystem)
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUTDIR=%ROOT%\build"
set "OUT=%OUTDIR%\CHZM_SiliconFuse.exe"

set "GXX="
where g++ >nul 2>&1 && set "GXX=g++"
if not defined GXX if exist "%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe" set "GXX=%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
if not defined GXX if exist "C:\msys64\mingw64\bin\g++.exe" set "GXX=C:\msys64\mingw64\bin\g++.exe"
if not defined GXX if exist "C:\mingw64\bin\g++.exe" set "GXX=C:\mingw64\bin\g++.exe"
if not defined GXX (
    echo [FAIL] MinGW-w64 g++ not found.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [INFO] Compiler : %GXX%
echo [INFO] Output   : %OUT%

"%GXX%" -std=c++20 -O2 -mavx2 -municode -Wl,--subsystem,windows -static -w ^
    -I"%ROOT%\src" ^
    "%ROOT%\src\main.cpp" ^
    "%ROOT%\src\engine\VramPool.cpp" ^
    "%ROOT%\src\engine\StorageBridge.cpp" ^
    "%ROOT%\src\engine\PagingManager.cpp" ^
    "%ROOT%\src\gui\MainWindow.cpp" ^
    -o "%OUT%" ^
    -ld3d12 -ldxgi -lole32 -lpsapi -lgdi32 -lmsimg32 -lshell32 -luser32 -ldwmapi

if errorlevel 1 (
    echo [FAIL] Build failed.
    exit /b 1
)

echo [ OK ] Build complete: %OUT%
exit /b 0
