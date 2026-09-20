# llm_agent for FreeDOS

FreeDOS target-agent port of [`../agent-win32/llm_agent.c`](../agent-win32/llm_agent.c).
It is not an MCP server itself; it speaks the **same** token-authed TCP wire
protocol, so [`../mcp-server/server.py`](../mcp-server/server.py) can expose it through
the repo's `legacy_*` MCP tools with no protocol fork.

## What works

| Command | Behavior |
|---|---|
| auth / `PING` / `QUIT` | Same as Windows agent |
| `EXEC` | Shell via `COMSPEC` (`system()`), stdout redirected to a temp file, then `LEN:`/`EXIT:` |
| `PUT` / `GET` | File transfer |
| `SYSINFO` | DOS version, free conventional memory, C: disk space |
| `REBOOT` | Keyboard-controller reset after `OK` |
| `SCREENSHOT` | 80×25 text mode (B800) rendered to a 640×200 24-bit BMP |
| `KEY` / `TYPE` | Stuff BIOS keyboard buffer (INT 16h AH=05h) |

Everything else (`EXECDETACH`, `CLICK`, `WINLIST`, `CLIPSET`, `REG*`,
`PSLIST`, `PSKILL`, `SHUTDOWN`) returns `ERR:not supported on DOS`.

**Limits:** `EXEC` command tails must stay short (~100 chars usable after
redirection). Put long work in a `.BAT` and `EXEC` that. `KEY` modifiers
(`ctrl-`/`alt-`) are accepted for protocol compatibility but only the base
key is injected (BIOS stuffing cannot hold modifiers down). Idle accept/recv
loops issue `HLT` so a VM does not peg a host core; a `DOSIDLE` TSR is
optional and does not replace that (the agent never enters DOS idle).

On the tested FreeCOM installation, nested batch commands can print to the
guest console while EXEC returns empty captured output. Direct `echo`
capture works; do not treat empty batch output as proof that nothing ran.

Trust model unchanged: cleartext token, lab/host-only network only.

## Build (Open Watcom + Watt-32, off-box)

### 1. Build Watt-32 (once)

```bat
git clone https://github.com/gvanem/Watt-32.git C:\Users\admin\code\Watt-32
set "WATT_ROOT=C:\Users\admin\code\Watt-32"
cd /d %WATT_ROOT%\src
configur.bat watcom
```

Do **not** run `owsetenv.bat` before `configur.bat` — it breaks that script’s `IF` tests.

Then build the **large** model library:

```bat
call c:\watcom\owsetenv.bat
set "W32_BIN2C=%WATT_ROOT%\util\win32\bin2c.exe"
set "W32_NASM=%WATT_ROOT%\util\win32\nasm.exe"
wmake -h -f watcom_l.mak
```

You should get `%WATT_ROOT%\lib\wattcpwl.lib` and `%WATT_ROOT%\inc\sys\watcom.err`.

### 2. Build this agent

```bat
cd C:\Users\admin\code\retro-ssh-server\agent-dos
set "WATT32=C:\Users\admin\code\Watt-32"
build.bat
```

(`build.bat` calls `owsetenv.bat` and `wcl`. Prefer it over `wmake` on Windows — Open Watcom’s make treats `\` as an escape and mangled include paths.)

Produces `llm_agent.exe` (DOS MZ).

## FreeDOS VM deploy

1. **NIC:** VMware AMD PCnet (or NE2000 with a matching packet driver).
2. **Packet driver** in `AUTOEXEC.BAT` *before* the agent, e.g.:

   ```bat
   LH PCNTPK INT=0x60
   ```

3. Copy to the guest (same directory):
   - `LLMAGENT.EXE` (built as `llm_agent.exe` on the host — rename on copy, or use the floppy image which already uses 8.3 names)
   - `LLMAGENT.INI` (from `LLMAGENT.INI.example` — set a real `token=`)
   - `WATTCP.CFG` (from `wattcp.cfg.example` — IP + `pkt.vector = 0x60`)
   - `LLMSTART.BAT` (optional timed Agent / Console prompt; included on the floppy)

4. Start after networking is up:

   ```bat
   C:\LLMAGENT\LLMAGENT.EXE
   ```

   Automatic startup is optional. Add `CALL C:\LLMAGENT\LLMSTART.BAT` after networking
   in the startup batch file actually selected by `CONFIG.SYS` or
   `FDCONFIG.SYS` (`FDAUTO.BAT` on many FreeDOS installations). The agent
   occupies the console, so manual startup is appropriate when the local
   prompt is the primary way to use the machine.

   **Memory:** the agent is a large real-mode EXE. Keep TSRs lean (`LH` the packet
   driver). If the console shows `Allocation of DOS memory failed` during
   `EXEC`, FreeCom is short on conventional RAM — rebuild with the size-optimized
   Watt-32 large lib (no `USE_DEBUG`) so the agent stays ~130KB on disk.

5. Bridge: add a section to `machines.ini`:

   ```ini
   [freedos-1]
   host = 192.168.56.20
   exec_port = 2222
   exec_token = your-shared-secret
   ```

## Console access and optional startup

This is a foreground program, not a background service. Local **Ctrl+C**
stops the agent from its idle network loops and returns to the calling
shell; this also ends remote agent access. Ctrl+C was confirmed against the
configured FreeDOS guest during console recovery. While `EXEC` runs a child,
keyboard handling belongs to that child. Run `CALL C:\LLMAGENT\LLMSTART.BAT`
again when finished using the local console, or launch `LLMAGENT.EXE`
directly if its network configuration is already selected.

`LLMSTART.BAT` offers a five-second **Agent / Console** prompt. Press **A**
to start immediately, **C** to keep the shell, or wait for the default Agent
selection. It requires [`CHOICE`](https://help.fdos.org/en/hhstndrd/batch/choice.htm)
on PATH, using the FreeDOS / MS-DOS 6.x `/C:CA /T:A,5` syntax. Only result
2 (Agent) launches it; Console, abort, and error results fall through to
the console. The wrapper checks for the EXE,
INI, and network config, sets `WATTCP.CFG` to the installation directory,
and launches the agent once. Exiting or failing startup returns to the
console; it does not immediately relaunch the agent.

Add this after successful packet-driver/network initialization in your
existing startup file; keep safe/emergency boot paths ahead of this call:

```bat
CALL C:\LLMAGENT\LLMSTART.BAT
```

The default installation directory is `C:\LLMAGENT`. For another location,
pass its absolute 8.3 path without spaces, for example
`CALL D:\LLM\LLMSTART.BAT D:\LLM`. Preserve the rest of your startup file
and do not load the packet driver twice. The floppy's `AUTOEXEC.BAT` is a
sample, not a replacement for an existing installation's startup file.

To update `LLMSTART.BAT` or its calling startup batch, stage the replacement
under another filename, stop the agent and let the batch return to the
console, then replace it. These batches remain active while the agent runs;
overwriting either in place can change the commands the shell reads when
the agent exits. This applies to both FreeDOS and MS-DOS.

The agent now checks Watt-32's initialization status and rejects a zero IP
address before announcing that it is listening. A failed network startup
returns to the console; configure working DHCP or an appropriate static
address before relying on unattended access.

Neither choice provides simultaneous local shell use and remote service.
A local hotkey that suspends networking, opens `COMMAND.COM`, and resumes
the agent when that shell exits would need new agent functionality.

## Dual boot with Windows for Workgroups

On a machine with `DOS` and `WFW` entries in `CONFIG.SYS`, add the startup
wrapper only to the `:DOS` branch of `AUTOEXEC.BAT`, after its packet driver.
Replace a direct `C:\LLMAGENT\LLMAGENT.EXE` launch with
`CALL C:\LLMAGENT\LLMSTART.BAT`. Leave the `:WFW` branch's native NDIS
network startup and `WIN` command alone. The outer OS menu's default is
independent of the Agent / Console default within DOS.

The DOS installation in `C:\LLMAGENT` and the Windows installation in
`C:\LLMWIN` each read their own `LLMAGENT.INI`. Updating one does not update
the other. If both boot modes use one inventory entry, synchronize that
entry's token to both files; otherwise configure separate inventory profiles
for their respective credentials and addresses. Verify networking in each
boot mode, since Windows and DOS use different network stacks.

On the tested VMware PCnet guest, warm restarts from Windows into DOS left
the packet driver unable to communicate, even with a static IP. A full VM
power-off/power-on restored DHCP and authenticated agent access. Loading the
packet driver without `LH` did not resolve the warm-restart failure. If you
encounter this symptom, shut down and power-cycle the guest before selecting
DOS; the startup wrapper does not reset virtual NIC hardware. Also disconnect
non-bootable installation floppy images before a cold boot.

## MCP smoke test checklist

Against a reachable FreeDOS guest with the agent listening:

1. `legacy_list_machines` — see `freedos-1`
2. `legacy_ping(machine="freedos-1")` — success
3. `legacy_sysinfo(machine="freedos-1")` — `os_family=dos`, memory/disk fields
4. `legacy_exec(machine="freedos-1", command="dir")` — directory listing in output
5. `legacy_upload` a small file → `legacy_download` it back — byte-identical
6. `legacy_screenshot` / `legacy_screenshot_file` — readable 80×25 text screen as image
7. `KEY`/`TYPE` only queue BIOS keystrokes. This agent is a foreground program, not a TSR: it does not leave a concurrently usable `COMMAND.COM` prompt, and cannot service the network while `system()` is running a child. Use `legacy_exec` for shell commands; do not treat an input `OK` as proof that an application consumed the keys.
8. Confirm an unsupported tool (e.g. `legacy_winlist`) returns a clear error, not a hang
9. Optional: `legacy_reboot(machine="freedos-1", confirm=True)` — guest resets; start the agent manually, or verify the optional startup wrapper relaunches it with working networking.

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `build.bat` | Recommended Windows build (Open Watcom + Watt-32) |
| `Makefile` | Alternate `wmake` build (path-fragile on Windows) |
| `make_floppy.py` | Builds `llm_agent.flp` with 8.3 names + fixed geometry |
| `LLMSTART.BAT` | Timed startup choice; defaults to Agent, returns to console on exit |
| `LLMAGENT.INI.example` | `port=` / `token=` (guest name: `LLMAGENT.INI`) |
| `wattcp.cfg.example` | Watt-32 IP + packet vector |
