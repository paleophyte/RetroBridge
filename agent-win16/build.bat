@echo off
REM Build llm_agent.exe for 16-bit Windows 3.1/WFW (Open Watcom + Winsock 1.1).
REM No Watt-32/packet-driver dependency here -- WFW's own WINSOCK.DLL is used.

call c:\watcom\owsetenv.bat
if "%WATCOM%"=="" (
  echo ERROR: WATCOM is not set after c:\watcom\owsetenv.bat -- edit WATCOM path in build.bat
  exit /b 1
)

echo Building llm_agent.exe (Win16) with WATCOM=%WATCOM%
REM -zW: Windows-style prologs/epilogs, required for an exported WNDPROC.
REM -bt=windows + -l=windows: 16-bit Windows target and default import libs.
REM -i must list H\WIN explicitly and before the default search -- without
REM it, <windows.h> resolves to the H\NT (Win32/NT) copy instead, which is
REM missing every Win16-only symbol this agent needs (GetModuleUsage,
REM GetFreeSystemResources, GFSR_SYSTEMRESOURCES, ...).
REM winsock.lib: not part of the -l=windows default set, add explicitly.
REM toolhelp.lib: same story, needed for PSLIST/PSKILL's TaskFirst/
REM TaskNext/ModuleFindHandle/TerminateApp (TOOLHELP.DLL).
REM -fm: emit a linker map so a GPF's "module:offset" can be traced back
REM to a function if this ever crashes again.
REM Stack size: Watcom's default for this target is 8K, which a GPF
REM traced (via llm_agent.map) to __STK/__STKOVERFLOW_ inside WinMain
REM suggested was too tight once every blocked accept()/recv() started
REM nesting BlockingHookProc -> GetMessage -> DispatchMessage -> WndProc
REM frames on top of whatever Winsock's own internals already use. -k
REM (wcl's DOS-oriented shortcut) has no effect combined with -l=windows
REM for this NE target -- pass wlink's own OPTION STACK directive
REM straight through instead (any arg wcl doesn't recognize goes to the
REM linker unchanged).
wcl -zq -zW -bt=windows -os -w4 -fm=llm_agent.map -i="%WATCOM%\H\WIN" -i="%WATCOM%\H" llm_agent.c -fe=llm_agent.exe -l=windows winsock.lib toolhelp.lib -"OPTION STACK=16384"
if errorlevel 1 exit /b 1
echo OK: llm_agent.exe
dir llm_agent.exe
