# llm_agent for classic Mac OS (System 7.x)

**68k Mac OS over MacTCP** target-agent port. It is not an MCP server
itself; it speaks the **same** token-authed TCP wire protocol as the
other ports, so [`../mcp-server/server.py`](../mcp-server/server.py) can
expose it through the repo's `legacy_*` MCP tools with no protocol fork.

Uses **MacTCP** (driver-style `PBControlSync`/`PBControlAsync` on a
`TCPiopb`, not Berkeley sockets) for networking. Built with
[Retro68](https://github.com/autc04/Retro68) targeting plain 68000 (runs
under any 68k Mac, including emulators like `qemu-system-m68k -M q800`).
Guest needs MacTCP configured and working (Control Panel shows an IP).

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `SYSINFO` | `os_family=mac68k`, `os_version` (Gestalt, hex BCD-ish), `machine_gestalt`, `free_mem_kb`, `agent` |
| `GET` / `PUT` | File transfer (data fork; plain `fopen`/`fread`/`fwrite`) |
| `SCREENSHOT` | Full screen via `CopyBits` into an offscreen `GWorld` → 24-bit BMP (any color depth, indexed or direct) |
| `QUITAGENT` | Terminates the agent process itself (see below) |
| `PSLIST` | Live Process Manager process list (`GetNextProcess`/`GetProcessInformation`) |
| `UPDATE` | Self-update with **no user interaction** (see below) |
| `MOUSEPOS` | Not part of the shared protocol — reports current cursor position (`LMGetMTemp()`) and button state (`Button()`) |

`EXEC`/`EXECDETACH` are **not implemented** — classic Mac OS has no
command shell (no `COMMAND.COM`/`CMD.EXE` equivalent). `WINLIST`/`REG*`
are also not implemented (no analogous concept on classic Mac OS).
`CLICK`/`DBLCLICK`/`KEY`/`TYPE` are **not implemented, deliberately** —
this isn't an oversight or a "not gotten to yet"; see "Mouse and
keyboard automation" below for why, and what was actually proven to
work if picking this back up. All unimplemented commands return
`ERR:unknown command`.

The agent runs with **no console and no windows at all** (see
`llm_agent.r`) — not minimized, not backgrounded-with-a-window, just no
UI whatsoever. There is no Finder menu, no dock, no window to close it
from, so `QUITAGENT` is the only way to stop it short of rebooting the
machine, and `UPDATE`/`llm_updater` is the only way to replace it short
of decoding a new build by hand.

## Two binaries: `llm_agent` and `llm_updater`

`llm_updater` is a small companion app with one job: turn a staged
MacBinary-encoded build into a running `llm_agent`, with no GUI
interaction. It must be deployed once, by hand, the same way `llm_agent`
itself is (see Deploy below) — after that, `UPDATE` never needs it
touched again.

### Why a separate app, and why MacBinary decoding is built into it

Classic Mac files are two forks (data + resource). Retro68's own build
output for an app, and what `hcopy -m` produces when extracting one from
a disk image, is a flat MacBinary-encoded blob — the file's forks,
type/creator, and Finder flags packed into one byte stream. Turning that
back into a real, launchable dual-fork file traditionally means dragging
it onto StuffIt Expander by hand. `llm_updater` does that decode itself
(`ParseMacBinaryHeader`/`CopyForkFromMacBinary` in `llm_updater.c`,
offsets taken directly from `hfsutils`' `copyin.c`/`copyout.c` — the
actual encoder/decoder `hcopy -m` uses, not the MacBinary spec from
memory), which is what makes hands-off self-update possible at all.

It has to be a separate process because `llm_agent` can't safely replace
its own file while it's still running (Retro68 apps aren't guaranteed
single-segment, so code can still be loaded from disk on demand), and
because of a real, confirmed race: MacTCP driver state for the agent's
stream isn't released the instant the process exits, so `llm_updater`
waits ~10 seconds — cooperatively, via `WaitNextEvent`, not a raw `Delay`
— before touching the old file. A raw `Delay()` there froze the whole
machine for the duration (confirmed live); a retry loop around the
delete was tried and reverted too, following the exact lesson
`agent-win16/restart.c` already learned the hard way: actively polling a
process's state while it's mid-teardown is itself a source of crashes on
a cooperatively-scheduled system, not just an inefficiency.

### `QUITAGENT` releases the MacTCP stream

`QUITAGENT` calls `TCPStreamAbortAndRelease()` before `ExitToShell()`.
Skipping this was a real, reproducible bug: the process list
(`PSLIST`) always looked clean afterward, but the *next* agent's own
`TCPStreamCreate`/`TCPListen` would hang indefinitely — with no timeout,
so no amount of waiting before relaunching ever helped — because
MacTCP's driver-level control block for the port was orphaned, not
released, by a quit that skipped straight to `ExitToShell()`.

### `UPDATE` wire format (Mac-specific — not the shared no-arg `UPDATE`)

`agent_client.py`'s generic `update()` sends a bare `UPDATE\n` and
expects the platform's own out-of-band helper to already have a new
binary staged. Classic Mac has no `EXEC` for a bridge to drive a helper
directly, so the whole hand-off has to happen inside the agent itself:

```
client -> server: UPDATE <size>\n
client -> server: <size> raw bytes (MacBinary-encoded new llm_agent,
                   the same encoding `hcopy -m` produces)
server -> client: "OK\n" | "ERR:<msg>\n"
```

On `OK`, the agent has already staged the bytes as `STAGED_AGENT.bin`,
confirmed `llm_updater` exists next to it, launched it via
`LaunchApplication`, released its own MacTCP stream, and called
`ExitToShell()` — all before the reply is sent back. Not yet wired into
`agent_client.py`'s generic `update()`/`legacy_self_update` bridge tool;
driving it today means opening the socket directly (see
`mcp-server/agent_client.py`'s wire-protocol docstring for the frame
shapes GET/PUT already use, which UPDATE's staging step follows).

## Build (Retro68 cross-toolchain, Linux host)

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
make
```

Produces `llm_agent.bin` / `llm_agent.APPL` / `llm_agent.dsk` and the
same set for `llm_updater`. `.bin` is the MacBinary-encoded form used
for transfer (CD-ROM or `PUT`/`UPDATE`); `.dsk` is a flat HFS image
`hfsutils`' `hmount`/`hcopy -m` can pull a `.bin` out of directly.

`vendor/MacTCP.h` and `vendor/AddressXlation.h` are vendored from a real
MacTCP 2.0.6 / Universal Interfaces 2.1 installation (Apple, 1995) —
needed because Retro68's own "Multiversal" header reimplementation
deliberately excludes MacTCP.h for licensing reasons. `vendor/AppleTalk.h`
is a small stub written for this project (MacTCP.h only needs the
`AddrBlock` type from Apple's real one). See `vendor/README.md`.

## Deploy

First deploy has no shortcut — get `llm_agent` and `llm_updater` onto
the guest once, by hand:

1. Transfer both `.bin` files to the guest. QEMU's SWIM floppy
   controller for `-M q800` is a non-functional stub — use a plain
   ISO9660/Joliet CD-ROM image (`genisoimage -J -r`) instead.
2. Decode each `.bin` with StuffIt Expander (drag onto it).
3. Copy `LLMAGENT.INI.example` to `LLMAGENT.INI` next to `llm_agent`,
   set a real `token=`.
4. Double-click `llm_agent` to launch it.

From then on, updates go through `UPDATE` — see above — with `llm_agent`
and `llm_updater` never touched by hand again unless `llm_updater`
itself needs a new build.

## Mouse and keyboard automation

`CLICK`/`DBLCLICK`/`KEY`/`TYPE` are not implemented. This was
investigated at length (an entire session), including a real
ground-truth check against QuicKeys 3.5.3 (the actual commercial
automation tool this era's technique is modeled on) running in the same
guest. **This conclusion was WRONG and has been superseded.** A later session
watchpointed the low-level event queue while a real QuicKeys click fired
and caught the mechanism directly: QuicKeys writes the target point into
`MTemp`/`RawMouse`/`Mouse`, then `Enqueue()`s a `mouseDown` `EvQEl`
whose `evtQWhere` is that same point, then a matching `mouseUp` about two
ticks later. `MBState` is never written and journaling is never used.
Synthetic clicks **are** possible; see
`QUICKEYS_CLICK_INVESTIGATION.md` for the evidence and the recipe.

The failures below are still accurate as records of what does not work on
its own -- in particular technique 4 failed because it enqueued an event
*without* first placing the cursor, which is the step that makes the
Toolbox believe the click.

### What was tried for mouse clicks, and why each failed

1. **`PostEvent`** posts into the *calling process's own* event queue,
   not the frontmost application's — useless for automating a different
   process (confirmed live: a position write held correctly but no click
   ever reached Finder, because we were posting to ourselves).
2. **Writing `MBState`** (the low-memory global the ADB Manager's real
   interrupt handler updates on a hardware button change) held its
   written value perfectly, but nothing happened — there is no separate
   task watching `MBState` for transitions and synthesizing events from
   it; real event generation is edge-triggered at the hardware interrupt
   level, and a software write to the *cached* state never triggers that.
3. **Trap-patching `_WaitNextEvent`/`_GetNextEvent`** (0xA860/0xA970 via
   `GetTrapAddress`/`SetTrapAddress`, with `SetCurrentA5`/`SetA5`
   bracketing every global access — trap dispatch does *not* switch A5,
   so without this every global read/write inside the patch silently
   hits whatever's at that same A5-relative offset in the *calling*
   process's memory instead) successfully delivered a fabricated
   `EventRecord` into Finder's own `GetNextEvent` call (confirmed via
   counters: correct A5, correct foreign-caller detection, correct
   `everyEvent` mask, successful delivery) — and still produced **zero**
   visible effect for icon selection, window dragging, or menu
   selection. Worse: leaving this patch installed while the *real* user
   interacted with the guest broke their actual mouse (menu clicks and
   drags stopped working) and appears to have caused a genuine CPU fault
   (`SR=2700`, non-maskable interrupt level, `MMUSR` fault logged) that
   `system_reset` could not clear — only a full QEMU process restart
   (kill + relaunch) fixed it. There's a real, unmitigated risk here:
   `QUITAGENT`/`UPDATE`'s exit path never restores the original trap
   addresses before the process exits, so quitting or self-updating
   while this patch is installed leaves the trap table pointing at
   soon-to-be-freed memory — Finder calls `GetNextEvent` dozens of times
   a second, so this would crash almost immediately. **Do not reuse this
   technique without adding proper trap restoration first.**
4. **`Enqueue()` into the real, low-level system event queue** — the
   actual historical technique (confirmed via a 1990s MacTech "Event
   Simulator" article with matching source code). The queue is a fixed
   low-memory global named `EventQueue` at address **0x014A** (a `QHdr`:
   `short qFlags` + `QElemPtr qHead` + `QElemPtr qTail`, 10 bytes,
   confirmed by the next low-memory global sitting exactly 10 bytes
   later) — **not** a trap. (`GetEvQHdr()` is declared in Multiversal's
   `Multiverse.h` with no trap encoding, the same gap as `PostEvent`;
   guessing a trap number for it — 0xA9CB — crashed the agent outright,
   since that's actually `_TEGetText`, an unrelated TextEdit trap. Don't
   guess trap numbers; verify against a real trap table.) This correctly
   inserts a synthetic `EvQEl` (`qType = evType`) that gets consumed by
   the *unmodified* `GetNextEvent`, with no trap patching and none of
   technique 3's stability risk — and it **still** produces no visible
   effect for icon/window/menu clicks.

The MacTech article explains exactly why technique 4 doesn't work for
these targets, in its own words: *"The Menu Manager is much too smart
to be fooled by the technique, so it's impossible to make an automatic
menu selection unless the item has a command key equivalent"* — because
real click/drag tracking (`StillDown()`/`Button()`/`GetMouse()`) polls
the *live* ADB hardware button state after a `mouseDown`, not just the
paired `mouseUp` event that arrives later via the queue. Since a
synthetic `mouseDown` never corresponds to an actual hardware press,
that poll fails immediately, regardless of how correctly the event is
queued. This is a genuine platform limitation confirmed by a real
1990s source with the exact same technique, not a bug in this codebase.
Reliable mouse automation would require actually simulating ADB
hardware transactions (not attempted — a much deeper, more fragile
undertaking, and still unproven even in principle for this repo's
"must also work on real hardware" requirement).

### What was proven to work: keyboard events via `Enqueue()`

Unlike click/drag tracking, menu command-key dispatch is a one-shot
check against the delivered event, not a continuous hardware poll — and
technique 4 above, tested with a synthetic Cmd-N (`keyDown`/`keyUp`,
`evtQMessage = 'n'`, `evtQModifiers = cmdKey`) enqueued into
`EventQueue`, **worked**: Finder created a new, selected, rename-mode
"untitled folder" exactly as a real Cmd-N would. This was verified live
and is a solid foundation for a real `KEY`/`TYPE` implementation in a
future session — the working primitives (`kEventQueue` at `0x014A`,
`EvQEl`/`Enqueue()` as already declared with correct trap encoding in
Multiversal's `Multiverse.h`, `evType = 4` from the `QTypes` enum) were
removed from `llm_agent.c` in this session's cleanup along with the
non-working mouse code, but the technique itself is proven and this
paragraph plus the git history (commit around 2026-09-10) has everything
needed to reimplement it without re-deriving any of the above.

### Ground truth: QuicKeys 3.5.3

To rule out "this exact environment just can't do it," QuicKeys 3.5.3
was installed and used to define a real "Click" macro (screen-coordinate
click, window-relative) via its own GUI. It **worked** — selected an
icon in Finder exactly as a real click would — proving the environment
itself supports real automated clicks *somehow*, just not via any of
the four software techniques above. (Also discovered along the way:
**StuffIt Deluxe 5.5 is broken outright on this ROM/System 7.5.3
combination** — crashes with "unimplemented trap" on any launch attempt,
not specific to any one archive; StuffIt Deluxe **5.0.2** works fine and
was used instead to install QuicKeys.)

## Gotchas that cost real debugging time

- **`CONSOLE` (Retro68's SIOUX-equivalent console library) crashes on
  this QEMU/ROM combination** with an Illegal Instruction before any
  application code runs — confirmed by reproducing it with Retro68's own
  unmodified `HelloWorld` sample, so it's not specific to this agent.
  `llm_agent` links without `CONSOLE` entirely; it never needed a
  console since the whole protocol is over TCP.
- **A `SIZE` resource that isn't `onlyBackground` holds the foreground
  layer forever** if the app's own loop only calls `SystemTask()` and
  never `WaitNextEvent()` — `SystemTask()` yields to drivers/desk
  accessories, not to *other applications* under MultiFinder. This
  looked exactly like a full system freeze (mouse moved via low-level
  cursor tracking, no click ever reached Finder) despite the agent
  itself being alive and answering the network the whole time.
- **Finder's icon rendering for these icon-less apps is not a reliable
  "is it running" signal** — the same dithered icon (and, inconsistently,
  a "currently open" corner badge) appeared whether or not a process
  actually still existed. Trust `PSLIST`, not the icon.
- **`QUITAGENT` must release the MacTCP stream before `ExitToShell()`.**
  Skipping this left the process list clean (`PSLIST` never showed a
  stale entry) but orphaned MacTCP's driver-level control block for the
  port, so the *next* agent's own `TCPStreamCreate`/`TCPListen` hung
  indefinitely — no timeout, so no amount of waiting before relaunching
  helped. Fixed by calling `TCPStreamAbortAndRelease(gStream)` first.
- **Reading `GetMainDevice()->gdPMap->baseAddr` directly doesn't work on
  this q800 setup** for `SCREENSHOT` — it returns 68k code, not pixels,
  even though the address matches what a real Mac uses and
  `qd.screenBits` agrees with it. Writing a marker there to find the
  real offset caused a fatal double MMU fault (real VRAM can't fault a
  CPU when written to, so that address isn't real VRAM). The ROM's own
  Shift-Command-3 screen capture works fine in the same environment,
  confirming real pixel data *is* reachable — just via `CopyBits`, not a
  raw memory read. Fixed by copying the screen into an offscreen
  `GWorld` via `CopyBits` and reading from that instead.
- **QEMU's monitor `system_reset` does not reliably reset this q800
  machine.** After a crash, `system_reset` left the CPU stuck in the
  same faulted state (confirmed via `info registers`: `SR=2700`,
  interrupt level 7, `MMUSR` fault still logged, `A5` still holding a
  pre-crash value) with a blank framebuffer — it silently no-opped
  rather than actually reinitializing hardware state. The reliable fix
  is a full process restart: `kill` the `qemu-system-m68k` process and
  relaunch with the identical command line (a true cold start). QEMU's
  monitor `screendump` (writes a `.ppm` directly from the emulated
  framebuffer) is useful for checking guest state without depending on
  `llm_agent` or the user's own VNC client, since both can be down/stuck
  at the exact moments you need to check.
- **A guest crash (not a clean shutdown) discards any setting changed
  but not yet flushed to disk** — 32-bit addressing was toggled on,
  then lost after a crash + hard process restart, because the crash
  happened before System 7 wrote the change to its on-disk preferences.
  Not a QEMU/qcow2 caching issue in that instance; just: only a clean
  guest-side Restart/Shut Down reliably persists a just-changed setting.
- **RAM bumped without also enabling 32-bit addressing silently breaks
  memory management.** After raising `-m` from 128MB to 256MB, `About
  This Macintosh` reported "System Software: 257,783K" used out of
  262,144K total (only ~4MB free) — nonsensical for System 7.5.3, which
  normally uses a few MB. Root cause: 32-bit addressing was still Off
  (24-bit mode can only cleanly address 16MB), so the Memory Manager's
  own bookkeeping broke down once physical RAM exceeded what 24-bit
  addressing can represent. Fixed by turning on 32-bit Addressing in the
  Memory control panel and restarting (required after any RAM increase
  beyond ~8MB on a 24-bit-capable ROM).
