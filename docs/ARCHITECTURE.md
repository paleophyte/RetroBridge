# Architecture

RetroBridge consists of native target agents, a shared Python protocol client,
and an MCP bridge on a modern control machine. The target does not run an LLM
or an MCP server. See the [documentation guide](README.md) for platform builds
and [MCP coverage](MCP_COVERAGE.md) for the exposed operations.

## Why not SSH

The project began with remote access to legacy Windows machines after freeSSHd
conflicted with an MSSQL installation in the lab. It expanded into a common
interface for shell commands where available, file transfer, screenshots,
input, and system inspection across several operating systems.

Native agents provide those operations without requiring a separate SSH or
VNC installation on each guest. This is a small automation protocol, without
terminal sessions, encrypted transport, or SSH compatibility. Existing SSH,
RDP, VNC, and hypervisor consoles can still be useful alongside it.

## Components and platform boundaries

| Component | Responsibility |
| --- | --- |
| `agent-win32/`, `agent-win16/`, `agent-dos/`, `agent-os2/`, `agent-os2-13/`, `agent-netware/`, `agent-mac-system7/` | Native OS operations and TCP command dispatch |
| `common/` | Shared framing, timeout, hashing, and execution helpers; keep it beside the agent directories when building |
| `mcp-server/agent_client.py` | Authentication, argument validation, framing, bounded responses, and explicit text conversion |
| `mcp-server/server.py` | MCP tools, inventory routing, platform guards, and update orchestration |
| `mcp-server/capabilities.py` | Advisory profiles based on SYSINFO; not proof that an arbitrary installed build supports every tool |

Most agents serve one command connection at a time. Jobs can keep work running
between connections on Win32, Win16, and OS/2, but do not make every native
operation concurrent. A blocked OS call can still occupy the command processor.

All ports implement the same authentication and basic framing. Commands and
effects vary: Mac has no shell; DOS/NetWare render text consoles; Win16 uses
task handles; Mac quit/power requests are cooperative. Unsupported commands
return errors. Do not infer another platform's behavior from a Win32 example.

The OS/2 ports are separate builds: 32-bit LX uses Watcom's `h/os2` headers
and SO32DLL/TCP32DLL, while 16-bit NE uses `h/os21x` and TCPIPDLL. Both implement
Presentation Manager capture/input. See their platform READMEs for prerequisites.

## Trust model

The agent token and all commands, file data, screenshots, and replies travel
in cleartext. Possession of the token grants broad control with the agent's
OS privileges. There is no per-operation authorization or untrusted-user
sandbox. Use a unique token per machine and an isolated lab network. Use a
separately secured tunnel or VPN for access across an untrusted network.

Network deadlines limit stalled sockets, not misuse by an authenticated
controller. There is no fairness scheduler or rate limiting. Keep real
configuration outside the checkout and set `LEGACY_MACHINES_FILE` explicitly.
An `agent_enabled = false` inventory entry is descriptive only; the bridge
does not provide SSH access to it.

## Win32 desktop capture and input

`SCREENSHOT` captures the desktop using GDI (`BitBlt` and `GetDIBits`) and
returns a 24-bit BMP. The bridge converts it to PNG. Input uses
`mouse_event`/`keybd_event`; TYPE maps ASCII characters through the guest
keyboard layout. An acknowledgment does not prove that a target application
consumed the input. Some unmappable Win32 characters can be skipped.

Deployment determines which desktop these operations can reach:

- On NT4/2000/XP, `--install` registers an interactive LocalSystem service.
  Desktop automation was tested on the local console. Locked/logon screens
  are not a reliable interactive target, even if a screenshot can capture them.
- An RDP session can differ from the console session seen by that service.
- On Windows 7, run `llm_agent.exe --run` in the logged-in user's session for
  desktop automation. Session 0 services cannot drive that user's desktop.
- On Windows 9x, `--install` registers `RunServices`; `--run` uses
  `RegisterServiceProcess` and detaches its console.

Synthetic `ctrl-alt-del` does not invoke Windows' secure attention sequence.
`legacy_wait_for_desktop` is a Win32 heuristic based on Explorer/windows,
not proof of successful login or readiness of a particular application.

## Win32 OS-family and build choices

The agent uses ANSI APIs and distinguishes Windows 9x from NT-family with
`GetVersion`. EXEC selects COMMAND.COM on 9x and CMD.EXE on NT. Process listing
resolves Toolhelp32 on 9x and PSAPI on NT dynamically; those APIs are not
interchangeable across the target range. A 32-bit agent can fail to resolve
names for 64-bit processes. Memory reporting uses `GlobalMemoryStatus` and
can clamp large physical-memory totals; disk reporting has an older-API fallback.

NT power requests enable the required privilege and use `ExitWindowsEx`.
The 9x implementation launches `rundll32.exe shell32.dll,SHExitWindowsEx`.
Power acknowledgments precede completion; verify the resulting machine state.

The Makefile uses the MSYS2 i686 legacy MSVCRT toolchain, explicit `-march=i486`
and PE OS/subsystem version 4.0. Keep `mingw32/bin` on PATH for compiler
subprocess dependencies as well as the compiler itself. UCRT builds are not
substitutes for this target. The executable imports Winsock 2 and guest CRT/GUI
DLLs, so a loader version stamp alone does not establish stock-OS compatibility.

Compiler flags do not rewrite prebuilt CRT objects. The reviewed build retains
CMOV in startup code and SSE2 in a CRT math-error routine. Real 486/non-Pro
Pentium compatibility is not certified. Validate imports, disassembly, and
the actual guest rather than assuming a toolchain triple proves compatibility.
See [binary provenance](BINARY_RELEASE.md) for recorded build inputs.

## Wire protocol

After connecting, send the token and a line terminator. Authentication returns
`OK` or `FAIL`; failure closes the connection. An authenticated connection can
carry several commands; `QUIT` ends it. The Python client ordinarily opens a
connection per operation. Text uses configured legacy codecs; binary bodies
are opaque bytes. See [text encodings](TEXT_ENCODINGS.md).

Lines end in LF or CRLF. Maximum line content is 4094 bytes on Win32 and
510 on other ports. Malformed, overlong, or incomplete lines fail the session
without dispatching their prefix or queued suffix. See [command framing](COMMAND_FRAMING.md).

| Operation | Request / reply shape |
| --- | --- |
| PING | `PING` → `PONG` |
| EXEC | `EXEC <command>` → zero or more `LEN:<n>` lines each followed by n raw bytes, then `EXIT:<code>`; some ports also return explicit execution errors |
| Detached launch | `EXECDETACH <command>` → `OK pid=<value>` or `ERR:<reason>`; Win16's value is an instance handle |
| Upload | `PUT <path> <size>` immediately followed by exactly size raw bytes → `OK` or `ERR:<reason>` |
| Download / screenshot | `GET <path>` / `SCREENSHOT` → `SIZE:<n>` plus n raw bytes, or `ERR:<reason>` |
| System/process/window queries | SIZE-framed text, with key=value or tab-separated fields as appropriate |
| Input / mutation | Typically `OK` or `ERR:<reason>`; platform-specific acceptance semantics apply |
| Registry | TAB-delimited REGGET/REGSET fields; GET returns DWORD, SIZE-framed text, or an error |

A zero-length LEN can be a heartbeat. A zero-length SIZE is a valid empty
payload. Neither means that a still-running command has completed. A failed
send or a failed file read after SIZE closes the connection instead of
inserting error text into the promised payload. Incomplete responses are errors.

The client validates command arguments before connecting and bounds reply
lines and payloads. Authentication and command lines have absolute deadlines;
transfers have progress deadlines. See [network deadlines](NETWORK_TIMEOUTS.md).
Job commands are documented in [long-running commands](LONG_RUNNING_COMMANDS.md),
and platform extensions in [MCP coverage](MCP_COVERAGE.md). This table is not a
complete per-agent command reference.

## Execution and file semantics

Win32 EXEC starts a fresh shell, captures merged stdout/stderr, and emits
five-second heartbeats while quiet. No working directory or shell variables
persist between calls. Completion follows the direct child, not pipe EOF:
descendants can outlive the shell. On disconnect the agent attempts to terminate
that direct child; this is not process-tree cancellation. Pipe/launch failures
return a diagnostic LEN payload and EXIT:-1, retaining the Windows error code.

Win32 EXECDETACH launches a program directly and returns its PID. Shell syntax
requires explicitly launching a shell or using EXEC. For tracked work and
recoverable output, use the job tools where `exec_jobs=1` is advertised.
Synchronous EXEC timeout, output, and exit-status behavior on other ports differs;
consult the [execution table](LONG_RUNNING_COMMANDS.md#synchronous-exec).

PUT/GET transfer whole files without resume or delta support. Paths can contain
spaces; PUT parses the size from the final field. Native size handling is bounded
by signed 32-bit values, and the client defaults to a smaller 64 MiB response cap.
Mac transfers data forks only; use its MacBinary updater for applications.

PUT success means the declared bytes arrived and no checked write/finalization
error was reported. All ports check short writes and flush/close results; Mac
uses File Manager calls because the reviewed runtime wrappers discard some
native errors. After a local file error, the agent drains the remaining declared
payload when possible to preserve framing. PUT overwrites directly: failure can
leave an empty or partial destination. Stage important replacements separately
and read them back before installation. This is not a power-loss durability guarantee.

## Self-update

A separate helper outlives the old agent and replaces its executable. Use the
platform bridge tool so staging and verification are coordinated:

| Target | Tool and helper | Main limitation / recovery location |
| --- | --- | --- |
| Win32 | `legacy_self_update`, `UPDATE.EXE` | NT helper assumes the installed LLMAgent service; previous executable and update log retained |
| OS/2 2.x | `legacy_self_update`, `UPDATE.EXE` | Independent session, AGENT.PID-based stop, backup and relaunch; WPS startup context required |
| OS/2 1.3 | `legacy_self_update`, `UPDATE.EXE` | Authenticated SELFEXIT using INI host/token, 8.3 backup, independent relaunch; historical intermittent rename failures require inspection |
| Win16 | `legacy_win16_self_update`, `RESTART.EXE` | Quiet teardown wait, staged rename/rollback; LLMAGENT.OLD and RESTART.LOG |
| Mac | `legacy_mac_self_update`, `llm_updater` | Two-fork file exchange; numbered recovery copies and UPDATER.LOG; helper updated separately |
| NetWare | `legacy_netware_self_update`, protocol-2 UPDATE.NLM | Retained transaction directory and readiness checks; loaded/unready candidates require operator recovery |
| DOS | No built-in updater | Stop locally and replace the executable; retain console recovery |

Updates refuse retained jobs, including completed results, until collected and
released. Stage distinct paths, preserve configuration, and keep console access
and an independent backup. Helpers do not provide a universal crash or power-loss
rollback guarantee. Platform READMEs describe the exact file and recovery rules.

The bridge freezes local inputs, checks staging where the protocol permits,
and normally waits for a changed startup instance plus matching startup and
installed-file fingerprints. Windows/OS2/NetWare use executable SHA-256 and
readback; Mac verifies both forks at the same application location, with the
explicit normalization of system-owned resource-header bytes described in
[MCP coverage](MCP_COVERAGE.md#update-result-semantics).

These fingerprints describe files read at startup, not live-memory attestation
or publisher signatures. PING, a build date, or an unchanged hash alone cannot
prove replacement. Older replacement builds without identity fields remain
unverified. Disabling the wait reports acceptance only. An uncertain handoff
is verified rather than automatically launched again. Win7 interactive installs
need a separate stop/relaunch procedure; the service helper cannot supply it.
