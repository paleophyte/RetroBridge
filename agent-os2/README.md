# llm_agent for OS/2 2.x

**32-bit OS/2 (LX)** target-agent port of
[`../agent-win32/llm_agent.c`](../agent-win32/llm_agent.c). It is not an MCP server itself;
it speaks the **same** token-authed TCP wire protocol as FreeDOS/Windows, so
[`../mcp-server/server.py`](../mcp-server/server.py) can expose it through the repo's
`legacy_*` MCP tools with no protocol fork.

`legacy_winclose(machine, title)` exposes the native WINCLOSE operation:
close/cancel requests to all exact title matches, ignoring case. Apps may
prompt or refuse; acknowledgment does not prove closure. See
[MCP coverage](../docs/MCP_COVERAGE.md) and `legacy_capabilities` for limits.

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
| `WINCLOSE` | Close/cancel requests to every exact case-insensitive title match |
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
cd C:\src\RetroBridge\agent-os2
build.bat
```

Produces `llm_agent.exe`, `update.exe`, `reboot.exe`, and `jobrun.exe`
(OS/2 LX). The optional floppy builder additionally needs `pyfatfs` installed
in the host Python environment:

```bat
..\mcp-server\.venv\Scripts\python.exe make_floppy.py
```

The current floppy builder is a minimal bootstrap: it includes the agent and
optional SOCKPING proof, but not UPDATE, REBOOT, or JOBRUN. Transfer those
companions separately before using their operations.

## Deploy

1. TCP/IP up; `SO32DLL.DLL` on `LIBPATH` (normally `C:\MPTN\DLL`).
2. Copy `LLMAGENT.EXE` and `JOBRUN.EXE` to e.g. `C:\LLMAGENT\`.
   Add `REBOOT.EXE` for reboot and `UPDATE.EXE` for updates. Copy
   `LLMAGENT.INI.example` as `LLMAGENT.INI`, choose a unique token, and
   match the host, port, and token in your private bridge inventory.
3. Run `LLMAGENT.EXE` from an OS/2 window, or register it for autostart
   (see below) — **never** via a `CONFIG.SYS RUN=` line (see warning).

### Autostart: WPS Startup folder only, never `CONFIG.SYS RUN=`

`RUN=` lines execute before the WPS/Session Manager finishes
initializing. A process launched that early gets an incomplete session
context: it can still do plain socket I/O (`PING`/`PSLIST`/`SYSINFO`/PM
calls like `SCREENSHOT`/`CLICK`), but every one of *its own* calls to
`DosStartSession` fails forever after with `ERROR_SMG_INVALID_CALL`
(rc=418) — silently breaking `EXEC`, `EXECDETACH`, `REBOOT`, and
self-update, with no way to recover short of killing and relaunching the
process properly. Confirmed live: identical binary, identical
`CONFIG.SYS` otherwise, only the launch path differed.

The correct fix is a WPS Startup-folder object (`<WP_START>`), which the
shell launches *after* WPS is fully up — same session lineage as
double-clicking the icon by hand. Register it once via REXX
(`llmstart.cmd` in this directory):

```
C:\llmagent\LLMSTART.CMD
```

It calls `SysCreateObject` with `OBJECTID=<LLMAGENT_START>` and
`REPLACE`, so it's safe to re-run. On a heavily-populated WPS object
database (lots of installed software) this can take 60-90s to return —
that's normal, not a hang; the agent's single-threaded accept loop just
won't answer new connections until the call completes.

## Self-update (from host)

```python
# after build.bat — use 8.3 names matching the guest deploy
from agent_client import AgentClient
# or bridge tool legacy_self_update(..., remote_dir=r"C:\llmagent",
#   new_agent_name="LLMNEW.EXE", update_exe_name="UPDATE.EXE",
#   target_agent_name="LLMAGENT.EXE")
```

## Why 32-bit?

This port uses the 32-bit `h/os2` header tree, LX executable format, and
SO32DLL/TCP32DLL for OS/2 2.x and later tested kernels. Watcom also supplies
16-bit PM APIs in `h/os21x`; the separate [OS/2 1.3 port](../agent-os2-13/README.md)
uses them with NE/TCPIPDLL. Desktop capture does not require a 32-bit OS.

## EXEC lifetime follow-up

EXEC now uses an invocation-specific `LXxxxxxx` output directory. Files left
by a timed-out or interrupted invocation are retained for recovery and never
reused by a later EXEC. Missing output does not cause automatic reexecution.
See [long-running commands](../docs/LONG_RUNNING_COMMANDS.md) for wait/status
semantics, retained-file cleanup, and live verification.

## Tracked command jobs

This build advertises `exec_jobs=1`. Deploy the OS/2-target `JOBRUN.EXE`
companion built by `build.bat` alongside the agent. Two shell jobs can run
independently, with marker-based completion and observed shell exit status.
Cancellation is unsupported; disk output is not capped, while retrieval
exposes at most the first 1 MiB. See [job lifecycle and limits](../docs/LONG_RUNNING_COMMANDS.md).
