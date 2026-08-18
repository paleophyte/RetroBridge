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
CFLAGS  := -O2 -Wall -DWIN32_LEAN_AND_MEAN
LDFLAGS := -static-libgcc \
           -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
           -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
           -lws2_32 -ladvapi32

OUT := llm_agent.exe
SRC := llm_agent.c

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

clean:
	rm -f $(OUT)

.PHONY: clean
