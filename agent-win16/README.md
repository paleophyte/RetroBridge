# llm_agent for Windows for Workgroups 3.11

Win16 target-agent port of [`../agent-win32/llm_agent.c`](../agent-win32/llm_agent.c). It is
not an MCP server itself; it speaks the **same** token-authed TCP wire protocol,
so [`../mcp-server/server.py`](../mcp-server/server.py) can expose it through the repo's
`legacy_*` MCP tools with no protocol fork.

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as the other agents |
| `EXEC` | `REDIR.EXE <outfile> <cmd>` (native DOS helper, see warning below), then `LEN:`/`EXIT:` -- **DOS-style console commands only** |
| `EXECDETACH` | `WinExec()` directly, no DOS box, fire-and-forget -- use for any native Windows program |
| `PUT` / `GET` | File transfer |
| `SYSINFO` | Windows/DOS version, free system resources %, real RAM total/free (ToolHelp `MemManInfo`), C: disk space |
| `SCREENSHOT` | Whole-desktop `BitBlt` rendered to a 24-bit BMP |
| `REBOOT` | `ExitWindows(EW_REBOOTSYSTEM)` -- a real machine reset, not just "exit to DOS" |
| `SHUTDOWN` | `ExitWindows(0)` -- **not a real power-off, see warning below** |
| `KEY` / `TYPE` / `CLICK` | `WH_JOURNALPLAYBACK` with an instance thunk; tested clicks/text, with modal/layout limits below |
| `WINLIST [hwnd]` | Enumerate top-level windows, or immediate children if `hwnd` is given |
| `WINMSG <hwnd> <msg> <wparam> <lparam>` | Synchronous scalar `SendMessage()`; avoid actions that may open a modal dialog |
| `POSTMSG <hwnd> <msg> <wparam> <lparam>` | Raw `PostMessage()` -- use this instead of `WINMSG` for button presses / listbox activation |
| `LBGETTEXT <hwnd> <index>` | Read full text from a standard string-backed listbox; size checked before copying |
| `PSLIST` | `TaskFirst`/`TaskNext` + `ModuleFindHandle` (ToolHelp) -- a real Task List view, `<hTask>\t<exe basename>` per line |
| `PSKILL <hTask>` | `TerminateApp(hTask, NO_UAE_BOX)` (ToolHelp) -- same call Task List's "End Task" uses; refuses to kill this agent's own task |
| `UPDATE` | Staged self-update via RESTART.EXE; see verification/recovery below |

Everything else (`CLIPSET`, `REG*`) returns `ERR:not supported on Windows 3.11`.

The MCP bridge exposes `legacy_winlist(..., parent_hwnd=...)`, `legacy_winmsg`,
`legacy_postmsg`, and `legacy_lbgettext` for these window/control operations.
Use Win16 message constants, which can differ from Win32. Only scalar message
parameters are passed; pointer-bearing messages are not marshalled. Prefer
queued `legacy_postmsg` for actions that might open a modal dialog. See
[MCP coverage](../docs/MCP_COVERAGE.md) and `legacy_capabilities` for limits.

### UPDATE: self-update without a full REBOOT

Do **not** `PUT` a new `LLMAGENT.EXE` over the running one. Windows 3.1
keeps a module's code resident in memory by name until every instance of
it has exited (`GetModuleUsage()` hits zero), so the Win16-safe update
flow stages the new binary as `LLMNEW.EXE` instead. `UPDATE` launches
`RESTART.EXE` (`restart.c`, a separate tiny helper built via
`build_restart.bat`) and then exits itself; `RESTART.EXE` waits for the
old agent to unload, renames `LLMAGENT.EXE` to `LLMAGENT.OLD`, moves
`LLMNEW.EXE` into place, and launches the fresh copy. The `.OLD` backup
is deliberately preserved for local recovery if a new build cannot speak
TCP.

The agent requests shutdown and returns through the server loop so sockets
close exactly once. The helper waits a quiet ten seconds before renaming and
relaunching; earlier module-state polling during teardown caused intermittent
crashes on the test guest and was removed. Do not replace this delay with a
busy poll without fresh lifecycle testing. A crash dialog can still block
unloading and require console recovery.

Use `legacy_win16_self_update`, which
uploads and reads back `LLMNEW.EXE` and `RESTART.EXE` before sending `UPDATE`.
`OK` means the helper launched; a failed `WinExec` returns `ERR` and leaves
the agent running. The helper checks backup removal and every rename, restores
the old application if installation or launch fails, and writes its outcome to
`RESTART.LOG`. A later application crash or loss of networking still requires
local recovery from `LLMAGENT.OLD`.

The bridge waits through a quiet settling period, then requires a changed
startup marker, the expected startup executable SHA-256, and matching installed
file readback. `SYSINFO` publishes `agent_exe`, `agent_started`, and
`agent_sha256`; the hash is captured once at startup. Older replacement builds
without these fields cannot be verified. Update paths must be short absolute
drive paths without whitespace, with a directory length of at most 126
characters (the helper uses unquoted Win16 arguments).


### PSLIST/PSKILL use HTASK as the "PID"

Windows 3.1 has no real process model -- no isolated address spaces, no
PIDs -- but it does have "tasks", and TOOLHELP.DLL (linked via
`toolhelp.lib`, not part of the default `-l=windows` set, same as
`winsock.lib`) is the documented, official API Windows 3.1's own Task
List (Ctrl+Esc) and Ctrl+Alt+Del handler use internally to enumerate and
terminate them. `HTASK` (a 16-bit handle) stands in for a PID -- it's
what `TerminateApp()`/`TaskFindHandle()` key off, and it's what
`PSLIST` reports in the pid column. `PSLIST` resolves each task's full
EXE path via `ModuleFindHandle()` where possible, falling back to
`TASKENTRY.szModule` (an 8-char internal module name) if a module entry
isn't found. Confirmed via direct testing: launching and then
`PSKILL`ing `CLOCK.EXE` removed it cleanly from both `PSLIST` and
`WINLIST` with no crash dialog and no effect on the agent's own
responsiveness -- `NO_UAE_BOX` genuinely suppresses the GPF-style dialog
a forced kill would otherwise show.

### SYSINFO's mem_* fields, and a ToolHelp naming gotcha

`SYSINFO` also reports `mem_total_kb`/`mem_free_kb` (real physical RAM,
via ToolHelp's `MemManInfo()`) and `mem_swapfile_capacity_kb`. That last
one is **not** "how much is currently swapped out" despite how it reads
at a glance -- `MEMMANINFO.dwSwapFilePages` is documented as pages
*available for* the 386 enhanced mode swap file, i.e. its configured
capacity. Confirmed suspicious by testing: it came back as exactly
49152 KB (48MB) on this box, a suspiciously round number for live usage,
consistent with it being a configured size rather than a measurement.

### ⚠ SHUTDOWN doesn't power off the VM -- and EW_RESTARTWINDOWS is a trap

Windows 3.1 predates ACPI/APM in its own API surface -- there is no Win16
equivalent of Win32's `ExitWindowsEx(EWX_POWEROFF)`, and no VMware Tools
exist for a DOS/Win3.1x guest to ask the hypervisor for a real power-off
either. `SHUTDOWN` calls `ExitWindows(0, 0)`, the same thing Program
Manager's File > Exit Windows does: it drops back to a DOS prompt and
stays there, leaving the VM itself running. That's the most "shut
down"-like gesture available on this OS, but treat it as "exit Windows
to DOS", not "power off the machine".

The obvious-looking alternative, `ExitWindows(EW_RESTARTWINDOWS, 0)`, is
a trap -- despite reading like "just exit, don't reboot", it means
exactly what its name says: **restart Windows**. Confirmed by direct
testing: sending it looked identical to `REBOOT` from the console, and
the agent came back on its own moments later with no manual
intervention. `ExitWindows()`'s low word is normally just an MS-DOS
errorlevel; `EW_RESTARTWINDOWS` (0x42) and `EW_REBOOTSYSTEM` (0x43,
which is what `REBOOT` uses) are the only two values it special-cases,
and both mean "reload", not "exit and stay out". Plain `0` is what
actually exits to DOS and stays there.

The status window's **Exit** button and system-menu Close request shutdown.
The server loop owns socket cleanup so message pumping cannot close a socket
twice during a network operation.

### ⚠ EXEC vs EXECDETACH -- this one froze the entire VM, not just the agent

`EXEC` always wraps its command in a virtualized DOS box (`REDIR.EXE
<outfile> <cmd>`, needed to capture stdout -- see below for why it's not
just `COMMAND.COM /c ... > tempfile`). That's correct for a real
DOS-style console command, but a DOS box cannot run a
native 16-bit *Windows* executable (NE format, not an MZ real-mode
EXE) -- asking one to try (`EXEC CONTROL.EXE`, say) doesn't error out,
it froze the whole VMware console once during testing, not just this
agent. **Use `EXECDETACH` for any Windows-format program.** `EXEC` also
now has a 30-second wait cap (a persistent/GUI target that never exits
would otherwise occupy it indefinitely). Expiry means the child was not
cancelled, not necessarily that the command was invalid. Use tracked jobs for
long-running DOS commands.

### ⚠ EXEC's redirection workaround -- why it doesn't just use "> file"

This VM's display driver used to have a severe, pre-existing fragility
around DOS-box video-mode transitions: manually opening a *windowed* DOS
box from Program Manager (not through this agent at all) reproduced
near-unrecoverable video corruption requiring a full reboot. That's
fixed now -- the stock Windows 3.1 `SVGA.DRV` was replaced with
[vbesvga.drv](https://github.com/PluMGMK/vbesvga.drv) (a modern
universal VESA driver; see `C:\SVGANEW` on the box and its own
`SETUP.EXE`), and DOS-box interaction, including full foreground
windows, has been solid since.

Once that stopped being a constraint, a second, unrelated bug surfaced:
**any secondary `COMMAND.COM /c <cmd> > file` invocation on this
specific box crashes with a Windows 3.1 "This application has violated
system integrity due to execution of an invalid instruction" UAE** --
confirmed independent of the target path, independent of
foreground/minimized show state, and independent of which agent command
triggered it. A non-redirected `/c <cmd>` never crashes. The plain-DOS
boot side of this same machine (CONFIG.SYS's `[DOS]` menu item, which
loads no network redirector at all) has never shown this for equivalent
redirected commands -- the leading theory is that WFW's DOS-box
network-redirector hook (`NET START`, loaded in `AUTOEXEC.BAT` before
`WIN`) corrupts the INT 21h handle-duplication sequence `COMMAND.COM`'s
own `> file` parsing relies on.

**Fix**: `REDIR.EXE` (`redir.c`, a separate *DOS-target* build --
`build_redir.bat`, not `build.bat`) does the file redirection itself,
via direct `dup2()`, before ever invoking a shell -- so the child shell
it spawns is never asked to parse `>` at all, sidestepping the buggy
code path entirely. `run_exec()` calls
`WinExec("<exedir>\REDIR.EXE <outfile> <cmdline>", ...)` instead of
building a `COMSPEC /c ... > outfile` line.

That alone wasn't sufficient, though -- it traded the crash for a silent
hang: `REDIR.EXE`'s own nested `system(cmdline)` call would run the
target command correctly (output written to the file was verified
correct) but then never return, leaving the VDM resident forever.
Isolated by testing a no-op sibling (does `dup2()` alone exit cleanly?
yes) and then reintroducing the nested spawn one primitive at a time:
`system()` specifically hangs once this process's own stdout no longer
points at the console; `spawnl(P_WAIT, comspec, comspec, "/C", cmdline,
NULL)` does the equivalent spawn-and-wait directly and does not. Use
`spawnl`, not `system()`, inside any DOS-target helper this agent
spawns.

A second, independent variable also mattered: `REDIR.EXE` must be
launched `SW_SHOWNORMAL` (foreground), not `SW_SHOWMINNOACTIVE`. The
exact same binary and command line that hangs forever when launched
minimized/background exits cleanly in well under a second launched
foreground -- confirmed by toggling only that flag. `SW_SHOWMINNOACTIVE`
was only ever chosen originally to hide the (now-fixed) video
corruption; there's no other reason to keep it, and keeping it
reintroduces the hang.

### KEY/TYPE/CLICK: journal playback and limits

These use a system-wide `WH_JOURNALPLAYBACK` hook. The September 21, 2026
fix supplies a `MakeProcInstance` thunk for the EXE callback, so callbacks
from another task use the agent's data segment. Previously, the hook could
read the wrong queue state, time out, or affect an unrelated application.
Live WFW 3.11 checks now pass for clicking a dialog, named keys, mixed-case
text with spaces/punctuation, and a 216-character multi-batch TYPE request.

TYPE resolves printable ASCII through `VkKeyScan` and `MapVirtualKey`.
Unmappable characters and mappings needing Ctrl/Alt are rejected before
input starts. Text beyond one event queue is sent in complete batches;
it is not silently truncated. A later playback failure can leave partial
text, so inspect the target before retrying. KEY still supports named keys
and the `shift-` prefix; Ctrl/Alt combinations remain unsupported. CLICK uses
1=left, 2=middle, 3=right, matching the bridge (older Win16 builds used 2=right).

Playback still temporarily replaces system-wide input and retains its
three-second wait cap per batch. This is bounded live-test evidence, not
certification across all display drivers, keyboard layouts and modal apps.
Prefer `WINLIST` plus `POSTMSG` for known controls where practical.

The callback setup follows Microsoft's Windows 3.1 Programmer's Reference,
volume 1, section 1.16.2 (instance procedures for EXE-based filter functions).

### ⚠ WINMSG (SendMessage) vs POSTMSG (PostMessage) -- the second wedge

`SendMessage` blocks the *caller* until the entire receiving chain
finishes processing -- including any nested `DialogBox()` call the
message triggers along the way. Double-clicking a Control Panel applet
via `WINMSG` (to open its settings dialog) wedged this agent for as
long as that dialog stayed open, since the agent's own `handle_client()`
was blocked inside the `SendMessage` call. `POSTMSG` (`PostMessage`)
returns the instant the message is queued, and whatever dialog it
triggers opens on the normal message pump's own time, fully decoupled
from the agent. Even a button's own internal click handling notifies
its parent via `SendMessage` synchronously, so `POSTMSG` has to be the
*entry point* -- post `WM_LBUTTONDOWN` then `WM_LBUTTONUP` directly to a
button's own hwnd to press it (no control ID needed), or post a
`WM_COMMAND` carrying an `LBN_DBLCLK` notification to activate a
listbox item. Reserve `WINMSG` for things that can't open a dialog --
reading/setting a checkbox (`BM_GETCHECK`/`BM_SETCHECK`), reading a
listbox's item count, selecting an item (`LB_SETCURSEL`, which does
*not* fire a notification on its own -- pair it with a `POSTMSG`'d
`WM_COMMAND`/`LBN_SELCHANGE` if the parent needs to react, or
`LBN_DBLCLK` to activate it).

### Listbox text and owner-drawn controls

`LBGETTEXT` accepts a standard `LISTBOX` window and a nonnegative item index.
It queries `LB_GETTEXTLEN`, allocates a separate Win16 global-memory block
including the terminating NUL, and rechecks the length after allocation
before requesting the text. The response remains `OK:<text>`, followed by
CRLF. Strings are returned in full rather than truncated to the old
159-byte limit. Text lengths above 32,767 bytes, allocation/lock failures,
invalid handles or indices, non-listbox controls, and embedded NUL/CR/LF
characters return `ERR:`. The limit does not guarantee that a particular
Win16 listbox can store an item that large.

The standard Win16 control reads run consecutively without pumping messages
or yielding. Custom/subclassed controls that change their contents during
these messages are outside this command's contract: `LB_GETTEXT` itself has
no destination-size parameter. The read does not select or alter an item.

Some Win16 UI (Control Panel's own applet grid, and WFW's Startup
Settings "Startup Options" icon row) stores its items in an
owner-drawn listbox with no real text. Without `LBS_HASSTRINGS`,
`LB_GETTEXT` writes a DWORD of item data rather than a string, so the agent
now rejects these controls explicitly. Owner-drawn listboxes that set
`LBS_HASSTRINGS` remain supported. The owner paints icons in response to
`WM_DRAWITEM`, keyed off an opaque per-item data value that's not ours
to interpret. Other controls' captions remain available through `WINLIST`;
`LBGETTEXT` is not a general window-text reader.

To find "which index is X" in an owner-drawn list without documentation
to fall back on, use whatever discoverable side effect exists (Control
Panel usefully repaints a `Text` static control with the hovered/
selected applet's description -- select each index in turn, force the
repaint with a `POSTMSG`'d `LBN_SELCHANGE`, and `WINLIST` the parent
to read the updated description back). Where there's no such tell
(WFW's 3-item Startup/Password/Event Log row had none), fall back to
the visual left-to-right/top-to-bottom order from a `SCREENSHOT`,
matching how dialog resources are conventionally authored.

Regression coverage: `python tests/test_lbgettext.py` compiles the production
handler against host-side Win16 API stubs (requires GCC, or set `CC`). It
checks text sizes through 32,767 bytes, allocation guard bytes, control types,
invalid input, allocation/lock failures, changed lengths, and send failures.
`tests/listbox_fixture.c` supplies native Win16 controls for live testing.
Live WFW 3.11 checks passed for empty strings, 1/159/160/161/4,096-byte items,
owner-drawn controls with and without strings, error responses, and continued
protocol access.
The native control refused the 32,767-byte fixture insertion with
`LB_ERRSPACE`; that upper boundary was tested only in the host harness.

### Idle behavior

The listener uses `WSAAsyncSelect` notifications and `WaitMessage`, rather
than blocking Winsock accept/recv polling. Accepted sockets are nonblocking;
network waits pump messages and obey the shared deadlines. The old idle-CPU
report described a superseded implementation. No custom Winsock blocking hook
is installed. Active requests and DOS child/PIF scheduling can still consume CPU;
this does not promise that every guest/driver combination idles identically.

### Worked example: disabling WFW's network logon prompt

On the tested WFW installation, the network logon prompt appears before
Program Manager starts the agent. For an unattended lab boot, configure the
Network applet's startup settings once Windows is running:

1. Launch `CONTROL.EXE` with EXECDETACH and inspect its windows/children.
2. Find and activate Network. Its applet list can be owner-drawn without
   strings, so inspect the displayed descriptions rather than assuming indices.
3. Open Startup Settings and clear Log On at Startup.
4. Confirm the dialogs, then verify the configuration and a normal reboot.

Use WINLIST to discover current handles and POSTMSG for actions that can open
modal dialogs. Handles and control indices from another boot are not reusable.
The [listbox guidance](#listbox-text-and-owner-drawn-controls) explains how to
inspect owner-drawn controls without treating item data as text.

The tested change wrote `AutoLogon=No` and related Network settings in
SYSTEM.INI, and a subsequent reboot had no prompt. That value skips network
logon; it does not store or supply a network password. Preserve other settings
and verify the behavior on your installation.

**Why less than the DOS agent, network-wise:** WFW already has a real
Winsock 1.1 stack (`WINSOCK.DLL`, Microsoft TCP/IP-32) once its own
`[386Enh]` NDIS chain is loaded (`PROTMAN.DOS`/`NDISHLP.SYS`/the NIC's
NDIS driver) and `NET START` has run -- there is no packet-driver /
Watt-32 equivalent needed here at all. If `WSAStartup` fails, that
network stack isn't up; check `CONFIG.SYS` and that `NET START`
succeeded, not this agent.

**EXEC results:** the current handler observes REDIR's completion and reports
synthetic `EXIT:0`; it does not recover the inner command's real exit code.
It reads at most 8,191 bytes (`EXEC_CAP - 1`) and discards excess output when
cleaning a completed spool. A shorter batch filename can fit the DOS command
tail but does not increase that output limit. Use tracked jobs for explicit
output retrieval/truncation, or redirect to a separate file and download it.
Win16 job completion also reports the inner exit status as unknown.

**Memory model:** one shared 64K near data segment (`-bt=windows`
medium model, no explicit `-mm` needed -- Watcom picks the right default
for this target). All buffers are static/global and sized accordingly;
see the comments in `llm_agent.c` before growing any of them.

Trust model unchanged: cleartext token, lab/host-only network only.

## Build (Open Watcom, off-box)

No Watt-32 build step needed -- this only needs the Open Watcom install
already set up for the DOS agent (see `../agent-dos/README.md`), which
ships its own Win16 SDK headers/import libs (`%WATCOM%\H\WIN`,
`%WATCOM%\LIB286\WIN\winsock.lib`).

```bat
cd C:\src\RetroBridge\agent-win16
build.bat
build_redir.bat
build_restart.bat
```

`build.bat` produces the Win16 agent and DOS-target `JOBRUN.EXE`; the other
scripts build `REDIR.EXE` for EXEC and `RESTART.EXE` for updates. It also emits
`llm_agent.map` (linker map -- if the agent ever GPFs again, search this
for the nearest preceding symbol to the faulting "module:offset"
address to trace it back to a function).

`build.bat` explicitly adds `-i="%WATCOM%\H\WIN"` ahead of the default
include path -- without it, `<windows.h>` resolves to Watcom's `H\NT`
(Win32/NT) copy instead, which is missing every Win16-only symbol this
agent uses (`GetModuleUsage`, `GetFreeSystemResources`,
`GFSR_SYSTEMRESOURCES`, ...) and fails to compile.

**Stack size**: `wcl`'s `-k<size>` flag (the natural thing to reach for)
silently has no effect combined with `-l=windows` for this NE target --
the linked `.exe`'s stack size doesn't change and there's no error.
`build.bat` instead passes wlink's own directive straight through via
`-"OPTION STACK=16384"` (any argument `wcl` doesn't recognize as a
compiler flag gets forwarded to the linker unchanged). Check
`llm_agent.map`'s `STACK` segment size to confirm a change actually
took -- don't trust the flag alone. Also watch `DGROUP`'s total size in
the same map: stack lives inside the one 64K near-data segment
everything else shares, so a stack that's too generous can fail
differently (this build's C runtime failed at startup with "Not enough
memory to allocate file structures" at a 32K stack, `DGROUP` at 0xf010
-- comfortably short of the 64K hard limit on paper, but apparently not
of whatever was actually free in that WFW session at the time).

## WFW deploy

1. Copy to the guest (same directory), e.g. `C:\LLMWIN`:
   - `LLMAGENT.EXE` (rename the host build `llm_agent.exe` to this 8.3 name)
   - `REDIR.EXE` for synchronous EXEC and `JOBRUN.EXE` for tracked jobs
   - `RESTART.EXE` for self-update
   - `LLMAGENT.INI` (from `LLMAGENT.INI.example` -- set a real `token=`;
     this is a **separate file** from the DOS agent's
     `C:\LLMAGENT\LLMAGENT.INI`. For one inventory entry used in both
     mutually exclusive boot modes, keep its token synchronized to both
     files; otherwise use separate profiles for their credentials/addresses.)

2. Make sure WFW's own networking is actually up first -- boot into the
   `WFW` `CONFIG.SYS` menu entry (see `../agent-dos/README.md`'s boot
   menu section) and confirm `NET START` succeeded before launching this.

3. Start it from within Windows -- File Manager double-click, or
   Program Manager's File > Run, pointing at `C:\LLMWIN\LLMAGENT.EXE`.
   A small status window appears showing the listening port, IP address,
   and whether a token is configured; leave it running (minimized is
   fine) rather than closing it.

   To autostart it with Windows instead of launching by hand: append it
   to `WIN.INI`'s `[windows]` `load=` line (loads minimized alongside
   whatever else is already there -- multiple programs on one `load=`
   line are space-separated), drop a shortcut in the `Startup` Program
   Manager group, or change the `WFW` branch of `AUTOEXEC.BAT` to
   `WIN C:\LLMWIN\LLMAGENT.EXE` instead of a bare `WIN`. Do this only
   once you've confirmed the agent runs cleanly by hand first. Note that
   `load=`/`Startup`-group items don't run until any network logon
   prompt is dismissed -- they're both driven by the Program Manager
   shell, which doesn't start until after that (see the worked example
   above if an unattended reboot needs to reach a running agent with
   nobody at the console to dismiss it).

4. Bridge: add a section to `machines.ini` (use a separate DOS
   profile if credentials/addresses differ; one entry can serve both mutually
   exclusive boot modes when their configuration matches):

   ```ini
   [wfw-1]
   host = 192.168.56.12
   exec_port = 2222
   exec_token = REPLACE_WITH_UNIQUE_TOKEN
   ```

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `build.bat` | Win16 agent and DOS JOBRUN helper; emits an agent `.map` |
| `build_redir.bat` / `build_restart.bat` | Build the EXEC and update companions |
| `make_floppy.py` | Builds `llm_agent_win16.flp` with 8.3 names; requires host `pyfatfs`; copy JOBRUN.EXE separately |
| `LLMAGENT.INI.example` | `port=` / `token=` (guest name: `LLMAGENT.INI`) |

## EXEC lifetime follow-up

EXEC now uses an invocation-specific `LXxxxxxx` output directory. Files left
by a timed-out or interrupted invocation are retained for recovery and never
reused by a later EXEC. Missing output does not cause automatic reexecution.
See [long-running commands](../docs/LONG_RUNNING_COMMANDS.md) for wait/status
semantics, retained-file cleanup, and live verification.

## Tracked command jobs

This build advertises `exec_jobs=1` and uses the DOS-target `JOBRUN.EXE`
companion (built by `build.bat`). It retains two job results and permits one
active foreground DOS-box command. Commands must fit 120 encoded bytes.
Completion uses a marker; the inner command's exit code is unknown.
Cancellation is unsupported, and redirected disk output is not capped.
See [job protocol, deployment and limits](../docs/LONG_RUNNING_COMMANDS.md).
