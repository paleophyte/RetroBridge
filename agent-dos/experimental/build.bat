@echo off
setlocal
if "%WATT32%"=="" set "WATT32=C:\Users\admin\code\Watt-32"
call C:\watcom\owsetenv.bat
cd /d "%~dp0"
wcc -zq -ml -bt=dos -os -w4 -s -i"%WATT32%\inc" probe_isr.c
if errorlevel 1 exit /b 1
wcl -zq -ml -bt=dos -os -w4 -k8192 -i"%WATT32%\inc" resident_probe.c probe_isr.obj -fe=DOSPOLL.EXE -l=dos "%WATT32%\lib\wattcpwl.lib"
if errorlevel 1 exit /b 1
wcl -zq -bt=dos -os -w4 probe_child.c -fe=PROBECH.EXE -l=dos
if errorlevel 1 exit /b 1
