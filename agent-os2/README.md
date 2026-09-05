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
| `CLICK` | PM `WinSetPointerPos` + `BM_CLICK` / button up-down (top-left coords) |
| `KEY` / `TYPE` | `WM_CHAR` / `WM_VIOCHAR` to the focus window (same keyspec grammar as Windows) |
| `WINLIST` | Switch-list entries (titles + top-left frame rects for CLICK) |
| `PSLIST` / `PSKILL` | `DosQProcStatus` process table / `DosKillProcess` |
| `REBOOT` | Detached `REBOOT.EXE` (OEMHLP/DOS$ IOCTL, then DOS `.COM` kbd reset) |
| `CLIPSET` | PM clipboard `CF_TEXT` via giveable shared mem (`CFI_POINTER`) |
| `EXECDETACH` | Independent session via `DosStartSession` (for `UPDATE.EXE` / `REBOOT.EXE`) |

The agent **minimizes itself** after listen (and `UPDATE.EXE` /
`EXECDETACH` / `STARTUP.CMD` start it with `/MIN` or `SSF_CONTROL_MINIMIZE`).

Self-update: build also produces `update.exe`. Bridge tool
`legacy_self_update` PUTs `LLMNEW.EXE` + `UPDATE.EXE`, then
`EXECDETACH`s the helper, which kills the old agent via `AGENT.PID`,
swaps the binary, and restarts `LLMAGENT.EXE`.

Reboot: also deploy `REBOOT.EXE` next to the agent (`build.bat` builds it).
Use `REBOOT` to cycle the guest. `SHUTDOWN` is **not supported** on OS/2
2.11 here — `DosShutdown` hard-locks a painted desktop, and
`WinShutdownSystem` blocks on per-session “close without saving?” dialogs
that we could not auto-dismiss reliably.

`CLIPSET` is verified by the agent returning `OK` after
`WinSetClipbrdData`. Pair with `KEY ctrl-v` to paste. Do **not** run PM
helpers under `EXEC` with stdout redirect — that has GPFd (`SYS3175`) on
2.11.

Everything else returns `ERR:not supported on OS/2` (including `REG*` /
`SHUTDOWN`).

## Build (Open Watcom on host)

Copy from the guest into `vendor/`:

- `SO32DLL.DLL`
- `TCP32DLL.DLL`

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-os2
build.bat
```

Produces `llm_agent.exe` and `update.exe` (OS/2 LX). Floppy:

```bat
..\bridge\.venv\Scripts\python.exe make_floppy.py
```

## Deploy

1. TCP/IP up; `SO32DLL.DLL` on `LIBPATH` (normally `C:\MPTN\DLL`).
2. Copy `LLMAGENT.EXE` + `LLMAGENT.INI` to e.g. `C:\llmagent\`.
3. Run `LLMAGENT.EXE` from an OS/2 window (or `STARTUP.CMD`).

## Self-update (from host)

```python
# after build.bat — use 8.3 names matching the guest deploy
from agent_client import AgentClient
# or bridge tool legacy_self_update(..., remote_dir=r"C:\llmagent",
#   new_agent_name="LLMNEW.EXE", update_exe_name="UPDATE.EXE",
#   target_agent_name="LLMAGENT.EXE")
```

## Why 32-bit?

Watcom’s PM headers are 32-bit-only (`os2.h` errors on `_M_I86`). Desktop
capture needs `WinGetScreenPS` / `GpiBitBlt`, so the agent is LX + SO32DLL
rather than 16-bit TCPIPDLL.
