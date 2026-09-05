@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

if not exist vendor\so32dll.lib (
  if not exist vendor\SO32DLL.DLL (
    echo ERROR: vendor\SO32DLL.DLL missing — copy from guest C:\MPTN\DLL
    exit /b 1
  )
  wlib -c vendor\so32dll.lib +vendor\SO32DLL.DLL
  if errorlevel 1 exit /b 1
)
if not exist vendor\tcp32dll.lib (
  if not exist vendor\TCP32DLL.DLL (
    echo ERROR: vendor\TCP32DLL.DLL missing — copy from guest C:\MPTN\DLL
    exit /b 1
  )
  wlib -c vendor\tcp32dll.lib +vendor\TCP32DLL.DLL
  if errorlevel 1 exit /b 1
)

wcc386 -bt=os2v2 -zq -i=c:\watcom\h\os2 llm_agent.c
if errorlevel 1 exit /b 1
wlink @llm_agent.lnk
if errorlevel 1 exit /b 1
echo Built llm_agent.exe ^(32-bit OS/2 LX^)
dir llm_agent.exe
