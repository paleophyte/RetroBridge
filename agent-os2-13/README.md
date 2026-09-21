# llm_agent for OS/2 1.3

**16-bit OS/2 1.x target-agent port** of
[`../agent-win32/llm_agent.c`](../agent-win32/llm_agent.c), sibling to
[`../agent-os2`](../agent-os2) (OS/2 2.11, 32-bit). It is not an MCP server
itself; it speaks the **same** token-authed TCP wire protocol, so
[`../mcp-server/server.py`](../mcp-server/server.py) can expose it through the repo's
`legacy_*` MCP tools with no protocol fork.

`legacy_winclose(machine, title)` exposes the native WINCLOSE operation:
close/cancel requests to all exact title matches, ignoring case. Apps may
prompt or refuse; acknowledgment does not prove closure. See
[MCP coverage](../docs/MCP_COVERAGE.md) and `legacy_capabilities` for limits.

OS/2 1.3 predates the 32-bit kernel, so this is a separate 16-bit NE build
using Open Watcom's `os21x` headers and the guest's TCPIPDLL. The 32-bit
port uses a different header tree, executable format, and socket ABI.

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `EXEC` | `CMD.EXE /C` with stdout redirected to a temp file → `LEN:`/`EXIT:` |
| `EXECDETACH` | `spawnv(P_NOWAIT, ...)` — returns immediately with the real child PID |
| `PUT` / `GET` | File transfer |
| `SYSINFO` | `os_family=os2`, `os2_major`/`os2_minor` (`DosGetVersion`), C: disk space |
| `SCREENSHOT` | Full PM desktop via `WinGetScreenPS` → 24-bit BMP |
| `CLICK` | PM `WinSetPointerPos` + `BM_CLICK` / button up-down (top-left coords) |
| `KEY` / `TYPE` | `WM_CHAR`/`WM_VIOCHAR` to the focus window (same keyspec grammar as Windows/2.x) |
| `WINCLOSE` | Close top-level frame(s) by title |
| `WINLIST` | Switch-list entries (titles + top-left frame rects for `CLICK`) |
| `PSLIST` | Runs `C:\OS2\PSTAT.EXE` and parses its process/thread table — see below, no kernel API needed |
| `PSKILL` | `DosKillProcess` — works if you already have a PID (e.g. from `EXECDETACH`'s or `PSLIST`'s reply) |
| `REBOOT` | Detached `IORESET.EXE`: `DosShutdown` then an 8042 pulse reset issued from a ring-2 I/O privilege segment — **confirmed working live**, see below |

Self-update: `update.exe` (same role as `../agent-win32/update.c` /
`../agent-os2/update.c`) — stop the old agent with a `SELFEXIT` request
over the wire (`DosKillProcess` can't: see below), swap the binary with
rollback, then relaunch it in a fresh session. Verified replacements have
passed, but an earlier intermittent rename failure remains unexplained;
read [self-update and recovery](#self-update-from-host) before deploying.

### AGTBOOT.LOG — why the agent started or stopped

The agent appends a timestamped line to `AGTBOOT.LOG`, next to the `.EXE`,
for every start, every listen, and every reason it stops.

This exists because the agent's own messages go to its session window and
that window dies with the process, so an agent that started and then
vanished left **no evidence whatsoever**. That cost real time here: an
agent seen exiting right after printing `listening on port 2222` could
equally have been a failed `bind`, a dead TCP stack, a crash, or a
perfectly ordinary `SELFEXIT` from `update.exe` — and there was no way to
tell them apart from the console. (It was the last one.) Same reasoning,
and same fix, as the Windows 9x agent's `agent_boot.log`.

A reboot now reads straight off the log:

```
2026-09-05 22:18:41 === starting: exedir=C:\llmagent port=2222 ===
2026-09-05 22:18:41 LISTENING on port 2222, pid=117, token=yes
2026-09-05 22:18:58 REBOOT requested - handing off to IORESET.EXE
2026-09-05 22:19:46 === starting: exedir=C:\llmagent port=2222 ===
2026-09-05 22:19:46 LISTENING on port 2222, pid=18, token=yes
```

Distinguishable exits: `SELFEXIT requested by a client`, `bind(...) failed
after N attempts` (names the likely cause — another agent already
running), `sock_init failed`, `listen() failed`, and `REBOOT FAILED:
could not spawn IORESET.EXE` (which is how a handle-starved agent fails).
The file is opened and closed per line — this agent lives inside OS/2 1.x's
20-handle budget — and restarts itself past 32KB so a crash-looping agent
can't fill the disk.

### PSLIST, via PSTAT.EXE

The 2.x agent's process enumeration (`DosQProcStatus`/`DOSCALLS.154`) is
itself an *undocumented* 2.x-era API — it doesn't appear in Watcom's
16-bit OS/2 1.x header set at all, and no kernel-level equivalent was
found. But OS/2 1.3 ships a real, general-purpose diagnostic utility that
solves the same problem from userspace: `C:\OS2\PSTAT.EXE` (confirmed
present on the real os2-13 box, dated 11-14-91 — original OS/2 1.3
media). `PSLIST` shells out to it (`CMD.EXE /C` + temp-file redirect,
same pattern as `EXEC`) and parses its process/thread table.

**Verification status**: confirmed working live end-to-end against the
real os2-13 box, via `mcp-server/agent_client.py`'s `pslist()` (13-14 real
processes each run, including the agent's own — `LLM_AGEN` — reported
correctly). The parser was also validated offline first against a real
captured `PSTAT.EXE` run before ever being wired into the agent.

### REBOOT, via a ring-2 I/O privilege segment

`REBOOT` spawns `IORESET.EXE` detached (same shape as the 2.x agent's
`REBOOT.EXE`). Files: `IORESET.EXE` + `IOSEG.DLL`, both next to
`LLMAGENT.EXE`. Needs `IOPL=YES` in `CONFIG.SYS` — the OS/2 1.3 default,
already present on the os2-13 box.

The mechanism is unremarkable: `out 0xFE` to port `0x64`, the 8042's
pulse-reset line, which is exactly what a PC/AT does for Ctrl-Alt-Del.
What took the work was being allowed to execute it. OS/2 1.x runs
applications at ring 3 with IOPL 0, so a plain `IN`/`OUT` faults. The
documented 1.x escape hatch is an **I/O privilege segment**, and it needs
four things to line up, all of which are in `ioseg.c` / `ioreset.c` /
`build.bat`:

1. `IOPL=YES` in `CONFIG.SYS`.
2. The port-I/O routines in a code segment of their own (`wcc -nt=IOSEG`)
   that the linker marks IOPL — and *only* that segment, so the C runtime
   stays at ring 3 (`wlink segment 'IOSEG' iopl preload`).
3. Each entry exported with a parameter **byte** count, so the linker
   builds a ring-2 call gate that copies that many bytes from the ring-3
   stack to the ring-2 stack. This is why every entry is `__pascal`
   instead of Watcom's default register convention: a call gate can only
   copy stack parameters, and its callee-cleans-up `RETF n` is exactly
   what a gate return wants.
4. `DosPortAccess()` per process, per port range, before any `IN`/`OUT`.

`IOSEG.DLL` is loaded at runtime with `DosLoadModule` by full path, so it
can sit beside the `.EXE` rather than having to be installed on `LIBPATH`.
Confirmed live that `DosGetProcAddr` really does hand back a call gate for
an IOPL export and not a plain far address — it returns `selector:0000`,
and calling through it from ring 3 executes the `IN` without faulting.

**Why the previous implementation never worked.** It tried four
mechanisms — 8042 pulse reset, Ctrl-Alt-Del scancode injection, BIOS
warm-boot vector jump, `0xCF9` chipset reset — as real-mode DOS `.COM`
stubs spawned into a DOS box, plus `OEMHLP$`/`DOS$` IOCTLs ported from the
2.x agent. This machine's `CONFIG.SYS` has **`PROTECTONLY=YES`**, so it
has no DOS box at all: confirmed live, any DOS binary, `.COM` or
`COMMAND.COM` itself, fails to start. Three of the four never executed a
single instruction, which is why four genuinely different mechanisms all
"failed" identically — and `spawnl`'s return code was discarded, so
nothing ever said so. The IOCTLs did run and did nothing; `OEMHLP$` is a
2.x device.

**`DosShutdown` is not optional here.** This box runs HPFS386 with a
~4.9MB lazy-write cache. Resetting without it left the volume dirty enough
for `AUTOCHECK` to run `CHKDSK` on every boot, and once silently shredded
a file that had been `PUT` seconds earlier — right length, 16,972 bytes of
garbage. `DosBufReset` alone is *not* enough: it flushes file buffers,
which is a layer above the HPFS386 cache, and it does not clear the
dirty-volume flag. `CACHE.EXE /LAZY:OFF` prevents the data loss but still
leaves the volume dirty, so `CHKDSK` still runs. Only `DosShutdown`
produces a clean boot.

Two ordering constraints come with it, both learned by wedging the box:

- **Nothing may touch a file between `DosShutdown` and the reset.**
  `DosShutdown` leaves the filesystem read-only, so a single log write
  after it blocks forever — the machine sits quiesced and hung, agent
  included, and never resets.
- **The IOPL segment must already be resident.** It is `preload`ed *and*
  called once before the shutdown (`warm_gate`), because a demand-load or
  swap-in on the first call needs disk I/O that no longer exists.

`IORESET.EXE` also arms a **watchdog** first: a second copy of itself, in
its own process, that resets unconditionally 25 seconds later. That
degrades the worst case from "hung until someone presses Ctrl-Alt-Del at
the console" to "resets a bit late with the volume still dirty". A
separate process rather than a thread, deliberately — the first version
used `DosCreateThread` and the process died on the spot, before
`DosShutdown` was even reached, against Watcom's single-threaded 16-bit
OS/2 runtime.

**Verification status**: confirmed live against the real os2-13 box, end
to end through the wire protocol — `REBOOT` returns `OK`, the box goes
down ~20s later and the `STARTUP.CMD` agent answers again ~60s after the
command, with no `CHKDSK`. Six reboots over the course of getting there.

Everything else returns `ERR:not supported on OS/2 1.3`:

- **`CLIPSET`** — 16-bit PM's `WinSetClipbrdData` clipboard convention
  needs a *giveable* real-mode-style segment (`DosAllocSeg(..., SEG_GIVEABLE)`
  + `DosGiveSeg`, `CFI_HANDLE`/`CFI_SELECTOR` instead of 2.x's flat-memory
  `CFI_POINTER`/`DosAllocSharedMem`). Deliberately left unimplemented
  rather than shipped unverified — this needs a real OS/2 1.3 box to get
  right, not just a header search. A working implementation would need separate live cross-application tests.
- `REG*`, `SHUTDOWN` — same reasons as the 2.x agent (registry doesn't
  really apply the same way pre-2.x; a clean interactive shutdown from an
  unattended service is the same unsolved problem there already documents).

## Why a separate 16-bit build

Open Watcom supplies the 1.x Dos/Win/Gpi APIs under `%WATCOM%\h\os21x`.
The `%WATCOM%\h\os2` tree used by the 32-bit port is not suitable for this
build. Segmented handles, the older BITMAPINFOHEADER layout, calling
conventions, and TCPIPDLL's ABI differ from the 32-bit interfaces.

Native builds and live tests cover file/command operations, PM screenshots,
input/window queries, PSTAT process listing, reboot, and verified updates on
the configured OS/2 1.3 guest. This is not certification of other TCP/IP
packages or hardware. Clipboard remains unimplemented.

## Build (Open Watcom on host)

Requires `vendor/TCPIPDLL.DLL` copied from the OS/2 1.3 guest (import lib
built automatically — see `vendor/README.md` if the guest's DLL turns out
to have a different name):

```bat
cd C:\src\RetroBridge\agent-os2-13
build.bat
```

Produces `llm_agent.exe`, `update.exe`, `ioreset.exe`, `ioseg.dll`, and
`jobrun.exe` (OS/2 16-bit). Floppy
(needs `pyfatfs`, e.g. the `mcp-server` venv):

```bat
..\mcp-server\.venv\Scripts\python.exe make_floppy.py
```

The current floppy builder includes available agent/update/reboot binaries
but omits JOBRUN.EXE. Transfer that companion separately before using jobs,
and check the image's actual file list rather than assuming every build exists.

## Deploy

1. TCP/IP up; `TCPIPDLL.DLL` on `LIBPATH`.
2. Copy `LLMAGENT.EXE`, `JOBRUN.EXE`, `IORESET.EXE`, `IOSEG.DLL` (+
   `UPDATE.EXE` for self-update) and a configured `LLMAGENT.INI` from the
   example to e.g. `C:\LLM\`. Choose a unique token. All of them go
   in the *same* directory — `IOSEG.DLL` is loaded by path, so it does not
   need to be on `LIBPATH`, and the agent finds `IORESET.EXE` next to
   itself.
3. Run `LLMAGENT.EXE` from an OS/2 window (or `STARTUP.CMD`).

`REBOOT` additionally needs `IOPL=YES` in `CONFIG.SYS` (the OS/2 1.3
default). Nothing else does.

Note that a running `LLMAGENT.EXE` cannot be overwritten in place — `COPY`
reports `0 file(s) copied`. Swap it while it is stopped, or in the window
between the reset and `STARTUP.CMD` starting it.

Add an `[os2-13]` section to your private `machines.ini`, for example at
`%USERPROFILE%\.retrobridge\machines.ini`. Set `host` to the guest's
address and `exec_token` to the same unique token as `LLMAGENT.INI`'s `token=`.

## Self-update (from host)

Use `legacy_self_update` with `remote_dir` set to the actual installation
and 8.3 filenames: `new_agent_name="LLMNEW.EXE"`,
`update_exe_name="UPDATE.EXE"`, `target_agent_name="LLMAGENT.EXE"`.
The bridge checks staged bytes and verifies a new startup identity and matching
installed executable. A reachable old binary is not a successful update.

The helper needs `host=` in `LLMAGENT.INI`, set to the guest's own reachable
LAN address, plus the matching `port=` and `token=`. The tested legacy TCP/IP
stack did not provide working loopback. This host field is for the updater's
authenticated SELFEXIT request; it is not the controller's address. Changing
the INI token while the old process still uses another token will prevent
that request from authenticating.

Implementation constraints established by live debugging:

- The helper cannot kill its parent with DosKillProcess; it asks the agent
  to SELFEXIT and waits for the listening port to go quiet.
- The backup replaces the extension (`LLMAGENT.BAK`). Appending a second
  extension failed with this 16-bit runtime's rename(), even on HPFS.
- The replacement starts with an independent DosStartSession inheriting
  shell handles, avoiding exhaustion down a chain of parent/child updates.
- Relaunch checks avoid starting a duplicate listening agent after a failed swap.

Successful swaps, byte readback, normal operations afterward, and later
startup-hash verification passed on the guest. An earlier four-update series
had three clean swaps and one rename failure (`errno=6`); the old binary
restarted and stayed usable. The remaining file holder was not identified.
Later successes do not establish that this intermittent condition is eliminated.

After an unverified result, inspect `UPDATE.LOG`, `AGTBOOT.LOG`, and the process
list. Let the existing helper exit before attempting another upload of
UPDATE.EXE; a still-running helper can keep its own file open. Verify which
agent and bytes are active before deciding whether to retry. Do not run an
unattended retry loop or overwrite a running executable.

If the agent cannot perform file/EXEC operations, recover from the console:
stop the agent/helper, preserve logs and current files, restore a known-good
executable and configuration, and launch one instance from a normal session.
A host-visible PING alone does not establish that its file handles are healthy.
Use the OS's normal filesystem recovery procedure after an unclean reset.

## Trust model

Same as every other agent in this repo: cleartext pre-shared token,
isolated lab/host-only network only. See
[`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) "Trust model".

## EXEC lifetime follow-up

EXEC now uses an invocation-specific `LXxxxxxx` output directory. Files left
by a timed-out or interrupted invocation are retained for recovery and never
reused by a later EXEC. Missing output does not cause automatic reexecution.
See [long-running commands](../docs/LONG_RUNNING_COMMANDS.md) for wait/status
semantics, retained-file cleanup, and live verification.

## Tracked command jobs

This build advertises `exec_jobs=1`. Deploy the 16-bit OS/2 `JOBRUN.EXE`
companion built by `build.bat` alongside the agent. Two shell jobs can run
independently, with marker-based completion and observed shell exit status.
Cancellation is unsupported; disk output is not capped, while retrieval
exposes at most the first 1 MiB. See [job lifecycle and limits](../docs/LONG_RUNNING_COMMANDS.md).
