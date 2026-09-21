# llm_agent for NetWare 3.12+

32-bit NLM target-agent port of [`../agent-win32/llm_agent.c`](../agent-win32/llm_agent.c).
It is not an MCP server itself; it speaks the **same** token-authed TCP wire
protocol as FreeDOS/OS/2/Windows, so [`../mcp-server/server.py`](../mcp-server/server.py)
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
| `AUTOEXEC` | Check explicit startup loads and CLIBAUX-before-LLMAGENT order; safely add missing loads |
| `SCREENSHOT` | Displayed/console text cells → 24-bit BMP |
| `KEY` / `TYPE` | Via **StuffKey** when `STUFFKEY.NLM` is present (INSTALL/NWSNUT). Else CLIB `ungetch` (console only) |
| `SCREENS` | List CLIB screens (`id`, displayed flag, name) for INSTALL discovery |
| `SHUTDOWN` | `OK` then `DownFileServer(1)` (force down — lab only) |
| `UPDATE` | Prepare protocol-2 helper, then `OK` and self-exit; retained backups and startup-aware rollback |
| `DEBUG` / `DEBUG 0` / `DEBUG 1` | Runtime verbose toggle; `DEBUG 1` truncates `SYS:SYSTEM\LLMAGENT.LOG` |

Everything else returns `ERR:not supported on NetWare`.

MCP tools now expose the platform extensions: `legacy_screens`,
`legacy_autoexec`, `legacy_debug`, and `legacy_netware_self_update`.
`legacy_capabilities` returns advisory coverage from SYSINFO. See
[MCP coverage](../docs/MCP_COVERAGE.md) for arguments and limitations.

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
Copy CLIBAUX.NLM and STUFFKEY.NLM to SYS:SYSTEM using file transfer.
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

### Screenshot screen IDs and handles

The September 21, 2026 fix converts the OS IDs from `ScanScreens` to CLIB
handles with `GetScreenInfo`/`CreateScreen` before copying or testing screen
state. High-bit NetWare 4 IDs are valid even when represented as negative
integers. The old code passed IDs directly, captured blank private-screen
cells and counted their spaces as successful content.

Capture prefers Install's existing StuffKey dump path, then a displayed
screen, then the system console. Blank or failed copies cannot win through
a priority bonus. It restores the calling thread's previous I/O context,
does not display a different screen, and does not probe arbitrary numeric
IDs. Live agent-produced screenshots now pass on NetWare 3.12 and 4.11.

See Novell's [ScanScreens](https://www.novell.com/documentation/developer/clib/ndev_enu/data/sdk1289.html)
and [GetScreenInfo](https://www.novell.com/documentation/developer/clib/ndev_enu/data/sdk1247.html)
contracts. SCREENS reports displayed state only when the CLIB query returns
1; an error is not interpreted as an active screen.

Listener initialization now calls `listen` before enabling nonblocking
accept polling, and reports its errno on failure. During validation, a
3.12 replacement and rollback both failed to listen until a normal server
shutdown/restart. The previous binary/configuration and all recovery records
were preserved; the subsequent update with the revised initialization
verified successfully. A failed listen/rollback still requires operator
recovery; do not assume that retrying UPDATE repairs the network stack.

### Persist across reboot

```text
AUTOEXEC
```

The command parses complete lines in `SYS:SYSTEM\AUTOEXEC.NCF`, rather than
searching for module names anywhere in the file. Only explicit, unconditional
`LOAD` commands with an exact CLIBAUX or LLMAGENT module name count. Matching
ignores case and accepts an optional `.NLM` extension, a volume/directory path,
or a quoted module path. Leading whitespace and `REM`, `#`, and `;` comment
lines are handled; comments and similarly named modules do not count.

If both loads are missing, it appends CLIBAUX followed by LLMAGENT. If only
LLMAGENT is missing, it appends that load. If only CLIBAUX is missing, it
inserts its load immediately before the existing LLMAGENT line. Existing
bytes, arguments, whitespace, and line endings are preserved; inserted lines
use CRLF, before a terminal DOS EOF byte if present.

Replies are `OK autoexec=present`, `OK autoexec=added`,
`OK autoexec=added-llmagent`, or `OK autoexec=added-clibaux`.
Reversed dependency order, duplicate loads, relevant UNLOAD commands, optional
`?` loads, implicit module commands, and recognized ambiguous LOAD wrappers
return `ERR` for manual review without changing the file. A directly listed
`LOAD TCPIP` or `BIND IP` after LLMAGENT also returns an ordering error.

Before editing, the agent reads the complete file and rejects read/close
errors, embedded control bytes, malformed parsed commands, and input or output
larger than 8,191 bytes. It writes `SYS:SYSTEM\LLMAUTO.NEW`, checks write,
flush, close, and byte-for-byte readback, then renames the original to
`SYS:SYSTEM\LLMAUTO.BAK` and installs the staged file. Installation failure
attempts to restore the original; a failed restore reports the backup path for
local recovery. The successful backup is retained. Existing recovery files
block a **new edit** until reviewed and moved aside; an already-correct file
still returns `present` without writing anything.

This checks the supported commands in this file; it does not execute or follow
other NCF files, interpret arbitrary control flow, check installed module
versions, or prove that TCP/IP will work on the next boot. In particular,
network setup delegated to INETCFG/another NCF is outside this check. Changes
assume no concurrent external editor. The backup-and-rename sequence provides
recovery from reported I/O errors, not an atomic transaction across power loss.

Syntax references: Novell's [NCF command ordering and optional commands](https://www.novell.com/documentation/nw6p/sos__enu/data/hxcnlm3b.html)
and [comment markers](https://support.novell.com/techcenter/articles/ann20000301.html).

### Remote update

```text
Upload current protocol-2 UPDATE.NLM to SYS:SYSTEM\UPDATE.NLM.
Upload the new agent build to SYS:SYSTEM\LLMAGENT.NEW.
UPDATE                         (agent command)
```

Prefer agent `UPDATE` (self-exit) over console `UNLOAD LLMAGENT` — the latter
has abended on this 3.12 lab box.
Console `LOAD SYS:SYSTEM\UPDATE.NLM` is also available when the agent is
already stopped. It does not stop a running agent; it waits and aborts if the
old module remains loaded.

Use a current **protocol-2 `UPDATE.NLM`**, built alongside the agent. The
bridge and current agent reject an older helper before launching it. The
embedded `RETRO_NW_UPDATE_PROTOCOL_2` marker identifies compatibility; it is
not a signature or authenticity check. During migration, an older agent can
still hand off to the new helper and a new agent.

`legacy_netware_self_update(machine, new_agent_local_path, update_nlm_local_path)`
freezes both inputs, verifies both staging readbacks, and waits by default for
a new startup instance, matching startup SHA-256, and installed NLM readback.
Set `wait_for_agent=False` for acceptance only. SYSINFO's loader-provided
`agent_exe`, immutable `agent_sha256`, and uptime/NLM-ID `agent_started`
fingerprint the startup file, not live relocated code or a publisher signature.
A different installation path fails preflight: the updater targets SYS:SYSTEM.

The agent launches the helper while keeping its listener available. It checks
the helper's preparation record against the current and staged fingerprints
before replying OK and self-exiting. Missing/incompatible helpers or failed
preparation leave the original agent running. A lost acknowledgment does not
cause an automatic retry. The helper waits for the outgoing NLM to leave the
loaded-module list, rechecks both files, then moves the installed executable
to its reserved backup before installing the staged NLM.

The replacement writes a checked readiness receipt after loading a nonempty
token and successfully opening its listener. The helper checks that receipt
against the expected startup hash and confirms the NLM is loaded. The bridge
still independently verifies authentication, instance, hash, and installed
bytes; a readiness receipt alone is not end-to-end connectivity verification.
The preparation wait is about eight seconds and each module-exit/readiness
wait about 30 seconds, measured with CLIB ticks. Blocking filesystem/loader
calls can exceed these bounds; these are not watchdogs for a hung server.

If the candidate exits or fails to load, the helper preserves it as `BAD.NLM`,
verifies a separate restoration copy, restores the previous executable, and
loads it. `OLD.NLM` is retained even after rollback. A matching readiness
receipt confirms the restored agent. If the candidate remains loaded without
readiness, or a required recovery operation fails, the helper stops with
recovery required. It never UNLOADs a candidate or renames a known loaded
module. An older restored agent without the receipt protocol can be reachable
but still require manual review.

### Recovery files and status

Each attempt reserves `SYS:SYSTEM\LLMUPD` exclusively. A preexisting directory
blocks another attempt; it is never overwritten or automatically removed.
The directory contains:

- `PLAN.TXT`: target, backup/archive locations, and old/new SHA-256 values.
- `LOG.TXT`: progress and failure messages; also written to the console.
- `OLD.NLM`: retained previous executable after the first successful rename.
- `BAD.NLM`: rejected installed candidate, when it could safely be moved.
- `RESTORE.NLM`: restoration copy if recovery was interrupted or failed.
- `EXPECT.TXT`, `PREPARED.TXT`, `READY.TXT`: bounded preparation/startup records.
- `RESULT.TXT`: final outcome when that record could be written.

Completed updates, confirmed rollbacks, and pre-swap aborts move this directory
to `SYS:SYSTEM\LUxxxxxx\RESULT`, using a newly reserved archive directory.
No existing archive or legacy `LLMAGENT.OLD` is deleted. Archives accumulate;
review and prune them manually when their backups are no longer needed.
`SYS:SYSTEM\LLMUPD.RES` records the latest outcome, intended new hash, and
archive path. If final archival fails, the live `LLMUPD` directory remains
and is authoritative even if that pointer names the intended destination.

SYSINFO advertises `update_protocol=2`, `update_state=idle|busy|recovery-required`,
and, when available, `update_last`. The bridge refuses staging while recovery
is unresolved, reports a confirmed rollback explicitly, and distinguishes a
verified executable from incomplete helper cleanup. A raw OK or PING is not
proof of successful replacement.

### Manual recovery after interruption

1. Inspect the console and `update_state`. Let an active helper finish. Preserve
   `LLMUPD`, its logs, and any archive named by `PLAN.TXT`/`update_last` before
   changing files. Do not remove the directory merely to bypass the guard.
2. Establish whether LLMAGENT is still loaded. If a candidate is loaded but
   unusable, arrange a controlled server shutdown/restart through the normal
   operator procedure; do not use console UNLOAD to force this agent out.
3. With the helper and agent stopped, use a NetWare client or file-maintenance
   utility to preserve any current candidate and restore a **copy** of the
   verified `OLD.NLM` to `SYS:SYSTEM\LLMAGENT.NLM`. Compare its SHA-256 with
   `old_sha256` in `PLAN.TXT`. Keep the original backup and configuration.
4. Archive the entire unresolved `LLMUPD` directory under a new unused name
   once the file state is understood. Then `LOAD SYS:SYSTEM\LLMAGENT.NLM`
   and verify SYSINFO, authentication, installed bytes, and configuration.
   Restoring a file alone does not establish that the right agent is running.

Checked writes, flushes, closes, and readbacks handle reported I/O errors;
this is not an atomic transaction across power loss or an NLM-induced kernel
abend. No automatic garbage collection or forced recovery from a loaded,
unresponsive candidate is provided.


Trust model unchanged: cleartext token, lab/host-only network only.

## Build (Open Watcom on host)

Needs locally supplied `clib.imp` and `prelude.obj` from the known Novell
CLIB NDK snapshot. From this directory, prepare an extracted SDK you are
entitled to use:

```bat
fetch_sdk.bat "C:\SDKs\novell-clib-devel-2007.10.02-1netware_windows"
```

Despite its historical name, `fetch_sdk.bat` no longer downloads anything.
It checks hashes and stages only these two files in `../.deps/netware-sdk`.
Set `NLM_SDK_DIR` to use a different destination; all three build scripts
check the inputs before compiling. Host Python 3 is required. See
[licensing and provenance](../THIRD_PARTY.md) for the manifest, original
package identification, and binary-release restrictions. Guest patch
installers, CLIBAUX, and StuffKey are not included in this source repository.

**NetWare 3.12 (default):**

```bat
cd C:\src\RetroBridge\agent-netware
build.bat
build_proof.bat
```

Produces `LLMAGENT.NLM` / `UPDATE.NLM` / `HELLO.NLM` / `SOCKPING.NLM` using Novell
`prelude.obj` + **explicit** CLIB imports only (do **not** `import @clib.imp`
— that embeds the entire modern CLIB catalog and NetWare 3.12 then fails
resolving hundreds of symbols).

## If LOAD shows CODE / _TEXT / your own symbols as "missing"

Open Watcom writes each NLM import as a 255-byte (`0xFF`) padded
field. NetWare 3.12 and 4.11 expect classic Novell length-prefixed imports, so the
loader desyncs and prints garbage symbol names. `build.bat` /
`build_proof.bat` and `nw4\build.bat` run `fix_nlm_imports.py` after link to rewrite the table.

(A current `LOADER.EXE` from 312PTD is still recommended, but it is not
sufficient by itself — this import rewrite is what fixes the HELLO failure.)

**NetWare 4.x+ (Novell CLIB):** see [`nw4/README.md`](nw4/README.md)
and `nw4\build.bat` — kept separate so the 3.12 path stays clean.
The 4.x build uses Novell CLIB throughout and normalizes the import table.
Live loading, PING, SYSINFO, staged UPDATE, and binary readback passed on 4.11.

## Deploy on NetWare 3.12

For CD deployment, the [all-agent ISO builder](../docs/BUILD_ISO.md) includes
both NetWare variants, `UPDATE.NLM`, and direct-CD loading instructions.

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
   The optional `make_floppy.py` builder requires host `pyfatfs` and copies
   `LLMAGENT.INI.example` as `LLMAGENT.INI` when present. This contains a
   placeholder, not a usable private configuration: set a unique token before
   loading. It can also include locally supplied StuffKey/CLIBAUX modules;
   generated lab media is not an approved public binary bundle.
4. Bridge `machines.ini`:

   ```ini
   [netware-1]
   host = 192.168.56.30
   exec_port = 2222
   exec_token = REPLACE_WITH_UNIQUE_TOKEN
   ```

5. Host smoke: set `NW_HOST` and `NW_TOKEN` privately to the actual host and
   token before running; the script's default token is only a placeholder.
   For example, after supplying `NW_TOKEN` in your local environment:

   ```bat
   set NW_HOST=192.168.56.30
   ..\mcp-server\.venv\Scripts\python.exe _smoke_test.py
   ```

Console `UNLOAD LLMAGENT` has caused abends on the tested 3.12 guest;
it is not a verified safe stop path. For replacement, use the staged
`UPDATE` flow described above.

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
| `../.deps/netware-sdk/clib.imp` | Locally supplied, verified CLIB import catalog (Novell NDK) |
| `fetch_sdk.bat` | Verify and stage a locally supplied CLIB SDK; no download |
| `LLMAGENT.INI.example` | Config template |
| `_smoke_test.py` | Host-side capability check |
| `_test_update.py` | Live UPDATE swap smoke |
| `make_floppy.py` | 1.44MB deploy image |
