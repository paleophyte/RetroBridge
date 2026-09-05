# llm_agent for OS/2 2.x

16-bit OS/2 port of [`../agent/llm_agent.c`](../agent/llm_agent.c). Speaks the
**same** token-authed TCP wire protocol as FreeDOS/Windows, so
[`../bridge/server.py`](../bridge/server.py) can drive an OS/2 2.11 VM with no
protocol fork.

Uses IBM **TCPIPDLL** (Socket/MPTS). Guest must have INET/IFNDIS loaded and
`TCPIPDLL.DLL` on `LIBPATH` (normally under `C:\MPTN\DLL`).

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `EXEC` | `system()` with stdout redirected to temp → `LEN:`/`EXIT:` |
| `PUT` / `GET` | File transfer |
| `SYSINFO` | `os_family=os2`, version fields, C: disk space |

Everything else returns `ERR:not supported on OS/2`.

Trust model unchanged: cleartext token, lab/host-only network only.

## Build (Open Watcom on host)

Requires `vendor/TCPIPDLL.DLL` copied from the guest (import lib is built
automatically):

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-os2
build.bat
```

Produces `llm_agent.exe` (OS/2 16-bit NE). Optional proof binary:

```bat
build_sockping.bat
```

Floppy image (needs `pyfatfs` — e.g. bridge venv):

```bat
..\bridge\.venv\Scripts\python.exe make_floppy.py
```

→ `llm_agent.flp` with `LLMAGENT.EXE`, `SOCKPING.EXE`, `LLMAGENT.INI`.

## Deploy on OS/2 2.11

1. TCP/IP up (`ifconfig lan0` works; PCnet + IFNDIS/INET in `CONFIG.SYS`).
2. Mount `llm_agent.flp`, copy to e.g. `C:\LLM\`.
3. Ensure `TCPIPDLL.DLL` is findable (`LIBPATH` includes its directory).
4. Edit `LLMAGENT.INI` (`port=` / `token=`).
5. Start from an OS/2 window:

   ```
   C:\LLM\LLMAGENT.EXE
   ```

6. Bridge `machines.ini`:

   ```ini
   [os2]
   host = 10.102.10.198
   exec_port = 2222
   exec_token = REPLACE_WITH_UNIQUE_TOKEN
   ```

7. Host smoke: `python _smoke_test.py` (from this directory, with bridge on
   `PYTHONPATH` or via the script’s path insert).

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `build.bat` / `build_sockping.bat` | Watcom 16-bit OS/2 builds |
| `vendor/os2sock.h` | Minimal TCPIPDLL prototypes |
| `vendor/TCPIPDLL.DLL` | Guest DLL for import lib |
| `LLMAGENT.INI.example` | Config template |
| `_smoke_test.py` | Host-side capability check |
| `make_floppy.py` | 1.44MB deploy image |
