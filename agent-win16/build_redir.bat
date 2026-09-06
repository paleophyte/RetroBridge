@echo off
REM Build REDIR.EXE -- tiny native DOS helper used by llm_agent.c's EXEC
REM command to work around a WFW DOS-box redirection crash. Unlike
REM llm_agent.exe (Windows target), this is a plain DOS-target build --
REM no windows.h, no winsock, just the C runtime -- so it doesn't need
REM any of the -bt=windows/-l=windows setup from build.bat.

call c:\watcom\owsetenv.bat
if "%WATCOM%"=="" (
  echo ERROR: WATCOM is not set after c:\watcom\owsetenv.bat -- edit WATCOM path in build_redir.bat
  exit /b 1
)

echo Building REDIR.EXE (DOS target) with WATCOM=%WATCOM%
wcl -zq -ml -bt=dos -os -w4 redir.c -fe=redir.exe -l=dos
if errorlevel 1 exit /b 1
echo OK: redir.exe
dir redir.exe
