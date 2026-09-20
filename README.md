# retro-ssh-server

A toolchain for giving an LLM tool-calling client hands-on access to legacy
machines — Windows for Workgroups 3.11, Windows 95/98/ME/NT4/2000/XP,
FreeDOS, OS/2, NetWare, and classic Mac OS (System 7) — on an isolated lab network: run shell commands,
transfer files, take screenshots, send mouse/keyboard input.
Built after `freeSSHd` turned out to break other software (couldn't install
MSSQL alongside it) — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for
why this isn't "just use a different SSH server."

Agents disconnect stalled clients after 10 seconds for authentication,
60 seconds for a command line, or 30 seconds without transfer progress.
These are network deadlines, not command runtime limits. See
[agent network deadlines](docs/NETWORK_TIMEOUTS.md).

Platform support varies: classic Mac OS has no shell, DOS/NetWare expose
text screenshots, and several desktop tools are platform-specific. See the
[capability matrix and publication audit](docs/PUBLICATION_AUDIT.md) for
the current limits, verified coverage, and outstanding issues.

## License and build dependencies

Original project code and documentation are [MIT licensed](LICENSE).
Third-party SDKs, libraries, and guest software retain their own terms; see
[licensing, provenance, and dependency setup](THIRD_PARTY.md). Apple and
Novell SDK inputs are supplied locally and verified against a file manifest.
They are not included in this repository or downloaded by the setup tool.
Binary-release redistribution requirements are reviewed separately.

## Nomenclature

This repo has three distinct layers:

- **Target agents** — the small `llm_agent` binaries that run on the legacy
  machines. These speak this repo's private TCP wire protocol.
- **MCP bridge / MCP server** — `mcp-server/server.py`, which runs on the modern
  control machine and translates MCP tool calls into target-agent protocol
  commands.
- **MCP tools** — the individual `legacy_*` functions exposed by the bridge,
  such as `legacy_exec`, `legacy_screenshot`, and `legacy_upload`.

So the short version is: **run a target agent on each legacy machine; run the
MCP bridge on the modern host; call the `legacy_*` MCP tools from your LLM
client.**

Pieces:

- **`agent-win32/`** — the Win32 target agent: `llm_agent`, a single small C
  service you cross-compile and copy onto the legacy Windows machine. One
  token-authed TCP channel
  does everything: runs commands, moves files, captures the screen, sends
  mouse/keyboard input. No third-party software required on the legacy
  box — screenshots/input are built straight into the agent with GDI and
  `mouse_event`/`keybd_event`, not a separately-installed VNC server. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#screenshotinput-built-into-the-agent-not-vnc-revised)
  for why (that started as a VNC-based design and changed).
- **`agent-dos/`** — FreeDOS target-agent port of the same wire protocol
  (Open Watcom + Watt-32). Supports exec/file transfer/sysinfo/reboot plus text-mode
  screenshot and BIOS keyboard inject. See [agent-dos/README.md](agent-dos/README.md).
- **`agent-os2/`** — OS/2 2.x target-agent port (Open Watcom 32-bit LX +
  SO32DLL + PM). Supports exec/file/sysinfo plus full desktop `SCREENSHOT`.
  See [agent-os2/README.md](agent-os2/README.md).
- **`agent-os2-13/`** — OS/2 1.3 target-agent port, a separate 16-bit NE
  build because OS/2 1.x predates the 32-bit kernel. See
  [agent-os2-13/README.md](agent-os2-13/README.md).
- **`agent-win16/`** — Windows for Workgroups 3.11 target-agent port
  (Win16 + Winsock 1.1). See [agent-win16/README.md](agent-win16/README.md).
- **`agent-netware/`** — NetWare 3.12+ NLM target-agent port (Open Watcom +
  CLIB BSD sockets). See [agent-netware/README.md](agent-netware/README.md).
- **`agent-mac-system7/`** — classic 68k Mac OS target agent (Retro68 +
  MacTCP). Supports data-fork file transfer, screenshots, processes, and
  limited input automation; no shell, cross-application clipboard, or
  window enumeration. See [agent-mac-system7/README.md](agent-mac-system7/README.md).
- **`mcp-server/`** — the MCP bridge/server you run on your modern control
  machine, exposing target-agent commands as MCP tools.

If you also want to *watch* the box yourself, live, independent of the
LLM tooling — RDP or your hypervisor's console both work fine and don't
need anything from this repo. See the RDP-vs-console-session caveat in
ARCHITECTURE.md if you use RDP for that, though: it can end up looking at
a different session than what `legacy_screenshot` sees.

**Trust model**: no transport encryption, just a shared token per
machine. Only put this on an isolated host-only/lab network — see
[docs/ARCHITECTURE.md#trust-model](docs/ARCHITECTURE.md#trust-model).

## 1. Build a target agent

Requires the MSYS2 `mingw-w64-i686` environment (not `ucrt64`/`mingw64` —
those target Vista+):

```bash
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-binutils
```

```bash
cd agent-win32
PATH="/c/msys64/mingw32/bin:$PATH" make
```

(`make` invokes `i686-w64-mingw32-gcc`, which itself shells out to
`cc1.exe`; that binary's runtime DLLs live in `mingw32/bin`, so that
directory has to be on `PATH` for the *whole* build, not just for finding
`gcc` itself — a silent, no-error-message failure otherwise.)

This produces `agent-win32/llm_agent.exe` and `agent-win32/update.exe` (see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#self-update) — used for
in-place updates, not part of normal deployment), both 32-bit PE binaries
stamped for Windows 4.0 (95/NT4-era) so the loader on old targets will
actually accept them. Sanity-check with `file llm_agent.exe` — should
read `PE32 executable for MS Windows 4.00 (console)`.

## 2. Deploy to each legacy machine

Repeat this for every legacy box you want the bridge to reach — each one
needs its own copy of the agent and its own unique token.

Copy `llm_agent.exe` and `agent-win32/llm_agent.ini.example` (renamed to
`llm_agent.ini`) to the target machine, in the same directory. Edit
`llm_agent.ini`:

```ini
port=2222
token=REPLACE_WITH_UNIQUE_TOKEN
```

Then install it as autostart and start it:

```
llm_agent.exe --install
net start LLMAgent
```

On NT-family (NT4/2000/XP) `--install` registers a service (`LLMAgent`),
launched with `SERVICE_INTERACTIVE_PROCESS` so it can actually see/drive
the logged-on user's real desktop for screenshot/click/key/type — pre-Vista
Windows has no Session 0 isolation, so this works, but only while someone
is logged in locally on the console (see ARCHITECTURE.md for the details
and the RDP-session caveat). On Windows 9x, `--install` adds a `RunServices`
registry key instead — it'll start at the next boot, or run it immediately
with `llm_agent.exe --run`.

If you already had an older `llm_agent.exe` installed as a service, run
`llm_agent.exe --uninstall` first and reinstall — the interactive-process
flag requires updating the service registration. New protocol commands
come from the replacement binary and do not themselves require reinstalling
the service.

## 3. Set up the bridge

```bash
cd mcp-server
python -m venv .venv
./.venv/Scripts/pip install -r requirements.txt
```

List every legacy machine in a `machines.ini` — copy `machines.ini.example`
and fill in one `[section]` per machine:

```ini
[win2k-1]
host = 192.168.56.10
exec_port = 2222
exec_token = REPLACE_WITH_UNIQUE_TOKEN
max_command_bytes = 4094
text_encoding = cp1252
exec_encoding = cp437

[winxp-1]
host = 192.168.56.11
exec_token = a-different-long-random-shared-secret
text_encoding = cp1252
exec_encoding = cp437
```

The section name (`win2k-1`, `winxp-1`, ...) is what you pass as the
`machine` argument to every tool below. `exec_port` is optional (defaults
to `2222`).

The shared client rejects CR, LF, and NUL in command arguments and tokens,
and tabs inside registry fields. Limits count bytes in the selected encoding, including
the command name and separators but excluding the final LF:

Text defaults to strict ASCII. Set `text_encoding` for the guest's general
text and `exec_encoding` for captured shell output (the examples above assume
English Windows ANSI 1252/OEM 437). Win16 also needs `file_encoding = cp437`
for DOS filenames on that locale. `exec_command_encoding` can override EXEC
input separately. `legacy_exec` accepts a per-call `output_encoding` override.
Binary transfers remain bytes; TYPE/KEY are ASCII only. See
[text encodings](docs/TEXT_ENCODINGS.md) for configuration and limitations.

| Setting | Default | Use |
|---|---|---|
| `max_command_bytes` | `510` | Safe for the 512-byte agent line buffers. Set `4094` only for a confirmed Win32 agent with a 4096-byte buffer. |
| `max_response_bytes` | `67108864` (64 MiB) | Maximum SIZE payload or cumulative EXEC output. Raise explicitly for larger downloads, up to `2147483647`. |

Both settings are optional per-machine INI keys and keyword arguments to
`AgentClient`. Commands over the configured limit fail before connecting;
they are not truncated or split. Shell and individual command limits may
be smaller, especially COMMAND.COM's command tail on Windows 9x/DOS.
Response lines are capped at 65536 bytes, allowing Win16's full listbox-text
reply. Invalid framing, oversized payloads, or incomplete replies close the connection.
The payload limit does not rewrite or inspect binary file contents.

Updated target agents also enforce the 510/4094-byte line limits directly,
including for raw TCP clients. Lines must end in LF or CRLF; overflow,
embedded NUL, and misplaced CR close the session without executing the
partial line or queued commands. Binary upload contents remain unchanged.
See [agent command framing](docs/COMMAND_FRAMING.md).

Sections default to enabled agents and require `host` and `exec_token`.
To keep an SSH host or other infrastructure in the same inventory, mark
it explicitly as inventory-only; no dummy token or port is needed:

```ini
[ubuntu-host]
host = 192.168.56.40
agent_enabled = false
```

Inventory-only entries still require `host` and appear in
`legacy_list_machines`, but agent tools reject them before connecting.
Their agent credentials, port, and limit settings are ignored. Other metadata may
remain in the file; this bridge does not provide SSH access. A malformed
enabled agent still rejects the inventory, so missing credentials are not
silently treated as disabled agents. Configuration is cached until the
bridge restarts. Use unique ASCII
tokens (up to 127 characters for compatibility with all ports), never the
example token. Examples, smoke scripts, and floppy builders use
`REPLACE_WITH_UNIQUE_TOKEN`; substitute a private per-machine value before
using them, and keep generated configuration/media out of Git. The Mac
agent reads `token=` and optional `port=` (default 2222) from `LLMAGENT.INI`
beside its executable; see [Mac configuration](agent-mac-system7/README.md#installation-and-configuration).

**Put your real `machines.ini` outside the repo**, e.g.
`~/.retro-ssh-server/machines.ini`, and point `LEGACY_MACHINES_FILE` at
it (rather than the in-repo default of `mcp-server/machines.ini`). This isn't
just tidiness — anything under `mcp-server/` is fair game for scratch/test
files during development on this repo itself, and a real config sitting
at the same default path a quick local test would use is a live token
one `rm`/overwrite away from being gone, with no git history to recover
it from since it's gitignored. Keeping the real file outside the repo
entirely removes that risk.

Wire the bridge into your MCP client config (e.g. Claude Code) as a stdio
server:

```json
{
  "mcpServers": {
    "retro-ssh-server": {
      "command": "C:\\path\\to\\retro-ssh-server\\mcp-server\\.venv\\Scripts\\python.exe",
      "args": ["C:\\path\\to\\retro-ssh-server\\mcp-server\\server.py"],
      "env": {
        "LEGACY_MACHINES_FILE": "C:\\Users\\you\\.retro-ssh-server\\machines.ini"
      }
    }
  }
}
```

If `LEGACY_MACHINES_FILE` isn't set, it falls back to `machines.ini` next
to `server.py` — fine for quick local testing, just not for the real
config (see above).

Exposed tools, all but `legacy_list_machines` taking a `machine` argument
naming the `machines.ini` section to target (call `legacy_list_machines`
first if you don't remember the exact name):

- `legacy_list_machines`
- `legacy_capabilities` — read `SYSINFO` and return advisory platform tools
  and limitations. This describes current source coverage, not negotiated
  support for every installed build. See [MCP coverage](docs/MCP_COVERAGE.md).
- `legacy_exec`, `legacy_exec_detach`, `legacy_ping` — run a command,
  launch a command without waiting, check connectivity. Use
  `legacy_exec_detach` for GUI apps and long-running helpers.
- `legacy_upload`, `legacy_download` — file transfer
- `legacy_screenshot`, `legacy_screenshot_file`, `legacy_click`,
  `legacy_key`, `legacy_type` — screen capture and input injection.
  Use `legacy_screenshot_file` for large screenshots you want saved on
  the control machine instead of emitted into the tool transcript.
- `legacy_ps`, `legacy_kill` — list/terminate processes by PID
- `legacy_sysinfo` — OS version, memory, disk space, computer name
- `legacy_winlist` — visible top-level windows (title/class/position),
  useful for finding dialogs/buttons without screenshot-guessing; Win16
  also accepts `parent_hwnd` to inspect immediate child controls
- `legacy_winmsg`, `legacy_postmsg`, `legacy_lbgettext` — Win16 scalar
  window messages and listbox text. Use Win16 constants; prefer queued
  `postmsg` for actions that could open a modal dialog.
- `legacy_winclose` — OS/2 close/cancel requests for all exact title matches
- `legacy_screens`, `legacy_autoexec`, `legacy_debug` — NetWare console
  screens, startup-load maintenance, and logging. `autoexec` can edit the
  startup file; enabling debug truncates the existing agent log.
- `legacy_double_click`, `legacy_mouse_position` — Mac double-click and
  cursor/button-state queries
- `legacy_clipboard_set` — set the clipboard (pair with
  `legacy_key(machine, 'ctrl-v')` to paste — more reliable than
  `legacy_type` for exact strings like product keys)
- `legacy_reg_get`, `legacy_reg_set` — native registry access
  (`REG_SZ`/`REG_DWORD` only); use instead of `legacy_exec` + `reg.exe`,
  which doesn't exist by default before XP
- `legacy_enable_autologon`, `legacy_disable_autologon` — configure or
  clear NT-family Winlogon autologon. This is the practical way to make
  the agent usable after reboot on a box that otherwise stops at the
  login screen. It stores the password in plaintext in the Winlogon
  registry key, so use it only on isolated lab machines.
- `legacy_wait_for_agent`, `legacy_wait_for_desktop` — poll after a
  reboot until the TCP agent responds, then until the interactive shell
  appears. `legacy_wait_for_desktop` looks for Explorer or visible
  top-level windows, which is a better readiness signal than ping alone.
- `legacy_reboot`, `legacy_shutdown` — **require `confirm=True`**; take
  the target down immediately and interrupt anything in progress on it
- `legacy_self_update` — updates the agent on a machine in place:
  uploads and reads back a new executable and update helper, launches the
  helper detached, then verifies a changed startup identity, matching startup
  SHA-256, and matching installed executable. Requires `remote_dir` (the
  absolute installation directory); current agents report `agent_exe` in
  `SYSINFO`. `legacy_win16_self_update` performs equivalent verification
  through Win16's staged `UPDATE` flow. Replacement builds without identity
  fields report **not verified**; disabling the wait reports acceptance only. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#self-update).
- `legacy_mac_self_update` — sends a MacBinary `.bin` to the installed
  companion updater; `legacy_netware_self_update` — stages and reads back
  the NLM and helper before `UPDATE`. Both report **replacement NOT verified**:
  these agents lack the loaded-image identity needed for the verification
  above. Acceptance or a subsequent PING does not prove replacement.

`legacy_key`/`legacy_type` note: a synthetic `ctrl-alt-del` will not
unlock a locked/secure-desktop screen — that's Windows intentionally
blocking software-simulated Ctrl+Alt+Del, not a bug here (real VNC/RDP
hit the same wall).

Recommended reboot/login workflow for a standalone NT-family lab box:

1. `legacy_enable_autologon(machine, username, password)` (omit `domain`
   to use the target computer name for a local account).
2. `legacy_reboot(machine, confirm=True)`.
3. `legacy_wait_for_agent(machine)`.
4. `legacy_wait_for_desktop(machine)`.
5. `legacy_screenshot_file(machine, "screenshots/after-reboot.png")` or
   `legacy_screenshot(machine)` to verify what the agent can see.

## Status

Exec and file-transfer protocol has been round-tripped locally (`ping`,
`exec`, a full `put`/remote-`type`/`get` byte-for-byte cycle) and against
a real Windows 2000 machine (`cucm413`) end-to-end. The multi-machine
`machines.ini` config has been exercised too — machine listing, a
reachable machine, an unreachable-but-configured one, and an unknown
machine name all produce correct, clean results.

Screenshot/click/key/type are verified end-to-end against a real
installed service on `cucm413`, not just locally: `legacy_screenshot`
returned a genuine, content-rich capture of the live desktop (confirmed
by eye, not just structurally) — proof `SERVICE_INTERACTIVE_PROCESS` is
working and the agent can actually see the interactive session, not a
disconnected window station. `legacy_click` followed by
`legacy_type('x')` produced a visible, verifiable effect: Explorer's
desktop jump-to-icon selected "Xlight Server" (the one icon starting with
`x`), confirmed by diffing before/after screenshots. `legacy_key` was
also exercised on both its error path and a real (harmless,
self-reverting Caps Lock toggle) success path.

`legacy_ps`/`legacy_kill`/`legacy_sysinfo`/`legacy_winlist`/
`legacy_clipboard_set`/`legacy_reg_get`/`legacy_reg_set` are verified
locally (this dev machine) at both the wire-protocol and bridge-tool
level — including a real spawn → list → kill round trip, and a real
registry write → read round trip against a disposable test key. Not yet
tested against `cucm413` or any other real legacy target.
`legacy_reboot` is verified end-to-end against real machines on both OS
families. Windows 9x (`win95`) took three live fix attempts — raw
`ExitWindowsEx` turned out not to work from any process context on real
Windows 9x, regardless of service-registration or message-queue state,
and needed `rundll32.exe shell32.dll,SHExitWindowsEx` instead — see "Bugs
found via live testing" in ARCHITECTURE.md. NT-family (`scm201`, NT4 SP6)
worked correctly on the first attempt with no workaround needed: the
direct `ExitWindowsEx` call, the SCM-installed service surviving the
reboot, and `legacy_enable_autologon`'s Winlogon autologon all confirmed
working together in one pass — real desktop back and confirmed via
screenshot ~30 seconds after the reboot was triggered. `legacy_shutdown`
is implemented and build-clean on both OS families but hasn't been
invoked against a real machine yet. The `confirm=True` gate itself is
verified: omitting it short-circuits before any network call happens at
all.

Testing the process-list/kill code specifically surfaced a real testing
caveat worth knowing about (not a target-environment bug): on this
64-bit dev machine, `legacy_ps` can only resolve names for 32-bit
processes (a 32-bit reader can't read module names from native 64-bit
processes under WOW64) — every genuine 9x/NT4/2000/XP target is 32-bit
only, so this won't happen there. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the details.

One real bug was found and fixed via live testing, not code review:
`legacy_exec` could wedge the *entire* agent — refusing all further
connections, including `legacy_ping` — if the command spawned something
that outlived it (e.g. launching any GUI app in the background). Fixed
by watching the agent's direct child process exit instead of waiting for
the redirected pipe to reach EOF. Reproduced hanging under the old code,
confirmed fixed under the new code (returns in ~0.1s, agent stays
responsive immediately after). See "Bugs found via live testing" in
ARCHITECTURE.md.

Not yet verified against a real Windows 9x/ME/NT4 machine (only Windows
2000 so far) — the subsystem-version/import-table checks confirm the
agent *should* load across the whole range, and the `EXEC` interpreter
now branches correctly for 9x's `COMMAND.COM`, but neither of those is
the same as actually booting on real old hardware or a period-accurate
VM. That's the natural next step.
