@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 exit /b 1
cd /d "%~dp0"

call "%~dp0sdk_env.bat"
if errorlevel 1 exit /b 1

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=_hello.obj _hello.c
if errorlevel 1 exit /b 1
wlink @_hello.lnk file '%NLM_SDK_DIR%\prelude.obj'
if errorlevel 1 exit /b 1
"%NLM_PYTHON%" fix_nlm_imports.py HELLO.NLM
if errorlevel 1 exit /b 1
echo Built HELLO.NLM

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=_sockping.obj _sockping.c
if errorlevel 1 exit /b 1
wlink @sockping.lnk file '%NLM_SDK_DIR%\prelude.obj'
if errorlevel 1 exit /b 1
"%NLM_PYTHON%" fix_nlm_imports.py SOCKPING.NLM
if errorlevel 1 exit /b 1
echo Built SOCKPING.NLM
dir HELLO.NLM SOCKPING.NLM
