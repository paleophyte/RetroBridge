# retro-ssh-server

A toolchain for giving an LLM agent hands-on access to legacy Windows boxes
(95/98/ME/NT4/2000/XP) on an isolated lab network: run shell commands,
transfer files, take screenshots, send mouse/keyboard input. Built after
`freeSSHd` turned out to break other software (couldn't install MSSQL
alongside it) — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for why
this isn't "just use a different SSH server."

Two pieces:

- **`agent/`** — `llm_agent`, a tiny C exec + file-transfer service you
  cross-compile and copy onto the legacy machine. Token-authed TCP, runs
  commands and moves files, streams output. Nothing else.
- **`bridge/`** — an MCP server you run on your modern control machine. It
  talks to `llm_agent` for shell exec/file transfer and to a VNC server
  (which you install on the legacy box yourself — see below) for
  screenshots and input, and exposes all of it as MCP tools.

Screenshots and input injection deliberately reuse existing,
battle-tested software (TightVNC/UltraVNC) instead of a custom protocol —
see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#why-vnc-not-a-custom-screenshotinput-protocol).

**Trust model**: no transport encryption on the exec/file-transfer channel,
just a shared token. Only put this on an isolated host-only/lab network —
see [docs/ARCHITECTURE.md#trust-model](docs/ARCHITECTURE.md#trust-model).

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

## 2. Deploy to the legacy machine

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
```

On NT-family (NT4/2000/XP) this registers a service (`LLMAgent`) —
start it with `net start LLMAgent`. On Windows 9x it adds a `Run`
registry key instead — it'll start on next logon, or run it immediately
with `llm_agent.exe --run`.

`llm_agent.exe --uninstall` removes either form of autostart.

## 3. Install a VNC server on the legacy machine

Not bundled here — grab **TightVNC 1.3.x** (last line supporting Windows
9x/NT4/2000) or UltraVNC from their official sites, install it on the
target, and set a VNC password. Both run fine in plain polling/GDI capture
mode with no kernel driver required.

## 4. Set up the bridge

```bash
cd bridge
python -m venv .venv
./.venv/Scripts/pip install -r requirements.txt
```

Point it at the legacy machine via environment variables:

```
LEGACY_HOST=192.168.x.x
LEGACY_EXEC_PORT=2222
LEGACY_EXEC_TOKEN=REPLACE_WITH_UNIQUE_TOKEN
LEGACY_VNC_PORT=5900
LEGACY_VNC_PASSWORD=whatever-you-set-in-the-vnc-server
```

Wire it into your MCP client config (e.g. Claude Code) as a stdio server,
e.g.:

```json
{
  "mcpServers": {
    "retro-ssh-server": {
      "command": "C:\\path\\to\\retro-ssh-server\\bridge\\.venv\\Scripts\\python.exe",
      "args": ["C:\\path\\to\\retro-ssh-server\\bridge\\server.py"],
      "env": {
        "LEGACY_HOST": "192.168.x.x",
        "LEGACY_EXEC_TOKEN": "REPLACE_WITH_UNIQUE_TOKEN",
        "LEGACY_VNC_PASSWORD": "whatever-you-set-in-the-vnc-server"
      }
    }
  }
}
```

Exposed tools: `legacy_exec`, `legacy_ping`, `legacy_upload`,
`legacy_download`, `legacy_screenshot`, `legacy_click`, `legacy_key`,
`legacy_type`.

## Status

Exec and file-transfer protocol has been round-tripped locally: `ping`,
`exec`, and a full `put` (upload) → remote `type` → `get` (download)
byte-for-byte cycle, all verified against a live `llm_agent.exe` instance
on this machine. Not yet verified against a real Windows 9x/NT4/2000 VM —
the subsystem-version/import-table checks confirm it *should* load, but
that's not the same as booting it on real old hardware or a
period-accurate VM. Do that before relying on it. The VNC-backed tools
(`legacy_screenshot`/`legacy_click`/`legacy_key`/`legacy_type`) are
untested end-to-end — no VNC server has been stood up yet to test against.
