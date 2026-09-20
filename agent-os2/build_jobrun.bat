@echo off
setlocal
call C:\watcom\owsetenv.bat
cd /d "%~dp0"
wcl386 -zq -bt=os2v2 -i=c:\watcom\h\os2 ..\common\jobrun.c -fe=JOBRUN.EXE -l=os2v2
if errorlevel 1 exit /b 1
