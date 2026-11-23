@echo off
rem Stress test 1: propusnost upisa za N = 1, 2, 5, 10.
rem
rem   stress1.bat            koristi x64\Release (merenja se rade na Release verziji)
rem   stress1.bat Debug      koristi x64\Debug
rem
rem Za svako N:
rem   1. pokrene LoadBalancer N i N Worker-a
rem   2. faza A: 4 User-a istovremeno salju po 2.500 upisa (ukupno 10.000 razlicitih
rem      brojila) - odavde su propusnost i kasnjenje za tabelu
rem   3. faza B: 4 User-a salju po 50.000 upisa (ukupno 200.000) dok typeperf svake
rem      sekunde belezi CPU, memoriju, niti, handle-ove, context switch-eve i TCP
rem   4. ugasi sve procese
rem Rezultati idu u folder rezultati\ (N<n>_A_ur<k>.txt, N<n>_B_ur<k>.txt, N<n>_perf.csv).

setlocal enabledelayedexpansion
set CFG=%1
if "%CFG%"=="" set CFG=Release
set DIR=%~dp0x64\%CFG%
set OUT=%~dp0rezultati

if not exist "%DIR%\LoadBalancer.exe" (
    echo Nema %DIR%\LoadBalancer.exe - prvo uradi Build ^(x64, %CFG%^).
    pause
    exit /b 1
)
tasklist /FI "IMAGENAME eq LoadBalancer.exe" 2>nul | find /I "LoadBalancer.exe" >nul && (
    echo LoadBalancer vec radi. Ugasi ga ^(q u njegovom prozoru^) pa pokreni ponovo.
    pause
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
echo %NUMBER_OF_PROCESSORS% > "%OUT%\jezgara.txt"

for %%N in (1 2 5 10) do call :runN %%N

echo.
echo Gotovo. Rezultati su u %OUT%
pause
exit /b 0

rem ---------------------------------------------------------------------------
:runN
set N=%1
echo ===== N = %N% =====
start "LB N=%N%" /min "%DIR%\LoadBalancer.exe" %N%
timeout /t 1 /nobreak >nul
set /a LAST=%N%-1
for /L %%i in (0,1,%LAST%) do start "WR %%i" /min "%DIR%\Worker.exe" %%i
timeout /t 2 /nobreak >nul

echo   faza A: 4 x 2.500 upisa
for /L %%u in (0,1,3) do (
    set /a FIRST=%%u*2500
    start "UR %%u" /min cmd /c ""%DIR%\User.exe" stress !FIRST! 2500 > "%OUT%\N%N%_A_ur%%u.txt""
)
call :waitUsers

echo   faza B: 4 x 50.000 upisa uz typeperf
rem Lista brojaca: niti LB-a i procesi Worker-a koji sada postoje + perf_brojaci.txt
set CF=%OUT%\brojaci_N%N%.txt
copy /y "%~dp0perf_brojaci.txt" "%CF%" >nul
typeperf -qx Thread | find "\Thread(LoadBalancer/" | find "Context Switches/sec" >> "%CF%"
typeperf -qx Process | find "\Process(Worker" | find "%% Processor Time" >> "%CF%"
start "typeperf" /min typeperf -cf "%CF%" -si 1 -o "%OUT%\N%N%_perf.csv" -f CSV -y
timeout /t 2 /nobreak >nul
for /L %%u in (0,1,3) do (
    set /a FIRST=100000+%%u*25000
    start "UR %%u" /min cmd /c ""%DIR%\User.exe" stress !FIRST! 25000 2 > "%OUT%\N%N%_B_ur%%u.txt""
)
call :waitUsers
timeout /t 2 /nobreak >nul

taskkill /F /IM typeperf.exe >nul 2>&1
taskkill /F /IM LoadBalancer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /F /IM Worker.exe >nul 2>&1
timeout /t 2 /nobreak >nul
exit /b 0

rem Ceka dok se svi User.exe procesi ne zavrse.
:waitUsers
timeout /t 1 /nobreak >nul
tasklist /FI "IMAGENAME eq User.exe" 2>nul | find /I "User.exe" >nul && goto waitUsers
exit /b 0
