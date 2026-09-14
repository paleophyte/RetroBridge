# llm_agent for classic Mac OS (System 7.x)

**68k Mac OS over MacTCP** target-agent port. It is not an MCP server
itself; it speaks the **same** token-authed TCP wire protocol as the
other ports, so [`../mcp-server/server.py`](../mcp-server/server.py) can
expose it through the repo's `legacy_*` MCP tools with no protocol fork.

## Command status at a glance

| command | status |
|---|---|
| `PING` `SYSINFO` `PSLIST` `SCREENSHOT` | working |
| `PUT` `GET` | working (paths may contain spaces) |
| `CLICK` `DBLCLICK` | working, optional third `button` argument; only button 1 exists |
| `KEY` `TYPE` | working; US layout only |
| `PSKILL` | working, but it *asks* a process to quit rather than killing it |
| `REBOOT` `SHUTDOWN` | working via direct Shutdown Manager calls; bypass other applications' save/quit handling |
| `UPDATE` `QUITAGENT` `QUIT` | working |
| `MOUSEPOS` | working (Mac-only extension) |
| `CLIPSET` `CLIPGET` | **disabled** -- the scrap is per-process, writes never reach other applications |
| `WINLIST` | **disabled** -- `WindowList` is per-process, only this agent's own windows are visible |
| `DRAG` `DRAGSTAT` `DRAGRESET` | **incomplete** -- movement works, the drag never terminates |
| `EXEC` `EXECDETACH` | not applicable, classic Mac OS has no shell |

Disabled commands return `ERR:` with the reason rather than a misleading `OK`.

**Finder Restart works with Quit-event handling:** the agent dispatches
incoming Apple events and honors Finder's Quit Application request. It
cancels pending MacTCP listen/receive operations, releases the stream, and
removes mouse/VBL hooks before exiting. Special > Restart was verified on
System 7.5.3 both while listening and with an authenticated idle client;
the agent returned through Startup Items in about 18 seconds. The agent's
own power commands still bypass Finder's application-quit negotiation.

Two recurring themes are worth knowing before extending this:

1. **Several low-memory globals are swapped per process** by the Process
   Manager -- the scrap variables and `WindowList` among them. A background
   agent sees its own copy, not the system's. Reading back your own write
   proves nothing; compare against another process's view.
2. **Toolbox tracking loops are unreachable by synthetic events.**
   `MenuSelect` for menus, `WaitMouseUp`/`DragGrayRgn` for dragging. Plain
   event delivery works fine -- clicks, double-clicks, typing -- but a loop
   polling live hardware state ignores anything posted to the event queue.

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
command shell (no `COMMAND.COM`/`CMD.EXE` equivalent). Registry commands
are unsupported. `WINLIST`, `CLIPSET`, and `CLIPGET` return explanatory
errors because this implementation cannot access other applications'
windows or clipboard. `CLICK`/`DBLCLICK`/`KEY`/`TYPE` are implemented;
the command summary above describes their current limits. The investigation
below includes superseded experiments, not additional current restrictions.

The MCP bridge exposes only part of this surface: `DBLCLICK`, `MOUSEPOS`,
and the Mac-specific `UPDATE <size>` transfer need a custom protocol client.
Generic `AgentClient.update()` sends a bare `UPDATE` and does not work here.
See the [publication audit](../docs/PUBLICATION_AUDIT.md) for additional
update and file-transfer failure cases. Update currently has no rollback
after the updater deletes the old application.

The agent runs with **no console and no windows at all** (see
`llm_agent.r`) — not minimized, not backgrounded-with-a-window, just no
UI whatsoever. There is no Finder menu, no dock, no window to close it
from. `QUITAGENT` or a Quit Application Apple event stops it, and
`UPDATE`/`llm_updater` is the only way to replace it short
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

`QUITAGENT`, a Quit Application Apple event, and a successful `UPDATE`
request all return through the main loop's cleanup before exiting. A Quit
event aborts any pending asynchronous listen/receive and waits for its
parameter block to complete before unwinding the stack. Cleanup calls
`TCPStreamAbortAndRelease()` and removes mouse/VBL hooks.
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

## QuicKeys is no longer required

QuicKeys 3.5.3 was the reference implementation the click mechanism was
reverse-engineered from. It is **not** a runtime dependency: with QuicKeys
removed from Startup Items entirely -- the `.QuicKeys` driver not resident, a
System heap scan for its signature finding nothing, `QuicKeys Toolbox` absent
from `PSLIST`, its control panel gone -- `CLICK` still selects a Finder icon
exactly as before. The mechanism was reimplemented natively, not delegated.

It is still worth being able to put back for one reason: if the unsolved half
of `DRAG` is picked up again, QuicKeys is the only known working example of a
macro tool ending a Finder tracking loop on this OS, and watching it do that is
likely the fastest route to the answer.

Removing it also shifts the heap, so any absolute address recorded in
`QUICKEYS_CLICK_INVESTIGATION.md` is stale again. Re-find them with a host-side
`pmemsave` dump plus `findsig.py` rather than trusting the written values.

## Protocol surface

Audited end to end against the real `../mcp-server/agent_client.py`, not by
hand. Everything below was actually exercised through that client.

**Implemented and verified:** `PING`, `SYSINFO`, `PSLIST`, `SCREENSHOT`,
`PUT`, `GET`, `CLICK`, `KEY`, `TYPE`, `PSKILL`, `REBOOT`, `SHUTDOWN`,
`UPDATE`, `QUIT`, plus the
Mac-only `DBLCLICK`, `MOUSEPOS`, `QUITAGENT` and the incomplete `DRAG`.

Anything unimplemented answers `ERR:unknown command` and the agent stays up --
confirmed for every missing verb below, so a client probing the surface cannot
hang or crash it.

### Not implemented

| command | feasibility on System 7.5.3 |
|---|---|
| `WINLIST` | **Feasible.** Walk the Window Manager's `WindowList` low-memory global (`0x09D6`) and read each title and `portRect`. Read-only, low risk. |
| `EXEC` / `EXECDETACH` | **Not applicable.** No shell exists. `EXECDETACH` could reasonably be redefined as "launch this application", which `llm_updater` already does with `LaunchApplication` -- but that is a deliberate protocol divergence, not an implementation. |
| `WINMSG`, `POSTMSG`, `LBGETTEXT`, `REGGET`, `REGSET` | **Not applicable.** Windows-specific. |
| `SCREENS`, `AUTOEXEC`, `DEBUG` | **Not applicable.** NetWare-specific. |

### Housekeeping

The `TEMP DIAGNOSTIC` commands built for the QuicKeys click investigation --
`PEEK`, `SCANSIG`, `SCANCDRV`, `HLEWATCH`, `DEVPROBE`, `DEVPROBE2`,
`TRAPADDR`, `RESLOOKUP` -- **have been removed** now that the investigation is
solved. 425 lines went, and the binary dropped from 72,576 to 67,712 bytes.

They were worth removing rather than leaving idle: `PEEK` crashes the agent by
design when pointed at unmapped memory, and `HLEWATCH` patches a trap vector,
which is the single most dangerous thing in this codebase if it is ever left
installed. Most of what they did is better done from the host anyway --
`pmemsave` plus `findsig.py` reads guest memory with no crash risk at all, and
the gdbstub gives breakpoints and watchpoints the guest cannot notice.

They remain in git history if ever needed again.

`DRAGSTAT` and `DRAGRESET` were deliberately kept: `DRAG` is unfinished, and
they are its instrumentation and its escape hatch.

## PSKILL asks, it does not kill

`PSKILL <pid>` is implemented and verified end to end through the real client.
The `<pid>` is what `PSLIST` reports: the low long of the process serial
number.

Classic Mac OS has no kill. There is no protected memory and nothing able to
reclaim another process, so the only thing available is to **ask**, by sending
a standard `quit` AppleEvent (`aevt`/`quit`). That is cooperative in the
fullest sense: a well-behaved application quits, one with unsaved changes may
put up a save dialog and sit there, and a wedged one ignores it entirely.

**So `OK` means the quit request was delivered, not that the process is gone.**
Those are genuinely different. Follow up with `PSLIST` if you need certainty.

Verified: launching Find File and then `pskill()`-ing it removes it from
`PSLIST`. A non-existent pid gives `ERR:no such process <n>`, and targeting the
agent itself is refused with a pointer to `QUITAGENT` -- that one is a
correctness point rather than caution, since quitting mid-command means the
reply never gets sent and the caller hangs.

> **This required a `SIZE` resource change.** The AppleEvent Manager will not
> let an application *send* a high-level event unless its own `SIZE` resource
> claims awareness of them. The agent was marked `notHighLevelEventAware`, so
> `AESend` returned `-903` (`noPortErr`) and nothing was delivered. It is now
> `isHighLevelEventAware`, which costs nothing here: the agent never processes
> incoming events, and `onlyLocalHLEvents` keeps the scope to this machine.

> Note that changing only `llm_agent.r` does **not** move the `agent_build`
> stamp, since `__DATE__`/`__TIME__` only update when the `.c` recompiles. Check
> the behaviour, or touch the `.c`, when verifying a resource-only change.

## WINLIST is not available, and will not be

Refused with `ERR:WINLIST unavailable, WindowList is per-process under
MultiFinder`. This is a platform limit, not a missing feature.

The obvious implementation -- walk the Window Manager list from `WindowList`
(`0x09D6`) via `nextWindow` -- was written and works. It just cannot see
anything useful: under MultiFinder `WindowList` is part of the per-process
low-memory state the Process Manager swaps on each context switch, so a
background application sees only its own windows.

Measured rather than assumed. With the visible/title filters removed the walk
returned exactly one entry:

```
hwnd 0x0FD425B0   (12,34) 621x441   windowKind 8   visible 1   titleLen 0
```

That is this agent's own untitled Retro68 console window, and `nextWindow` was
NULL so it was the whole list -- while four windows belonging to other
applications were plainly on screen at the time.

Returning an empty list would be worse than refusing. The client documents
`WINLIST` as "visible top-level windows", and `[]` reads as "there are no
windows" when there are several that simply cannot be reached from here.

The route that could work is **AppleEvents** -- asking each running application
for its windows, the way a scripting client would. That needs real AppleEvent
plumbing and only covers scriptable applications, so it is separate work rather
than a fix. `WINLIST <parent>` is refused outright: it lists a dialog's child
controls, and classic Mac controls are not windows.

## CLIPSET and CLIPGET are not available: the scrap is per-process

Both refuse. Answering `OK` would be a trap -- set the clipboard, get a
success, then paste something else entirely.

Copy and paste **does** work normally between applications on this machine.
The agent simply is not part of the exchange.

### The evidence

`InfoScrap()` returns `0x0960`, so that address is correct -- but its
*contents* depend on whose context is current. Reading it from the hypervisor
repeatedly while asking the agent the same question shows two different value
sets at one address:

```
host  0x0960:       size=46  handle=00000000  state=0   <- other applications
host  0x0960:       size=30  handle=0fc040b4  state=1   <- this agent
agent InfoScrap():  size=30  handle=0fc040b4  state=1
```

The second host sample caught the agent scheduled, and matches its own view
exactly. The scrap variables are swapped per process by the Process Manager,
the same way `WindowList` is.

That is also why `GetScrap('TEXT')` returns `-102` here while a perfectly good
scrap exists elsewhere. Dumping guest RAM finds the real chain --

```
TEXT len=5   "hello"
styl len=22
```

-- at `0x001705A0` in the System heap, holding text copied inside ClarisWorks.
It is not reachable from this process.

### A separate fact worth knowing

Applications write the desk scrap when they are **suspended**, not when Copy is
pressed. Text copied by hand in SimpleText did not change the shared scrap at
all until the application was switched away from.

### What misled me

Four successive explanations were published as fact before this one, and the
cause was the same every time: `CLIPSET` then `CLIPGET` round-tripped
byte-exact, and `ScrapInfo` always corroborated it. That only ever proved
**this process was self-consistent with itself**. On a system with per-process
low memory, reading back your own write is not even weak evidence that anyone
else can see it.

One of those four was a retraction of the correct answer: the per-process
explanation was reached, then withdrawn because four consecutive host samples
happened to catch the same non-agent context and looked identical. Sampling a
swapped global without controlling for which process is scheduled is worthless.
The fix was to read both views side by side in the same breath.

Use `TYPE` to get text into an application. That is verified working, into both
ClarisWorks and Find File.

## Keyboard: KEY and TYPE

Both implemented and verified end to end through the real
`../mcp-server/agent_client.py`. They reply `OK` / `ERR:...`.

`TYPE <text>` types literal text. `KEY <keyspec> [<keyspec> ...]` sends named
keys; each keyspec may carry `-`-joined modifiers. Modifier names follow the
other ports (`ctrl`, `alt`, `shift`) with the Mac spellings added: `cmd` /
`command`, and `opt` / `option` as aliases for `alt`.

```
TYPE Hello
KEY enter
KEY cmd-w          # verified: closes the front Finder window
KEY shift-tab down
```

Named keys: `enter`/`return`, `tab`, `space`, `esc`/`escape`,
`bksp`/`backspace`/`del`, `fwddel`/`delete`, `left`/`right`/`up`/`down`,
`home`/`end`/`pgup`/`pgdn`, `f1`-`f12`. Anything unrecognised is refused with
`ERR:unknown keyspec` rather than silently dropped.

### How it works

Same route as `CLICK`: `PPostEvent` (trap `0xA12F`) returns the queue element
and the fields are filled in. The message layout came from a real keypress
captured off the event queue, not from a manual --
`evtQMessage = 0x00023260` for a grave keypress, i.e.
`(adbAddr << 16) | (keyCode << 8) | charCode` with adbAddr 2, and `0x32` is
indeed the ADB code for the grave key. The US key-code table is anchored to
that observation.

> **keyUp is best-effort, and that is correct.** Classic Mac OS leaves
> `keyUpMask` out of `SysEvtMask` by default, so `PostEvent` refuses keyUp with
> `evtNotEnb` -- real keypresses do not enqueue a keyUp either. The first cut
> treated that refusal as fatal and rejected every single character. keyDown is
> the event that must land; keyUp is posted and its failure ignored.

### Limits

The event queue is a fixed pool that only drains when the receiving
application runs, and it cannot run until the command returns. A long enough
`TYPE` will therefore exhaust the queue. That surfaces as
`ERR:keyDown rejected (OSErr n) after <k> of <n> characters` -- a short count,
never silent truncation. Split long text across several `TYPE` calls.

The key-code table is US layout only.

## Clicking

`CLICK <x> <y> [button]` and `DBLCLICK <x> <y> [button]`, in global screen
coordinates (the same space `MOUSEPOS` reports). Both reply `OK` / `ERR:...`.

The button argument is **optional** -- omitted means 1 -- so both the two-argument
form and the shared protocol's `CLICK <x> <y> <button>` work. `agent_client.py`
always sends three arguments with `button=1` by default, and that path is
verified end to end against the real client.

This hardware has one mouse button, and System 7.5.3 has no contextual menus for
a second one to open. Buttons 2 and 3 are therefore **refused** with an `ERR:`
rather than quietly performing a left click, which would let a caller believe
it had right-clicked when nothing of the sort happened.

> Two earlier divergences from the shared protocol are fixed here: `CLICK` used to
> ignore the button argument entirely, and used to answer with a `SIZE:` body.
> `agent_client.py`'s `_simple_command` compares the first reply line against
> `"OK"`, so the old reply made every `client.click()` raise
> `AgentProtocolError`.

### One connection at a time

The agent serves a single connection and re-arms its MacTCP listener after each
one closes. Reconnecting immediately can come back `ECONNREFUSED` before the
listener is up again, which looks like a crash but is not -- seen repeatedly
while scripting several commands back to back. Leave a couple of seconds
between connections, or retry on `OSError`.

## Restart and shutdown

Finder's **Special > Restart** uses the application's Quit-event handler.
This path was tested with no client connected and with an authenticated
client waiting for its next command; both restarted successfully and
relaunched the agent. Special > Shut Down uses the same handler but was
not separately tested after this fix.

`REBOOT` and `SHUTDOWN` are implemented and working, matching the shared
protocol in `../mcp-server/agent_client.py` (both reply `OK`).

These protocol commands bypass Finder's save/quit negotiation with other
applications; an `OK` does not establish that those applications saved
their work. They call the Shutdown Manager (trap `0xA895`) -- `ShutDwnStart()` and
`ShutDwnPower()`. That choice matters: the Shutdown Manager runs registered
shutdown procedures and flushes/unmounts volumes, so the guest comes back
clean. Verified by rebooting *without* sending the usual dismiss keystroke --
the agent was answering again 24s later with no "restarted improperly"
dialog. A hard stop leaves that dialog up, and it blocks Startup Items
processing, so the agent would not relaunch by itself.

`SHUTDOWN` is a true power-off: the QEMU process exits with the guest, so
recovering needs `launch_vm.sh` on the host, not just a guest boot.

## QEMU bridge connectivity when Docker is installed

If the Ubuntu QEMU host can reach the Mac but another machine cannot,
check the Linux bridge firewall as well as MacTCP and the outer hypervisor.
Docker can set the IPv4 `FORWARD` policy to `DROP`. When
`net.bridge.bridge-nf-call-iptables=1`, traffic crossing from the host's
physical interface to the Mac's TAP interface passes through those rules.
Host-originated connections do not use that same forwarding path.

This was confirmed live: incoming SYN packets appeared on the Ubuntu NIC
but never reached the Mac TAP. A scoped `DOCKER-USER` allowance for the
control PC to the Mac agent's TCP port, plus established replies, restored
authenticated access and screenshots. VMware's effective port policy
already permitted the nested guest's MAC traffic. The fix belongs on the
Ubuntu bridge, not in the Mac's address/gateway settings.

Use explicit interface/address/port matches and arrange for the rules to
be applied after Docker starts. The test host uses Docker's snap service,
`snap.docker.dockerd.service`, rather than `docker.service`. See
[Docker's forwarding guidance](https://docs.docker.com/engine/network/firewall-iptables/).

## Mouse and keyboard automation

**`CLICK x y` and `DBLCLICK x y` are implemented and working** (global
screen coordinates, same space `MOUSEPOS` reports). `KEY`/`TYPE` are
implemented for the US keyboard layout; see [Keyboard](#keyboard-key-and-type).

Verified live: `CLICK` selects a Finder icon and `DBLCLICK` opens it (an
Apple Menu Options control panel window actually opened). The mechanism is
the one QuicKeys uses -- place the cursor via `MTemp`/`RawMouse`/`Mouse`,
then post `mouseDown`/`mouseUp` through `PPostEvent` (trap `0xA12F`) and
fill in `evtQWhere` on the returned queue element.

The history below is kept because it explains why this looked impossible. This was
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
