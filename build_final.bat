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
for %%P in ("%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
            "C:\msys64\mingw64\bin\g++.exe"
            "C:\mingw64\bin\g++.exe") do (
    if not defined GXX if exist %%P set "GXX=%%~fP"
)
if not defined GXX for /f "delims=" %%I in ('where g++ 2^>nul') do (
    if not defined GXX set "GXX=%%I"
)
if not defined GXX (
    echo [FAIL] MinGW-w64 g++ not found.
    exit /b 1
)

for %%I in ("%GXX%") do set "BINDIR=%%~dpI"
set "WINDRES=%BINDIR%windres.exe"
if not exist "%WINDRES%" (
    echo [FAIL] windres.exe not found next to g++.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [INFO] Compiler : %GXX%
echo [INFO] Output   : %OUT%

echo [INFO] Resources: src\app.rc
"%WINDRES%" -I "%ROOT%\src" -O coff -o "%OUTDIR%\app_res.o" "%ROOT%\src\app.rc"
if errorlevel 1 (
    echo [FAIL] windres failed.
    exit /b 1
)

"%GXX%" -std=c++20 -O2 -mavx2 -municode -Wl,--subsystem,windows -static -w ^
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    -I"%ROOT%\src" ^
    "%ROOT%\src\main.cpp" ^
    "%ROOT%\src\engine\VramPool.cpp" ^
    "%ROOT%\src\engine\StorageBridge.cpp" ^
    "%ROOT%\src\engine\PagingManager.cpp" ^
    "%ROOT%\src\engine\CryoSwap.cpp" ^
    "%ROOT%\src\engine\CommitGuard.cpp" ^
    "%ROOT%\src\engine\StandbyPurge.cpp" ^
    "%ROOT%\src\gui\MainWindow.cpp" ^
    "%OUTDIR%\app_res.o" ^
    -o "%OUT%" ^
    -ld3d12 -ldxgi -lole32 -luuid -lpsapi -lgdi32 -lmsimg32 -lshell32 -luser32 -ldwmapi

if errorlevel 1 (
    echo [FAIL] Build failed.
    exit /b 1
)

echo [ OK ] Build complete: %OUT%
exit /b 0
