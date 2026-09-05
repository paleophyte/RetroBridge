@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

if not exist vendor\tcpipdll.lib (
  if not exist vendor\TCPIPDLL.DLL (
    echo ERROR: vendor\TCPIPDLL.DLL missing — copy from OS/2 guest
    exit /b 1
  )
  wlib -c vendor\tcpipdll.lib +vendor\TCPIPDLL.DLL
  if errorlevel 1 exit /b 1
)

wcc -bt=os2 -ml -zq -i=vendor _sockping.c
if errorlevel 1 exit /b 1
wlink system os2 op map name _sockping file _sockping library vendor\tcpipdll.lib
if errorlevel 1 exit /b 1
echo Built _sockping.exe
dir _sockping.exe
