@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

if not exist ..\vendor\imports\clib.imp (
  echo ERROR: ..\vendor\imports\clib.imp missing — run ..\fetch_sdk.bat
  exit /b 1
)

REM NetWare 4.x+ style: Watcom -bt=netware + static clib3s RTL (format v4).
wcc386 -bt=netware -ms -3s -zq -s -i=..\vendor -fo=llm_agent.obj ..\llm_agent.c
if errorlevel 1 exit /b 1

wlink @llm_agent.lnk
if errorlevel 1 exit /b 1

echo Built nw4\LLMAGENT.NLM (format v4, Watcom clib3s)
dir LLMAGENT.NLM
