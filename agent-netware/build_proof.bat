@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 exit /b 1
cd /d "%~dp0"

if not exist vendor\imports\clib.imp exit /b 1
if not exist vendor\imports\prelude.obj exit /b 1

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=_hello.obj _hello.c
if errorlevel 1 exit /b 1
wlink @_hello.lnk
if errorlevel 1 exit /b 1
set PYTHON=python
if exist ..\mcp-server\.venv\Scripts\python.exe set PYTHON=..\mcp-server\.venv\Scripts\python.exe
%PYTHON% fix_nlm_imports.py HELLO.NLM
if errorlevel 1 exit /b 1
echo Built HELLO.NLM

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=_sockping.obj _sockping.c
if errorlevel 1 exit /b 1
wlink @sockping.lnk
if errorlevel 1 exit /b 1
%PYTHON% fix_nlm_imports.py SOCKPING.NLM
if errorlevel 1 exit /b 1
echo Built SOCKPING.NLM
dir HELLO.NLM SOCKPING.NLM
