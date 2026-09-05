# OS/2 agent — session handoff

Point a new Cursor chat at this file (and the repo root
`C:\Users\admin\code\retro-ssh-server`). Goal: **`llm_agent`** on
**OS/2 2.11** (lab VM **BETELGEUSE**, `10.102.10.198`) so the existing MCP
bridge can drive it with **no protocol fork**.

## What this project is

An LLM tool-calling harness for legacy VMs — not SSH. A small TCP agent
on the guest + `bridge/server.py` (MCP) on the host.

| Piece | Path |
|---|---|
| Windows reference agent | `agent/llm_agent.c` |
| FreeDOS agent (done, working) | `agent-dos/llm_agent.c` |
| **OS/2 2.11 agent** | `agent-os2/llm_agent.c` |
| Wire protocol client | `bridge/agent_client.py` |
| MCP tools | `bridge/server.py` |
| Architecture notes | `docs/ARCHITECTURE.md` |
| Host machine config | `C:\Users\admin\.retro-ssh-server\machines.ini` |

## Protocol (must match)

Cleartext TCP, lab network only:

1. Client connects → sends `token\n` → expects `OK\n` (else `FAIL`)
2. Commands are one line; binary payloads framed with `LEN:` / `SIZE:` etc.
3. See header comments in `bridge/agent_client.py` and FreeDOS
   `agent-dos/llm_agent.c` for the full command set.

**MVP (OS/2):**

| Implement | Defer / `ERR:not supported on OS/2` |
|---|---|
| auth, `PING`, `QUIT` | `EXECDETACH`, `CLICK`, `WINLIST` |
| `EXEC` (capture stdout) | `CLIPSET`, `REG*`, `PSLIST`/`PSKILL` |
| `PUT` / `GET` | `SHUTDOWN`, `REBOOT` |
| `SYSINFO` (`os_family=os2`, …) | `SCREENSHOT`, `KEY`, `TYPE` |

## FreeDOS status (do not re-do)

FreeDOS agent is **live and smoke-tested** against `[dos]` in
`machines.ini` (`10.102.10.224:2222`).

## OS/2 2.11 status

- Guest: **IBM OS/2 2.11** in VMware (BETELGEUSE), AMD PCnet (`PCNTND`).
- TCP/IP: Socket/MPTS — `IFNDIS.SYS` + `INET.SYS` + `CNTRL.EXE` in
  `CONFIG.SYS`; tools under `C:\MPTN\BIN`. No `C:\TCPIP` tree (normal).
- PCnet **PermaNet Server feature = TRUE** was required for ping to work
  in this lab VM.
- Build: Open Watcom on host, **16-bit OS/2 NE**, import lib from guest
  `TCPIPDLL.DLL` (`agent-os2/vendor/`). See `agent-os2/README.md`.
- Bridge section: `[os2]` → `10.102.10.198:2222`.

### Networking pitfalls already hit

- MPTS “Reading system information” can hang; Ctrl-Alt-Del is reboot on
  OS/2 (use Ctrl-Esc / Alt-Esc for Window List / session switch).
- `INETWAIT: Unable to access INET shared segment` = `INET.SYS` not in
  `CONFIG.SYS` (UI config alone is not enough).
- `TCPIP.NIF` names `INET.SYS` + `IFNDIS.SYS` (no `TCPIP.OS2` on this
  stack).
- `LNM0038` = duplicate `LANMSGEX`; REM one start. LAN Requester /
  `NET START` can hang boot — keep REM’d for TCP-only work.

## Deploy / smoke

```bat
cd agent-os2
build.bat
..\bridge\.venv\Scripts\python.exe make_floppy.py
```

On guest: copy from floppy → `C:\LLM\`, run `LLMAGENT.EXE` with TCP up.
Host: `python _smoke_test.py`.

## Out of scope (still)

- Protocol fork, Windows-parity GUI/registry tools, internet exposure
