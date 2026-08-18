# retro-ssh-server

A toolchain for giving an LLM agent hands-on access to legacy Windows boxes
(95/98/ME/NT4/2000/XP) on an isolated lab network: run shell commands,
transfer files, take screenshots, send mouse/keyboard input. Built after
`freeSSHd` turned out to break other software (couldn't install MSSQL
alongside it) — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for why
this isn't "just use a different SSH server."

Two pieces:

- **`agent/`** — `llm_agent`, a single small C service you cross-compile
  and copy onto the legacy machine. One token-authed TCP channel does
  everything: runs commands, moves files, captures the screen, sends
  mouse/keyboard input. No third-party software required on the legacy
  box — screenshots/input are built straight into the agent with GDI and
  `mouse_event`/`keybd_event`, not a separately-installed VNC server. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#screenshotinput-built-into-the-agent-not-vnc-revised)
  for why (that started as a VNC-based design and changed).
- **`bridge/`** — an MCP server you run on your modern control machine,
  exposing the agent's commands as MCP tools.

If you also want to *watch* the box yourself, live, independent of the
LLM tooling — RDP or your hypervisor's console both work fine and don't
need anything from this repo. See the RDP-vs-console-session caveat in
ARCHITECTURE.md if you use RDP for that, though: it can end up looking at
a different session than what `legacy_screenshot` sees.

**Trust model**: no transport encryption, just a shared token per
machine. Only put this on an isolated host-only/lab network — see
[docs/ARCHITECTURE.md#trust-model](docs/ARCHITECTURE.md#trust-model).

## 1. Build the agent

Requires the MSYS2 `mingw-w64-i686` environment (not `ucrt64`/`mingw64` —
those target Vista+):

```bash
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-binutils
```

```bash
cd agent
PATH="/c/msys64/mingw32/bin:$PATH" make
```

(`make` invokes `i686-w64-mingw32-gcc`, which itself shells out to
`cc1.exe`; that binary's runtime DLLs live in `mingw32/bin`, so that
directory has to be on `PATH` for the *whole* build, not just for finding
`gcc` itself — a silent, no-error-message failure otherwise.)

This produces `agent/llm_agent.exe`, a 32-bit PE binary stamped for
Windows 4.0 (95/NT4-era) so the loader on old targets will actually accept
it. Sanity-check with `file llm_agent.exe` — should read
`PE32 executable for MS Windows 4.00 (console)`.

## 2. Deploy to each legacy machine

Repeat this for every legacy box you want the bridge to reach — each one
needs its own copy of the agent and, ideally, its own token.

Copy `llm_agent.exe` and `agent/llm_agent.ini.example` (renamed to
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
and the RDP-session caveat). On Windows 9x, `--install` adds a `Run`
registry key instead — it'll start on next logon, or run it immediately
with `llm_agent.exe --run`.

If you already had an older `llm_agent.exe` installed as a service, run
`llm_agent.exe --uninstall` first and reinstall — the interactive-process
flag and the newer commands only take effect on a fresh service
registration, not an in-place binary swap.

## 3. Set up the bridge

```bash
cd bridge
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

[winxp-1]
host = 192.168.56.11
exec_token = a-different-long-random-shared-secret
```

The section name (`win2k-1`, `winxp-1`, ...) is what you pass as the
`machine` argument to every tool below. `exec_port` is optional (defaults
to `2222`).

**Put your real `machines.ini` outside the repo**, e.g.
`~/.retro-ssh-server/machines.ini`, and point `LEGACY_MACHINES_FILE` at
it (rather than the in-repo default of `bridge/machines.ini`). This isn't
just tidiness — anything under `bridge/` is fair game for scratch/test
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
      "command": "C:\\path\\to\\retro-ssh-server\\bridge\\.venv\\Scripts\\python.exe",
      "args": ["C:\\path\\to\\retro-ssh-server\\bridge\\server.py"],
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
  useful for finding dialogs/buttons without screenshot-guessing
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
`legacy_reboot`/`legacy_shutdown` are implemented and build cleanly but
have **never been invoked against any real machine** — rebooting a
machine mid-session isn't something to do just to prove the code path
works, so this is reasoned from `ExitWindowsEx`'s documented behavior,
not empirically confirmed. The `confirm=True` gate itself is verified:
omitting it short-circuits before any network call happens at all.

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
