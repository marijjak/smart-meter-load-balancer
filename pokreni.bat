@echo off
rem Pokrece ceo sistem: LoadBalancer sa N Worker-a, N Worker-a i jednog User-a.
rem
rem   pokreni.bat                    N = 10, User radi funkcionalni test 1
rem   pokreni.bat 5                  N = 5
rem   pokreni.bat 10 meni            User otvara meni
rem   pokreni.bat 10 stress 0 10000  User radi stress test (10.000 upisa)
rem
rem Svaki proces dobija svoj prozor. Prozori ostaju otvoreni posle kraja rada,
rem da se vidi ispis. LB se gasi sa q u njegovom prozoru, i tada se gase i Worker-i.

setlocal
set N=%1
if "%N%"=="" set N=10
set MODE=%2
if "%MODE%"=="" set MODE=test1
set ARGS=%3 %4 %5
set DIR=%~dp0x64\Debug

if not exist "%DIR%\LoadBalancer.exe" (
    echo Nema %DIR%\LoadBalancer.exe - prvo uradi Build u Visual Studio-u ^(x64, Debug^).
    pause
    exit /b 1
)

start "LB" cmd /k ""%DIR%\LoadBalancer.exe" %N%"
timeout /t 1 /nobreak >nul

set /a LAST=%N%-1
for /L %%i in (0,1,%LAST%) do start "WR %%i" /min cmd /k ""%DIR%\Worker.exe" %%i"
timeout /t 2 /nobreak >nul

start "User" cmd /k ""%DIR%\User.exe" %MODE% %ARGS%"
endlocal
