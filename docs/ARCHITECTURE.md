# Architecture

## Why not SSH

The original ask was "a modern-ish SSH server for Windows 2000." Nothing
current exists (see the project's origin conversation) — the newest
Cygwin/OpenSSH build that actually runs on Win2000 is OpenSSH 6.2p1 from
around 2013, predating Ed25519/Curve25519/ChaCha20-Poly1305. Bitvise and
similar modern servers refuse to install below XP SP3.

But the real requirement underneath "SSH server" turned out to be narrower:
**an LLM tool-calling harness needs to run commands, move files, see the
screen, and send input on a legacy box.** That's not what SSH is for. SSH's
value is a general-purpose encrypted multiplexed terminal for human
interactive sessions — host key exchange, PTY allocation, agent
forwarding, SFTP subsystem, rekeying. None of that serves a single trusted
automated client on an isolated lab network, and freeSSHd's breakage (it
couldn't coexist with an MSSQL install) is a plausible symptom of exactly
that kind of general-purpose surface: LSA-level auth hooks and a
background service model that other installers step on.

So this project splits the actual requirement into two much smaller,
narrower pieces instead of one do-everything SSH-alike:

**Command execution + file transfer + screenshot + input** all live in one
place — `agent/llm_agent.c` (`llm_agent`): a single-purpose,
single-threaded TCP service. No session multiplexing, no PTY, no
auth-subsystem hooks. Small surface area means small opportunity to
collide with anything else running on the box.

`bridge/server.py` is the piece that actually talks to an LLM harness: an
MCP server, running on your modern control machine, that exposes the
agent's commands as tools.

## Screenshot/input: built into the agent, not VNC (revised)

**Original decision**: use an externally-installed VNC server (TightVNC
1.3.x / UltraVNC) for screen capture and input injection, and have the
bridge speak RFB to it via `vncdotool`. Reasoning at the time: screen
capture and synthetic input on Windows 9x through XP is a solved problem
with decades of hardening (GDI `BitBlt`/`GetDIBits`, `keybd_event`/
`mouse_event`), wrapped in the RFB protocol, and TightVNC/UltraVNC both
trace back to the original ORL/AT&T VNC that targeted exactly this OS
range from the start — so re-deriving that plumbing looked like effort
spent for no real benefit.

**Revised**: that reasoning was aimed at *reimplementing VNC* — streaming
video, multiple wire encodings, a general remote-desktop protocol for
human interactive use. What's actually needed here is much narrower:
on-demand "grab one frame" and "inject one click/key," driven by a single
trusted automated client. That's a handful of well-documented GDI/input
calls, not RFB. Requiring a separate third-party service install also cut
directly against the rest of this design's whole point (see "Why not
SSH" above) — one more service to install, configure, and keep from
colliding with whatever else is running — and in practice was the thing
actually blocking getting this working at all. So `SCREENSHOT`/`CLICK`/
`KEY`/`TYPE` moved into `llm_agent` itself, on the existing token-authed
channel, and `vncdotool`/VNC dropped out of the bridge entirely. Nothing
stops you from *also* running a VNC server or using RDP for your own
independent, human, live view of the box — the agent's tools just don't
depend on one anymore.

Implementation specifics:

- Capture: `BitBlt` off the screen DC into a memory DC, `GetDIBits` into a
  24-bit-forced `BITMAPINFOHEADER` (this does the color-depth conversion
  for us — matters for old 8-bit/256-color palette displays, which need
  no special-case handling as a result). Sent over the wire as a raw BMP;
  the bridge converts to PNG via Pillow before handing it to the MCP
  client, since BMP is uncompressed (a full-screen capture easily runs
  several MB) and PNG shrinks that by ~20x on typical UI content.
- Injection: `mouse_event`/`keybd_event`, not the newer `SendInput` —
  `SendInput` doesn't exist on Windows 9x, and these do, keeping the same
  API surface across the whole 9x-XP range as everything else in the
  agent.
- Key names: `KEY <keyspec>` parses `mod-mod-key` (`ctrl-alt-del`,
  `shift-a`, `alt-tab`), a small named-key table (`enter`, `esc`, `f1`-
  `f12`, arrows, ...) for non-printable keys, and `VkKeyScanA` for single
  ASCII characters (handles which ones need Shift on the current
  keyboard layout). `TYPE <text>` is the same character path, looped.

Two gotchas worth knowing about, both structural rather than bugs:

- **Session 0 / interactive desktop.** Pre-Vista Windows has no Session 0
  isolation, so a LocalSystem service *can* see and drive the logged-on
  user's real desktop — but only if registered with
  `SERVICE_INTERACTIVE_PROCESS` (see `install_nt_service()`). Without it,
  these commands would silently operate against an invisible,
  disconnected window station: `SCREENSHOT` would "succeed" and return a
  blank/black image, `CLICK`/`KEY` would "succeed" and visibly do
  nothing. This isn't unique to rolling this ourselves — VNC-as-a-service
  hits the identical wall on pre-Vista Windows, which is why a lot of
  legacy VNC install guides tell you to run it as a per-user startup app
  instead of a service. It also only works with a user actually logged in
  locally; capturing/driving the Winlogon screen (nobody logged on, or
  the workstation locked) isn't reliable.
- **A synthetic Ctrl+Alt+Del does not trigger the secure Winlogon SAS.**
  Windows intentionally blocks software-simulated Ctrl+Alt+Del from
  reaching the secure attention sequence, specifically so malware can't
  fake it — real VNC and RDP hit this same limitation (RDP's "Send
  Ctrl+Alt+Del" menu item works through a different, privileged path, not
  simple key injection). `KEY ctrl-alt-del` here will not unlock a locked
  screen.
- **Session identity when using RDP.** If you RDP into a box that has
  Terminal Services in remote-administration mode, that RDP session is a
  *different* session from the physical console (session 0) — which is
  what an interactive LocalSystem service touches. `legacy_screenshot`
  would then show the console desktop, not whatever you're looking at
  over RDP. Watching over the VMware/hypervisor console instead doesn't
  have this mismatch, since that *is* the console session.

## Trust model

The agent authenticates with a single pre-shared token sent in the
clear, and the wire protocol itself is unencrypted. This is a deliberate
simplification, not an oversight — it's only defensible because:

- The legacy machine is assumed to sit on an isolated lab/VM host-only
  network, reachable only from your control machine.
- Windows 95/98/NT4 have no usable modern crypto story to build on (no
  CryptoAPI on stock Win95, no CNG until Vista) — doing real transport
  encryption would mean bundling a TLS stack, which reintroduces exactly
  the kind of heavyweight dependency this design is trying to avoid.

This now covers screenshot/click/key/type too, since those went through
the same channel (see "revised" above) rather than a separately-secured
VNC connection.

**Do not expose this port beyond that isolated network.** If the target
machine ever needs to be reachable from a less-trusted network, put a real
VPN/tunnel in front rather than trying to harden the agent protocol itself.

## OS-family handling

Windows 9x and NT-family (NT4/2000/XP) diverge in two places the agent
cares about:

- **Autostart**: NT-family gets installed as a real service via
  `CreateService`/SCM. Windows 9x has no service manager, so the agent
  instead writes a `Run` registry key and, once launched, calls the
  9x-only `RegisterServiceProcess` kernel32 export so it survives logoff
  and stays off the taskbar — the standard pattern legitimate background
  tools of that era used.
- **Unicode**: Windows 9x's wide-char ("W"-suffixed) API entry points are
  mostly unimplemented stubs. The agent is built and linked against the
  ANSI ("A"-suffixed) API surface throughout — no `-DUNICODE`.

Detection is `GetVersion()`'s high bit (set → Windows 9x), which is
reliable across the whole range and doesn't require the XP-only
`VerifyVersionInfo`.

## Toolchain notes (the part that actually breaks silently)

Building something that boots on Windows 95 in 2026 has one real trap:
**the linker's PE subsystem/OS version stamp**. A binary built with a
default modern toolchain gets a subsystem version the old loader compares
against its own version and refuses ("is not a valid Win32 application")
— with a misleading error that looks like a corrupt binary, not a version
mismatch.

The fix, in `agent/Makefile`:

- Use the MSYS2 **`mingw-w64-i686`** environment, not `ucrt64`/`mingw64`.
  UCRT-linked binaries depend on `ucrtbase.dll`, which doesn't exist before
  Vista SP2. The classic i686 toolchain links `msvcrt.dll` instead, which
  has shipped since Win95 OSR2/98/NT4.
- Force `--major-subsystem-version 4 --minor-subsystem-version 0` and the
  matching `--major-os-version`/`--minor-os-version` linker flags. Verified
  with `file llm_agent.exe` → `PE32 executable for MS Windows 4.00 (console)`.
- Verified import table is exactly `ADVAPI32`, `KERNEL32`, `msvcrt.dll`,
  `USER32`, `WS2_32` — no `api-ms-win-*` forwarder DLLs (those are a Win7+
  concept and won't exist on old targets even if the import would
  otherwise resolve).

One more trap worth flagging explicitly: the whole toolchain — not just
`gcc` — needs `mingw32/bin` on `PATH`. `gcc` shells out to `cc1.exe`
(under `mingw32/lib/gcc/...`), which dynamically loads runtime DLLs
(`libwinpthread-1.dll`, `zstd.dll`, etc.) that live in `mingw32/bin`. If
only `gcc`/`ld` are reachable and `mingw32/bin` isn't actually on `PATH`
for the child process, `cc1.exe` fails to load with **no diagnostic
output at all** — the build just silently produces no object file. Looks
identical to "the source has an error the compiler didn't bother to
report," which it isn't.

## Wire protocol (llm_agent)

Line-oriented, one TCP connection per session:

```
client -> server: <token>\n
server -> client: OK\n | FAIL\n            (closes on FAIL)
client -> server: EXEC <cmdline>\n | PUT <path> <size>\n | GET <path>\n
                  | SCREENSHOT\n | CLICK <x> <y> <button>\n | KEY <keyspec>\n
                  | TYPE <text>\n | PING\n | QUIT\n
server -> client (EXEC): (LEN:<n>\n <n raw bytes>)* EXIT:<code>\n
client -> server (PUT):  <size> raw bytes, immediately after the PUT line
server -> client (PUT):  OK\n | ERR:<msg>\n
server -> client (GET):  SIZE:<n>\n <n raw bytes>  |  ERR:<msg>\n
server -> client (SCREENSHOT): SIZE:<n>\n <n raw BMP bytes>  |  ERR:<msg>\n
server -> client (CLICK/KEY/TYPE): OK\n | ERR:<msg>\n
server -> client (PING): PONG\n
```

`EXEC` runs `cmd.exe /C <cmdline>` and streams combined stdout+stderr.
There's no persisted shell state across calls — each `EXEC` is a fresh
`cmd.exe /C`, so `cd` doesn't carry over. Good enough for installer/test
automation; would need a persistent-shell mode if that becomes limiting.

`PUT`/`GET` move a single file per command, whole-file (no resume, no
delta transfer). `PUT`'s `<path>` may contain spaces — it's parsed from
the *right* (last space = the size field), since Windows paths routinely
have spaces (`Program Files`) but the size never does. File sizes are
handled as signed 32-bit values (`GetFileSize`, no high-DWORD result
combined in), so there's a practical ceiling around 2GB — well past
anything from this OS era's installer/driver media, but not meant for
large modern payloads.

`CLICK <x> <y> <button>` moves the cursor and clicks (`button`: 1/2/3 =
left/middle/right). `KEY <keyspec>` presses one key or `mod-mod-key`
combo (`enter`, `ctrl-alt-del`, `shift-a`); see "Screenshot/input: built
into the agent" above for the two gotchas that actually matter
(interactive-service requirement, no synthetic secure-SAS). `TYPE <text>`
is per-character `KEY` in a loop — no newlines in `<text>` (send `KEY
enter` instead), and unmappable characters are silently skipped rather
than erroring the whole command.
