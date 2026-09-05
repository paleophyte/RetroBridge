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

wcc -bt=os2 -ml -zq -i=vendor llm_agent.c
if errorlevel 1 exit /b 1
wlink system os2 op stack=8192 op map name llm_agent file llm_agent library vendor\tcpipdll.lib
if errorlevel 1 exit /b 1
echo Built llm_agent.exe
dir llm_agent.exe
