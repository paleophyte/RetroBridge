@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

call "%~dp0..\sdk_env.bat"
if errorlevel 1 exit /b 1

REM Use Novell prelude and CLIB throughout, as in the 3.12 build.
REM Watcom's static sprintf wrapper has an incompatible va_list for CLIB.
REM -zl suppresses default libraries; -3s selects stack-based API calls.
wcc386 -ms -3s -zq -s -zl -fpc -i=..\vendor -fo=llm_agent.obj ..\llm_agent.c
if errorlevel 1 exit /b 1

wlink @llm_agent.lnk file '%NLM_SDK_DIR%\prelude.obj'
if errorlevel 1 exit /b 1

REM NetWare 4.11 also needs classic length-prefixed imports; Watcom's
REM padded fields produce garbage "missing symbol" lists in its loader.
"%NLM_PYTHON%" ..\fix_nlm_imports.py LLMAGENT.NLM
if errorlevel 1 exit /b 1

echo Built nw4\LLMAGENT.NLM (format v4, Novell prelude and CLIB)
dir LLMAGENT.NLM
