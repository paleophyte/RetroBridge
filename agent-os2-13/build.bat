@echo off
setlocal
call c:\watcom\owsetenv.bat
if errorlevel 1 (
  echo ERROR: c:\watcom\owsetenv.bat failed
  exit /b 1
)
cd /d "%~dp0"

if not exist vendor\tcpipdll.lib (
  if not exist vendor\TCPIPDLL.DLL (
    echo ERROR: vendor\TCPIPDLL.DLL missing - copy from the OS/2 1.3 guest.
    echo See vendor\README.md if the guest's DLL has a different name.
    exit /b 1
  )
  wlib -c vendor\tcpipdll.lib +vendor\TCPIPDLL.DLL
  if errorlevel 1 exit /b 1
)

rem os21x = Watcom's real 16-bit OS/2 1.x header set (Dos*/Win*/Gpi*).
rem The default h\os2 tree is 32-bit-only and hard-errors on -bt=os2 - see
rem README.md "Why this works" for how that was confirmed.
wcc -bt=os2 -ml -zq -i=c:\watcom\h\os21x -i=vendor llm_agent.c
if errorlevel 1 exit /b 1
wlink system os2 op stack=16384 op map name llm_agent file llm_agent ^
  library vendor\tcpipdll.lib library c:\watcom\lib286\os2\os2.lib
if errorlevel 1 exit /b 1
echo Built llm_agent.exe (OS/2 1.3, 16-bit NE)
dir llm_agent.exe

wcc -bt=os2 -ml -zq -i=c:\watcom\h\os21x -i=vendor update.c
if errorlevel 1 exit /b 1
wlink system os2 op stack=8192 name update file update ^
  library vendor\tcpipdll.lib library c:\watcom\lib286\os2\os2.lib
if errorlevel 1 exit /b 1
echo Built update.exe (self-update helper)
dir update.exe

rem ---------------------------------------------------------------------
rem IOSEG.DLL - the I/O privilege segment REBOOT needs, and IORESET.EXE,
rem the helper that drives it. See ioseg.c for why this is split in two
rem and why the link options below are what they are.
rem ---------------------------------------------------------------------

rem -nt=IOSEG puts this module's code in a segment of its own so the
rem linker can mark that one IOPL without dragging the C runtime to
rem ring 2 with it. -zu because SS != DS across a ring-2 call gate, and
rem -s to drop the __STK stack probes: ring-2 code has no business
rem calling back out to a C runtime helper.
wcc -bt=os2 -ml -zq -zu -s -nt=IOSEG -i=c:\watcom\h\os21x ioseg.c
if errorlevel 1 exit /b 1

rem "preload", not the linker's default load-on-call: a discardable IOSEG
rem would have to be demand-loaded from disk on first call, and the one
rem call that matters happens after DosShutdown has taken the disk away.
rem
rem Each export's trailing number is its parameter size in BYTES - the
rem count the ring-2 call gate copies from the ring-3 stack to the ring-2
rem stack. Wrong here and the callee reads garbage, so it tracks ioseg.c's
rem prototypes exactly: USHORT is 2 bytes.
wlink system os2_dll ^
  option quiet, map name ioseg file ioseg ^
  library clibl.lib ^
  segment 'IOSEG' iopl preload ^
  export IOIN8.1 resident 2 ^
  export IOOUT8.2 resident 4 ^
  export IOKBDRESET.3 resident 0 ^
  export IOPORT92RESET.4 resident 0 ^
  export IOCF9RESET.5 resident 0 ^
  export IOCAD.6 resident 0
if errorlevel 1 exit /b 1
echo Built ioseg.dll (IOPL segment, ring 2)
dir ioseg.dll

rem No import library: ioreset.c resolves IOSEG.DLL's entries at runtime
rem with DosLoadModule, so the DLL can live beside the .EXE rather than
rem having to be installed somewhere on LIBPATH.
wcc -bt=os2 -ml -zq -i=c:\watcom\h\os21x ioreset.c
if errorlevel 1 exit /b 1
wlink system os2 option quiet, map name ioreset file ioreset ^
  library c:\watcom\lib286\os2\os2.lib
if errorlevel 1 exit /b 1
echo Built ioreset.exe (reboot helper)
dir ioreset.exe
