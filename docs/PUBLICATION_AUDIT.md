# Publication audit — 2026-09-14

Audited `0ce30d12c28e8187fd47f6f0ab79a9ed7efb53a1` plus the nine files
already modified in the working tree. This is a source, documentation,
build, and bounded live-behavior review, not a claim of exhaustive safety
or compatibility. **History cleanup is complete; release review remains
open.** Live-token rotations are complete for all 12 configured agent
endpoints. Vendor redistribution questions and functional findings below
still need remediation or explicit disposition.
Commit references to reachable development history use the rewritten IDs.

## Secret and publication findings

1. **Former Mac token removed from this publication history.** The original
   token appeared in `agent-mac-system7/QUICKEYS_CLICK_INVESTIGATION.md`
   across 30 reachable commit snapshots. It was rotated on both ends;
   authentication with the retired value was explicitly rejected. The
   publication copy's history was then rewritten to replace the retired
   value everywhere. Exact-value scans of every remaining Git object and
   a fresh Gitleaks scan passed. The private original checkout and backup
   retain the old history and must not be merged or pushed into this copy.
2. **Shared example token removed; live rotations complete.** Seven configured
   lab entries accepted the old shared example token during the audit.
   This publication history now uses `REPLACE_WITH_UNIQUE_TOKEN` in its
   examples, smoke scripts, and floppy builders. A subsequent live rotation
   assigned unique random tokens to all 12 configured agent endpoints,
   including the corrected XP and separate Windows 95 entries. Each new
   token authenticated and each retired token was explicitly rejected.
   The placeholder is not a credential to deploy. The report deliberately
   omits credential values and network addresses.
3. **Resolved: deployed config names are now ignored.** `LLMAGENT.INI`
   is the correct filename for DOS and Win16; both loaders and deployment
   instructions agree. The original `.gitignore` covered only
   `llm_agent.ini` (a different basename), not `LLMAGENT.INI`. The missing
   rules were added and `git check-ignore` now confirms both deployment
   paths are ignored. No config rename or loader change is needed. No real
   machine inventory or private-key file was found among tracked paths.
4. **Release ownership/provenance is incomplete.** No root license file
   is tracked. Apple SDK headers/source and numerous Novell binaries,
   patch installers, and SDK objects are tracked under `vendor/`.
   `agent-mac-system7/vendor/README.md` itself notes licensing concerns
   around the original MacTCP headers. Record the applicable distribution
   terms and notices or replace these with user-supplied dependencies
   before publishing. This audit did not establish redistribution rights.
   Lab addresses, usernames, VM setup paths, and author metadata also
   remain visible; those are disclosure choices, not credential findings.

### Scan coverage

- Gitleaks 8.30.1, default rules, `git --log-opts=--all`, fully redacted
  output: 68 reachable commits scanned, no rule matches. **That scan
  missed the bespoke Mac token**, which the exact-value scan caught.
- Exact matching of every nonempty configured agent token against all
  712 local Git objects, including unreachable objects, commit messages,
  and binary blobs where applicable. There were 423 blob objects and
  70 commit objects, including the two dropped-stash commits.
- Reviewed credential-like assignments and credential/private-key
  markers across reachable text objects; inspected tracked-file names.
  No additional non-example configured agent token matched. This does
  not rule out unknown, encrypted, compressed, or encoded secrets.
- The repository is not shallow; it has one local branch, one worktree,
  no listed stashes, and no configured remote. Remote-only history,
  hosting artifacts, and other clones were outside this scan.

## Current agent capability matrix

“Implemented” below is source review, not a promise that every OS release
or error path was exercised. Per-port READMEs retain detailed restrictions.

| Agent | Execution | Files / screenshot | Input / windows | Processes | Update |
|---|---|---|---|---|---|
| Win32 | Shell `EXEC`; direct `EXECDETACH` | PUT/GET; desktop BMP | Mouse/key/type, WINLIST, clipboard, registry | List / force kill | EXE helper through `legacy_self_update` |
| DOS | Synchronous `system()`; no detach | PUT/GET; 80×25 text BMP | BIOS key queue only; no mouse/windows/clipboard | Unsupported | No built-in update |
| OS/2 2.x | Shell plus detached session | PUT/GET; PM desktop BMP | PM input, WINLIST/WINCLOSE, clipboard; no registry | List / kill | EXE helper |
| OS/2 1.3 | Shell plus detached child | PUT/GET; PM desktop BMP | PM input, WINLIST/WINCLOSE; no clipboard/registry | PSTAT parsing / kill | EXE helper plus SELFEXIT |
| Win16 | DOS helper `EXEC`; WinExec detach, no real PID | PUT/GET; desktop BMP | Journal input unreliable; window/message/listbox primitives; no clipboard/registry | ToolHelp task handles | UPDATE + RESTART.EXE; staged update changes committed |
| NetWare | Console `system()`; no output capture or detach | PUT/GET; selected console text BMP | StuffKey or console fallback; SCREENS; no GUI windows/clipboard | Unsupported | UPDATE.NLM; no dedicated MCP wrapper |
| Mac System 7 | No shell or detach | Data fork only; main-screen BMP | Left click/double-click, US keyboard; window/clipboard commands refuse; drag incomplete | List / request application quit | MacBinary `UPDATE <size>`; no MCP wrapper |

Power behavior also differs: Win32 and Mac implement reboot/shutdown;
DOS and both OS/2 ports implement reboot but reject shutdown. Win16
shutdown exits Windows to DOS. NetWare shutdown downs the server and
does not implement reboot. These are not interchangeable “power off” tools.

## Functional findings still open

### High priority

- **Fixed: Mac protocol power commands bypassed application-quit negotiation.**
  `HandlePower()` previously called ShutDwnStart/ShutDwnPower. Apple's
  [Shutdown Manager documentation](https://developer.apple.com/library/archive/documentation/mac/pdf/Processes/Shutdown.pdf)
  warns that this skips asking other applications to quit and save their
  work. Normal protocol requests now go through Finder; save/cancel and
  completed power operations were tested live (see the protocol power
  follow-up below). This builds on the earlier Quit-event handling fix.
- **Fixed: Win16 listbox read could overwrite memory.**
  `handle_lbgettext` previously passed a 160-byte buffer to `LB_GETTEXT`
  and clamped the length only after the write. It now queries the length,
  allocates a separate global-memory buffer, and rejects non-string controls.
  Long-item reads and control-type rejection passed live WFW 3.11 tests;
  see the listbox follow-up below for coverage and limits.
- **Fixed: uploads could acknowledge failed writes.** All seven ports
  now check write counts and final flush/close results before acknowledging
  success. Mac `PUT`/`UPDATE` also drain payloads after local errors, and
  failed update transfers cannot launch the updater or quit the agent.
  Host-side fault injection and live checks on all 12 connected endpoints
  passed; see the upload follow-up below. Ordinary `PUT` remains non-atomic
  and can leave a partial destination on failure.
- **Fixed: Mac update could remove the working application with no rollback.**
  The updater now prepares and verifies a separate two-fork file, exchanges
  it with the installed application while retaining a recovery copy, and
  exchanges back if launching the replacement fails. Helper launch errors
  keep the original agent running. A real Process Manager launch failure
  exercised restoration and relaunch on System 7; successful staging cleanup
  also passed. See the rollback follow-up below for coverage and limits.
- **Fixed: the supplied mixed inventory blocked the MCP bridge.**
  SSH-only infrastructure can now use `agent_enabled = false` without a
  dummy token or port. These entries remain visible in the machine list,
  and agent tools reject them before creating a connection. Enabled
  entries still require valid agent configuration. The supplied Ubuntu
  entry was marked inventory-only; a fresh MCP stdio session loaded all
  13 entries and authenticated PING passed on all 12 enabled endpoints.
  See the mixed-inventory follow-up below.

### Compatibility, protocol, and workflow gaps

- **Fixed: NetWare 4 build and runtime failures.** Missing imports were
  corrected, then live 4.11 testing exposed malformed import fields and
  an incompatible mixture of Watcom and Novell runtimes. The current build
  uses Novell's prelude and CLIB throughout and normalizes imports. PING,
  SYSINFO, staged UPDATE, and binary readback passed on 4.11; see the build
  and network-deadline follow-ups.
- **Windows 95's original partial failure is no longer reproducible.**
  The initial audit saw 15-second timeouts on SCREENSHOT and shell EXEC
  despite successful basic commands and file transfers. Both distinct
  Windows 95 clients now pass those original operations repeatedly, through
  both MCP and persistent agent connections. Their installed binaries match
  the later upload-fix deployment. The original cause remains unconfirmed;
  this is a successful retest, not proof of a specific root-cause fix. See
  the Windows 95 follow-up below.
- **Fixed: Win32 EXEC silently failed to launch.** CreatePipe and
  CreateProcessA failures now return a diagnostic LEN payload followed by
  EXIT:-1, preserving the Windows error code across cleanup. The connection
  remains usable after a complete error response. This was not proven to
  cause the original Windows 95 timeout. EXECDETACH retains its separate ERR
  response. See the EXEC launch-error follow-up below.
- **Fixed: Win32 persistent sessions were misframed after errors.** All
  literal text replies and text headers now use `send_cstr`/`send_all`,
  eliminating the 34 remaining length mismatches after the upload fix.
  The original extra-NUL/PONG failure reproduced on all six Win32 endpoints;
  after deployment, each passed 17 error cases followed by two clean PING
  replies on the same connection. Host-side tests also cover every literal
  reply, short sends, and socket failures. See the framing follow-up below.
- **Fixed: Win32 binary replies assumed a complete `send`.** GET,
  screenshots, process/system/window lists, and registry strings now use
  checked `send_all` calls. Send failures shut down the session so queued
  commands cannot append replies to an incomplete payload. GET also closes
  the session on read failure or premature EOF after SIZE. Fault injection
  and live transfer/disconnect checks passed on all six Win32 endpoints;
  see the binary-send follow-up below. This was not proven to cause the
  original Windows 95 screenshot timeout.
- **Fixed: the shared client did not enforce command framing.** All public
  commands now validate CR/LF/NUL and encoded length before connecting;
  registry fields also reject tabs, and numeric arguments require integers.
  Tokens use strict ASCII validation. Response lines and payloads are bounded,
  sizes must be nonnegative decimal integers, and EXEC has a cumulative
  output limit. Invalid or incomplete responses close the socket without
  sending QUIT. Tests and live checks passed; see the client-framing follow-up.
- **Fixed: raw agent line readers accepted truncated or hidden commands.**
  All seven ports now require complete LF/CRLF lines, enforce the advertised
  byte limits, and reject embedded NUL and misplaced CR. Malformed input or
  EOF before LF fails the session without dispatching a partial command or
  processing its suffix. Binary bodies keep their separate readers. See
  [command framing](COMMAND_FRAMING.md) and the follow-up below.
- **Fixed: stalled network clients could occupy single-client agents.**
  All ports now enforce complete authentication/command-line deadlines and
  transfer-progress deadlines, then discard the failed session. See
  [network deadlines](NETWORK_TIMEOUTS.md) and the follow-up below.
- **Executing commands can still occupy single-client agents.** Network
  deadlines do not cancel a running command or interrupt a blocked disk/OS
  call. DOS's synchronous `system()` cannot receive commands or pump its
  network while the child executes. Win32 retains EXEC heartbeats and has
  no total runtime cap. An authenticated client making continued progress
  can also retain the server; fairness and minimum transfer rates remain
  separate work.
- **Text encoding is not portable Unicode.** The bridge uses UTF-8 while
  target APIs consume ANSI/OEM/Mac encodings. Non-ASCII filenames, titles,
  output, and input can be corrupted. ASCII-only checks do not verify
  localized systems. The client-framing fix now rejects non-ASCII tokens
  instead of silently dropping bytes; the earlier inventory fix disabled
  ConfigParser interpolation so percent characters remain literal.
- **Some implemented commands are unreachable through MCP.** The bridge
  registers 26 tools, but provides no tools for Win16 WINMSG/POSTMSG/
  LBGETTEXT or WINLIST's parent argument; NetWare SCREENS/AUTOEXEC/DEBUG/
  UPDATE; OS/2 WINCLOSE; or Mac DBLCLICK/MOUSEPOS/MacBinary UPDATE. Some
  have `AgentClient` methods, others require a custom wire client. A shared
  protocol does not imply complete MCP coverage. There is no capability
  negotiation command or consistent build identifier across all ports.
- **Update verification corrected (2026-09-20).** Both bridge update tools
  now read back staged inputs and require a new startup identity, matching
  startup SHA-256, and matching installed executable. Win16 checks helper
  launch and swap results and attempts rollback on launch failure. Builds
  without startup identity explicitly remain unverified. See the follow-up
  validation entry below and the architecture's Self-update section.
- **NetWare AUTOEXEC corrected (2026-09-20).** Exact, explicit LOAD commands
  replace substring detection. Dependency order is checked, missing CLIBAUX
  is inserted before an existing agent load, and ambiguous/reversed entries
  return ERR without editing. Complete reads and a checked staged replacement
  with retained backup also prevent false success and direct truncation on I/O
  failures. This remains a bounded NCF check, not proof of a successful boot;
  see the NetWare README and follow-up validation below.
- **Mac installation is partly hardcoded.** Port is fixed at 2222, and
  the token fallback path names `MacOS:Desktop Folder:LLMAGENT`. The update
  path uses the current directory. Different volume/folder names and
  alternate startup contexts need explicit instructions or path discovery.
- **Mac screenshots have a source-level width limit.** The header describes
  the full screen width, but each row emits at most 2048 pixels. Wider
  displays produce a short/misframed BMP. Reject unsupported dimensions
  or emit rows in chunks; this was not tested on a live wide Mac display.
- **Build stamps do not establish stock-OS compatibility.** Win32 links
  `WS2_32.dll`, `msvcrt.dll`, and GDI, and its Makefile documents residual
  CMOV instructions in prebuilt CRT startup code. A PE subsystem stamp
  of 4.0 alone does not establish required DLL availability or real
  486/Pentium compatibility. The existing tests cover configured guests.

## Documentation corrections made

- Added the Mac agent and capability/audit link to the main README;
  corrected Windows 9x autostart from Run to RunServices.
- Removed the Mac README's claim that working click/keyboard commands
  were deliberately unimplemented; clarified missing MCP wrappers and
  lack of update rollback.
- Corrected the DOS smoke checklist's implication that this foreground
  agent could type into a simultaneously active command prompt.
- Removed NetWare's contradictory safe-unload instruction and marked
  the NetWare 4 build as currently broken.
- Marked the OS/2 handoff as historical and the architecture protocol as
  principally Win32-oriented. Old debugging narratives are evidence of
  past tests, not current certification of every build.

Remaining examples/tool docstrings sometimes describe Windows behavior
as universal (combined stderr/stdout, direct detach PIDs, power-off, and
Explorer-based desktop readiness). The matrix above qualifies those
claims. Localized input, shutdown/reboot, process termination, GUI input,
and actual self-updates were not retested in this audit.

## Verification performed

Built working-tree source in a private copy with installed toolchains and
the already-available dependencies. Existing workspace binaries were not
overwritten or deployed.

| Build | Result |
|---|---|
| Win32 agent + update helper | Pass, MinGW i686 / i486 flags |
| DOS | Pass; two existing duplicate-symbol warnings from Watt-32/Watcom |
| Win16 + REDIR + RESTART | Pass |
| OS/2 2.x + update/reboot helpers | Pass |
| OS/2 1.3 + update/IOSEG/IORESET | Pass |
| NetWare 3.12 + update helper | Pass |
| NetWare 4 | Initially failed; fixed in the NetWare 4 build follow-up below |
| Mac | Not rebuilt; Retro68 toolchain not exercised on this host |
| Bridge in fresh venv from requirements.txt | Pass, resolved MCP 2.2.0 |

The local preexisting bridge environment uses MCP 2.0.0. Neither it nor
the fresh environment establishes a minimum supported dependency version;
requirements are unpinned. Floppy builders additionally use `pyfatfs`,
which is not in the bridge requirements and was not installed/tested by
the fresh bridge check.

Live checks reached **10 distinct endpoints through 11 inventory entries**.
Windows NT4/2000, one Windows 95 endpoint labelled `winxp`, Windows 7,
DOS, Win16, and three OS/2 endpoints passed basic execution and screenshot
checks. Supported process/window enumeration also passed; DOS correctly
refused those commands. The other Windows 95 endpoint had the failures
above. `dos622` actually reported Win16, and `os2-3` returned the 20.45
kernel version; inventory names are not OS detection.

All 10 reachable endpoints passed a 4,366-byte binary PUT/GET comparison
(all byte values, NUL, and CR/LF), rejected an incorrect token, and answered
a final PING. All audit scratch files were removed and cleanup verified.
Screenshot tests checked BMP framing/dimensions; they did not certify
every screen's visual content. Mac timed out with both 5- and 15-second
timeouts; a final phase-specific probe confirmed timeout during TCP connect,
before authentication. No NetWare endpoint was supplied. Those ports were source/build
review only. The loaded agent binaries were not proven byte-identical to
the newly built source.

## Missing-commit investigation

`git fsck --full --no-reflogs --unreachable` in the original private checkout
found two unreachable commits. These two IDs identify recovery-archive
objects, intentionally absent from the cleaned publication history:

- `c1c11a381dcfff7d9396dca2e508df503548897d`: dropped WIP stash from
  2026-09-06, based on `bb05f2a`.
- `d8d2f9fd9702cd83aac1434ccb8f6e5d055c3dd2`: that stash's index parent;
  its tree is unchanged from the stash base.

The stash's only change is eight `.gitignore` lines for `os2idle` build
outputs. No missing agent implementation was found there. Seventeen
unreachable blobs were recovered and compared: four `.gitignore`
intermediates and thirteen source/document versions. The latter are
intermediate directory-renaming snapshots or older versions superseded by
current commits/working changes, not a newer missing feature branch.
Applying them wholesale would undo newer work.

The nine preexisting modified files contained substantive uncommitted
work: Win16 idle-listener changes and staged-update handling; preservation
of updater backups on Win32/OS2; an OS/2 floppy filename adjustment;
and bridge `vm_name` plus `legacy_win16_self_update`. These changes are
preserved and were subsequently reviewed and committed at the user's request before the history rewrite.

Recovery evidence (unreachable-object pack, blob exports, dropped-stash
patch, and the original working-tree patch) was saved outside the repo
under the private configuration directory's `publication-audit-20260914`
folder. Do not publish that recovery folder: it includes historical source
and private audit context. The recovery investigation itself performed no
reset, checkout, stash apply, cherry-pick, commit, or rewrite; the later
commit/history-cleanup work is recorded below. Work already garbage-collected
or residing only in another clone cannot be recovered from this object
database.

## Follow-up: Mac recovery later on 2026-09-14

At the user's request, restarted the System 7 QEMU process on its Ubuntu
host with its original argv, environment, working directory, disks, TAP
network, and console settings. System 7.5.3 reached the desktop. From the
Ubuntu host, one authenticated session successfully completed PING,
SYSINFO, PSLIST (two processes), and SCREENSHOT (921,654-byte valid BMP).
Direct TCP connections from the Windows control host still timed out.
Immediate reconnects between separate AgentClient calls also encountered
connection refusals; the persistent session worked. These follow-up
results supersede the initial lack of Mac live coverage for those four
commands only; they do not verify updates, input, or file transfers.

## Follow-up: Finder Restart fix on 2026-09-14

Commit `e25f658` enabled `isHighLevelEventAware` for PSKILL without
adding incoming Apple-event dispatch. The agent discarded Finder's Quit
request, preventing normal restart/shutdown from completing. The fix
installs a Quit Application handler and dispatches high-level events from
`Idle()`. Quitting cancels pending MacTCP listen/receive operations and
waits for their parameter blocks to complete before unwinding the stack.
The main loop releases the TCP stream and removes mouse/VBL hooks before
exiting; QUITAGENT and UPDATE share this cleanup.

Built with Retro68 in a separate host directory and deployed via MacBinary
UPDATE. The installed build reports `Sep 14 2026 22:14:05`. A QEMU snapshot
was saved before deployment. Using QEMU's emulated ADB input to operate
Finder's actual Special > Restart menu:

- No client connected: guest restarted and the updated agent answered
  SYSINFO again in 17.9 seconds.
- Authenticated idle client connected (pending TCPRcv): the connection was
  reset, the guest restarted, and SYSINFO answered again in 17.1 seconds.
- After restart, PSLIST showed Finder and llm_agent; SCREENSHOT returned a
  valid 921,654-byte BMP. The desktop had no improper-restart dialog.
- Repeated UPDATE from the fixed build to itself to exercise its shared
  exit cleanup; UPDATE returned OK and the agent returned in 12.9 seconds.

This verifies both asynchronous wait paths. It does not establish that
active file transfers, an in-flight drag, or Finder Shut Down are fully
covered. The prior build was not subjected to a new controlled before/after
menu test; the regression attribution combines the user's reproduction,
commit/source evidence, and successful tests after this fix.

## Follow-up: direct Mac connectivity on 2026-09-14

Packet capture identified Docker's IPv4 FORWARD policy on the Ubuntu QEMU
host as the cause of direct Windows-to-Mac timeouts. The Linux bridge was
subject to iptables filtering (`bridge-nf-call-iptables=1`). Windows SYNs
arrived on the host NIC but did not reach the TAP, while Ubuntu-originated
connections worked. Read-only vCenter inspection confirmed the Ubuntu VM's
effective port policy already allowed promiscuous reception, MAC changes,
and forged transmits; no hypervisor setting was changed.

Two scoped DOCKER-USER rules now allow the current Windows control PC to
reach the Mac on TCP 2222 and permit established return traffic. A dedicated
`retro-mac-forwarding.service` applies them after Docker's snap service and
is enabled for boot, with a PartOf relationship for Docker restarts. The
helper was tested for idempotence and service restart; the Ubuntu host and
Docker were not restarted. The exact control IP and interface names are
host-specific and must be updated if they change. The previous firewall
rules and installed helper are backed up outside the repository.

Direct Windows access passed authentication, PING, SYSINFO, PSLIST, and a
921,654-byte BMP screenshot. Authentication took about 4.5 seconds; a first
four-second diagnostic timeout was too short, whereas the normal 15-second
client timeout succeeded. These results supersede earlier statements that
Mac live tests required running through the Ubuntu host.

## Follow-up: commits and publication-history cleanup

Before committing or rewriting, saved and CRC-verified a complete private
checkout archive, including ignored local configuration and all Git objects.
Also created and verified a Git bundle after committing the pending work.
The original checkout remains private and unchanged by the rewrite.

The five work commits, using their rewritten public IDs, are:

- `675e6570`: Improve Win16 idle handling and stage agent self-updates.
- `6095c373`: Preserve previous Windows and OS/2 agent binaries after updates.
- `bdd9c5e2`: Give the OS/2 floppy image a platform-specific filename.
- `e4f797e8`: Handle Finder Quit events and clean up Mac agent resources.
- `8e8b971c`: Document agent capabilities, audit findings, and Mac recovery fixes.

A separate clone was rewritten with git-filter-repo 2.47.0. Both the retired
Mac token and the old shared example token were replaced in file content
and commit/tag messages. All 73 development commits were retained. Every
rewritten commit's complete file tree was compared with its original,
allowing only the planned substitutions; parent relationships, authors,
and timestamps were preserved. The final documentation commit updates
these references and records the cleanup, bringing this copy to 74 commits.

Verification included Gitleaks 8.30.1 over all publishable history, exact-value
searches for all seven distinct known credentials across every local Git
object (including unreachable objects), Python syntax checks, and Git
integrity checks. No known credential matches or Gitleaks findings remained.
This is bounded verification, not a guarantee about unknown/encoded secrets.
The earlier build/live results still apply because compiled agent code was
unchanged by the rewrite; example and helper token literals were replaced.

The publication copy contains only the master branch, with no tags, remote,
private Codex snapshot refs, deployed inventory, `.env`, or recovery archive.
No GitHub push was performed. Use this cleaned copy for future publication;
fetching or merging the private original can reintroduce the old history.
The private commit map and verification reports were retained for recovery.

Examples and smoke/floppy scripts now contain an explicit placeholder.
Supply a unique real token outside version control before deployment or a
live smoke test. Host-specific QEMU/firewall changes and the live Mac token
rotation reside outside Git and are not deployed by cloning this repository.

## Follow-up: all connected agent tokens rotated

All 12 configured agent endpoints received separate 256-bit random tokens:
four NT-family clients (NT4, Windows 2000, XP, and Windows 7), two Windows
95 clients, FreeDOS, Win16, three OS/2 clients, and the System 7 Mac.
The XP and second Windows 95 inventory addresses were corrected after
identifying the actual endpoints. The SSH-only Ubuntu host was excluded;
no NetWare endpoint was supplied. This rotated agent authentication tokens,
not operating-system passwords or SSH keys.

For every endpoint, the deployed INI was read back, the agent was restarted,
the new token authenticated, the old token received an explicit authentication
rejection, and a fresh connection using the updated private inventory passed
PING. Configuration content apart from the token was preserved. Temporary
restart helpers were removed; Win16's existing executable and prior binary
backup were preserved. Tokens and recovery records remain outside Git.

The restart work also exposed operational constraints:

- Windows 7's registered agent service was disabled; the active agent was
  a normal `--run` process. Restarting the service could not reload its token.
  Restarting the identified process succeeded without changing service setup.
- OS/2 1.3's updater reads its authentication token from disk. Changing the
  INI first would make its SELFEXIT request use the new token against the
  old running process. A delayed restart helper and the already-authenticated
  session avoided that mismatch. The other OS/2 clients also restarted only
  their agents.
- FreeDOS intentionally used manual agent startup to keep its console
  available; this is an operating choice, not a missing installation step.
  A temporary boot entry launched it
  after reboot, but its deployed DHCP configuration did not produce reachable
  networking. Console recovery with an explicit address matching the current
  mTCP DHCP lease restored access; the inventory address changed accordingly.
  The original boot script was restored byte-for-byte and temporary network
  configuration removed. Reliable DHCP configuration for this installed
  binary remains follow-up work. Optional autostart should preserve a way
  to choose the console (see the DOS README); this rotation does not
  establish unattended recovery after another reboot.
- The Mac used the previously verified fixed binary to restart its agent;
  Finder Restart behavior was not changed by token rotation.

## Follow-up: optional DOS startup with console access

Added `agent-dos/LLMSTART.BAT` and included it in the floppy image and sample
startup instructions. CHOICE offers Agent / Console, defaulting to Agent
after five seconds. Console selection, aborted/failed selection, missing
deployment files, and agent exit return to the shell without a restart loop.
The wrapper selects the installation's network configuration and supports
an alternate absolute 8.3 installation directory. DOS batch files on the
generated floppy use CRLF.

The DOS agent now checks Watt-32's initialization status and refuses a zero
IP address before claiming to listen. Built and deployed the updated agent
to the FreeDOS client, preserving the original binary and startup file in
private recovery storage. Its existing DHCP configuration was retained.
The boot hook runs after network initialization and skips the existing
safe and emergency boot choices.

Live verification covered the five-second default, explicit Agent and
Console selections, Ctrl+C returning to the shell, unavailable CHOICE, missing installation
files, and an isolated unavailable-packet-driver configuration returning to
the console. An unattended reboot restored authenticated access and EXEC
in 21.2 seconds; the installed executable matched the build byte-for-byte.
This supersedes the earlier absence of an autostart path on this guest.
It does not test a future DHCP lease/address change. Floppy generation and
readback verified the startup scripts, CRLF endings, placeholder token, and
1.44 MB geometry. No simultaneous local shell/network service or shell
suspend/resume hotkey was implemented.

The console tests also exposed a DOS EXEC output limitation: a nested batch
test returned EXIT 0 with empty captured output while its messages appeared
on the guest console. Direct `echo` output was captured successfully. This
FreeCOM batch-redirection behavior remains unresolved; startup verification
used the VM console and subsequent authenticated connections.

## Follow-up: MS-DOS / Windows for Workgroups dual boot

Confirmed a separate DOS agent installation in `C:\LLMAGENT` on the Windows
for Workgroups test guest. Deployed the same updated DOS executable and
`LLMSTART.BAT` used on FreeDOS, replacing only the DOS branch's direct agent
launch with a `CALL` to the wrapper. Preserved the Windows branch, the
original packet-driver command, DHCP configuration, and the outer boot menu's
Windows default and five-second timeout. Synchronized the separate DOS INI
to the current Windows agent token so the existing private inventory entry
works in either boot mode. Original files remain in private recovery storage.

On MS-DOS 6.22, verified explicit Console and Agent choices, Ctrl+C returning
to the shell, and two cold boots that obtained DHCP and started the updated
agent automatically. Authenticated PING, EXEC, file upload/download, installed
binary readback, and explicit rejection of the old DOS token passed. The
guest was then returned to Windows; its INI, authenticated access, EXEC, and
original default boot configuration were verified.

A remaining guest networking limitation was identified: warm restarts from
Windows into DOS left the PCnet packet driver unable to communicate. Both
DHCP and a temporary static address failed; loading the driver without `LH`
did not help. A full VM power-off/power-on restored networking with the
original DHCP and driver settings. This suggests retained NIC state, but
the precise cause was not proven and no driver fix was made. The DOS README
documents the cold-boot workaround. A connected non-bootable installation
floppy also prevented cold boot until disconnected; its image was preserved.

Changing a still-active startup batch during these checks produced a stray
command when the agent exited, confirming that the staging/console-update
guidance applies to MS-DOS and to the outer startup batch as well as the
wrapper. Final boot files were read back and subsequently boot-tested.

## Follow-up: cooperative Mac protocol power requests

Replaced direct Shutdown Manager calls in `HandlePower()` with Finder's
Restart and Shutdown Apple events. The agent locates Finder by its `MACS`
signature, activates it, yields to complete activation, and sends `FNDR/rest`
or `FNDR/shut` to its process serial number. Explicit activation was needed
on the test guest for reliable subsequent requests after cancelling a save
dialog. Event delivery permits interaction without waiting for a reply.

`OK` acknowledges delivery only. Lookup, descriptor creation, activation,
and send failures return an error; no forced-power fallback is present.
The agent retains its normal Quit-event cleanup and remains available until
Finder asks it to exit. A pending Quit event does not suppress the final
bounded protocol reply while its stream is still valid. The Mac README and
MCP tool descriptions now explain save dialogs, cancellation, and the need
to verify the resulting machine state.

Built with Retro68 and deployed to the System 7.5.3 guest, retaining its
authentication configuration and a private recovery binary. Live tests
used a disposable unsaved SimpleText document: both protocol commands
produced a Save Changes dialog; cancellation retained the document and
authenticated access. Repeating a power request after cancellation worked
with explicit Finder activation. Completed restart restored the agent
through Startup Items in about 16 seconds. Completed shutdown emitted QEMU's
`guest-shutdown` event and exited its process; relaunching the same VM command
restored authenticated access in about 16 seconds. No improper-shutdown
dialog needed dismissal. Finder quit order can still close the agent before
another application cancels; the agent cannot guarantee it remains running
in every cancellation scenario.

`agent-mac-system7/tests/test_power.py` compiles the production power handler
and reply helper against host-side Toolbox stubs. It checks both event IDs,
Finder selection/activation, failure paths, descriptor disposal, and Quit
events during activation or delivery. These fault-injection checks passed;
they do not emulate Finder. The build retains the existing SDK/macro warnings.

## Follow-up: Win16 listbox read bounds

Replaced `handle_lbgettext`'s fixed 160-byte buffer with a size-checked Win16
global-memory allocation. The handler validates the handle/index and standard
LISTBOX class, rejects owner-drawn controls without `LBS_HASSTRINGS`, and asks
for the text length before allocating space including the NUL terminator.
It rechecks the length after allocation and performs the final two standard
control reads without yielding or pumping messages. Far-pointer contents
are copied into the existing near I/O buffer in chunks for transmission;
the global block is unlocked/freed on success and failure.

Valid text is returned in full using the existing `OK:<text>` response.
The handler rejects lengths above 32,767 bytes and embedded NUL/CR/LF
characters, preserving line framing. Custom control procedures that mutate
items during these read messages are outside the supported contract;
`LB_GETTEXT` has no buffer-size parameter. README and client documentation
now distinguish string-backed owner-drawn controls from opaque item data.

The Open Watcom build passed and was deployed through Win16's staged updater.
The installed binary was read back, its INI remained byte-identical, and the
previous working executable was retained as `LLMAGENT.OLD` and in private
recovery storage. A native fixture verified exact empty, 1-, 159-, 160-,
161-, and 4,096-byte reads, owner-drawn string retrieval, and rejection of
owner item data, non-listbox controls, invalid indices/handles, trailing
arguments, and newline-containing text. PING succeeded after each response.
The control refused a 32,767-byte fixture insertion with `LB_ERRSPACE`, so
that boundary was not exercised live. The fixture was closed and removed
after testing; the Windows agent remains running.

Host-side fault injection in `agent-win16/tests/test_lbgettext.py` checks
the production handler through 32,767 bytes with allocation guard bytes,
both owner-draw style flags, malformed/overflowing arguments, allocation and
lock failures, size changes after allocation, malformed results, and send
failures. All passed, including memory cleanup checks. The native fixture
source is retained in `agent-win16/tests/listbox_fixture.c` for reproduction.

## Follow-up: upload write/finalization errors

All seven `PUT` implementations now reject write/finalization failures.
Win32 checks `WriteFile`'s status and exact byte count, `FlushFileBuffers`,
and `CloseHandle`; five stdio ports check exact `fwrite` counts plus
`fflush`, `ferror`, and `fclose`. Mac initially used these stdio checks too,
but the rollback follow-up found discarded errors in the runtime wrappers
and replaced them with checked File Manager calls. NetWare's explicit CLIB import list now
includes `fflush`. A local error stops further writes while consuming the
remaining declared payload. Two Win32 PUT error replies also had their
lengths corrected to exclude the trailing NUL. Other erroneous Win32 send
lengths remain a separate finding.

Mac PUT and UPDATE share the checked receive/finalize path. Open failures
now drain instead of leaving file bytes to be parsed as commands. PUT
rejects negative sizes and overlong paths; UPDATE rejects nonpositive sizes.
A failed UPDATE transfer never launches the helper or requests agent exit.
Failed staging cleanup is attempted and explicitly reported if it fails.
At this stage, the updater's unchecked launch, destructive replacement,
and failed staging cleanup remained unresolved. The rollback follow-up below
supersedes those findings and moves acknowledgment after checked helper launch.

`python tests/test_uploads.py` compiles the production handlers against
host-side I/O stubs for eight paths (seven PUTs plus Mac UPDATE). It covers
empty and multichunk transfers, open failure, short/failed writes, deferred
flush and close errors, sticky stdio errors, interrupted reception, payload
draining, Mac update gating/cleanup failure, and Mac size/path rejection.
All passed, along with the earlier Mac power and Win16 listbox regression
checks. Disk exhaustion and close errors were simulated, not induced on
live disks; these tests do not emulate the target filesystem/runtime.

Native builds passed for DOS, Win16, OS/2 2.x, OS/2 1.3, NetWare 3.12,
Win32, and System 7. Existing DOS linker and Mac SDK/CMake warnings remain.
The separate NetWare 4.x build's previously documented unresolved imports
were not addressed, and no live NetWare server was available for this fix.

Deployed to all 12 connected agent endpoints: Windows 2000, NT4, XP, 7,
two Windows 95 machines, WFW 3.11, FreeDOS, three OS/2 machines, and System
7. Each passed an 8,466-byte binary PUT/GET comparison, an empty-file upload,
and a missing-parent upload followed by PING on the same connection. The
Mac's normal UPDATE also passed using the fixed handler. Non-Mac installed
binaries were read back; Mac's build identity and update log were checked.
All active configurations remained byte-identical, with prior binaries
and configurations preserved privately. Test files and temporary helpers
were removed; the Mac retains its current build in the updater's staging
file because of the existing cleanup failure. The inactive MS-DOS 6.22
installation on the WFW dual-boot guest was also updated/read back, but
was not separately boot-tested during this fix.

PUT still opens its destination for overwrite and offers no transaction or
rollback. A failed transfer can leave an empty, truncated, or partial file;
important replacements should be staged separately and verified before
replacement. Success covers errors reported by the runtime/OS, not a
power-loss durability guarantee. Transport send failures and malformed
command framing remain separate audit findings.

## Follow-up: Mac update preparation, rollback and recovery

Replaced delete-before-create with a prepared-file exchange. The updater
validates a classic MacBinary application header (no secondary header),
rejects invalid/overflowing lengths and truncated or extra payload bytes,
and writes both forks to the first unused `llm_agent.saved.001` through
`.099`. It closes and flushes the candidate, checks fork sizes, and reads
both forks back against the archive before touching the installed agent.
The resource header's system-reserved bytes 16–127 are excluded only after
validating the resource layout: HFS changes this area when saving catalog
recovery metadata. Application bytes, resource data/maps, and the rest of
the header remain subject to comparison. A live exact-byte comparison
initially caught this documented reserved-region behavior at offset 48;
it was not treated as permission to skip resource-fork verification.

`FSpExchangeFiles` swaps both forks while preserving the installed file's
ID, as described in Apple's File Manager reference linked from the Mac
README. This preserves Startup Items aliases and avoids deleting the target.
The numbered file then contains the previous application. Existing numbered
files are never reused or removed automatically; a full set of 99 slots
causes a preparation error. After failed preparation or exchange the updater
attempts to relaunch the installed application. Failed post-exchange flush
or replacement launch triggers an exchange back and relaunch of the old
application. If rollback fails, both files are retained and it tries the
previous application directly from the saved path, logging the need for
manual recovery. Logs distinguish failed updates from accepted launches.

The agent checks `LaunchApplication` before acknowledging UPDATE and exiting.
Failed helper lookup or launch returns ERR and keeps the agent running.
Both helper and agent now use File Manager calls for critical file I/O and
`FSpDelete` for staging cleanup. Inspection of the deployed Retro68 runtime's
`libretro/syscalls.c` showed that its stdio wrappers ignore native FSWrite,
FSClose, FlushVol and seek errors, and implement unlink as unconditional
failure. Checking only stdio results (the initial upload fix) was therefore
insufficient on this toolchain. Mac PUT/UPDATE now check open/truncate,
write status/count, close and volume flush directly, and continue to drain
payloads after local errors. The updater also reads/seeks/closes its source
archive through checked File Manager calls.

Host-side `agent-mac-system7/tests/test_update.py` compiles production code
against a two-fork File Manager model. It covers malformed/truncated input,
creation/open/write/close/flush/readback failures, full recovery slots,
exchange failures, launch rejection, rollback failure, failed recovery
launch, and cleanup failure. It verifies preservation of the installed
application and earlier backups, permits HFS changes only in the reserved
resource-header area, and rejects application-byte differences. The shared
upload tests additionally cover failed helper lookup and launch without
agent exit. These tests and the Mac power regression checks passed.

Built both programs with Retro68 and deployed the companion through a
clean guest shutdown and offline disk edit, retaining the original guest
disk and both prior binaries privately. Live System 7 checks demonstrated:

- An invalid MacBinary header preserved and relaunched the original agent.
- A valid resource file without CODE resources passed preparation, failed
  `LaunchApplication` with error -192, then exchanged back and relaunched
  the original agent. The repeatable source is `tests/no_code.r`.
- A real update exchanged successfully, launched the new build, retained
  the previous application, and removed STAGED_AGENT.bin with no error.

The updated agent also passed binary and empty PUT/GET, failed-open payload
draining with a subsequent PING, truncated-archive rejection with relaunch,
and successful self-update with staging cleanup. One test reused a
three-second socket timeout and expired before the reply; repeating with a
30-second timeout received OK in 3.3 seconds. A subsequent protocol reboot
autostarted the current build through the existing Startup Items alias.
Offline fork comparison then verified the installed agent/helper and the
previous working recovery copy (excluding HFS-owned reserved header bytes);
the deliberately unlaunchable test copy was removed. The final configuration
remained byte-identical. The independent original disk backup was retained,
and intermediate working disk images were removed.

Limits: an accepted launch is not a health check; later crashes or failure
to listen do not trigger automatic rollback. File Manager errors can prevent
rollback or relaunch, and neither operation promises power-loss recovery.
Keep an independent backup. Recovery files from failed attempts can contain
rejected or incomplete candidates, so consult UPDATER.LOG before using them.
The Mac README describes the recovery names, slot limit, acknowledgment
semantics, supported archive layout and separate companion deployment.

### Mixed-inventory follow-up

The loader now supports `agent_enabled = false` for infrastructure entries.
These still require a host, but need no agent token or port, and are clearly
marked inventory-only in `legacy_list_machines`. All agent connections pass
through a shared guard that rejects disabled entries before constructing
`AgentClient`. The two readiness-polling tools return configuration errors
immediately rather than retrying until their timeout. SSH remains outside
this bridge; ancillary SSH metadata is ignored.

Existing sections default to enabled. Missing tokens and invalid boolean or
port values still reject the inventory; this avoids silently disabling a
misconfigured agent. INI values are literal (including percent characters),
and parser/validation errors omit offending values so credential-bearing
lines are not echoed to tool output.

`tests/test_machines.py` covers mixed inventories, default/explicit enabled
agents, disabled routing with no client construction, immediate polling
rejection, required fields, invalid settings, sanitized parser errors,
literal percent characters, UTF-8 BOM input, and the supplied example.
The private Ubuntu entry was updated with only the new flag after making a
private backup and verifying all existing fields were preserved. A fresh
MCP stdio session using the updated public-checkout server and real inventory
passed initialization, tool discovery, a 13-entry listing, explicit Ubuntu
rejection, and authenticated PING on all 12 enabled endpoints. Running
bridge processes must restart with the updated server to load the new code
and cached configuration.

### NetWare 4 build follow-up (2026-09-15)

This records the initial compile-only fix. The later network-deadline
follow-up below supersedes its runtime strategy and verification limits:
the current build requires Novell SDK inputs and has been tested on 4.11.

The earlier 15 unresolved symbols were reproduced with the current shared
agent source. `nw4/llm_agent.lnk` now imports the missing console, screen,
scheduling, socket-control, unload and shutdown APIs explicitly. The build
continues to use Watcom's static `clib3s` runtime. Its unused `clib.imp`
existence check was removed, and `.gitattributes` now specifies CRLF for
the NetWare 4 batch file.

Native Open Watcom 2.0 builds passed in an isolated copy and the public
working tree, with no compiler/linker warnings or errors. A further clean
copy containing only the build files, shared C source and two headers also
built successfully without Novell `clib.imp` or `prelude.obj`. The resulting
81,251-byte NLM has format version 4, bounded code/data images and entrypoints,
and 55 distinct imports matching the supplied SDK catalog, including all 15
previously missing symbols. The link map confirms `fwrite`, `fflush`, and
`fclose` still resolve from the static runtime. `git diff --check` passed.

This fixes compilation and linking; it does not establish compatibility
with a running NetWare 4 loader, CLIB revision, or TCP/IP stack. No NetWare 4
endpoint was available for deployment, so authentication, binary transfers,
screen/input, and module lifecycle behavior remain live-test work. The
supplied PROCYON server authenticated successfully with its existing token,
but `SYSINFO` reported 3.12; an independent `VERSION` console command viewed
through VMware confirmed Novell NetWare v3.12. No replacement was staged
or loaded on that server. The NetWare 3.12 build path was not changed by
this follow-up.

### Windows 95 failure follow-up (2026-09-15)

Retested the two corrected inventory entries independently: `win95` (W95)
and `win95-novell` (NWCLIENT95). Both report Windows 95 build 1111, service
pack C. Initial direct-client checks passed SYSINFO, `EXEC echo`, and a
complete 2,359,350-byte, 1024x768 BMP screenshot. Both screenshots decoded
and showed the expected interactive desktops.

Each endpoint then passed five `legacy_exec`/`legacy_screenshot` cycles
through a fresh MCP stdio server using the real mixed inventory and normal
15-second agent timeout. The original `echo RETRO_AUDIT_OK` command returned
its marker and exit code zero. Each returned image decoded successfully;
the slowest MCP request was 0.344 seconds. Five further EXEC/SCREENSHOT/PING
cycles on one persistent connection per endpoint also passed. A nonexistent
executable through EXECDETACH returned its expected error immediately, and
the following PING was correctly framed on the same connection.

Byte comparison confirmed both installed agents match the build deployed
during the upload fix. Both saved pre-update binaries already contain the
COMMAND.COM shell prefix, so these results do not establish that a missing
9x shell selection caused the original failure. Prior deployments and
process restarts changed the environment; the earlier failing runtime
state was not recreated. No agent code, configuration, or installed binary
was changed during this retest.

The initial Windows 95 timeouts are therefore no longer an observed current
failure. That retest did not resolve silent EXEC launch failures, incorrect
error-response lengths, or unchecked short sends. The subsequent framing
and binary-send follow-ups below fix response lengths and short sends;
silent EXEC launch failures remain open.

### Win32 persistent-session framing follow-up (2026-09-15)

Before the change, all six Win32 endpoints returned `ERR:unknown command`
followed by `\0PONG` for an unknown command and PING on one connection.
The next command inherited a byte from the preceding literal; some other
error branches also requested bytes beyond the terminating NUL.

Converted 50 hand-sized literal replies and seven strlen-sized text headers
to the existing `send_cstr` helper. It excludes the terminating NUL and uses
`send_all` to retry short text writes. This removes the 34 remaining literal
length mismatches; the earlier upload fix had corrected two others.

`tests/test_win32_framing.py` compiles the production send helpers and every
literal reply call site against a fake socket. Each reply is followed by
PONG and checked byte-for-byte, with full writes and 1-, 2-, and 7-byte short
writes. Socket-error and zero-byte-send cases must terminate with failure.
The test fails against the pre-fix source and passes against the update;
it exercises power/registry error text without invoking those operations.
All upload fault-injection tests still pass with the adapted Win32 stub.
The native MinGW i486/PE 4.0 build completed without diagnostics, and
`git diff --check` passed.

Deployed and verified on Windows 95 (both clients), NT4, Windows 2000, XP,
and Windows 7. On each, 17 safe error cases were followed by two exact PONG
replies on the same connection: malformed commands, invalid keys/PIDs,
missing files/registry values, failed direct launches, and failed-upload
payload draining. Subsequent SYSINFO, PSLIST, WINLIST, and SCREENSHOT payloads
also left the following PING correctly framed; EXEC and binary/empty file
transfers passed. Installed binaries matched the new build and configuration
files remained byte-identical. Prior binaries/configurations were backed up
privately, and temporary deployment helpers were removed.

Scope: this corrected text-response lengths and short text sends. Binary
payload writes were addressed in the subsequent follow-up below. Silent
EXEC launch failures and input-line validation are separate from these fixes.

### Win32 binary-send follow-up (2026-09-15)

All remaining binary writes now use `send_all`: GET chunks, both BMP headers
and pixels, process/system/window lists, and registry string payloads. The
helper shuts down both directions on socket error, zero-byte send, or a
negative length. A partial response therefore ends the session instead of
letting queued commands append replies that look like payload bytes.
Handlers stop sending and release resources after a failed header or payload.

GET now reads exactly the advertised size. Premature EOF and file read
errors after SIZE shut down the connection without appending an ERR line;
growth beyond the advertised size is not transmitted. Empty files still
return SIZE:0 and allow the next command normally.

`tests/test_win32_binary.py` compiles production send helpers and GET/BMP
handlers with file, graphics, and socket stubs. Tests verify byte-exact data
with embedded NULs, 1/2/7-byte short writes, empty/growing/truncated files,
read errors, and socket failure or zero-byte send at every offset through
the GET response and the text/BMP/pixel response. Failure paths must shut
down the session and release file/graphics/memory resources. A source guard
allows raw `send` only inside the helper. The pre-fix GET fails the one-byte
send regression; the updated tests pass. Text-framing and upload regression
tests also passed. Native MinGW i486/PE 4.0 compilation completed without
diagnostics; `git diff --check` passed.

Deployed on both Windows 95 clients, NT4, Windows 2000, XP, and Windows 7.
Each passed a byte-exact 4 MiB PUT/GET roundtrip, a GET read in 4 KiB chunks
with pauses, and SCREENSHOT/PSLIST/SYSINFO/WINLIST/REGGET payloads followed by
an exact PONG on the same connection. Each also passed an empty GET. A
small-receive-window client queued a file-creation command behind a large
GET, then reset the connection after receiving the SIZE header. Every
agent recovered, and the queued file was not created. These live checks
exercise transfer/backpressure/disconnect behavior; deterministic short
write counts come from the host-side fault injection.

The NT4 fixture lacks the ProductName registry value initially used by the
test; its error and following PONG were correctly framed. Repeating with
the available CurrentVersion string completed that check. All six clients
also passed the prior 17 error/PING/PING cases and normal EXEC. Installed
binaries were read back and compared, configurations remained byte-identical,
and private backups were retained. Temporary files and deployment helpers
were removed. The agent remains single-client, and a slow connected reader
could still occupy it at that point. The later network-deadline follow-up
addresses stalled sockets; whole-transfer checksums remain separate work.

### Shared-client framing follow-up (2026-09-15)

Every public command uses a validated command session: reject CR/LF/NUL,
encode UTF-8 without replacement, check byte length, then connect and
authenticate. KEY validates before whitespace/comma normalization, registry
fields reject their tab delimiter, and numeric fields cannot inject strings.
Tokens must be 1-127 ASCII bytes without line terminators or NUL; validation
errors omit input values. Binary PUT data is sent unchanged after its header.

The default command cap is 510 encoded bytes excluding LF. This reserves
room for both LF consumption and the C terminator under the current
512-byte line-reader loop. A confirmed Win32 target may use 4094 through
`max_command_bytes`; this is also available per inventory entry. The six
known Win32 entries in the private inventory were explicitly configured
after a backup, with all existing fields preserved. Other entries retain
the 510-byte default. This does not remove smaller shell/handler limits.

Response lines are capped at 65536 bytes and must end with LF (optional
preceding CR); embedded NUL/CR and partial lines fail. SIZE/LEN fields must
be nonnegative decimal counts within `max_response_bytes`, default 64 MiB.
The same limit bounds cumulative EXEC output, while LEN:0 heartbeats remain
valid. The response cap is configurable per client/inventory entry up to
2147483647 bytes. Downloads are written locally only after receiving the
complete valid payload. Authentication, send, parse, or receive failures
close the socket without appending QUIT to a potentially unfinished upload.
Win16 listbox replies up to 32767 text bytes remain supported.

`tests/test_agent_client.py` covers all public command paths, each registry
field delimiter, token and numeric validation, ASCII and multibyte boundary
lengths, unchanged binary uploads, all SIZE-bearing operations, malformed
and oversized numbers, cumulative EXEC bounds, CRLF and partial lines,
full-size listbox text, failure cleanup, and MCP rejection before connecting.
Twelve client tests and thirteen inventory tests passed (with parameterized
subcases). A fresh MCP session rejected newline and overlength commands for
all twelve enabled inventory entries and then authenticated PING normally.
Live direct-client checks passed SYSINFO, screenshots, GET, and supported
shell EXEC on all twelve agents, including System 7 read operations. The
separate PROCYON NetWare 3.12 endpoint passed authentication, SYSINFO, NLM
download, and SCREENS. No target binaries or target configuration changed.

Restart an existing bridge with the updated server/client to load the new
code and cached limit settings. Raw agent parser hardening, portable text
encodings, streaming large files, and total operation deadlines remain
separate work; the validation is not an agent-side enforcement boundary.

### Agent network-deadline follow-up (2026-09-15)

The shared policy gives the complete authentication line 10 seconds, the next
command line 60 seconds including idle time, and binary I/O 30 seconds without
progress. A partial line cannot extend its deadline by trickling characters.
A failed session stays failed until the next accepted connection, so queued
commands and trailing replies cannot resume a timed-out transfer.

All socket ports use nonblocking reads/writes; DOS pumps Watt-32 while waiting,
Win16 pumps messages, and NetWare yields to other NLMs. MacTCP sends/receives
are asynchronous, abort on expiry, and retain their request storage until the
original I/O completes. The listener itself still waits for connections.

Live compatibility work verified the original IBM ioctl command number and
the separate 16-/32-bit calling conventions. Zero-byte writes on legacy stacks
are retried as no progress, with a deadline. DOS, OS/2, and NetWare uploads now
read chunks instead of paying timer overhead per byte. Win16 window-close
handling lets the server loop close each socket once; a live window close,
helper restart, binary readback, and screenshot succeeded.

A further NetWare 3.12 upload issue surfaced during validation: Watcom's
`ferror` macro inspects Watcom's private FILE layout, but this build uses
Novell CLIB FILE objects. The macro is now disabled and the real CLIB function
is imported. Both NetWare builds now resolve the function through CLIB.
This corrects the FILE-layout mismatch in the earlier upload error checks.

The production-loop tests cover silent/trickled clients, binary stalls,
zero/partial writes, uptime wrap, failure persistence, progressing transfers,
and commands that run longer than the network deadlines. The full top-level
suite passed 31 tests, with parameterized cases across all ports. Native
builds passed for Win32, Win16, DOS, OS/2 1.3 and 2.x, System 7, and both
NetWare variants. A newly configured NetWare 4.11 endpoint also became
available during this follow-up. Its separate setup task had corrected the
loader's import format and a SYSINFO crash caused by mixing Watcom's sprintf
wrapper with Novell's incompatible vsprintf argument convention. Those
build fixes were carried into the publication copy before deployment:
the nw4 build now uses Novell's prelude and CLIB, normalizes imports, and
imports the real ferror, fflush, and GetCurrentTicks functions. The format,
import names, and absence of the mixed static runtime were checked.

The updated binaries were deployed to the original twelve inventory agents
and the NetWare 3.12 server, with configuration bytes preserved.
Silent and trickled authentication, idle commands, stalled uploads, and
reconnection passed on all thirteen endpoints. Stopped-reader tests also
passed on Windows 95, Win16, both OS/2 binaries, MacTCP, and NetWare. Mac's
stalled UPDATE removed the incomplete stage and retained the running build.
Normal binary transfers and persistent framing were rechecked. The inactive
DOS copy on the Win16/DOS dual-boot disk was updated and read back; that
machine stayed in Windows for this round of tests.

The additional NetWare 4.11 server received the corrected nw4 binary, bringing
deployment coverage to fourteen endpoints. Its complete previous executable
was backed up and verified against a checksum computed on the server before
the staged update. Executable readback, unchanged INI/startup files, SYSINFO,
and PING passed. Silent/trickled authentication expired in about 10 seconds,
idle commands in 59 seconds, and a stalled upload in 30 seconds. A verified
4 MiB roundtrip completed in 4.5 seconds; a stopped download reader also
released the agent for a fresh authenticated PING. Screen enumeration passed.
Other NetWare 4.x versions, console input, and power operations were not
tested in this follow-up.

OS/2 1.3 uses a bounded IBM `select` wait and a 4 KiB buffer to avoid the
latency of polling small TCP windows once per scheduler tick. A 4 MiB
transfer with nonrepeating binary data was verified in about four seconds
using the normal 15-second client socket timeout. Initial compatibility
testing required recovery of the OS/2 1.3 guest; the corrected interface
was tested on a separate listener before deployment. An interrupted download
also reproduced the old NetWare agent's connection stall. Its complete
executable was backed up through small verified chunks before a normal
server shutdown/restart and the staged update. NetWare's new build passed
a complete executable download and a 4 MiB roundtrip.

Private executable/configuration backups and recovery VM snapshots were
retained. Temporary transfer files, backup helpers, and datastore screenshots
were removed after verification. These are agent changes; restarting the
Python bridge alone does not install them.

Running commands, blocked OS/disk calls, continuously active authenticated
clients, raw-parser hardening, and transactional PUT replacement remain
separate limits. See [network deadlines](NETWORK_TIMEOUTS.md) for details.

### Raw agent command-line framing follow-up (2026-09-20)

All seven authentication/command readers now use `common/command_line.h`.
They return a command only after LF or CRLF, enforce 510 content bytes
(4094 on Win32), and reject NUL, misplaced/repeated CR, overflow, and EOF
before the terminator. Failure clears the partial command and marks the
session failed until a new connection. No partial command or queued suffix
is dispatched, and no reply is appended to the failed session. These agent
changes supersede the raw-parser limitation recorded in earlier follow-ups.

The binary readers are unchanged. PUT and Mac UPDATE payloads still accept
arbitrary bytes, including line terminators and command-looking content.
Tabs, non-ASCII line bytes, empty-line handling, and valid pipelined commands
retain their prior semantics. Per-command argument validation and encoding
conversion remain separate work. See [command framing](COMMAND_FRAMING.md).

The full host suite passed 32 tests. The new test compiles all seven production
readers and exercises exact/over-limit lines, malformed authentication and
commands, EOF at each CRLF prefix, buffer guards, failure persistence, binary
boundaries, and valid LF/CRLF commands. Existing production-loop timeout,
upload fault-injection, shared-client, and Win32 response tests also passed.
Native builds passed for all eight binary variants, including both NetWare
builds and Retro68. Existing DOS linker and Mac SDK/CMake warnings remain.

Live raw-socket checks passed on all fourteen endpoints: fourteen malformed
authentication/command cases per endpoint, followed by fresh authenticated
PINGs; exact maximum-length LF/CRLF lines; a CR and LF sent separately;
queued PUT commands that must not create a file; opaque upload bodies and
same-session PING framing. Mac also passed a normal binary screenshot.
The second Windows 95 deployment required recovery from preexisting VMware
Tools/V86 memory errors and a stuck normal shutdown. After a recovery snapshot
and VM reset, its executable/configuration readback and the same raw-wire
tests passed. The network login prompts were canceled to reach the local
desktop. A recovery VM snapshot and private binary/configuration backups
were retained. Temporary test files, deployment helpers, and datastore
screenshots were removed; final authenticated PING checks passed on all
fourteen endpoints. Configurations were preserved.

FreeDOS rebooted into its updated build but received a new DHCP address.
The new address was authenticated and its binary/configuration compared
before updating only that host field in the private inventory. The inactive
DOS copy on the Win16 dual-boot disk was updated and read back; that guest
stayed in Windows during this follow-up.

### Win32 EXEC launch-error follow-up (2026-09-20)

CreatePipe and CreateProcessA failures no longer return silently. The agent
sends an ordinary LEN payload naming the failed API and numeric Windows
error, followed by EXIT:-1. It does not include the requested command in
the diagnostic. The CreateProcessA error is saved before closing either
pipe handle so cleanup cannot replace the original error code. Existing
clients already understand this framing; the shared client returns the
diagnostic and nonzero exit code, and MCP displays them normally.

The failure response uses checked sends. A successfully delivered response
leaves the session usable for another command. A partial send instead fails
the connection, preventing an EXIT or later command reply from being appended
to an incomplete frame. A launch failure creates no child process. Errors
reported by a successfully launched shell remain ordinary shell output and
exit status, and EXECDETACH retains its separate ERR response.

The full host suite passed 34 tests. Production-handler fault injection
covered both failure points, several Windows errors including 32-bit bounds,
cleanup that deliberately overwrites the last-error value, exact handle
closure, subsequent commands, short sends, and disconnection at every byte
boundary of the failure response. Successful binary output, nonzero child
exit, and quiet-command heartbeats were also checked. Shared-client tests
confirm that failure diagnostics return with exit_code=-1 without a protocol
change.

An optional native fixture, `agent-win32/tests/exec_fixture.c`, includes the
production handler, injects a pipe-allocation error without exhausting guest
resources, and triggers a real CreateProcessA failure using a nonexistent
application. It collects wire output in memory, checks cleanup/error framing,
and then runs a successful command through the same handler. This fixture
passed on both Windows 95 guests, NT4, Windows 2000, XP, and Windows 7. It
does not inject failures into the installed production agent or open a TCP
listener. See the optional `exec_fixture.exe` Makefile target.

The production build was deployed to all six Win32 endpoints. Full executable
readback matched the new build and configuration bytes were preserved. Live
checks passed pipelined EXEC/PING pairs, a shell-reported missing-command
response followed by PING, shared-client EXEC, and binary-transfer/framing
smoke tests. Temporary native fixtures and update helpers were removed;
private executable/configuration backups were retained. This fix does not
add a command-runtime limit or resolve arbitrary pipe/process API failures
after a child has successfully started.

### Self-update verification follow-up (2026-09-20)

Both bridge update tools now freeze local inputs, validate paths, and read
back both staged files before starting the updater. After 15 quiet seconds,
verification requires a changed startup marker, the expected startup
executable path and SHA-256, exact installed-file hash agreement, and stable
identity across that readback. Win32, Win16, OS/2 1.3, and 32-bit OS/2 expose
these startup fields in SYSINFO. PING and AGENT.PID are no longer verification
fallbacks. A replacement build without identity fields remains explicitly
unverified. A lost launch reply triggers verification without launching a
second updater; disabling the wait reports acceptance or an unknown outcome.

Win16 checks that staging exists and that RESTART.EXE actually launched before
acknowledging UPDATE and exiting. The acknowledgement precedes the shutdown
flag so the network deadline code can still send it. RESTART.EXE checks backup
removal and rename results, attempts to restore and launch the old binary on
installation/launch failure, and records the outcome in RESTART.LOG. Its
argument parser rejects truncation, and bridge/raw UPDATE path limits agree.
Failure after a successful WinExec still requires local recovery if the new
application crashes or cannot serve the network.

All 47 host tests passed, including 13 update tests covering SHA-256 vectors
and block/padding boundaries, startup hash immutability, old-process responses,
wrong builds and paths, installed/staged corruption, absent identity, transient
disconnects, identity changes during readback, local rebuilds during staging,
launch-reply loss, Win16 launch failure, and swap/rollback faults. Native builds
passed for all four affected ports and the Win16 helper. OS/2 1.3's existing
DGROUP was full: moving the 16 KiB PSLIST output buffer into a separate far
data segment preserved the full 16 KiB stack; PSLIST also passed live.

Live deployment and executable/configuration readback passed on both Windows
95 guests, NT4, Windows 2000, XP, Windows 7, WFW 3.11, and OS/2 1.3, 2.11,
and Warp 4.50. Nine endpoints used the bridge's update tools; the manually
launched Windows 7 agent used the existing process-targeted helper followed
by the same verifier. Identical-binary repeat updates on Windows 95 and WFW
confirmed that a matching hash alone does not finish verification. Live
negative checks rejected an unchanged startup marker and a wrong expected
hash. Private backups were retained and temporary helpers/staging removed.

The fingerprint is of the executable file read once at startup, not a
signature or attestation of loaded memory. These checks assume the trusted
lab agent and no concurrent external file replacement. The Win32 disassembly
comparison found no added SIMD instructions; six SSE2 instructions in the
preexisting CRT `__matherr` routine remain outside this change.

### NetWare AUTOEXEC follow-up (2026-09-20)

The substring detector is replaced by a bounded, line-oriented NCF planner.
Only exact explicit LOADs count, case-insensitively, with optional `.NLM`,
paths, and quoted module paths. Comment lines and similarly named modules do
not satisfy the check. Missing loads are appended in dependency order; a
missing CLIBAUX load is inserted before an existing LLMAGENT load. Existing
bytes and arguments are preserved. Reversed/duplicate loads, relevant UNLOADs,
optional loads, and recognized ambiguous forms return ERR without mutation.
Explicit TCPIP loads or BIND IP commands after LLMAGENT also fail the check.

The handler now rejects incomplete/oversized reads, malformed text, and
oversized output. Changes are staged to LLMAUTO.NEW, checked through write,
flush, close, and exact readback, then installed through a backup-and-rename
sequence. LLMAUTO.BAK is retained for recovery. A failed install attempts to
restore the original; a failed restore reports the recovery file. Existing
recovery files block edits until reviewed, while an already-correct NCF can
still return present without rewriting anything. Neither arbitrary NCF control
flow nor recursively invoked scripts are evaluated, so present describes the
supported startup entries rather than proving a successful reboot/network.

All 49 host tests passed. New production-code tests cover comment and name
false positives, case/path/extension forms, missing dependencies, ordering,
duplicates, optional and unload commands, malformed/NUL/EOF input, bounded
file sizes, preservation, repeated calls, read/write/flush/close/readback
failures, backup/install failures, and successful/failed rollback. Both native
NLM builds passed with Novell CLIB and normalized imports.

A scratch-file native fixture using the production planner and handler passed
on NetWare 3.12 and 4.11, exercising actual file insertion, backup/replacement,
idempotence, path parsing, and rejection of reversed/optional loads. It never
edited or executed the real boot script. Updated agents were deployed through
staged UPDATE; complete binary and INI readback matched. Repeated real AUTOEXEC
commands returned present, and the real NCF bytes remained unchanged on both
guests. Temporary fixture files were removed; private backups and the existing
4.11 LLMAUTO.BAK recovery file were retained. No reboot was needed or tested.
