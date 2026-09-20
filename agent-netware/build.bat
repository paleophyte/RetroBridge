@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

call "%~dp0sdk_env.bat"
if errorlevel 1 exit /b 1

REM NetWare 3.12 path: Novell prelude + CLIB imports (no Watcom static RTL).
REM Leave NLM file-format version 4 as emitted by wlink — do not patch to 3
REM (3.12 rejects patched v3 with "Invalid load file version").
REM For the separate NetWare 4.x+ CLIB build, see nw4\build.bat.
wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=llm_agent.obj llm_agent.c
if errorlevel 1 exit /b 1

wlink @llm_agent.lnk file '%NLM_SDK_DIR%\prelude.obj'
if errorlevel 1 exit /b 1

"%NLM_PYTHON%" fix_nlm_imports.py LLMAGENT.NLM
if errorlevel 1 exit /b 1

wcc386 -ms -3s -zq -s -zl -fpc -i=vendor -fo=update.obj update.c
if errorlevel 1 exit /b 1
wlink @update.lnk file '%NLM_SDK_DIR%\prelude.obj'
if errorlevel 1 exit /b 1
"%NLM_PYTHON%" fix_nlm_imports.py UPDATE.NLM
if errorlevel 1 exit /b 1

echo Built LLMAGENT.NLM UPDATE.NLM
dir LLMAGENT.NLM UPDATE.NLM
