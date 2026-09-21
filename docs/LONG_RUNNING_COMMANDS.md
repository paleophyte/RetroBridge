# Long-running commands: implementation and remaining experiments

## Win32 background jobs

Win32 now advertises `exec_jobs=1` and `job_cancel_scope=process` in SYSINFO.
The bridge requires that advertisement before sending job commands. Use
`legacy_job_start(machine, command, shell=True)` to start a shell command,
or `shell=False` for a direct executable command line. The reply contains a
32-character lowercase hexadecimal ID. Start returns promptly; the command
continues after disconnect. `legacy_job_status(machine)` lists retained jobs;
pass `job_id` to query one. No launch is automatically retried after an
uncertain response: the bridge returns the generated ID so it can be queried.

`legacy_job_output` reads at a byte offset (up to 65,536 bytes per call),
returning exact base64 bytes, decoded text when possible, and the next offset.
It uses the configured EXEC output codec or a per-call override. An empty
read means no bytes currently available, not completion. Decode failures,
including multibyte characters split across reads, preserve the raw bytes.

There are four slots, each retaining the first 1 MiB of merged stdout/stderr
in memory. Excess output is drained and discarded with `truncated=true`.
Polling continues with no connected client and is bounded to 16 KiB per
job per pass. Network transfers can delay polling and backpressure the child.
Blocking EXEC is refused while jobs are active. Completed results remain
until explicitly released; full slots reject new starts without evicting data.
Jobs inherit the agent's environment and working directory and have NUL stdin;
they are intended for unattended commands. Interactive programs need a
different launch/control workflow.

States are `running`, `cancelling`, `cancelled`, `finished`, or `unknown`.
Exit status is null while unavailable; errors do not become exit zero.
`legacy_job_cancel` uses the retained process handle, then reports cancelled
only after termination is observed. This covers the direct process only on
both 9x and NT: a shell's children can survive. Direct-child exit finishes
capture after a bounded final drain even if descendants retain the pipe;
later descendant output is not captured. Prefer direct mode when suitable.
`output_error` reports a pipe-read error separately from process exit.

`legacy_job_release` refuses active jobs and removes completed output/handles.
Agent restart loses the job registry and output. Graceful agent stop attempts
to terminate direct children; crashes/forced termination can leave children
running. Shutdown/reboot follows the requested machine operation. Bridge
self-update refuses any retained job, including completed results: retrieve
and release those first. This guard is advisory against another concurrent
controller; raw detached updater launches can bypass it.

Wire commands (after ordinary authentication):

```
JOBSTART <id> S <shell command>
JOBSTART <id> D <executable command line>
JOBS
JOBSTATUS <id>
JOBREAD <id> <byte offset> <count up to 65536>
JOBCANCEL <id>
JOBRELEASE <id>
```

Start/status/cancel return `SIZE:n` followed by ASCII key=value metadata.
List returns SIZE-framed newline-separated IDs; read returns SIZE-framed raw
bytes; release returns OK. Failures return ERR. IDs must be 32 lowercase hex
digits. Repeating an existing retained ID with identical mode and command
returns that job without launching again. Reusing it after release or restart
can launch again; IDs are not a persistent exactly-once ledger.

The Win32 native fixture covers binary/nonzero output, output-cap draining,
four-slot exhaustion, repeated starts, launch failure, direct cancellation,
and a descendant holding the output pipe open. It passed on the build host,
Windows 95, and XP. Live 95/XP tests also kept PING and screenshots responsive
during a quiet 20-second child, ran concurrent commands, decoded OEM output,
recovered a job after disconnect immediately after launch, confirmed
cancellation, and released all test results.

All six connected Win32 guests received the build with executable hash,
new startup identity, and unchanged configuration verified. Native job
start/status/output/release passed on each. Both MCP SDK 2.0.0 and 2.2.0
discovered 43 tools and passed live 95/XP jobs and unsupported-build rejection.


Windows 7 required an interactive restart: its service was disabled, while
an interactive agent remained running. The NT updater replaced the file but
could not start the disabled service (error 1058), and the bridge correctly
reported replacement unverified. Restarting the known interactive instance
then passed identity/hash/configuration and job checks. The generic NT updater
still assumes a service installation; interactive NT deployments need an
explicit restart appropriate to their launch configuration.
The tested Win7 installation also has a per-user HKCU Run entry for the
interactive agent. Its registration was read back and desktop capture verified;
a fresh logon was not exercised. It starts only when that user logs on.
This is lab validation, not an autostart step performed by `--install`.

## Win16 and OS/2 file jobs

These agents implement the same job commands using a separately built
`JOBRUN.EXE` beside the agent. Build scripts now build that companion too;
deploy the correct DOS, 32-bit OS/2, or 16-bit OS/2 helper and verify its bytes
before updating the agent. Do not replace a helper while jobs are active.
The helper reads a command file, redirects stdout/stderr itself, waits for
the shell, closes output, then renames a completion marker into place.
Spool directories are reserved atomically and never reused across restart.

Two results can be retained. OS/2 can run two shell jobs concurrently; Win16
allows only one active DOS-box job, with up to two retained results. Direct
mode is rejected. Command limits are 120 encoded bytes on Win16 and 450 on
OS/2, within the overall protocol line limit. Installation paths must be
short (up to 100 bytes) and contain no spaces. Win16 converts ANSI command
text to OEM for the DOS helper. All jobs use NUL stdin.

Win16 must launch the DOS box in the foreground on the tested WFW setup.
Minimized launches were suspended before creating output. Switching focus
or changing PIF scheduling can affect progress: this does not promise a
background DOS scheduler. PING, screenshots, status, and file reads remain
available while a foreground DOS job runs. Win16 reports a completed inner
command's exit code as null because COMMAND.COM /C lost its nonzero status
in live tests. OS/2 preserved the test child's real exit status 7.

Until a valid completion marker arrives, state is `unknown`: it means
completion has not been observed, not proof that a process is still alive.
Missing/corrupt markers or a helper failure can leave a job unknown indefinitely.
`completion_tracking=marker`, `pid=null`, and `cancel_scope=unsupported`
make these restrictions explicit. Cancellation always returns an error and
is excluded from the advisory capability list. Release refuses a missing
completion marker, preventing removal of files a child may still use. A
helper-reported launch/I/O error remains unknown with `output_error` nonzero,
but its completed marker allows cleanup. Local recovery of a stuck unknown
job requires confirming the helper/child has stopped before restarting the
agent and removing that invocation's files. No automatic orphan cleanup runs.

**Disk output is not capped.** `output_storage=file_unbounded` is returned
by every job status and advertised in SYSINFO. Reads expose the first 1 MiB,
marking truncation beyond that; the redirected file can continue growing.
Use bounded-output commands and watch free space. Output reads are at most
64 KiB and use a reusable transfer buffer; a concurrent file-read failure
closes the connection instead of misframing a following command. Metadata
may not show buffered child output immediately. Completion tracks the shell,
not descendants that it launched independently.

Restart loses the registry while spool files remain on disk; children may
continue. Bridge updates refuse retained jobs as on Win32. Existing EXEC is
refused while job completion remains pending so it cannot block job queries.

The Win16 build shares its transfer buffer and sizes stored commands to the
platform limit. It sets Watcom's `_amblksiz` before CRT startup and reserves
a 4 KiB local heap: the default 8 KiB allocation increment failed startup
after the new static data reduced contiguous room in the shared 64 KiB
data/stack segment. The live failure was recovered using the previous binary;
the corrected build passed verified replacement and job operations.

Open Watcom builds and host tests cover repeat IDs, slot limits, malformed
offsets/counts, binary output, prefix truncation, helper launch errors,
missing completion, explicit cancellation rejection, and cleanup. Live
Win16 and all three OS/2 guests ran a quiet 20-second child while PING and
screenshots stayed available. OS/2 also ran a concurrent second command
without mixing output. Win16 rejected that second active job. Encoded shell
output and release passed, and configuration was preserved on deployment.
Both MCP SDK 2.0.0 and 2.2.0 passed the file-job lifecycle on all four guests.
OS/2 1.3 launches helpers with `P_NOWAITO` so the CRT does not retain a child
wait result that the marker-based implementation would never collect.

## Synchronous EXEC

The job interfaces above are implemented. Existing EXEC remains useful for
short commands, with platform-specific wait and output behavior:

| Agent | Current behavior | Long-running consequence |
| --- | --- | --- |
| Win32 | Fresh shell, merged stdout/stderr, five-second quiet heartbeats, no total runtime limit | Occupies the command connection; disconnect attempts to terminate the direct shell, not its descendants |
| Win16 | Foreground REDIR helper, messages/heartbeats while waiting up to 30 seconds; captures at most 8191 bytes | Expiry reports still running/not cancelled and retains its unique spool; completed EXEC uses synthetic exit zero |
| OS/2 2.x | Independent CMD session and completion marker, up to 60 seconds with heartbeats | Expiry reports uncertain completion and retains its unique spool; completed EXEC uses synthetic exit zero |
| OS/2 1.3 | Synchronous system() with unique output spool | No command service during execution; missing output reports an error and never reruns the command |
| DOS | Synchronous system() and redirected output | Cannot pump Watt-32 or accept status/cancel requests while the child runs |
| NetWare | CLIB system() for a console command, no stdout capture | Not a tracked subprocess job; no generic cancellation |
| Mac | No EXEC/shell implementation | Process and UI operations have their own acceptance/completion semantics |

The Python socket timeout is an inactivity timeout, not a command deadline.
Heartbeats can keep a quiet operation alive; silent agents can outlive the
client's read timeout. Closing a connection does not prove the command stopped.
Do not repeat a side-effecting command just because its result was lost.

Invocation-specific spools prevent later EXEC calls from overwriting an
unfinished child's files. Wait expiry is not cancellation. Inspect retained
files and confirm the child has stopped before deleting them. The verification
section below records the tested timeout behavior.

For DOS, a foreground run-and-resume mode with persisted results remains a
proposal. It would restore access after the child exits, without concurrent
network service or remote cancellation. NetWare would need console/NLM-specific
lifecycle work before exposing tracked jobs. Neither platform advertises the
implemented Windows/OS2 job interface.

## DOS resident monitor feasibility

A TSR is possible in principle; a limited resident monitor is worth testing
before considering a full background agent. The current build is 16-bit
real-mode Open Watcom large model with Watt-32, not a protected-mode DOS
extender. During `system()`, the parent agent already remains in memory. An
interrupt-driven monitor could therefore be prototyped around that existing
parent first, without immediately converting the executable into a TSR.

The installed Watt-32 source and [upstream `src/pcintr.c`](https://github.com/gvanem/Watt-32/blob/master/src/pcintr.c)
contain a timer-interrupt network poller with a real-mode Watcom path. It
switches to a private stack, checks the InDOS and critical-error flags, and
can call `tcp_tick(NULL)` and a callback. The agent currently pumps TCP only
from its foreground loops and does not enable this facility. Its existence
is a concrete implementation lead, not evidence that our build/packet driver
can safely service the full agent from interrupts. The poller's guard and
the stack's own reentry guard do not certify arbitrary C runtime or agent
operations as interrupt-safe.

DOS calls must be deferred when DOS is in a critical section; Microsoft's
[GetInDOSF description](https://github.com/microsoft/MS-DOS/blob/main/v2.0/source/SYSCALL.txt)
documents that restriction. A resident handler also needs to preserve the
interrupted program's machine/DOS context and respect BIOS, packet-driver,
and C-runtime reentrancy. InDOS alone is not a complete execution scheduler.

Candidate scope for a first prototype:

- Keep bounded network progress and report an in-memory running/busy status
  while a foreground child executes. A DOS-safe callback must not invoke the
  current blocking command dispatcher, file I/O, allocation, or `system()`.
- Evaluate raw text-screen snapshots and queued keystrokes separately. Current
  BMP construction and BIOS injection cannot simply be called from an ISR.
- Defer file transfer and new command execution to an explicitly safe context.
  Cooperative break requests would be best-effort; a TSR does not provide a
  general safe process-kill primitive for arbitrary DOS programs.

A tiny shim that loads the full agent has additional limits: loading the
EXE is itself a DOS operation, and a foreground program may have consumed the
conventional memory it needs. Swapping that program out and restoring it is
a much larger design. Keeping only the packet-driver TSR resident does not
keep TCP/authentication/agent services alive; those still need a resident
implementation or a different wake-up transport with an explicit handoff.
The foreground and resident halves must agree on one owner of network state.

First test a separate experimental binary on both FreeDOS and MS-DOS 6.22:
timer/stack/context integrity, then bounded network progress during a CPU
loop, a disk-intensive child, and a program waiting for keyboard input.
Measure conventional-memory use and foreground slowdown, test disconnects
and return to the parent, and restore interrupt vectors on every normal
exit. Programs that disable interrupts or retain unsafe DOS/driver contexts
can still starve a monitor. Keep the current installed agent and boot path
until those experiments establish what can be supported reliably.

## Execution-hazard fixes and verification (2026-09-20)

Win16 and both OS/2 agents now reserve an invocation-specific directory using
atomic directory creation (`LX` plus six hex digits), skipping old directories
even after restart. A timed-out child's output cannot be reused by another
command. Win16's 30-second and OS/2 2.x's 60-second waits emit heartbeats and
report not-cancelled/uncertain completion on expiry. OS/2 uses its system clock
rather than counting sleeps. OS/2 1.3 never retries a command because its output
file cannot be opened; it reports the failure with the returned command status.
Completed Win16/OS2 2.x EXEC still uses synthetic status zero: observing
completion does not establish the underlying command's exit status.

Completed invocations remove their known files/directory. Timed-out or
connection-interrupted invocations retain their files for recovery. Inspect
and remove those directories only after confirming the child has stopped.
These retained files are not bounded background-job logs, and no automatic
orphan deletion is performed. Win16 rejects command tails that cannot fit
REDIR's DOS invocation rather than silently truncating them.

All three targets built with Open Watcom. Host tests inject wait expiry,
launch failure, disconnect, preexisting spool directories, and OS/2 1.3
missing output. They verify single execution, no false completion, heartbeats,
and separation from the next command. Compiled tests run in temporary
working directories.

Verified deployment preserved configuration and checked startup/executable
hashes on Win16, OS/2 1.3, and two newer OS/2 guests. A native test child ran
40 seconds on Win16 and 70 seconds on both newer OS/2 guests. The waits ended
with explicit unfinished results at 30.0 / 60.1 / 60.2 seconds. A second EXEC
completed while the first child still ran, and the first child's retained
output subsequently contained its own start/end markers, without the second
command's text. The initial OS/2 sleep-count loop exceeded its intended
60-second deadline; the system-clock version fixed that live discrepancy.
An OS/2 1.3 test child preserved exit status 7. Test files were removed.

## DOS prototype outcome

The separate [resident monitor experiment](../agent-dos/experimental/README.md)
was built and live-tested on FreeDOS and MS-DOS 6.22. An authenticated parent
control sent two heartbeats with 36 timer callbacks, but callbacks stopped
during child execution even when a separate observer reported InDOS zero.
FreeDOS completed all three workloads with intact parent canaries and
restored vectors; MS-DOS stalled in the disk phase and required a reset.
Both machines were returned to their normal agents, with original startup
files/configuration restored and experiment files removed.

This completes the feasibility prototype, not a production TSR. DOS has no
new advertised job capability. Investigating the child/poller interaction
and the disk stall is required before proceeding to a usable resident agent.
