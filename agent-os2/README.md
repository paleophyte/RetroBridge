# llm_agent for OS/2 2.x

**32-bit OS/2 (LX)** port of [`../agent/llm_agent.c`](../agent/llm_agent.c). Speaks the
**same** token-authed TCP wire protocol as FreeDOS/Windows, so
[`../bridge/server.py`](../bridge/server.py) can drive an OS/2 2.11 VM with no
protocol fork.

Uses IBM **SO32DLL** / **TCP32DLL** (Socket/MPTS) and PM (`PMWIN`/`PMGPI`) for
desktop screenshots. Guest must have INET/IFNDIS loaded; `C:\MPTN\DLL` on
`LIBPATH`.

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `EXEC` | `CMD.EXE /C` with stdout redirected → `LEN:`/`EXIT:` |
| `PUT` / `GET` | File transfer |
| `SYSINFO` | `os_family=os2`, version, C: disk space |
| `SCREENSHOT` | Full PM desktop via `WinGetScreenPS` → 24-bit BMP |

Everything else returns `ERR:not supported on OS/2`.

## Build (Open Watcom on host)

Copy from the guest into `vendor/`:

- `SO32DLL.DLL`
- `TCP32DLL.DLL`

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-os2
build.bat
```

Produces `llm_agent.exe` (OS/2 LX). Floppy:

```bat
..\bridge\.venv\Scripts\python.exe make_floppy.py
```

## Deploy

1. TCP/IP up; `SO32DLL.DLL` on `LIBPATH` (normally `C:\MPTN\DLL`).
2. Copy `LLMAGENT.EXE` + `LLMAGENT.INI` to e.g. `C:\LLM\`.
3. Run `LLMAGENT.EXE` from an OS/2 window.

## Why 32-bit?

Watcom’s PM headers are 32-bit-only (`os2.h` errors on `_M_I86`). Desktop
capture needs `WinGetScreenPS` / `GpiBitBlt`, so the agent is LX + SO32DLL
rather than 16-bit TCPIPDLL.
