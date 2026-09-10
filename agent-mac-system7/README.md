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

`EXEC`/`EXECDETACH` are **not implemented** — classic Mac OS has no
command shell (no `COMMAND.COM`/`CMD.EXE` equivalent) — and
`CLICK`/`KEY`/`TYPE`/`WINLIST`/`REG*` are not implemented either. All
return `ERR:unknown command`.

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
