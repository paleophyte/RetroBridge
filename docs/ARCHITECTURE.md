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

1. **Command execution + file transfer** — `agent/llm_agent.c`
   (`llm_agent`): a single-purpose, single-threaded TCP service. It does
   exactly three things (run a command and stream the output back, push a
   file, pull a file) and nothing else — no session multiplexing, no PTY,
   no auth-subsystem hooks. Small surface area means small opportunity to
   collide with anything else running on the box.
2. **Screen + input** — a VNC server (TightVNC 1.3.x or UltraVNC) that you
   install yourself. See "Why VNC, not custom" below.

`bridge/server.py` is the piece that actually talks to an LLM harness: an
MCP server, running on your modern control machine, that exposes both
channels as tools.

## Why VNC, not a custom screenshot/input protocol

Screen capture and synthetic input on Windows 9x through XP is a solved
problem with decades of hardening: GDI `BitBlt`/`GetDIBits` for capture,
`keybd_event`/`mouse_event` (or `SendInput` on NT-family) for injection,
wrapped in the RFB protocol. TightVNC and UltraVNC both trace back to the
original ORL/AT&T VNC that targeted exactly this OS range from the start,
so "does it still run on Windows 98" isn't a live question the way it is
for a from-scratch tool.

Rolling a custom equivalent would mean re-deriving all of that GDI/input
plumbing for marginal benefit — the wire protocol isn't the hard part here,
the OS-version-dependent capture/injection code is, and VNC has already
paid that cost. `vncdotool` (Python, MIT) gives the bridge a scripting
client for RFB with exactly the primitives an LLM tool needs
(`captureScreen`, `mouseMove`/`mousePress`, `keyPress`) instead of a raw
protocol implementation.

Net effect: this repo owns the one piece that's actually novel (a minimal
exec/file-transfer channel that won't collide with other software), and
leans on existing, well-tested tools for the rest.

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
- Same applies to VNC: point it at a machine only your control host can
  reach, set a VNC password as a minimum bar, and don't forward the port
  anywhere else.

**Do not expose either port beyond that isolated network.** If the target
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
                  | PING\n | QUIT\n
server -> client (EXEC): (LEN:<n>\n <n raw bytes>)* EXIT:<code>\n
client -> server (PUT):  <size> raw bytes, immediately after the PUT line
server -> client (PUT):  OK\n | ERR:<msg>\n
server -> client (GET):  SIZE:<n>\n <n raw bytes>  |  ERR:<msg>\n
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
