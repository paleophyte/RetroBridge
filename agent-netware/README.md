# llm_agent for NetWare 3.12+

32-bit NLM target-agent port of [`../agent/llm_agent.c`](../agent/llm_agent.c).
It is not an MCP server itself; it speaks the **same** token-authed TCP wire
protocol as FreeDOS/OS/2/Windows, so [`../bridge/server.py`](../bridge/server.py)
can expose it through the repo's `legacy_*` MCP tools with no protocol fork.

Uses Novell **CLIB** BSD sockets. Guest must already have TCP/IP loaded
(`TCPIP` / your usual stack) and `CLIB.NLM` available (autoload via the NLM
header).

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `EXEC` | `system()` (console-style). **No stdout capture** — replies `LEN:0` + `EXIT:rc` |
| `PUT` / `GET` | File transfer (NetWare paths, e.g. `SYS:SYSTEM\FOO.TXT`) |
| `SYSINFO` | `os_family=netware`, server name/version, SYS: volume space |
| `AUTOEXEC` | Ensure `AUTOEXEC.NCF` has `LOAD CLIBAUX` + `LOAD LLMAGENT` |
| `SCREENSHOT` | Best-scoring console text cells → 24-bit BMP |
| `KEY` / `TYPE` | Via **StuffKey** when `STUFFKEY.NLM` is present (INSTALL/NWSNUT). Else CLIB `ungetch` (console only) |
| `SCREENS` | List CLIB screens (`id`, displayed flag, name) for INSTALL discovery |
| `SHUTDOWN` | `OK` then `DownFileServer(1)` (force down — lab only) |
| `UPDATE` | `OK` then `LOAD UPDATE` (needs `LLMAGENT.NEW` + `UPDATE.NLM` on SYS:SYSTEM) |
| `DEBUG` / `DEBUG 0` / `DEBUG 1` | Runtime verbose toggle; `DEBUG 1` truncates `SYS:SYSTEM\LLMAGENT.LOG` |

Everything else returns `ERR:not supported on NetWare`.

Console logging is quiet by default. Prefer `DEBUG 1` over editing the INI for
short sessions: enable → exercise → `GET SYS:SYSTEM\LLMAGENT.LOG` → `DEBUG 0`.
Boot-time `debug=1` in `SYS:SYSTEM\LLMAGENT.INI` still works. Do **not** leave the
token on `A:` — put `LLMAGENT.INI` on `SYS:SYSTEM` only (`A:` is a load fallback).

### Driving INSTALL menus

Needs both on `SYS:SYSTEM`:
- **STUFFKEY.NLM** (TID 2948742 / `stufkey5.exe`)
- **CLIBAUX.NLM** (required on NetWare 3.12 — without it StuffKey loads
  but does nothing useful / may flash screens)

```text
PUT SYS:SYSTEM\CLIBAUX.NLM
PUT SYS:SYSTEM\STUFFKEY.NLM
LOAD INSTALL                    (console or EXEC)
KEY down down enter             (batched — one StuffKey run)
TYPE text
```

Prefer `install_menu.py` for INSTALL navigation: it batches keystrokes into one
StuffKey script and waits for unload (rapid per-key LOAD/UNLOAD abends 3.12
with “how many zombies?”). Example: `install_menu.py --path write-autoexec`.

Agent writes `SYS:SYSTEM\L.SK` and runs via short `SK.NCF`
(`LOAD STUFFKEY SYS:SYSTEM/L.SK /sr /d=80`) because `system()` truncates ~32
chars (`/s` avoid flashing targets, `/r` restore the operator screen).

### Persist across reboot

```text
AUTOEXEC
```

Idempotent: appends `LOAD CLIBAUX` + `LOAD LLMAGENT` to
`SYS:SYSTEM\AUTOEXEC.NCF` if missing (after existing TCP setup). Replies
`OK autoexec=added` or `OK autoexec=present`.

### Remote update

```text
PUT SYS:SYSTEM\UPDATE.NLM     (once)
PUT SYS:SYSTEM\LLMAGENT.NEW    (new agent build)
UPDATE                         (agent command — or console: LOAD UPDATE)
```

Prefer agent `UPDATE` (self-exit) over console `UNLOAD LLMAGENT` — the latter
has abended on this 3.12 lab box.

`UPDATE.NLM` pauses, unloads/replaces, then `LOAD LLMAGENT`. Expect a brief
disconnect; reconnect and `PING`.

Trust model unchanged: cleartext token, lab/host-only network only.

## Build (Open Watcom on host)

Needs `vendor/imports/clib.imp` and `vendor/imports/prelude.obj` (from the
Novell CLIB NDK). Refresh with:

```bat
fetch_sdk.bat
```

**NetWare 3.12 (default):**

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-netware
build.bat
build_proof.bat
```

Produces `LLMAGENT.NLM` / `HELLO.NLM` / `SOCKPING.NLM` using Novell
`prelude.obj` + **explicit** CLIB imports only (do **not** `import @clib.imp`
— that embeds the entire modern CLIB catalog and NetWare 3.12 then fails
resolving hundreds of symbols).

## If LOAD shows CODE / _TEXT / your own symbols as "missing"

Open Watcom writes each NLM import as a 255-byte (`0xFF`) padded
field. NetWare 3.12 expects classic Novell length-prefixed imports, so the
loader desyncs and prints garbage symbol names. `build.bat` /
`build_proof.bat` run `fix_nlm_imports.py` after link to rewrite the table.

(A current `LOADER.EXE` from 312PTD is still recommended, but it is not
sufficient by itself — this import rewrite is what fixes the HELLO failure.)

**NetWare 4.x+ (Watcom static RTL):** see [`nw4/README.md`](nw4/README.md)
and `nw4\build.bat` — kept separate so the 3.12 path stays clean.

## Deploy on NetWare 3.12

1. TCP/IP already up (you can ping the server).
2. Copy to `SYS:SYSTEM`:
   - `LLMAGENT.NLM`
   - `LLMAGENT.INI` (from `LLMAGENT.INI.example` — set a real `token=`)
   - optionally `UPDATE.NLM`
3. On the server console:

   ```
   LOAD LLMAGENT
   ```

   Or add `LOAD LLMAGENT` to `AUTOEXEC.NCF` after TCP/IP.
   Floppy images from `make_floppy.py` ship binaries + README only — **no INI**.
4. Bridge `machines.ini`:

   ```ini
   [netware-1]
   host = 192.168.56.30
   exec_port = 2222
   exec_token = REPLACE_WITH_UNIQUE_TOKEN
   ```

5. Host smoke (set host/token to match):

   ```bat
   set NW_HOST=192.168.56.30
   ..\bridge\.venv\Scripts\python.exe _smoke_test.py
   ```

Unload with `UNLOAD LLMAGENT` (warns if a client is connected).

## EXEC notes

NetWare has no `CMD.EXE` redirection. Prefer `PUT` of scripts/NLMs, then
`EXEC` something like `LOAD FOO` or an `.NCF` name your server understands.
Output capture may come later (console scrape); v1 is status-only.

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `update.c` / `update.lnk` | Remote swap helper (no `UNLOAD`) |
| `install_menu.py` | INSTALL menu driver (StuffKey batching + AUTOEXEC edit) |
| `build.bat` / `llm_agent.lnk` | Watcom NetWare build |
| `build_proof.bat` | Hello + sockping NLMs |
| `vendor/nwsock.h` | Minimal CLIB socket / server prototypes |
| `vendor/imports/clib.imp` | CLIB import list (from Novell NDK) |
| `fetch_sdk.bat` | Download CLIB NDK zip + refresh `clib.imp` |
| `LLMAGENT.INI.example` | Config template |
| `_smoke_test.py` | Host-side capability check |
| `_test_update.py` | Live UPDATE swap smoke |
| `make_floppy.py` | 1.44MB deploy image |
