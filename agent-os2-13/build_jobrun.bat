@echo off
setlocal
call C:\watcom\owsetenv.bat
cd /d "%~dp0"
wcl -zq -ml -bt=os2 -i=c:\watcom\h\os21x ..\common\jobrun.c -fe=JOBRUN.EXE -l=os2
if errorlevel 1 exit /b 1
