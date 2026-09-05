# Cross-compile llm_agent.c for 16-bit DOS with Open Watcom + Watt-32.
#
# IMPORTANT for cmd.exe:
#   set "WATT32=C:/Users/admin/code/Watt-32"
# (quotes prevent a trailing space; wmake treats \ as escape so use /)
#
# Build Watt-32 large lib once:
#   set "WATT_ROOT=C:\Users\admin\code\Watt-32"
#   cd /d %WATT_ROOT%\src
#   configur.bat watcom
#   call c:\watcom\owsetenv.bat
#   set "W32_BIN2C=%WATT_ROOT%\util\win32\bin2c.exe"
#   set "W32_NASM=%WATT_ROOT%\util\win32\nasm.exe"
#   wmake -h -f watcom_l.mak
#
# Then:
#   call c:\watcom\owsetenv.bat
#   set "WATT32=C:/Users/admin/code/Watt-32"
#   wmake -f Makefile

!ifndef WATT32
WATT32 = $(%WATT32)
!endif

# Literal slash written as $[/] so wmake does not treat it as a macro.
SLASH = $[/]
WATT32INC = $(WATT32)$(SLASH)inc
WATT32LIB = $(WATT32)$(SLASH)lib$(SLASH)wattcpwl.lib

CC = wcl

all: llm_agent.exe .SYMBOLIC

llm_agent.exe: llm_agent.c
	$(CC) -zq -ml -bt=dos -os -w4 -k8192 -i=$(WATT32INC) llm_agent.c -fe=llm_agent.exe -l=dos $(WATT32LIB)

clean: .SYMBOLIC
	-del llm_agent.exe *.obj *.err
