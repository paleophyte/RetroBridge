# Cross-compiles llm_agent.c for legacy 32-bit Windows (95/98/ME/NT4/2000/XP).
#
# Uses the i686-w64-mingw32 toolchain (msvcrt.dll based), NOT the
# UCRT64/x86_64 MSYS2 environments - those link ucrtbase.dll, which doesn't
# exist before Vista SP2.
#
# The --major-subsystem-version/--major-os-version flags force the PE header
# down to 4.0. Without this, the linker stamps a modern subsystem version
# and the Windows loader on old OSes refuses to run the binary at all
# ("is not a valid Win32 application") even though nothing else is wrong.

CC      := i686-w64-mingw32-gcc
# No -DUNICODE/-D_UNICODE: we want the ANSI "A" API surface everywhere,
# since Windows 9x's wide-char entry points are mostly unimplemented stubs.
#
# -march=i486 overrides this toolchain's baked-in default of
# -march=pentium4 (confirmed via `gcc -v`: -mtune=generic -march=pentium4,
# from how this MSYS2 package itself was built - nothing we ever set).
# Pentium 4 means SSE2, and at -O2 GCC happily vectorizes plain
# ZeroMemory()/memset()-shaped code into pxor/movups/movdqu - invalid
# opcodes on any pre-2000 CPU. Crashed for real on a genuine Windows 95 VM
# ("illegal operation", LLM_AGENT.EXE, fault instruction 66 0f ef c0 =
# pxor %xmm0,%xmm0) despite working fine on Windows 2000/NT4 targets,
# whose VM CPUs happen to still expose SSE2 - this was latent in every
# binary built before this flag existed, not something specific to 9x
# code paths. i486 disables MMX/SSE/SSE2 and the P6-only cmov for code
# WE compile - but -march only affects fresh compilation, not prebuilt
# object code pulled in from the toolchain's own static libraries (see
# the CRT-formatted-I/O note in llm_agent.c for the bigger piece of that:
# _snprintf/printf/sscanf were removed from this file entirely, since
# each one statically links a large chunk of mingw's own prebuilt
# printf/scanf internals, built with the SAME unsafe pentium4 default,
# that -march=i486 here cannot retroactively fix). A handful of cmov
# instructions remain in mingw's mandatory CRT startup internals
# (thread-local-storage setup, PE image introspection) that run before
# main() and aren't something this project's own code can avoid short of
# replacing the CRT entry point - accepted as a narrow, undocumented-
# further residual gap (real 486 / non-Pro Pentium only; every VM tested
# against so far has CMOV, just not SSE2).
CFLAGS  := -O2 -Wall -march=i486 -DWIN32_LEAN_AND_MEAN
LDCOMMON := -static-libgcc \
           -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
           -Wl,--major-os-version,4 -Wl,--minor-os-version,0
LDFLAGS := $(LDCOMMON) -lws2_32 -ladvapi32 -lgdi32 -luser32

all: llm_agent.exe update.exe

llm_agent.exe: llm_agent.c ../common/session_timeout.h ../common/command_line.h
	$(CC) $(CFLAGS) llm_agent.c -o llm_agent.exe $(LDFLAGS)

# update.exe (see docs/ARCHITECTURE.md "Self-update") replaces a running
# llm_agent installation with a new binary and restarts it. Same
# -march=i486 / no-CRT-formatted-IO discipline as llm_agent.c - it's a
# separate compiled binary, so it's just as exposed to the toolchain's
# unsafe pentium4 default. Only needs advapi32 (SCM calls) - no sockets,
# no GDI.
update.exe: update.c
	$(CC) $(CFLAGS) update.c -o update.exe $(LDCOMMON) -ladvapi32

clean:
	rm -f llm_agent.exe update.exe

.PHONY: all clean
