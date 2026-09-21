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
| `KEY` / `TYPE` / `CLICK` | `WH_JOURNALPLAYBACK` input injection -- **unreliable, see warning below** |
| `WINLIST [hwnd]` | Enumerate top-level windows, or a window's children if `hwnd` given -- read-only, always safe |
| `WINMSG <hwnd> <msg> <wparam> <lparam>` | Raw `SendMessage()` -- **never for anything that might open a dialog, see warning below** |
| `POSTMSG <hwnd> <msg> <wparam> <lparam>` | Raw `PostMessage()` -- use this instead of `WINMSG` for button presses / listbox activation |
| `LBGETTEXT <hwnd> <index>` | Read full text from a standard string-backed listbox; size checked before copying |
| `PSLIST` | `TaskFirst`/`TaskNext` + `ModuleFindHandle` (ToolHelp) -- a real Task List view, `<hTask>\t<exe basename>` per line |
| `PSKILL <hTask>` | `TerminateApp(hTask, NO_UAE_BOX)` (ToolHelp) -- same call Task List's "End Task" uses; refuses to kill this agent's own task |
| `UPDATE` | Self-update without a full system REBOOT -- **read the warning below before touching this** |

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

Getting there took three rounds, each surfacing a different failure:

1. **First version** had `UPDATE` call `DestroyWindow()` to reuse the
   Exit button's `WM_DESTROY` cleanup path. That GPFs -- confirmed via
   the linker map, inside the C runtime's own `_exit_` -- because
   `DestroyWindow()` here runs from deep inside `accept()` ->
   `handle_client()` -> `handle_update()`, not from *within* `WndProc`'s
   own `WM_COMMAND` handling the way the Exit button's identical-looking
   call does. Worse, the resulting crash dialog blocked
   `GetModuleUsage()` from ever reaching zero until a human dismissed
   it, defeating the entire point.
2. **Second version** dropped `DestroyWindow()` in favor of manually
   force-closing `g_client`/`g_listen`, mirroring `WM_DESTROY`. That
   froze the *entire desktop*, not just this agent, needing a VM reboot
   to recover -- `server_main()`'s own loop already closes both sockets
   exactly once as `handle_update()`'s synchronous return unwinds
   through it (unlike `WM_DESTROY`'s case, which really is async and
   mid-`accept()`), so this was a silent double-`closesocket()`.
   `WINSOCK.DLL`'s state is shared system-wide across every Win16 app,
   not per-process, which is almost certainly why a bug here didn't
   stay contained to just this agent. Fix: `handle_update()` just sets
   `g_shutdown = 1` and returns -- nothing else.
3. **Third version** (`RESTART.EXE` itself) polled
   `GetModuleHandle("LLMAGENT")`/`GetModuleUsage()` in a tight
   `Yield()`-driven loop waiting for the old instance to unload. That
   was intermittently fatal too -- a different fault each time (a GPF
   inside `_exit_` once, an illegal instruction inside `strpbrk_`
   another time), always right around when this poll loop was actively
   querying the old task's module state while it was mid-teardown.
   Fixed by removing the polling entirely: `RESTART.EXE` now just waits
   a flat 10 seconds, touching nothing about the old task's state at
   all, before launching the fresh copy. Confirmed reliable three times
   in a row after this change (zero successes in a row before it).

Net effect: `UPDATE` now takes about 10 seconds (the flat delay) instead
of the ~2 seconds the polling version achieved when it worked, but it
actually works -- confirmed three clean runs in a row after the fix,
versus roughly 50% of attempts needing a manual GPF-dialog dismiss
before it. If a GPF dialog somehow still appears after `UPDATE` (hasn't
recurred since this fix, but this OS has earned the caveat), it'll block
`RESTART.EXE`'s relaunch the same way it always did -- dismiss it by
hand and the new instance should come up right after, or fall back to
`REBOOT` if it doesn't.

Future bridge-side updates should use `legacy_win16_self_update`, which
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

The status window also has an **Exit** button -- clicking it calls
`DestroyWindow()`, funneling through the exact same `WM_DESTROY`
shutdown path the system menu's Close uses (force-closes both sockets
so `server_main()`'s `accept()`/`recv()` loop actually notices and
exits, rather than leaving a headless process still bound to the port).

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
would otherwise wedge the agent forever waiting for `GetModuleUsage()`
to hit zero) -- hitting that cap is itself a sign the wrong command was
used.

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

### ⚠ KEY/TYPE/CLICK are unreliable -- prefer WINLIST+WINMSG/POSTMSG

These use a system-wide `WH_JOURNALPLAYBACK` hook (the mechanism
`Recorder.exe` uses) since it's the only way to reach a window that
isn't this agent's own without knowing its handle. In practice, across
repeated testing, queued events have often failed to fully deliver
within the wait deadline (`run_journal_events()`, bounded to 3 real
seconds via `GetTickCount()` so a stuck hook can't freeze system input
forever) and have at least twice caused an unrelated foreground app
(Write) to close unexpectedly -- root cause not found; there's no
debugger here, only header-derived documentation. Treat these three as
experimental. **Prefer `WINLIST` + `WINMSG`/`POSTMSG` wherever the
target window's hwnd can be discovered** -- `SendMessage`/`PostMessage`
only ever touch the one window named, with none of the journal hook's
system-wide reach or its failure modes. This is how, e.g., disabling
WFW's network logon prompt via Control Panel was actually automated --
see below.

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

### This agent pins the host CPU while idle -- known, not yet fixed

With this agent running, VMware raises a "Virtual machine CPU usage"
alert for the VM; the alert clears within moments of the agent process
exiting, with nothing else about the guest's workload changing (host
CPU usage drops immediately, confirmed repeatedly). `WQGHLT.386`, a
tiny idle-detection VxD already loaded in `SYSTEM.INI`'s `[386Enh]`
(exactly the DOSIDLE-for-Win3.x equivalent), is not the problem and
doesn't need touching -- root cause is this agent spending nearly all
its life blocked in `accept()`/`recv()`, serviced by 16-bit Winsock's
own default blocking hook, which behaves like a tight polling loop
rather than a real blocking wait. A loop that never truly blocks never
lets the VMM see genuine idle, so `WQGHLT` never gets a chance to `HLT`
regardless of how correct it is.

**A fix was attempted and reverted.** 16-bit Winsock lets an app install
its own blocking hook via `WSASetBlockingHook()`, and the intent was to
replace the default with one that calls `GetMessage()` (a real blocking
wait) instead. It GPF'd inside the compiler's stack-check runtime
(`__STK`, found via `llm_agent.map`) at the *same* address regardless of
whether the stack was 8K, 16K, or 32K -- which rules out simple stack
exhaustion and points at a calling-convention mismatch instead: the
hook's exact required signature (`void FAR PASCAL BlockingHookProc(void)`
was used) was recalled from memory, not verified against real Winsock
1.1 documentation, which wasn't available to check against. If that
signature is wrong, *every* call Winsock makes into the hook corrupts
the stack -- which is consistent with what happened. **Do not reinstall
a custom blocking hook without a verified signature to build it
against.** The 16K stack (up from Watcom's 8K default for this target)
and the Exit button survived the revert and are harmless keepers on
their own.

### Worked example: disabling WFW's network logon prompt

WFW pops an "Enter Network Password" dialog on every boot, before this
agent (or anything else) has a chance to run -- there is no way to
answer it programmatically, since the agent itself only starts after
it's dismissed (see `../agent-dos/README.md`'s boot menu docs for why).
The actual fix is turning the prompt off entirely, which *is*
scriptable once Windows is up, via Control Panel's Network applet:

```python
# 1. Launch Control Panel (EXECDETACH -- it's a native Windows EXE).
client.exec_detach("CONTROL.EXE")

# 2. Find its owner-drawn applet listbox (WINLIST is always safe).
cp = next(w for w in client.winlist() if w.class_name == "CtlPanelClass")
lb = next(w for w in client.winlist(cp.hwnd) if w.class_name == "lb")

# 3. Select "Network" (index found by walking every index with
#    LB_SETCURSEL + a POSTMSG'd LBN_SELCHANGE and reading the resulting
#    description back off the dialog's Text control -- index 9 in this
#    install, may differ if extra software added its own applets).
LB_SETCURSEL = 0x0407
client.winmsg(lb.hwnd, LB_SETCURSEL, 9, 0)

# 4. Activate it with POSTMSG, not WINMSG -- this opens a modal dialog.
WM_COMMAND, LBN_DBLCLK = 0x0111, 2
client.postmsg(cp.hwnd, WM_COMMAND, 20, (LBN_DBLCLK << 16) | lb.hwnd)
time.sleep(2)

# 5. Find the "Startup..." button in the resulting dialog (another
#    owner-drawn listbox here; index 0 confirmed from a SCREENSHOT)
#    and activate it the same way.
netdlg = next(w for w in client.winlist() if w.title == "Microsoft Windows Network")
startup_lb = 16068  # discover via winlist(netdlg.hwnd) -- varies per boot
client.winmsg(startup_lb, LB_SETCURSEL, 0, 0)
client.postmsg(16000, WM_COMMAND, 1, (LBN_DBLCLK << 16) | startup_lb)
time.sleep(2)

# 6. The "Log On at Startup" checkbox is a real Button control this
#    time -- WINMSG (SendMessage) is fine, toggling it opens nothing.
BM_SETCHECK = 0x0401
startup_dlg = next(w for w in client.winlist() if w.title == "Startup Settings")
checkbox = next(w for w in client.winlist(startup_dlg.hwnd) if "Log On at" in w.title)
client.winmsg(checkbox.hwnd, BM_SETCHECK, 0, 0)  # 0 = unchecked

# 7. OK both dialogs -- POSTMSG'd click, not WINMSG, in case OK itself
#    validates/prompts.
def press(hwnd):
    WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
    lp = (8 << 16) | 10
    client.postmsg(hwnd, WM_LBUTTONDOWN, 1, lp)
    client.postmsg(hwnd, WM_LBUTTONUP, 0, lp)

ok1 = next(w for w in client.winlist(startup_dlg.hwnd) if w.title == "1:OK")
press(ok1.hwnd); time.sleep(1.5)
ok2 = next(w for w in client.winlist(netdlg.hwnd) if w.title == "1:OK")
press(ok2.hwnd)
```

This writes `AutoLogon=No` (plus a few sibling keys the dialog owns --
`StartMessaging`, `LoadNetDDE`, `LMLogon`) into `SYSTEM.INI`'s
`[Network]` section and survives reboots. Confirmed via `SYSTEM.INI`
diff and an actual reboot with no prompt. Note the *value format*:
`Yes`/`No` strings, not `0`/`1` -- and the direction is inverted from
what the key name suggests: `AutoLogon=No` is what makes the box skip
the interactive logon step (and its prompt) entirely, not what enables
automatic silent logon.

**Why less than the DOS agent, network-wise:** WFW already has a real
Winsock 1.1 stack (`WINSOCK.DLL`, Microsoft TCP/IP-32) once its own
`[386Enh]` NDIS chain is loaded (`PROTMAN.DOS`/`NDISHLP.SYS`/the NIC's
NDIS driver) and `NET START` has run -- there is no packet-driver /
Watt-32 equivalent needed here at all. If `WSAStartup` fails, that
network stack isn't up; check `CONFIG.SYS` and that `NET START`
succeeded, not this agent.

**EXEC exit codes:** `WinExec()` is fire-and-forget under Win16 -- there
is no `WaitForSingleObject`/exit-code API. The generated batch appends
`echo LLMEXITCODE:%ERRORLEVEL%` as its last line, which this agent peels
back off the captured output before replying. Output is capped at 8KB
(`EXEC_CAP` in `llm_agent.c`); put larger jobs in a `.BAT` and `EXEC`
that file, same guidance as the DOS agent.

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
cd C:\src\retro-ssh-server\agent-win16
build.bat
```

Produces `llm_agent.exe` (16-bit NE executable, ~30KB) and
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
   - `LLMAGENT.EXE`
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
   below if an unattended reboot needs to reach a running agent with
   nobody at the console to dismiss it).

4. Bridge: add a section to `machines.ini` (a separate one from the DOS
   entry, even though the IP is the same host -- port can be shared
   since only one of the two agents is ever running at a time):

   ```ini
   [wfw-1]
   host = 192.168.56.12
   exec_port = 2222
   exec_token = your-shared-secret
   ```

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `build.bat` | Open Watcom build (`-bt=windows`, Winsock 1.1, emits a `.map`) |
| `make_floppy.py` | Builds `llm_agent_win16.flp` with 8.3 names |
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
