@echo off
REM Build RESTART.EXE -- tiny native Win16 helper llm_agent.c's UPDATE
REM command launches to relaunch a fresh copy once the old instance's
REM module has actually unloaded. Windows target (needs GetModuleHandle/
REM GetModuleUsage/WinExec, all Win16 KERNEL APIs) but no winsock or
REM toolhelp -- no window/WNDPROC either, so no -zW needed.

call c:\watcom\owsetenv.bat
if "%WATCOM%"=="" (
  echo ERROR: WATCOM is not set after c:\watcom\owsetenv.bat -- edit WATCOM path in build_restart.bat
  exit /b 1
)

echo Building RESTART.EXE (Win16) with WATCOM=%WATCOM%
wcl -zq -bt=windows -os -w4 -i="%WATCOM%\H\WIN" -i="%WATCOM%\H" restart.c -fe=restart.exe -l=windows
if errorlevel 1 exit /b 1
echo OK: restart.exe
dir restart.exe
