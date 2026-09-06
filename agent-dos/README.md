# llm_agent for FreeDOS

FreeDOS target-agent port of [`../agent/llm_agent.c`](../agent/llm_agent.c).
It is not an MCP server itself; it speaks the **same** token-authed TCP wire
protocol, so [`../bridge/server.py`](../bridge/server.py) can expose it through
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

4. Start after networking is up:

   ```bat
   C:\LLM\LLMAGENT.EXE
   ```

   Or add that line to `AUTOEXEC.BAT` after the packet driver.

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

## MCP smoke test checklist

Against a reachable FreeDOS guest with the agent listening:

1. `legacy_list_machines` — see `freedos-1`
2. `legacy_ping(machine="freedos-1")` — success
3. `legacy_sysinfo(machine="freedos-1")` — `os_family=dos`, memory/disk fields
4. `legacy_exec(machine="freedos-1", command="dir")` — directory listing in output
5. `legacy_upload` a small file → `legacy_download` it back — byte-identical
6. `legacy_screenshot` / `legacy_screenshot_file` — readable 80×25 text screen as image
7. `legacy_type(machine="freedos-1", text="echo hi")` then `legacy_key(..., "enter")` — text appears on the DOS console (agent must not be the foreground hog; best when the guest is sitting at a `COMMAND.COM` prompt)
8. Confirm an unsupported tool (e.g. `legacy_winlist`) returns a clear error, not a hang
9. Optional: `legacy_reboot(machine="freedos-1", confirm=True)` — guest resets; start agent again from `AUTOEXEC.BAT`

## Layout

| File | Purpose |
|---|---|
| `llm_agent.c` | Agent source |
| `build.bat` | Recommended Windows build (Open Watcom + Watt-32) |
| `Makefile` | Alternate `wmake` build (path-fragile on Windows) |
| `make_floppy.py` | Builds `llm_agent.flp` with 8.3 names + fixed geometry |
| `LLMAGENT.INI.example` | `port=` / `token=` (guest name: `LLMAGENT.INI`) |
| `wattcp.cfg.example` | Watt-32 IP + packet vector |
