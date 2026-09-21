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

This is a genuinely separate build, not a recompile of `../agent-os2` for a
smaller target: **OS/2 1.3 has no 32-bit kernel at all** (that arrived with
2.0), so everything here is 16-bit NE, built with Open Watcom's `os21x`
header set. There used to be a 16-bit OS/2 agent in this repo's git history
(the very first `agent-os2` commit, before it was rewritten 32-bit to get
desktop screenshots — see "Why this exists" below); this directory starts
from that same EXEC/PUT/GET/SYSINFO baseline and builds it back up to
closer parity with the current 2.x agent, not just a straight restore.

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
rollback, then relaunch it in a fresh session. Working and safe to use,
with one intermittent failure that rolls back cleanly — read
"Self-update" below.

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
  right, not just a header search. Worth revisiting once the rest of this
  agent is confirmed working live.
- `REG*`, `SHUTDOWN` — same reasons as the 2.x agent (registry doesn't
  really apply the same way pre-2.x; a clean interactive shutdown from an
  unattended service is the same unsolved problem there already documents).

## Why this exists (vs. just recompiling `../agent-os2`)

The 2.x agent's README says: *"Watcom's PM headers are 32-bit-only — `os2.h`
errors on `_M_I86`"* — and that's true of the specific header tree
(`%WATCOM%\h\os2`) that a 32-bit build points at. Confirmed directly:

```
wcc -bt=os2 -ml -zq -i=%WATCOM%\h\os2 anything.c
...\os2.h(21): Error! E1091: This os2.h is for 32-bit development only!
```

But Open Watcom ships a **second, separate 16-bit OS/2 1.x header tree**,
`%WATCOM%\h\os21x` — a real Dos*/Win*/Gpi* API surface for 16-bit OS/2, not
a stub. `PID`, `STARTDATA`, `DosStartSession`, `DosKillProcess`,
`WinGetScreenPS`, `GpiBitBlt`, `WinQuerySwitchList`, `WM_CHAR`, `BM_CLICK`
— all present, just with 16-bit-shaped types (`APIRET` doesn't exist there;
Dos calls return `USHORT`; `HAB`/`HWND`/etc. are `void far *` segmented
handles; `BITMAPINFOHEADER` is the older non-`2` GPI 1.x layout; `STARTDATA`
has no `ObjectBuffer` field; several `Win*` calls take an extra trailing
`BOOL` the 2.x versions dropped). None of that showed up in the 2.x
project's own research because it only tried the 32-bit header tree.

Everything in this directory has been **compiled and linked** against the
real `%WATCOM%\h\os21x` headers and `%WATCOM%\lib286\os2\os2.lib` import
library (link-tested with a stub socket library standing in for the real
guest `TCPIPDLL.DLL`, which isn't available on this dev machine) — so the
API usage is confirmed correct at the type/signature level. `PSLIST` and
`REBOOT` have since been confirmed live against the real os2-13 box (see
their sections above). Treat anything PM-based
(`SCREENSHOT`/`CLICK`/`KEY`/`TYPE`/`WINLIST`/`WINCLOSE`) as still
needing live verification before relying on it, the same way this
project's other agents document verified-vs-reasoned status in
[`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

## Build (Open Watcom on host)

Requires `vendor/TCPIPDLL.DLL` copied from the OS/2 1.3 guest (import lib
built automatically — see `vendor/README.md` if the guest's DLL turns out
to have a different name):

```bat
cd C:\src\retro-ssh-server\agent-os2-13
build.bat
```

Produces `llm_agent.exe` and `update.exe` (both OS/2 16-bit NE). Floppy
(needs `pyfatfs`, e.g. the `mcp-server` venv):

```bat
..\mcp-server\.venv\Scripts\python.exe make_floppy.py
```

## Deploy

1. TCP/IP up; `TCPIPDLL.DLL` on `LIBPATH`.
2. Copy `LLMAGENT.EXE`, `IORESET.EXE`, `IOSEG.DLL` (+ `UPDATE.EXE` if you
   want self-update) and `LLMAGENT.INI` to e.g. `C:\LLM\`. All of them go
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
`%USERPROFILE%\.retro-ssh-server\machines.ini`. Set `host` to the guest's
address and `exec_token` to the same unique token as `LLMAGENT.INI`'s `token=`.

## Self-update (from host) - WORKING, ONE INTERMITTENT FAILURE LEFT

Same shape as the other agents - `legacy_self_update` in
`mcp-server/server.py`, with OS/2 8.3 remote names:

```
legacy_self_update(machine="os2-13", ...,
    new_agent_name="LLMNEW.EXE", update_exe_name="UPDATE.EXE",
    target_agent_name="LLMAGENT.EXE")
```

**Status, from real live testing against os2-13, not just reasoned
about.** Two long-standing failures were root-caused and fixed; a third
problem was found underneath them and is *not* fixed.

### Fixed: `DosKillProcess` could never stop the agent

`update.exe` is a *child* of the agent (`EXECDETACH` -> `spawnv`), and
OS/2 only allows killing descendants, so killing its own parent always
failed with `ERROR_NOT_DESCENDANT` (rc=305). Replaced with a `SELFEXIT`
wire command - `update.exe` connects as an ordinary authenticated client
and asks the agent to exit itself. Confirmed working live. (This also
needs the `host=` line in `LLMAGENT.INI`: an earlier version connected to
`127.0.0.1` and hung forever, since this TCP/IP stack's loopback doesn't
work.)

### Fixed: the swap itself - it was the backup *filename*

The rename after `SELFEXIT` failed every time, and the old notes here
blamed a lingering file lock. That was wrong. The backup name was
`<target>.OLD`, i.e. `LLMAGENT.EXE.OLD`, and **Watcom's 16-bit `rename()`
rejects a second dot** - `errno=1`, on any file, locked or not, even
though the volume is HPFS and CMD.EXE's own `REN` accepts that exact
name. Isolated with `_rentest.c` on a throwaway file with no agent
involved:

```
rename(DUMMY.EXE -> DUMMY.EXE.OLD)   rc=-1 errno=1
rename(DUMMY.EXE -> DUMMY.OLD)       rc=0
```

The backup is now built by *replacing* the extension (`LLMAGENT.BAK`),
never by appending. The swap then succeeds on the **first attempt**, and
a full update cycle takes ~10s.

Two plausible-sounding theories died on the way, both recorded so nobody
re-derives them:

- **Not a timing race.** Widening the retry window from 8s to 30s changed
  nothing, and an independent observer agent renamed the same file
  successfully ~2.4s after the agent exited.
- **Not the parent/child relationship.** An `fopen`/`fclose` on the target
  immediately before the rename (which `file_exists()` does every
  iteration) makes no difference, and the fixed version works fine while
  still running as the agent's child.

The diagnosis needed an **independent observer agent** - a second copy
running from another directory on port 2223 - because `SELFEXIT` kills
the very agent you would otherwise use to watch. `PSLIST` from outside
showed the old agent leaving the process table ~5s in, which is what
ruled out the lock theory.

### Fixed: the agent ran out of file handles down the update chain

Each generation is spawned by `update.exe`, which was itself spawned by
the previous agent, and OS/2 children inherit their parent's open
handles:

```
agent -> (EXECDETACH/spawnv) update.exe -> (spawnl) new agent -> ...
```

OS/2 1.x gives a process 20 handles by default, so it ran out fast.
Measured live from a freshly booted machine: generations 1-3 healthy,
generation 4 unable to open a file at all - it could not read back the
`AGENT.PID` it had just written - and every generation after it dead on
arrival. **This degrades silently and is the nastiest failure here**: the
agent keeps answering `PING` and `SYSINFO` while `PUT`/`GET`/`EXEC` all
fail and `REBOOT` stops working too (it has to spawn `IORESET.EXE`). On
the console it shows up as `SYS1071: The handle could not be duplicated
during redirection of handle 1` - that is `EXEC`'s `CMD.EXE /C ... >
tmpfile` failing to redirect stdout. It is not a per-request leak: 125
requests across `PING`/`GET`/`PUT`/`SYSINFO`/`EXEC` on one agent left it
perfectly healthy.

`start_agent()` now uses
`DosStartSession(SSF_RELATED_INDEPENDENT, SSF_INHERTOPT_SHELL)` instead
of `spawnl`, so each generation inherits from the shell rather than from
the update chain. **Confirmed live**: the agent now survives every round,
including rounds whose swap fails.

### Fixed: duplicate agents wedging all future updates

`start_agent()` used to run unconditionally, even when the swap had
failed. Once two agents existed, `SELFEXIT` could only ever stop whichever
one held the port, while the other kept `LLMAGENT.EXE` open - so the
rename failed with a permanent `errno=6` and *every* subsequent update was
wedged. Seen live as two `LLM_AGEN` entries in `PSLIST`, and on the
console as repeated `listening on port 2222` banners stacking up in one
session.

`update.exe` now (a) waits for the agent's port to actually go quiet
before touching the binary, rather than sleeping a fixed 500ms, and
(b) only starts an agent if nothing is already listening.

### Verification status, and what is still intermittent

A real swap, verified by content rather than by mechanics: the
boot-logging agent build (md5 `bcb846...`) replaced the previous one
(`b6d7cd...`) through `legacy_self_update`, agent pid 44 -> 117, and the
installed `LLMAGENT.EXE` compared byte-identical to the new local build
afterwards.

From a clean boot, four consecutive updates through the real
`legacy_self_update`: **rounds 1-3 completely clean** - exactly one agent
throughout, `port 2222 quiet after 2 check(s)`, `renamedOld=1 after 0
attempt(s)`, agent healthy (`files=True exec_rc=0`) after each, ~10s per
round.

**Round 4 still failed** with `rename ... errno=6`, the swap skipped and
the old binary restarted - a safe failure, and the agent stayed healthy,
but it is not yet understood. Something still held `LLMAGENT.EXE` after
the port went quiet. Note `update.exe` holds *its own* binary open while
it runs, so a failed round also blocks the next round's upload of
`UPDATE.EXE` (`ERR:write failed`) until it exits - if you see that, wait
for `PSLIST` to show no `UPDATE` process and retry.

So: self-update is now **safe** (it no longer bricks the agent, and it
rolls back), and usually works, but is not yet reliable enough to fire
blindly in a loop. Check the result and retry rather than assuming
success.

### Recovering a box in this state

A handle-starved agent cannot `EXEC`, `PUT`, or `REBOOT`, so it has to be
fixed from the console: stop the agent, and run `CHKDSK C: /F` if the
volume has taken unclean resets (repeated hard resets on HPFS386 will
report `SYS0562: The system detected lost data on disk`).

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
