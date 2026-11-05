@echo off
rem ===========================================================================
rem  CHZM SiliconFuse - WDDM lease arbitration test
rem
rem  Sequence:
rem    1. start CHZM_LeaseBroker (holds a Tier-1 pool in GDDR6)
rem    2. wait for the pool to stabilise
rem    3. start CHZM_GpuPressure (requests 8 GB of VRAM and touches it)
rem    4. wait for the broker to evict, then restore after pressure ends
rem    5. write docs\WDDM_LEASE_REPORT.txt
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\docs\WDDM_LEASE_REPORT.txt"
set "BLOG=%TEMP%\chzm_broker_run.log"
set "PLOG=%TEMP%\chzm_pressure_run.log"

if not exist "%ROOT%\build\CHZM_LeaseBroker.exe" (
    echo [FAIL] CHZM_LeaseBroker.exe not found. Run build_broker.bat first.
    exit /b 1
)
if not exist "%ROOT%\build\CHZM_GpuPressure.exe" (
    echo [FAIL] CHZM_GpuPressure.exe not found. Run build_broker.bat first.
    exit /b 1
)
if not exist "%ROOT%\docs" mkdir "%ROOT%\docs"

del /q "%BLOG%" "%PLOG%" 2>nul

echo [RUN] Broker start
start "CHZM_LeaseBroker" /b "%ROOT%\build\CHZM_LeaseBroker.exe" --target-mb 4096 --slice-mb 1024 --dirty-mb 512 --run-seconds 55 > "%BLOG%" 2>&1

echo [RUN] Waiting 6 s for pool stabilisation
ping -n 7 127.0.0.1 >nul

tasklist /fi "IMAGENAME eq CHZM_LeaseBroker.exe" 2>nul | find /i "CHZM_LeaseBroker.exe" >nul
if errorlevel 1 (
    echo [ABORT] Broker exited during startup. The GPU is probably busy.
    echo         Close the game and run again. Broker log:
    type "%BLOG%"
    exit /b 2
)

echo [RUN] GPU pressure start (8 GB commit, 8 GB touched, 5 s)
"%ROOT%\build\CHZM_GpuPressure.exe" --size-mb 8192 --touch-mb 8192 --seconds 5 > "%PLOG%" 2>&1
set "PRC=%ERRORLEVEL%"
echo [RUN] GPU pressure exit code: %PRC%

echo [RUN] Waiting 20 s for pool restoration
ping -n 21 127.0.0.1 >nul

echo [RUN] Waiting for broker shutdown
set /a WAIT=0
:wait_broker
tasklist /fi "IMAGENAME eq CHZM_LeaseBroker.exe" 2>nul | find /i "CHZM_LeaseBroker.exe" >nul
if errorlevel 1 goto broker_done
set /a WAIT+=2
if %WAIT% GEQ 80 goto broker_timeout
ping -n 3 127.0.0.1 >nul
goto wait_broker

:broker_timeout
echo [WARN] Broker did not exit in time; terminating.
taskkill /f /im CHZM_LeaseBroker.exe >nul 2>&1

:broker_done

if not exist "%BLOG%" echo [WARN] Broker log missing. > "%BLOG%"
if not exist "%PLOG%" echo [WARN] Pressure log missing. > "%PLOG%"

> "%OUT%" echo ======================================================================
>> "%OUT%" echo CHZM-SiliconFuse WDDM Lease Arbitration Report
>> "%OUT%" echo ======================================================================
>> "%OUT%" echo Test date          : %DATE% %TIME%
>> "%OUT%" echo Broker pool request: 4096 MB in 4 x 1024 MB slices
>> "%OUT%" echo Dirty working set  : 512 MB per slice
>> "%OUT%" echo Pressure request   : 8192 MB commit, 8192 MB touched, 5 s
>> "%OUT%" echo Safety interlock    : abort if GPU utilization ^> 40%% (median) at start
>> "%OUT%" echo ----------------------------------------------------------------------
>> "%OUT%" echo [BROKER LOG]
>> "%OUT%" type "%BLOG%" >> "%OUT%"
>> "%OUT%" echo ----------------------------------------------------------------------
>> "%OUT%" echo [PRESSURE LOG]
>> "%OUT%" type "%PLOG%" >> "%OUT%"
>> "%OUT%" echo ----------------------------------------------------------------------
>> "%OUT%" echo [END OF REPORT]

echo.
type "%OUT%"
echo.
echo [DONE] Report written to %OUT%
exit /b 0
