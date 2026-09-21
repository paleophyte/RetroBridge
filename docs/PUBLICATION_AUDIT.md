# Validation and remaining issues

Last consolidated: 2026-09-21, after the agent screenshot fixes. This is a
source review plus bounded builds, fault injection, and live VM tests. It is
not certification of every OS release, locale, CPU, or failure path. Earlier
investigation narratives are available in Git history; this document records
the current conclusions rather than preserving superseded instructions.

The source is published as [RetroBridge](https://github.com/paleophyte/RetroBridge).
Original project code is MIT. Source publication and compiled distribution
have separate requirements: see [source checks](PUBLICATION.md),
[provenance](../THIRD_PARTY.md), and [binary release work](BINARY_RELEASE.md).

## Current status

- The bridge exposes 43 tools. The last functional validation passed 92 root
  tests plus two Mac and one Win16 platform fixtures (95 total), and a fresh
  43-tool MCP stdio smoke. The [root README](../README.md#validation-and-known-limits)
  gives the test commands and compiler prerequisites.
- Native builds and live deployment cover all seven agent ports, including
  separate NetWare 3.12/4.11 builds. Compatibility is limited to the configured
  guests below; build stamps and tool names are not broader certification.
- The [gallery](SCREENSHOTS.md) contains 13 unique running OS/version combinations,
  all captured through agents. There are two tested Windows 95 installations,
  represented once. The Ubuntu/QEMU host is inventory-only. The dual-boot
  MS-DOS/WFW guest was running WFW during gallery capture.
- All four bridge update tools verify replacement when the new build supports
  startup identity. Mac and NetWare have recovery copies; NetWare protocol 2
  also supports readiness-based rollback of failed/exited candidates.
- The DOS TSR remains deferred. Its [concept and experiment report](../agent-dos/experimental/README.md)
  retain what was built, the live failures, recovery, and proposed next work.

## Agent capability and validation scope

Detailed command behavior belongs in the linked platform guides and
[MCP coverage](MCP_COVERAGE.md). This table summarizes the tested configurations.

| Port / tested systems | Main capabilities | Boundaries |
| --- | --- | --- |
| [Win32](../README.md): Windows 95 (two guests), NT4 SP6, 2000, XP SP2, 7 SP1 | Shell, detach/jobs, files, desktop capture/input, process/window queries, clipboard, registry, update | Win7 desktop automation is interactive; NT update helper assumes a service; stock 98/ME and physical old CPUs not certified |
| [DOS](../agent-dos/README.md): FreeDOS 1.4; separate MS-DOS 6.22 boot tests | Synchronous shell, files, text capture, BIOS key queue, reboot | No concurrent shell/network service, jobs, mouse, process list, or built-in update |
| [Win16](../agent-win16/README.md): WFW 3.11 | DOS helper execution/jobs, Windows launch, files, desktop/input, task/window/control operations, update | Foreground DOS scheduling, synthetic/unknown command exit status, modal UI and layout limits; shutdown exits to DOS |
| [OS/2 32-bit](../agent-os2/README.md): 2.11 and kernel 4.50 | Shell/detach/jobs, files, PM desktop/input, process/window operations, clipboard, reboot/update | WPS startup context required; no shutdown or registry tools |
| [OS/2 16-bit](../agent-os2-13/README.md): 1.3 | Shell/detach/jobs, files, PM desktop/input, PSTAT-backed process list, reboot/update | No clipboard/shutdown; reboot helper needs IOPL; intermittent update rename failure remains a historical unresolved limit |
| [NetWare](../agent-netware/README.md): 3.12 and 4.11 | Console execution, files, text capture/input, screen list, startup maintenance, down/update | No stdout capture, process jobs, or reboot; console UNLOAD has abended; helper recovery can need an operator |
| [Mac](../agent-mac-system7/README.md): System 7.5.3 on QEMU q800 | Data-fork files, main-display capture, limited input, processes/cooperative Quit, power/update | No shell, cross-application clipboard/window enumeration, or reliable drag; US keyboard mapping |

## Remaining work and operational limits

- **Binary distribution:** finish the artifact-specific Watcom source/notice,
  Watt-32 coverage, Mac newlib/SDK, and Novell terms review in
  [BINARY_RELEASE.md](BINARY_RELEASE.md). Existing hashes describe diagnostic
  builds, not later release artifacts.
- **Win32 deployment/CPU compatibility:** Winsock 2 and runtime imports must
  exist on the guest. Reviewed prebuilt CRT code retains CMOV and some SSE2;
  a PE 4.0 stamp and `-march=i486` do not certify 486/Pentium hardware. Win7's
  interactive autostart registration was read back but next-logon execution
  was not tested. The generic NT updater still expects the LLMAgent service.
- **OS/2 1.3 updates:** later verified replacements succeeded, but the earlier
  intermittent rename failure was not independently root-caused. Inspect
  UPDATE.LOG, processes, and the returned verification result before retrying.
- **Long-running work:** Win32 cancellation covers the direct child only.
  Win16/OS2 marker jobs cannot cancel children and do not cap disk output;
  restart loses the registry while files/children can remain. DOS EXEC blocks
  networking. Other blocked OS calls can still occupy the agent. See
  [job and EXEC semantics](LONG_RUNNING_COMMANDS.md).
- **Input and encodings:** keyboard tools remain ASCII-only; Mac uses a US
  layout. Other layouts, DBCS, and localized native startup/update paths are
  not broadly validated. Strict decoding cannot detect a wrong but valid
  single-byte codec. See [text encodings](TEXT_ENCODINGS.md).
- **Mac:** dragging is experimental; clipboard/window commands explicitly
  refuse. Update can roll back a rejected launch, but not a later crash or
  failure to listen. Power/Quit delivery can be cancelled or blocked by apps.
  Wide screenshots were host-tested; live capture was 640×480.
- **NetWare:** loaded/unready NLMs, failed restoration, and damaged stack state
  can require controlled server recovery. AUTOEXEC checks a bounded subset of
  one NCF file, not arbitrary boot control flow. Other CLIB/TCP/IP versions,
  full 4.x input/power coverage, and physical hardware remain unverified.
- **Optional bootstrap media:** current Win16/OS2 floppy builders omit
  JOBRUN; the 32-bit OS/2 image also omits update/reboot helpers. Deploy those
  files separately as the platform guides describe. NetWare media can include
  locally supplied third-party utilities and a placeholder INI; generated lab
  floppies are not curated public binary-release bundles.
- **Transfers/networking:** ordinary PUT is non-atomic and may leave a partial
  file. Deadlines recover stalled connections, not running commands or a hung
  filesystem. There is no transport encryption, fairness, or rate limiting.
- **DOS boot behavior:** the tested PCnet dual-boot guest required a cold boot
  after Windows to restore DOS networking. Nested FreeCOM batches can print
  to the console while EXEC returns empty captured output. DHCP lease changes
  may require updating the controller inventory.
- **Earlier Win95 timeouts:** the original EXEC/SCREENSHOT failures stopped
  reproducing after deployments/restarts; their precise cause was not proven.
  Later framing/launch fixes should not be presented as a proven explanation.

## Fixes and evidence retained from the publication review

| Area | Current behavior and verification |
| --- | --- |
| Uploads | All ports check short writes and finalization errors; host I/O fault injection and live binary/empty/error transfers. Mac uses checked File Manager calls. |
| Win32 responses | Literal replies exclude NUL, short sends retry, incomplete binary replies close the session. Host byte-boundary faults and live persistent error/PING checks on all six Win32 guests. |
| Framing/deadlines | Shared-client validation plus production line readers on all seven ports; malformed/trickled/stalled sessions, opaque binary bodies, and reconnection tested across 14 endpoints. See [framing](COMMAND_FRAMING.md) and [deadlines](NETWORK_TIMEOUTS.md). |
| Win32 EXEC | Pipe/launch failures return diagnostic output and EXIT:-1. Native failure fixtures and following successful commands passed on all six Win32 guests. |
| Win16 listboxes | Length-checked allocation, string-backed control validation, and cleanup. Live reads through 4096 bytes; 32767-byte boundary tested in the host fixture because the guest control rejected that insertion. |
| Execution/jobs | Invocation-specific spools, no missing-output rerun, explicit timeout uncertainty; quiet/concurrent/disconnected jobs and native failure fixtures. See [execution evidence](LONG_RUNNING_COMMANDS.md). |
| Update identity | Frozen/staged input checks, new startup instance, expected startup hash, and installed readback. Same-binary reinstallation and negative verification cases tested. Windows/OS2, Mac, and NetWare live updates passed. |
| Mac rollback/power/config | Both-fork prepared exchange and retained recovery; real invalid-application launch rollback, normal update, Finder Restart, save/cancel power dialogs, relocated application folder and temporary port tests. Configuration parser/API failures host-tested. |
| NetWare build/AUTOEXEC | Corrected import format and consistent Novell CLIB runtime on 3.12/4.11. Startup planner native scratch-file fixtures passed; real AUTOEXEC returned present without changing installed NCF bytes. |
| NetWare rollback | Host fixture covers 22 transaction faults and seven handoff cases. A valid NLM exiting at startup caused real rollback on both guests, preserving OLD/BAD bytes; subsequent normal updates passed. Power loss/kernel abends were not induced to test rollback. |
| MCP/encoding | Mixed inventories, platform guards, 43-tool discovery, strict configured codecs, and live legacy text/file paths. MCP SDK 2.0.0 and 2.2.0 exercised; latest fresh environment used Python 3.12, MCP 2.2.0 and Pillow 12.3.0. |

Test sources under `tests/` and the platform test directories provide repeatable
host fixtures. Live results are records of those lab runs, not tests performed
on every documentation commit. Guest configuration was preserved during verified
deployments; private backups and intentional recovery archives were retained.

## Agent screenshot gallery and Win16 input (2026-09-21)

Gallery preparation found two additional bugs. NetWare confused scanned OS
screen IDs with CLIB handles, rejected negative 4.x IDs, and counted blank
screens as successful capture. The fix converts handles, rejects empty/error
copies, preserves Install's StuffKey path, and restores the calling I/O context.
Both 3.12 and 4.11 now produce readable agent screenshots.

Win16 journal callbacks lacked the EXE instance thunk needed for their data
segment. The corrected build passes actual dialog clicking, named keys,
mixed-case text, spaces/punctuation, and a 216-character multi-batch TYPE check.
Mouse buttons now follow 1=left, 2=middle, 3=right. Host fixtures cover thunk
cleanup, failed hook installation, timeouts, text validation/batching, and mapping.

During the first NetWare 3.12 deployment, both candidate and restored old binary
failed to listen. The updater retained originals and recovery records. A normal
DOWN/EXIT/SERVER cycle restored service; files/configuration were verified and
records archived before a successful update. Listener setup now enables
nonblocking polling after listen and logs errno. The exact old-stack failure
mechanism is unproven; this is not automatic recovery from every TCP/IP fault.

## Publication and credential review

The publication history was cleaned before the initial GitHub push. Known
retired tokens, former shared example credentials, SDK/media payloads, and raw
proprietary research excerpts were removed; needed dependencies are supplied
locally and hash-checked. Connected agent credentials were rotated and retired
values rejected. No real inventory or deployed INI belongs in source control.

The last post-gallery scan covered 98 reachable commits and 1091 local objects,
including exact comparison against 19 known current/retired credentials: no
matches, no Gitleaks findings, and no removed vendor payload fingerprints.
Git integrity passed. Earlier default-rule scanning missed a bespoke token,
which is why known-value comparison was also used. This does not rule out
unknown, encoded, encrypted, or externally stored secrets.

Non-secret historical lab names/addresses, local paths, and Git authorship were
intentionally retained. The private original checkout and recovery copies still
contain removed material and must not be merged or pushed into this history.
The missing-commit investigation and private recovery artifacts are not needed
to build or use RetroBridge. Follow [PUBLICATION.md](PUBLICATION.md) when
publishing another commit; keep scan reports containing private context outside Git.
