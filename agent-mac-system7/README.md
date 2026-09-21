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
| `REBOOT` `SHUTDOWN` | cooperative Finder requests; applications may prompt to save or cancel; `OK` means delivered |
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
the agent returned through Startup Items in about 18 seconds. The protocol
power commands now use Finder's application-quit negotiation as well.

Two recurring themes are worth knowing before extending this:

1. **Several low-memory globals are swapped per process** by the Process
   Manager -- the scrap variables and `WindowList` among them. A background
   agent sees its own copy, not the system's. Reading back your own write
   proves nothing; compare against another process's view.
2. **Queue delivery does not guarantee tracking-loop behavior.** Clicks,
   double-clicks, and typing have passed, but dragging does not reliably end.
   Code polling live button state may react differently from code consuming
   queued mouse events; treat menu/drag workflows as application-specific.

Uses **MacTCP** (driver-style `PBControlSync`/`PBControlAsync` on a
`TCPiopb`, not Berkeley sockets) for networking. Built with
[Retro68](https://github.com/autc04/Retro68) targeting 68000 instructions. Live validation used System 7.5.3 on
`qemu-system-m68k -M q800`; other 68k machines and OS revisions need testing.
Guest needs MacTCP configured and working (Control Panel shows an IP).

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as other agents |
| `SYSINFO` | `os_family=mac68k`, `os_version` (Gestalt, hex BCD-ish), `machine_gestalt`, `free_mem_kb`, `agent`, `agent_build`, `agent_port`, `config_location=application-folder` |
| `GET` / `PUT` | Data-fork file transfer; relative paths start in the application folder; `PUT` checks native File Manager writes |
| `SCREENSHOT` | Main display via `CopyBits` into an offscreen `GWorld` → 24-bit BMP (1/2/4/8-bit indexed, 16-bit RGB555, or 32-bit source) |
| `QUITAGENT` | Terminates the agent process itself (see below) |
| `PSLIST` | Live Process Manager process list (`GetNextProcess`/`GetProcessInformation`) |
| `UPDATE` | Self-update with **no user interaction** (see below) |
| `MOUSEPOS` | Not part of the shared protocol — reports current cursor position (`LMGetMTemp()`) and button state (`Button()`) |

`EXEC`/`EXECDETACH` are **not implemented** — classic Mac OS has no
command shell (no `COMMAND.COM`/`CMD.EXE` equivalent). Registry commands
are unsupported. `WINLIST`, `CLIPSET`, and `CLIPGET` return explanatory
errors because this implementation cannot access other applications'
windows or clipboard. `CLICK`/`DBLCLICK`/`KEY`/`TYPE` are implemented;
the command summary above describes their current limits.

The MCP bridge exposes `DBLCLICK` as `legacy_double_click`, `MOUSEPOS` as
`legacy_mouse_position`, and the Mac-specific `UPDATE <size>` transfer as
`legacy_mac_self_update`. The shared client has corresponding `double_click`,
`mouse_position`, and `mac_update` methods. Generic `AgentClient.update()`
sends a bare `UPDATE` and does not work here. `legacy_capabilities` describes
platform coverage and limits without probing mutating commands.
See the [publication audit](../docs/PUBLICATION_AUDIT.md) for additional
update and file-transfer limits. Update prepares a separate application and
retains the previous version for rollback; both the agent and companion
updater must be upgraded to get the complete behavior described below.

The agent runs with **no console and no windows at all** (see
`llm_agent.r`) — not minimized, not backgrounded-with-a-window, just no
UI whatsoever. There is no Finder menu, no dock, no window to close it
from. `QUITAGENT` or a Quit Application Apple event stops it, and
`UPDATE`/`llm_updater` is the only way to replace it short
of decoding a new build by hand.

## Installation and configuration

Keep `llm_agent`, `llm_updater`, and `LLMAGENT.INI` together in a writable
folder on an HFS volume. The volume and folder can have any name. Use a
Finder **alias** to `llm_agent` in the System Folder's Startup Items for
automatic launch. Copying the application there creates a separate
installation that needs its own INI and companion updater.

The agent and updater ask the native Process Manager for their actual
application file and use its volume reference and parent directory ID.
They also select that directory for relative stdio reads. Configuration,
staging, recovery copies, helper lookup, and logs therefore stay beside the
application, independent of the launch directory. No hardcoded `MacOS`
volume name or global Preferences file is used. These APIs are described in
Apple's [Process Manager](https://developer.apple.com/library/archive/documentation/mac/pdf/Processes/Process_Manager.pdf)
and [File Manager](https://developer.apple.com/library/archive/documentation/mac/pdf/Files/File_Manager.pdf)
references. Quit the agent before moving its folder or editing configuration.

Start with `LLMAGENT.INI.example`, replace the token, and optionally set the
listening port:

```ini
token=REPLACE_WITH_UNIQUE_TOKEN
port=2222
```

The example token is deliberately rejected. Use a unique token of 1–255
printable ASCII characters without spaces or tabs. Use at most 127 characters
for compatibility with the shared Python client and other ports. `port` is optional and
defaults to 2222; allowed values are decimal integers from 1 through 65535.
Existing token-only files continue to work. Restart the agent to apply
changes, and match the bridge inventory's `exec_port` to the chosen port.

The file must be ASCII, at most 4,096 bytes, with lines at most 511 bytes.
Classic Mac CR, Unix LF, and Windows CRLF endings are accepted. Blank lines
and lines beginning with `#` or `;` after whitespace are ignored. Spaces and
tabs around keys and values are trimmed. Only lowercase `token` and `port`
keys are accepted, once each; sections and inline comments are unsupported.
Malformed, missing, unreadable, or incomplete configuration makes the agent
exit before listening. It attempts to append a reason to `AGENT.LOG` beside
itself without recording token values. Logging is best effort if that
directory cannot be found or written.

Relative `GET`/`PUT` paths refer to the application folder; full HFS paths
such as `Other Volume:Folder:File` remain supported. Self-update requires
the application filename `llm_agent` and a real `llm_updater` beside it.
A renamed agent can serve other commands but rejects `UPDATE`, draining
its payload without modifying a sibling application. Upgrade **both**
binaries to obtain the location handling described here.

For bridged QEMU guests, match the port in any host firewall rules too; see
[QEMU/Docker connectivity](#qemu-bridge-connectivity-when-docker-is-installed).

## Screenshot limits

`SCREENSHOT` captures the main display and streams complete rows in chunks,
including widths above 2,048 pixels, without allocating a second full image
for the BMP. Output rows have four-byte padding, included in both the file
size and BMP image-size field. Dimensions must be positive and at most
32,767 each; the complete BMP is capped at 64 MiB to match the shared client's
default response limit. QuickDraw's source row stride and available memory
can impose smaller practical limits.

Unavailable pixels, unsupported layouts, allocation/locking failures, and
oversized output return `ERR:` before a `SIZE:` header. A send failure stops
conversion immediately; the offscreen pixels are unlocked and freed on both
success and failure. Native live checks cover the 640×480 System 7.5.3
desktop and consecutive screenshot/PING requests. Host regression tests cover
all six supported source depths, row padding, widths through 4,093 pixels,
and capture/send failures; a wider physical display has not been tested.

## Two binaries: `llm_agent` and `llm_updater`

`llm_updater` is a small companion app with one job: turn a staged
MacBinary-encoded build into a running `llm_agent`, with no GUI
interaction. It must be deployed once, by hand, the same way `llm_agent`
itself is (see Deploy below). `UPDATE` replaces only the agent; companion
updater fixes require deploying a new `llm_updater` separately.

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

`OK` confirms that all declared bytes were staged as `STAGED_AGENT.bin`,
the File Manager write/close/volume-flush checks passed, and the Process
Manager accepted launching `llm_updater`. The agent then requests normal
cleanup/exit. A missing helper or failed helper launch returns `ERR` and
keeps the agent running. The reply does not confirm successful replacement;
reconnect and check `SYSINFO` and `UPDATER.LOG` after an update.
Use `legacy_mac_self_update(machine, new_agent_local_path)` or
`AgentClient.mac_update(path)` with the Retro68 `.bin`. The generic
`legacy_self_update` tool is for Windows/OS2. The Mac tool validates the
container before transfer and, by default, waits for a new startup instance
at the same application location with matching startup and fresh installed
fork hashes. Set `wait_for_agent=False` for acceptance only. Older replacement
builds lacking the identity fields remain unverified; PING is insufficient.

SYSINFO exposes `agent_started` (ticks and Process Serial Number), an opaque
`agent_location` (volume reference, parent ID, hex filename), startup
`agent_data_sha256`/`agent_resource_sha256`, and freshly computed
`disk_data_sha256`/`disk_resource_sha256`. Failed native fork reads/closes
leave the affected hash empty. Startup hashes never refresh during service.

The resource hash advertises `agent_resource_hash_mode=sha256-zero-system-16-127-v1`.
Only bytes 16..127 are replaced with zeros when hashing. Apple's
[Inside Macintosh I, Resource File Format](https://mirrors.apple2.org.za/www.bitsavers.org/pdf/apple/mac/Inside_Macintosh_Vol_1_1984.pdf)
identifies those 112 bytes as system directory metadata; System 7 changes
them during the updater's file exchange. The layout header, application data
at bytes 128..255, resource data, and map are still hashed. Bounds checks
reject headers whose resource/map regions reach outside the fork or into
that reserved area. The data fork uses ordinary SHA-256. This fingerprints
the startup file, not live code memory or a cryptographic publisher signature.
Fresh disk hashing adds file I/O to SYSINFO and can take longer on slow Macs.


Both `PUT` and `UPDATE` reject failed writes, flushes, and closes, and drain
the declared payload after local file errors so the next command remains
framed. These operations use File Manager calls directly because Retro68's
stdio wrappers discard some native errors. A failed update transfer keeps
the agent running and never launches the updater; it uses `FSpDelete` to
remove the failed stage and reports cleanup failure explicitly. The updater
also uses `FSpDelete` for staging cleanup after a successful launch.
Ordinary `PUT` is not atomic and can leave
a partial destination after an error. Negative sizes are rejected, updates
must be nonempty, and `PUT` rejects paths longer than 255 bytes instead of
silently truncating them. Host-side fault tests for both handlers are in
`../tests/test_uploads.py`.

After its cooperative ten-second teardown wait, the updater validates the
MacBinary header and exact padded file length, writes both forks to a
separate file, closes and flushes them, and reads them back for comparison. HFS rewrites the resource header's
system-reserved bytes 16–127; verification excludes only that region after
validating the resource layout, and compares all application bytes.
It then uses `FSpExchangeFiles` to exchange both forks with `llm_agent`,
preserving the installed file's ID for Startup Items aliases, as described
in [Apple's File Manager reference](https://developer.apple.com/library/archive/documentation/mac/pdf/Files/File_Manager.pdf). It never
deletes the installed application. Preparation failure relaunches the
untouched agent; a failed replacement launch or post-exchange flush triggers
an exchange back and a launch of the previous application. An unsupported
exchange operation fails without a destructive replacement fallback.

Recovery copies use the first unused `llm_agent.saved.001` through `.099`.
Existing copies are never overwritten automatically; remove obsolete ones
manually when space or slots run low. After a successful exchange the saved
file contains the previous application. Following a failed attempt it may
instead contain an incomplete or rejected candidate; consult `UPDATER.LOG`
before choosing a recovery copy. If rollback itself fails, both files are
retained and the updater attempts to launch the previous application from
the saved path. Manual repair is then required before relying on startup.

A successful `LaunchApplication` is not a health check: a build can launch
and subsequently crash or fail to listen. The updater also cannot guarantee
recovery from power loss or disk corruption. Keep an independent backup.
Run `python tests/test_update.py` here for host-side failure injection.

## Build (Retro68 cross-toolchain, Linux host)

```bash
python3 ../tools/prepare_dependencies.py --component mac-sdk --source-root /path/to/MacTCP-headers
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
make
```

Produces `llm_agent.bin` / `llm_agent.APPL` / `llm_agent.dsk` and the
same set for `llm_updater`. `.bin` is the MacBinary-encoded form used
for transfer (CD-ROM or `PUT`/`UPDATE`); `.dsk` is a flat HFS image
`hfsutils`' `hmount`/`hcopy -m` can pull a `.bin` out of directly.

`MacTCP.h` and `AddressXlation.h` must be supplied locally from the known
MacTCP 2.0.6 / Universal Interfaces 2.1b1 SDK snapshot (Apple, 1995).
Preparation installs verified copies in the ignored `../.deps/mac-sdk`
directory. For an external location, prepare with `--destination /path/to/sdk`
and configure with `-DMAC_TCP_SDK_DIR=/path/to/sdk`. Configuration checks
both hashes and requires host Python 3. `vendor/AppleTalk.h` is a small
project-written type stub. See [dependency provenance](../THIRD_PARTY.md)
and `vendor/README.md`; the project MIT license does not cover Apple headers.

## Deploy

First deploy has no shortcut — get `llm_agent` and `llm_updater` onto
the guest once, by hand:

1. Transfer both `.bin` files to the guest. The tested q800 installation
   used an ISO9660/Joliet CD-ROM image (`genisoimage -J -r`); its floppy
   path was not usable. Choose media supported by your emulator or hardware.
2. Decode each `.bin` with StuffIt Expander (drag onto it).
3. Copy `LLMAGENT.INI.example` to `LLMAGENT.INI` next to `llm_agent`,
   set a real `token=` and, if needed, `port=` as described above.
4. Double-click `llm_agent` to launch it.

From then on, updates go through `UPDATE` — see above — with `llm_agent`
and `llm_updater` never touched by hand again unless `llm_updater`
itself needs a new build.

## Input implementation provenance

QuicKeys 3.5.3 was used as a behavioral reference during mouse-input
investigation, including event-queue and low-memory observations under an
emulator debugger. This was reverse engineering, not a formal clean-room
implementation. QuicKeys is not a runtime dependency: clicks were verified
after its driver and application were removed from the running environment.

The useful finding was to keep cursor globals and queued event positions in
agreement: place the cursor, allow tracking to consume the change, then post
mouse-down/up entries with matching coordinates/modifiers. The implementation
is in `SetMouseTo`, `PPostEventTrap`, and `PostMouseEvent` in `llm_agent.c`,
with bounded cursor-update waits and checked event posting. Drag remains
experimental; successful clicks do not prove all tracking loops work.

Raw disassembly, signatures, and debugger transcripts are excluded from the
public source. The generic `findsig.py` accepts signatures supplied by its user.
See [licensing and provenance](../THIRD_PARTY.md#project-maintained-compatibility-code).

## Protocol extensions and exclusions

The command table above describes the implemented surface. The bridge exposes
Mac DBLCLICK, MOUSEPOS, and MacBinary UPDATE in addition to shared supported
commands. QUIT ends a connection; QUITAGENT exits the application. DRAGSTAT and
DRAGRESET are experimental instrumentation, without MCP tools. Removed research
commands such as PEEK and trap probes are not part of the current protocol.

WINLIST and clipboard operations explicitly refuse. Windows registry/control
messages and NetWare screen/startup commands are unsupported. An unsupported
command does not become available through a generic MCP wrapper.

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
> `isHighLevelEventAware`. The agent also dispatches incoming Apple events
> and handles Quit Application; omitting that handler previously blocked
> Finder Restart. `onlyLocalHLEvents` limits the scope to this machine.

> Note that changing only `llm_agent.r` does **not** move the `agent_build`
> stamp, since `__DATE__`/`__TIME__` only update when the `.c` recompiles. Check
> the startup and installed fork hashes when verifying a resource-only change;
> the build timestamp alone is insufficient.

## Window and clipboard limits

WINLIST refuses with an explanation: under MultiFinder, WindowList is swapped
with process context, so this background agent sees its own windows rather
than the other applications visible in a screenshot. A previous implementation
returned only its own console window. Returning that as a desktop inventory
would be misleading. Asking scriptable applications for windows through Apple
events would be separate, application-dependent functionality, not implemented.

CLIPSET and CLIPGET also refuse. The agent's scrap writes/readbacks were
self-consistent but did not reach other applications; low-memory scrap state
varied with the scheduled process. Normal user copy/paste works, but this
background implementation does not participate in that exchange. Use short
TYPE requests or file transfer. Neither limitation proves that a different
future implementation is impossible.

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
between connections. Retry a failed connection only when no command was
sent; do not blindly replay an operation after an uncertain reply.

## Restart and shutdown

Finder's **Special > Restart** uses the application's Quit-event handler.
This path was tested with no client connected and with an authenticated
client waiting for its next command; both restarted successfully and
relaunched the agent. Special > Shut Down uses the same handler but was
not separately tested after this fix.

`REBOOT` and `SHUTDOWN` send the Finder event class `FNDR` with event IDs
`rest` and `shut`, respectively. The agent finds Finder by its application
signature `MACS` and addresses its process serial number.
This follows the cooperative power-request path described in Apple's
[Shutdown Manager documentation](https://developer.apple.com/library/archive/documentation/mac/pdf/Processes/Shutdown.pdf).
Finder lets applications save or cancel before it invokes the Shutdown
Manager to flush/unmount volumes and perform the power operation.

`OK` means the Apple event was delivered, **not that the machine has restarted
or shut down**. A save dialog, cancellation, or unresponsive application can
prevent completion. The asynchronous request allows applications to display
UI and does not wait for a reply. Finder lookup, descriptor creation,
activation, or delivery failures return `ERR:` with the OS error; there is
no forced-power fallback. Finder is brought to the foreground before
delivery so requests also work after a cancelled save dialog.

The agent releases mouse automation before delivery but keeps serving until
Finder asks it to quit. It then uses the normal Quit-event cleanup path.
Depending on quit order, Finder may already have closed the agent before
another application cancels; relaunch it locally if needed. Callers should
verify the resulting machine state rather than repeatedly sending requests
while a save dialog is pending.

On the QEMU q800 test guest, a completed shutdown powers off the virtual
machine and exits QEMU. Relaunch it with your host VM launcher. Other
Mac models may instead display a safe-to-power-off screen.

Host-side fault-injection tests cover both commands, every event-creation/
delivery failure, descriptor cleanup, and a Quit event during delivery:
`python3 tests/test_power.py` (requires a host C compiler, selectable with
`CC`). These tests do not emulate Finder or replace live guest checks.

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
be applied after Docker starts. A port change must match the INI, bridge
`exec_port`, and any destination-port and reply source-port firewall rules.
The test host uses a local `retro-mac-forwarding.service`/helper, not shipped
by this repository; remove its old rules before applying changed ones. The test host uses Docker's snap service,
`snap.docker.dockerd.service`, rather than `docker.service`. See
[Docker's forwarding guidance](https://docs.docker.com/engine/network/firewall-iptables/).

## Experimental dragging

CLICK selected Finder icons and DBLCLICK opened them in live tests. DRAG can
move an object but does not reliably end the tracking operation. DRAGSTAT and
DRAGRESET remain diagnostic commands, without a supported MCP drag tool.
Prefer keyboard shortcuts and verified click actions where they cover the task.

Earlier failed event/trap experiments remain in Git history. They are not
current installation instructions or evidence that the working input methods
are unavailable. The provenance section above records the behavioral reference.

## Emulator and implementation troubleshooting

These observations are from the tested System 7.5.3/q800 environment:

- The Retro68 console library faulted before application startup, including
  in its unmodified sample. The agent therefore builds without CONSOLE.
- Cooperative background code must yield with WaitNextEvent; SystemTask alone
  did not let other applications progress in the tested loop.
- Finder icon appearance did not reliably indicate whether the agent was
  running. Use PSLIST and authenticated protocol checks.
- Capture uses CopyBits into a GWorld; direct reads of the apparent framebuffer
  did not return valid pixels on this setup.
- After some guest faults, QEMU's monitor system_reset did not recover the VM.
  A full QEMU process restart with the original launch settings was needed.
  Prefer clean guest shutdown/restart when responsive; forced recovery can
  lose unflushed files and preferences.
- Increasing emulated RAM while 32-bit addressing was disabled produced
  incorrect memory accounting. Check the Memory control panel and perform a
  clean restart after changing the addressing setting.

These are configuration-specific debugging findings, not guarantees about
other QEMU versions, ROMs, or physical Macs. Keep the original VM launch
configuration and a recoverable disk backup before changing that environment.
