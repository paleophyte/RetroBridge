@echo off
setlocal
call C:\watcom\owsetenv.bat
cd /d "%~dp0"
wcl -zq -ml -bt=dos -os -w4 -dJOBRUN_DOS ..\common\jobrun.c -fe=JOBRUN.EXE -l=dos
if errorlevel 1 exit /b 1
