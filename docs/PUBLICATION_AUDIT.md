# Publication audit — 2026-09-14

Audited `dd885732233c83b74dbf04ae212c69da7a8258f6` plus the nine files
already modified in the working tree. This is a source, documentation,
build, and bounded live-behavior review, not a claim of exhaustive safety
or compatibility. **The repository is not yet cleared for publication:**
a retired credential remains in Git history, and the failures below
need remediation or explicit acceptance as release limitations.

## Secret and publication findings

1. **Former Mac token in history (rotated).** The original token in the supplied private
   inventory appeared verbatim in
   `agent-mac-system7/QUICKEYS_CLICK_INVESTIGATION.md:321`. It first appears
   in `347cdc7238cfeede8e5c2f2d69821570ca1de46a` and is present in 30
   reachable commit snapshots. The audit redacted the current document;
   that does **not** remove the historical copies. In the subsequent
   follow-up, the Mac token was replaced on both ends with a fresh random
   256-bit token, the agent was restarted, and the old token was explicitly
   rejected. Remove the retired token from the history being published, or
   publish a reviewed clean initial snapshot. No history rewrite was
   performed. The replacement token was not added to this repository.
2. **Example token used by live machines.** Seven configured entries use
   the same example token found in tracked examples, smoke scripts, and
   floppy builders. All seven accepted authentication during this audit.
   They need unique replacement tokens before publication. The report
   deliberately omits credentials and network addresses.
3. **Ignore rules missed deployed config names.** DOS and Win16 ignored
   `llm_agent.ini`, but their deployment instructions use `LLMAGENT.INI`
   (a different basename, not just a case difference). `git check-ignore`
   confirmed the gap. This audit added the two missing rules. No real
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
| Win16 | DOS helper `EXEC`; WinExec detach, no real PID | PUT/GET; desktop BMP | Journal input unreliable; window/message/listbox primitives; no clipboard/registry | ToolHelp task handles | UPDATE + RESTART.EXE; staged update changes currently uncommitted |
| NetWare | Console `system()`; no output capture or detach | PUT/GET; selected console text BMP | StuffKey or console fallback; SCREENS; no GUI windows/clipboard | Unsupported | UPDATE.NLM; no dedicated MCP wrapper |
| Mac System 7 | No shell or detach | Data fork only; main-screen BMP | Left click/double-click, US keyboard; window/clipboard commands refuse; drag incomplete | List / request application quit | MacBinary `UPDATE <size>`; no MCP wrapper |

Power behavior also differs: Win32 and Mac implement reboot/shutdown;
DOS and both OS/2 ports implement reboot but reject shutdown. Win16
shutdown exits Windows to DOS. NetWare shutdown downs the server and
does not implement reboot. These are not interchangeable “power off” tools.

## Functional findings still open

### High priority

- **Mac protocol power commands bypass application-quit negotiation.**
  `HandlePower()` directly calls ShutDwnStart/ShutDwnPower. Apple's
  [Shutdown Manager documentation](https://developer.apple.com/library/archive/documentation/mac/pdf/Processes/Shutdown.pdf)
  warns that this skips asking other applications to quit and save their
  work. Route normal power requests through Finder. The separate ignored
  Quit-event bug that blocked Finder Restart has been fixed and live-tested
  (see follow-up below); the protocol power commands retain this limitation.
- **Win16 listbox read can overwrite memory.**
  `agent-win16/llm_agent.c`, `handle_lbgettext`, passes a 160-byte static
  buffer directly to `LB_GETTEXT` at line 764. It clamps the returned
  length only *after* the control has written the string. A longer item
  can corrupt the agent. Query `LB_GETTEXTLEN`, allocate/check the size,
  and handle control types appropriately. The README describes this as
  read-only, but it can still corrupt memory. Not exercised live
  because the failing case could crash the Win16 environment.
- **Uploads can acknowledge failed writes.** Win32 `handle_put` ignores
  `WriteFile`'s return value and byte count (`agent-win32/llm_agent.c:461`).
  Mac `HandlePut` and `HandleUpdate` ignore `fwrite` results (lines 1731
  and 1781). Several stdio-based ports also ignore final `fclose` errors.
  Disk-full/short-write failures can therefore receive `OK`, including
  when staging an update. Check writes and final close/flush; stage and
  verify important files before replacement. Small healthy-disk transfer
  tests passed; disk exhaustion was not induced.
- **Mac update can remove the working application with no rollback.**
  `llm_updater.c` deletes the target before creating/writing both forks.
  Failure after deletion leaves no known-good copy, yet its final failure
  log says `target not touched`. The agent also ignores the return from
  `LaunchApplication` when handing off, then exits. Prepare/validate a
  replacement first, preserve the old application, and verify helper
  launch. Source finding; not live fault-injected.
- **The supplied mixed inventory blocks the MCP bridge.**
  `load_machines` rejects the SSH-only `ubuntu` section because it lacks
  `exec_token`. Loading is all-or-nothing, so even valid named machines
  become unusable through `server.py`. Reproduced with the supplied file.
  Keep a separate agent-only inventory or add explicit schema support for
  non-agent sections. The live tests used `AgentClient` directly and did
  not modify the private inventory.

### Compatibility, protocol, and workflow gaps

- **NetWare 4 build is broken.** The supplied batch file's LF endings
  caused `cmd.exe` parsing failures. With CRLF normalized in the private
  build copy, linking still failed on 15 symbols, including `CreateScreen`,
  `ThreadSwitchWithDelay`, `DownFileServer`, and `AtUnload`. Its explicit
  import list has not kept pace with the shared agent source. No working
  NetWare 4 binary was produced; the NetWare 3.12 build succeeded.
- **Windows 95 has a live partial failure.** One unique configured
  endpoint (two aliases) passes auth, PING, SYSINFO, PSLIST, WINLIST, and
  file transfers but times out on SCREENSHOT and normal EXEC at the
  bridge's 15-second timeout. An explicitly addressed `C:\COMMAND.COM`
  through EXECDETACH does execute and was used to clean up test files.
  Root cause is unconfirmed. In source, Win32 EXEC returns without an
  error response when CreatePipe/CreateProcess fails, explaining how a
  launch failure can become a client timeout. Screenshot failure needs
  separate diagnosis; it is not proven to share the execution cause.
- **Win32 persistent sessions are misframed after errors.** Thirty-six
  literal `send` lengths disagree with their text length. Most send an
  extra NUL; three read one byte beyond the literal's terminating NUL
  (lines 1039, 1261, 1302). Live on the Windows 2000 endpoint, an unknown
  command followed by PING on the same connection returned a leading NUL
  before PONG. Use `send_cstr`/`send_all`. The bundled client usually hides
  this by reconnecting per operation.
- **Win32 GET and screenshots assume a complete `send`.** The data paths
  use single `send()` calls without checking/retrying short sends, despite
  having a `send_all` helper. This can truncate a declared SIZE response.
  It was not proven to cause the Windows 95 screenshot timeout.
- **The shared client does not enforce command framing.** EXEC, paths,
  and registry fields can carry newline delimiters; an offline capture
  reproduced two wire commands from one `exec()` argument. Only TYPE and
  CLIPSET explicitly reject newlines. Agent line readers return a full
  buffer as a command without draining/rejecting the remainder. Limits
  are 4096-byte line buffers on Win32 and 512 on the other ports; clients
  should validate encoded lengths and delimiters. Response parsing also
  accepts negative sizes as empty bytes and has no global size/line cap.
- **Single-client agents can be occupied by stalled clients.** Accept and
  read loops generally have no application-level authentication or idle
  deadline. Most agents also cannot process another connection while a
  command is running. A 15-second client socket timeout is not a command
  cancellation guarantee: DOS's synchronous `system()` cannot receive
  commands or pump its network while the child executes. Win32 has EXEC
  heartbeats, so its client timeout is an idle timeout, not a runtime cap.
- **Text encoding is not portable Unicode.** The bridge uses UTF-8 while
  target APIs consume ANSI/OEM/Mac encodings. Non-ASCII filenames, titles,
  output, and input can be corrupted. ASCII-only checks do not verify
  localized systems. Token encoding silently drops non-ASCII bytes;
  ConfigParser interpolation also makes literal percent signs problematic.
- **Some implemented commands are unreachable through MCP.** The bridge
  registers 26 tools, but provides no tools for Win16 WINMSG/POSTMSG/
  LBGETTEXT or WINLIST's parent argument; NetWare SCREENS/AUTOEXEC/DEBUG/
  UPDATE; OS/2 WINCLOSE; or Mac DBLCLICK/MOUSEPOS/MacBinary UPDATE. Some
  have `AgentClient` methods, others require a custom wire client. A shared
  protocol does not imply complete MCP coverage. There is no capability
  negotiation command or consistent build identifier across all ports.
- **Update readiness is weaker than update verification.** The uncommitted
  `legacy_win16_self_update` immediately calls `legacy_wait_for_agent`,
  which can see the outgoing process. The generic helper falls back to
  ping when AGENT.PID is unavailable; even a changed PID proves restart,
  not that the intended bytes were installed. Win16 helper launch/rename
  results are also incompletely checked. Avoid claiming a verified update
  without checking the resulting build and executable.
- **NetWare AUTOEXEC detection is only a substring search.** A comment
  mentioning `llmagent` or `clibaux` satisfies `buf_has_token_ci`; this can
  report `present` without an active LOAD command. It also does not verify
  dependency ordering. Parse active commands before claiming persistence.
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
| NetWare 4 | Fail, including after private CRLF normalization |
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

`git fsck --full --no-reflogs --unreachable` found two unreachable commits:

- `c1c11a381dcfff7d9396dca2e508df503548897d`: dropped WIP stash from
  2026-09-06, based on `43bd3f7`.
- `d8d2f9fd9702cd83aac1434ccb8f6e5d055c3dd2`: that stash's index parent;
  its tree is unchanged from the stash base.

The stash's only change is eight `.gitignore` lines for `os2idle` build
outputs. No missing agent implementation was found there. Seventeen
unreachable blobs were recovered and compared: four `.gitignore`
intermediates and thirteen source/document versions. The latter are
intermediate directory-renaming snapshots or older versions superseded by
current commits/working changes, not a newer missing feature branch.
Applying them wholesale would undo newer work.

The nine preexisting modified files contain substantive **uncommitted**
work: Win16 idle-listener changes and staged-update handling; preservation
of updater backups on Win32/OS2; an OS/2 floppy-documentation adjustment;
and bridge `vm_name` plus `legacy_win16_self_update`. These changes are
still present and were preserved. They must be reviewed and committed
explicitly if intended for the public release.

Recovery evidence (unreachable-object pack, blob exports, dropped-stash
patch, and the original working-tree patch) was saved outside the repo
under the private configuration directory's `publication-audit-20260914`
folder. Do not publish that recovery folder: it includes historical source
and private audit context. No reset, checkout, stash apply, cherry-pick,
commit, or history rewrite was performed. Work already garbage-collected
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

Commit `6665919` enabled `isHighLevelEventAware` for PSKILL without
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
