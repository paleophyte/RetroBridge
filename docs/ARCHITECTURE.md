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

## Later additions: process control, system info, power, windows, clipboard, registry

`PSLIST`/`PSKILL`, `SYSINFO`, `REBOOT`/`SHUTDOWN`, `WINLIST`, `CLIPSET`,
and `REGGET`/`REGSET` all followed the same shape as everything above:
narrow, native, on the same channel, instead of depending on CLI tools
that don't exist on this OS range by default (`tasklist.exe`/
`taskkill.exe`/`shutdown.exe`/`reg.exe` are all XP+ ; `sc.exe` isn't
reliably present before 2000). Design specifics that mattered:

- **`PSLIST` has no single enumeration API across the whole range.**
  `CreateToolhelp32Snapshot` covers 9x and 2000+ but *not* NT4 (added for
  Windows 2000). `EnumProcesses`/`GetModuleBaseNameA` (PSAPI) cover
  NT4/2000/XP but `psapi.dll` doesn't exist on 9x *at all*. Both are
  resolved via `LoadLibraryA`/`GetProcAddress` at runtime, never a static
  import — a static import of either would fail to load the *entire
  binary*, not just this feature, on whichever OS family lacks it. Same
  trap `RegisterServiceProcess` already has to work around. Branch is
  `is_windows_9x()`: Toolhelp32 there, PSAPI everywhere else.
  **Verified locally, but the verification itself surfaced a real caveat
  for local testing specifically**: on this 64-bit dev machine, every
  process except the 32-bit `llm_agent.exe` and one 32-bit installer came
  back with no resolvable name (`GetModuleBaseNameA` needs a
  bitness-matched target, and this 32-bit agent can't read module names
  from native 64-bit processes under WOW64). This is a testing artifact,
  not a real-target bug — every genuine 9x/NT4/2000/XP target is 32-bit
  only, no WOW64, no mismatch possible.
- **`SYSINFO`'s memory figures use the old, non-Ex `GlobalMemoryStatus`**
  (present since Win95/NT 3.1, unlike `GlobalMemoryStatusEx`, which
  wasn't universal until 98/2000). It's documented to clamp both
  `dwTotalPhys` and `dwAvailPhys` to 2GB on any machine with more than
  that installed — confirmed locally (`total_phys_mb=2047` on a modern
  box with far more RAM than that). Harmless for every real target here,
  since no legitimate legacy 9x/NT4/2000/XP box has anywhere near 2GB of
  RAM, but worth knowing if `SYSINFO` is ever pointed at something modern
  for testing. Disk space resolves `GetDiskFreeSpaceExA` dynamically
  (missing on original Win95 retail / pre-SP NT4) and falls back to the
  always-present `GetDiskFreeSpaceA`, doing the cluster/sector math by
  hand.
- **`REBOOT`/`SHUTDOWN` need `SeShutdownPrivilege` explicitly enabled**
  on NT-family — LocalSystem doesn't get it by default, unlike 9x, which
  has no privilege model at all and just calls `ExitWindowsEx` directly.
  The privilege-adjustment calls (`OpenProcessToken`/
  `LookupPrivilegeValueA`/`AdjustTokenPrivileges`) are the same class of
  NT-security advapi32 function as the SCM calls `install_nt_service()`
  already relies on being present (if only as no-op compatibility stubs)
  on 9x — see the note in "OS-family handling" below on that assumption's
  actual verification status. **Never tested against any real machine,
  local or remote** — rebooting either the dev machine this was built on
  or `cucm413` mid-session would be a genuinely disruptive, unrequested
  action, not a reasonable thing to do just to prove a `ExitWindowsEx`
  call works. The MCP tools (`legacy_reboot`/`legacy_shutdown`) require an
  explicit `confirm=True` argument as a result — verified that omitting
  it short-circuits before any network call happens at all.
- **`WINLIST`** uses `EnumWindows`/`GetWindowTextA`/`GetClassNameA`/
  `GetWindowRect` — plain user32 exports present since Windows 3.1/95/
  NT 3.1, safe to call directly with no dynamic resolution needed, unlike
  `PSLIST`'s APIs.
- **`CLIPSET`** uses the classic `OpenClipboard`/`GlobalAlloc`+
  `GlobalLock`/`SetClipboardData(CF_TEXT, ...)` sequence from the
  Windows 3.x era, safe across the whole range. Only sets the clipboard —
  pairing it with `KEY ctrl-v` to actually paste is left to the caller,
  matching the small-composable-primitives style `CLICK`/`KEY` already
  use rather than one combined "set and paste" command.
- **`REGGET`/`REGSET` use TAB-delimited wire arguments**, not
  space-delimited like `EXEC`/`PUT`. Registry key paths can contain
  spaces the same way filesystem paths can (e.g. `...\App Paths`), and
  tabs essentially never appear in real key/value names — simpler than
  `PUT`'s right-to-left parsing trick for the same underlying problem.
  `RegOpenKeyExA`/`RegQueryValueExA`/`RegCreateKeyExA`/`RegSetValueExA`
  are safe to link statically across the whole 9x-XP range — the
  registry itself is a core OS feature on both family branches, not an
  NT-only concept merely stubbed for 9x compatibility the way the SCM/
  token functions are. Only `REG_SZ`/`REG_EXPAND_SZ`/`REG_DWORD` are
  supported for now — covers the large majority of legacy app/installer
  registry needs without the added complexity of `REG_BINARY`/
  `REG_MULTI_SZ` handling.

## Bugs found via live testing (not anticipated in advance)

Four real correctness bugs surfaced only once the agent was actually
exercised against a live machine/desktop, not from code review. All are
worth recording since the pattern ("looks right on paper, breaks the
moment something realistic happens") is likely to recur as more of this
OS range gets tested.

**EXEC hung forever, wedging the whole agent, the moment a command
spawned something that outlives it.** The original implementation
`ReadFile`-looped on the redirected pipe until it saw EOF, which requires
*every* handle to the pipe's write end to close. `cmd.exe` inherits that
handle so it can write to it — but if `cmd.exe` itself spawns something
(e.g. `start /b notepad.exe`), that inheritance passes transitively to
the grandchild too, by default, with no way for us to prevent it from our
side of the `CreateProcess` call for `cmd.exe`. Notepad stays open
indefinitely, so the pipe's write end never fully closes, so `ReadFile`
never returns, so `run_exec()` never returns, so `handle_client()` never
returns — and since the agent is single-threaded, the accept loop never
gets back to `accept()` either. One ordinary "launch a GUI app in the
background" command — extremely plausible during real installer
automation — wedges the *entire* agent against *all* future connections,
including `PING`, until the orphaning process is manually closed.

Fix: stop waiting for pipe EOF. Poll the *direct* child (`cmd.exe`/
`command.com`) via `WaitForSingleObject` instead, draining whatever's
currently buffered each poll via `PeekNamedPipe`+`ReadFile` (non-blocking
checks, not the blocking read that caused the hang). We're done as soon
as our own child exits, regardless of what any orphaned grandchild still
holds open. A side effect worth having anyway: a `LEN:0\n` heartbeat
(zero bytes follow — already valid under the existing framing, no
protocol change needed) goes out every 5 seconds of silence so a
long-running-but-currently-quiet command (a silent installer step, say)
doesn't trip a client-side read timeout while it's still legitimately in
progress.

**Reproduced and fixed** — confirmed hung under the original
implementation (`start /b notepad.exe` never returned), confirmed fixed
under the rewrite (returns in ~0.1s, agent stays responsive to a
follow-up `PING` immediately after, `PSLIST`/`PSKILL` find and kill the
spawned process cleanly).

**Windows 9x has no `cmd.exe` at all.** `run_exec()` originally hardcoded
`cmd.exe /C <cmdline>` unconditionally. `cmd.exe` is NT-family only —
Windows 95/98/ME's command interpreter is `COMMAND.COM`. Every `EXEC`
call would have simply failed on a 9x target ("file not found"), which
would have broken nearly everything else too (`PUT`/`GET` verification
and most real workflows lean on `EXEC`). Fixed by branching on
`is_windows_9x()`. `COMMAND.COM` also takes `/C`, so the fix is narrow,
but it has a much smaller command-tail buffer (~127 characters) than
`cmd.exe` — a long `EXEC` command that works fine on NT-family may need
shortening (or writing to a batch file first) to run on 9x. Not yet
tested on real 9x — this is reasoned from documented `COMMAND.COM`
behavior, not verified empirically like the pipe-hang fix above.

**`net stop` on the installed NT service hung, then reported failure,
then refused a second stop attempt** — the exact same underlying pattern
as the `EXEC` pipe-hang, just in the accept loop instead of a pipe read.
`svc_ctrl_handler()` (the callback the SCM invokes to deliver
`SERVICE_CONTROL_STOP`) set `g_running = 0` and reported
`SERVICE_STOP_PENDING`, but that callback runs on the SCM's own
control-dispatch thread — a *different* thread from the one blocked in
`server_main()`'s `accept()` (or, if a client happened to be connected,
`recv()` inside `handle_client()`). Neither blocking call has a timeout
or any way to notice a flag changing on another thread; nothing was ever
going to make them return on their own. Reproduced live on `cucm413`:
first `net stop llmagent` printed "service is stopping........" then
"could not be stopped"; a second attempt failed differently ("service
could not be controlled in its present state," error 2189) because the
SCM now considered a stop already in progress — confirming the process
itself was still alive and genuinely parked, not crashed.

Fix: track the listening socket and the currently-active client socket
(if any) in globals, and have `svc_ctrl_handler()` call `closesocket()`
on both when a stop is requested. Closing a socket that a *different*
thread is blocked in `accept()`/`recv()` on is a documented, valid way to
force that call to return on Winsock — confirmed with a standalone
cross-thread test (a thread blocked in `accept()`, closed from the main
thread after a delay) before touching the real service code: unblocked
within 50ms. Deliberately accepted a small, low-consequence race on
those two globals rather than adding real synchronization — worst case
a stop takes one extra connection-cycle to notice, not a hang, and that
matches the lightweight-single-threaded register the rest of this agent
is written in.

This fix could only be validated for the underlying mechanism locally
(the cross-thread `closesocket()` test above) — actually exercising the
real SCM-integrated `net stop` flow needs an elevated, installed service,
which this dev environment doesn't have. Confirming `net stop` actually
completes cleanly against the real fix still needs to happen on
`cucm413` (or another real target) directly.

Only NT-family goes through this SCM/`net stop` path at all — Windows 9x
never calls `StartServiceCtrlDispatcherA` (see `main()`'s `--run`
handling), so `svc_ctrl_handler` is never registered or invoked there,
and this specific bug couldn't occur on 9x. 9x has no equivalent
"request a graceful stop" mechanism for an ordinary background process in
the first place.

**The agent crashed outright on a real Windows 95 VM** — "This program
has performed an illegal operation and will be shut down," `LLM_AGENT`
executed an invalid instruction at a specific `CS:EIP`, with a full
register dump. This is the most severe bug found in this project: not a
hang, an actual fault, and the first time the whole 9x code path had ever
been exercised against real hardware/a real VM rather than just reasoned
about. The fault bytes (`66 0f ef c0`) decode to `pxor %xmm0,%xmm0` — an
SSE2 instruction. SSE2 shipped with the Pentium 4 in 2000, five years
after Windows 95, and this VM's virtual CPU is deliberately configured
without it for period accuracy.

Root cause: the `i686-w64-mingw32-gcc` toolchain (the MSYS2 package
itself, confirmed via `gcc -v`) defaults to `-mtune=generic
-march=pentium4` — nothing this project ever set. At `-O2`, GCC happily
vectorizes plain `ZeroMemory()`/struct-zeroing code into `pxor`/`movups`/
`movdqu`. This was latent in *every* binary built before this fix, on
every target, not something specific to 9x code paths — it simply never
crashed on `cucm413`/`scm201` because those VMs' virtual CPUs still
expose SSE2. Confirmed via `objdump -d`: dozens of SSE2 instructions
scattered through functions with completely ordinary `ZeroMemory()`
calls (`run_exec`, `handle_sysinfo`, etc.) — this was never confined to
one function or one command.

Fix, in two parts:

1. `agent/Makefile` now passes `-march=i486` explicitly, which disables
   MMX/SSE/SSE2 (and the Pentium-Pro-only `cmov`) for anything compiled
   fresh from `llm_agent.c`.
2. That alone wasn't enough — `-march` only affects code GCC compiles
   from source in this build, not object code already sitting in the
   toolchain's own prebuilt static libraries (`-static-libgcc` links
   those in verbatim). `objdump` after step 1 still showed `cmov`/`xmm`
   instructions, all inside mingw's own `__mingw_pformat`/`__gdtoa`/D2A
   (its printf-family float-formatting internals) — pulled in wholesale
   the moment *anything* in the program references `_snprintf`/`printf`/
   `sscanf`, regardless of which format specifiers are actually used at
   runtime, since the linker can't know a `%s`-only call site will never
   need the float path. `_snprintf` (introduced alongside `EXECDETACH`,
   used on every `EXEC`/`EXECDETACH` call — the hot path) and `sscanf`
   (in `handle_click`) were removed entirely and replaced with
   hand-written string/int parsing (`build_shell_command`'s manual
   `memcpy` concatenation, `parse_int`). `printf` (only in `--install`/
   `--uninstall`/usage output, never in the service hot path, but
   statically linked into the binary regardless of whether that code
   path runs) was replaced with `wsprintfA`-into-a-buffer plus a new
   `con_msg()` helper wrapping plain `fputs` — formatting via a
   dynamically-resolved user32.dll export instead of statically-linked
   CRT internals.

After both fixes, `objdump -d` shows zero `pxor`/`movups`/`movdqu`/
`punpck` instructions anywhere in the binary. A handful of `cmov`
instructions remain, but only inside mingw's own mandatory CRT startup
internals (thread-local-storage setup, PE image base lookup) that run
unconditionally before `main()` — not something reachable through this
project's own code, and not fixable short of replacing the CRT entry
point entirely, which isn't warranted for a gap this narrow (real 486 or
non-Pro Pentium only; every VM tested against so far has `cmov`, just
not SSE2). Documented as a known, accepted residual limitation rather
than silently ignored.

**The practical lesson, worth restating**: a cross-compiler's *default*
target architecture is not something to trust implicitly just because
the toolchain's triple says "i686" — that triple names an ABI/toolchain
convention, not a hard instruction-set floor, and this MSYS2 package's
actual default (`pentium4`) was five CPU generations newer than anything
in scope for this project. `-march` needs to be pinned explicitly, and
verified by disassembly, not assumed from the target triple.

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

Windows 9x and NT-family (NT4/2000/XP) diverge in several places the
agent cares about:

- **Autostart**: NT-family gets installed as a real service via
  `CreateService`/SCM. Windows 9x has no service manager, so the agent
  instead writes a `Run` registry key and, once launched, calls the
  9x-only `RegisterServiceProcess` kernel32 export so it survives logoff
  and stays off the taskbar — the standard pattern legitimate background
  tools of that era used.
- **Unicode**: Windows 9x's wide-char ("W"-suffixed) API entry points are
  mostly unimplemented stubs. The agent is built and linked against the
  ANSI ("A"-suffixed) API surface throughout — no `-DUNICODE`.
- **Command interpreter**: NT-family uses `cmd.exe`; Windows 9x has no
  `cmd.exe` at all and uses `COMMAND.COM` instead (also takes `/C`, but
  with a much smaller ~127-character command-tail buffer). `EXEC`
  branches on this — see "Bugs found via live testing" below, since this
  one was a real, shipped bug, not a proactively-handled gotcha.

Detection is `GetVersion()`'s high bit (set → Windows 9x), which is
reliable across the whole range and doesn't require the XP-only
`VerifyVersionInfo`.

**An assumption still riding on unverified ground**: `install_nt_service()`
statically links `OpenSCManagerA`/`CreateServiceA` and friends, and the
new `REBOOT`/`SHUTDOWN` privilege-adjustment code statically links
`OpenProcessToken`/`AdjustTokenPrivileges` — all NT-security concepts.
The working assumption is that Windows 9x's `advapi32.dll` exports these
as documented no-op compatibility stubs (specifically so apps built
against them don't fail to *load* on 9x, even though calling them there
does nothing/returns an error), which is genuinely how Microsoft
documented 9x's compatibility shims of this era to work. But it hasn't
been verified empirically on real 9x yet, unlike the Toolhelp32/PSAPI
split above (which *is* dynamically resolved specifically because the
equivalent assumption for *that* pair of APIs is false). If a real 9x
boot ever fails to load `llm_agent.exe` at all — not just fails a
specific command, but won't start — this static-link assumption is the
first thing to check.

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
client -> server: EXEC <cmdline>\n | EXECDETACH <cmdline>\n
                  | PUT <path> <size>\n | GET <path>\n
                  | SCREENSHOT\n | CLICK <x> <y> <button>\n | KEY <keyspec>\n
                  | TYPE <text>\n | PSLIST\n | PSKILL <pid>\n | SYSINFO\n
                  | REBOOT\n | SHUTDOWN\n | WINLIST\n | CLIPSET <text>\n
                  | REGGET\t<root>\t<subkey>\t<valuename>\n
                  | REGSET\t<root>\t<subkey>\t<valuename>\t<type>\t<data>\n
                  | PING\n | QUIT\n
server -> client (EXEC): (LEN:<n>\n <n raw bytes>)* EXIT:<code>\n
                         (a bare LEN:0\n with no bytes may appear as a
                         heartbeat during a long-running, currently-quiet
                         command - see "Bugs found via live testing")
server -> client (EXECDETACH): OK pid=<pid>\n | ERR:<msg>\n
client -> server (PUT):  <size> raw bytes, immediately after the PUT line
server -> client (PUT):  OK\n | ERR:<msg>\n
server -> client (GET):  SIZE:<n>\n <n raw bytes>  |  ERR:<msg>\n
server -> client (SCREENSHOT): SIZE:<n>\n <n raw BMP bytes>  |  ERR:<msg>\n
server -> client (CLICK/KEY/TYPE): OK\n | ERR:<msg>\n
server -> client (PSLIST): SIZE:<n>\n <n raw bytes of "<pid>\t<name>\r\n" lines>
server -> client (PSKILL): OK\n | ERR:<msg>\n
server -> client (SYSINFO): SIZE:<n>\n <n raw bytes of "key=value\r\n" lines>
server -> client (REBOOT/SHUTDOWN): OK\n | ERR:<msg>\n
server -> client (WINLIST): SIZE:<n>\n <n raw bytes of
                            "<hwnd>\t<x>\t<y>\t<w>\t<h>\t<class>\t<title>\r\n" lines>
server -> client (CLIPSET): OK\n | ERR:<msg>\n
server -> client (REGGET): DWORD:<value>\n | SIZE:<n>\n <n raw bytes> | ERR:<msg>\n
server -> client (REGSET): OK\n | ERR:<msg>\n
server -> client (PING): PONG\n
```

`EXEC` runs `cmd.exe /C <cmdline>` (or `command.com /C <cmdline>` on 9x)
and streams combined stdout+stderr. There's no persisted shell state
across calls — each `EXEC` is a fresh interpreter invocation, so `cd`
doesn't carry over. Good enough for installer/test automation; would need
a persistent-shell mode if that becomes limiting. Completion is detected
by our direct child process exiting, not by the pipe reaching EOF — see
"Bugs found via live testing" for why that distinction matters.
If the client disconnects while `EXEC` is still running, the next output
send or heartbeat send fails; the agent terminates its direct child and
returns to the accept loop so one abandoned command cannot permanently
consume the single connection slot.

`EXECDETACH` launches a program directly — deliberately *not* through
`cmd.exe`/`command.com` — and returns immediately after `CreateProcess`
succeeds, with the actual launched program's PID. Use it for GUI
programs, browser launches, or background helper scripts that write
their own log file; use normal `EXEC` for anything needing shell syntax
(`&&`, `%VAR%` expansion, redirection, built-ins like `dir`), since it
intentionally waits for the direct child to exit.

First implementation wrapped `EXECDETACH` through the shell the same way
`EXEC` does, for consistency. Caught before it was ever committed, via
the same kind of live-testing habit that found the `EXEC`/`net stop`
bugs above: `cmd.exe /C foo.exe` spawns `foo.exe` as *cmd.exe's own
child* rather than replacing it, so the PID `CreateProcess` hands back is
cmd.exe's, not the target's. Confirmed concretely —
`EXECDETACH("notepad.exe")` reported cmd.exe's PID; `PSKILL` on that PID
killed cmd.exe while notepad.exe kept running, orphaned and untracked.
That's exactly the "spawn via `EXEC`, then `PSLIST`-match by name to find
the real PID" workaround this command exists to eliminate — a shell-
wrapped `EXECDETACH` doesn't actually solve the problem it was built for.
Fixed by skipping the shell wrapper entirely for this one command; the
lost shell features aren't things GUI apps / browser launches /
standalone helpers typically need anyway.

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

`PSLIST` returns `<pid>\t<name>` per running process; `PSKILL <pid>` force
-terminates one, with no protection against killing critical processes
(including the agent's own) — same trust model as `EXEC` already allowing
arbitrary commands. `SYSINFO` returns `key=value` lines describing the OS/
hardware. `REBOOT`/`SHUTDOWN` wrap `ExitWindowsEx`; `OK` is sent *before*
the machine actually goes down, since `ExitWindowsEx` only needs to
signal the shutdown sequence to start. `WINLIST` returns one line per
visible top-level window with a non-empty title. `CLIPSET <text>` sets
the clipboard (pair with `KEY ctrl-v` to paste). `REGGET`/`REGSET` are the
two commands with TAB-delimited arguments instead of space-delimited —
see "Later additions" above for why.
