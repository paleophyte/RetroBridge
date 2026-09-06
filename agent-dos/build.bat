@echo off
REM Build llm_agent.exe for 16-bit DOS (Open Watcom + Watt-32 large model).
REM Prefer this over Makefile on Windows -- wmake treats \ as an escape.

if "%WATT32%"=="" set "WATT32=C:\Users\admin\code\Watt-32"
if not exist "%WATT32%\lib\wattcpwl.lib" (
  echo ERROR: "%WATT32%\lib\wattcpwl.lib" not found.
  echo Build Watt-32 first: see README.md
  exit /b 1
)
if not exist "%WATT32%\inc\tcp.h" (
  echo ERROR: "%WATT32%\inc\tcp.h" not found.
  exit /b 1
)
if not exist "%WATT32%\inc\sys\watcom.err" (
  echo ERROR: "%WATT32%\inc\sys\watcom.err" missing -- run configur.bat watcom in Watt-32\src
  exit /b 1
)

call c:\watcom\owsetenv.bat
if "%WATCOM%"=="" (
  echo ERROR: WATCOM is not set after c:\watcom\owsetenv.bat -- edit WATCOM path in build.bat
  exit /b 1
)

echo Building llm_agent.exe with WATT32=%WATT32%
REM -k8192: default DOS stack (~2KB) overflows under Watt-32 + system()/EXEC.
REM Keep under ~16KB so DGROUP (near data+BSS+stack) stays within 64KB.
REM -os: favor size so FreeCom still has conventional RAM for EXEC children.
wcl -zq -ml -bt=dos -os -w4 -k8192 -i"%WATT32%\inc" llm_agent.c -fe=llm_agent.exe -l=dos "%WATT32%\lib\wattcpwl.lib"
if errorlevel 1 exit /b 1
echo OK: llm_agent.exe
dir llm_agent.exe
