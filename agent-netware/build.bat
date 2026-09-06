@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

if not exist vendor\imports\clib.imp (
  echo ERROR: vendor\imports\clib.imp missing
  echo Run fetch_sdk.bat once, or copy clib.imp from the Novell CLIB NDK.
  exit /b 1
)
if not exist vendor\imports\prelude.obj (
  echo ERROR: vendor\imports\prelude.obj missing — run fetch_sdk.bat
  exit /b 1
)

REM NetWare 3.12 path: Novell prelude + CLIB imports (no Watcom static RTL).
REM Leave NLM file-format version 4 as emitted by wlink — do not patch to 3
REM (3.12 rejects patched v3 with "Invalid load file version").
REM For NetWare 4.x+ / Watcom clib3s builds, see nw4\build.bat.
wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=llm_agent.obj llm_agent.c
if errorlevel 1 exit /b 1

wlink @llm_agent.lnk
if errorlevel 1 exit /b 1

set PYTHON=python
if exist ..\mcp-server\.venv\Scripts\python.exe set PYTHON=..\mcp-server\.venv\Scripts\python.exe
%PYTHON% fix_nlm_imports.py LLMAGENT.NLM
if errorlevel 1 exit /b 1

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=update.obj update.c
if errorlevel 1 exit /b 1
wlink @update.lnk
if errorlevel 1 exit /b 1
%PYTHON% fix_nlm_imports.py UPDATE.NLM
if errorlevel 1 exit /b 1

echo Built LLMAGENT.NLM UPDATE.NLM
dir LLMAGENT.NLM UPDATE.NLM
