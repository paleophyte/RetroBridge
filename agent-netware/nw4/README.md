# NetWare 4.x+ build (Watcom static RTL)

This is the **format-v4 / Open Watcom `clib3s`** agent build. Prefer it on
NetWare 4.x and later. NetWare 3.12's loader does not reliably accept these
binaries (you get either a garbage "missing symbol" dump or
`Invalid load file version` if the header version field is forced).

The default `..\build.bat` is the NetWare 3.12-oriented path (Novell
`prelude.obj` + CLIB imports only).

## Build

From this directory (or run `build.bat` here):

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-netware\nw4
build.bat
```

Produces `LLMAGENT.NLM` (NLM file-format version 4, linked with
`%WATCOM%\lib386\netware\clib3s.lib`).

Requires `..\vendor\imports\clib.imp` (same as the 3.12 tree).
