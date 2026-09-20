# Long-running commands: findings and implementation proposal

Source review against `86cf91b`, 2026-09-20. This document records current
behavior and a proposed implementation; background job commands are not yet
implemented. The encoding changes in that commit do not change execution
lifetime or cancellation.

## Current behavior

| Agent | EXEC behavior | Consequence for long-running work |
|---|---|---|
| Win32 | Polls a shell process, streams stdout/stderr, sends five-second heartbeats; no runtime limit | Keeps the single command connection occupied. Disconnect handling terminates the direct shell, without guaranteeing termination of its children. |
| Win16 | Launches REDIR.EXE through WinExec; pumps messages while waiting up to 30 seconds | Timeout reports an error but leaves the child running. Later EXEC calls reuse LLMOUT.TMP, so output can collide with a still-running command. Completed EXEC reports zero rather than the child's real exit status. |
| OS/2 2.x | Starts an independent CMD.EXE session and polls LLMDONE.FLG for about 60 seconds | Does not distinguish deadline expiry from completion; can report EXIT:0 while work continues. A subsequent EXEC reuses the script, output file, and completion flag. PID/termination-queue limitations are already noted in the source. |
| OS/2 1.3 | Blocks in system(), then reads a rotating output file | No network service during the wait. If the output file cannot be opened, it executes the command again with different quoting, potentially repeating side effects. |
| DOS | Blocks in system(), then reads the output file | The same thread must pump Watt-32; it cannot service status/cancel requests while the child runs. |
| NetWare | Calls CLIB system() for a console command; returns no captured output | Synchronous console-command execution is not a tracked subprocess job. Generic process cancellation would be misleading. |
| Mac | No EXEC/shell implementation | Existing application launch/process/UI operations need their own completion semantics. |

These are findings from the current native handlers, not new live tests of
stalled processes. Relevant sources are `run_exec` in each agent's
`llm_agent.c`, Win16's `redir.c`, and the shared client's `AgentClient.exec`.

The Python socket timeout is an inactivity timeout, not an execution deadline.
Win32 heartbeats prevent it from ending a quiet command. Conversely, agents
that send nothing during execution can outlive the client's read timeout.
Closing a connection does not establish whether a command stopped. Increasing
the bridge timeout alone neither releases the native command processor nor
provides cancellation or recoverable results.

## Recommended implementation

Start with a native Win32 job interface, then adapt it to Win16 and OS/2.
Keep existing EXEC semantics for short commands. A job interface should expose:

- Start: launch once and return a job ID promptly, with explicit direct-program
  versus shell behavior. An uncertain launch response must not cause an
  automatic second execution.
- Status/list: report running, finished, cancellation requested, cancelled, or
  unknown; preserve a real exit status only where available. Allow recovery
  of an ID after a client disconnect.
- Output: retrieve captured bytes by offset through bounded reads, preserving
  the configured EXEC decoder at the bridge. Distinguish end-of-current-output
  from completed execution, and report truncation explicitly.
- Cancel: target retained process/session identity, rather than reopening a
  possibly reused PID. Report whether cancellation covers the direct process
  or descendants, and whether termination has actually been observed.
- Release: discard a completed job's retained output/resources explicitly.

The job must continue without its initiating TCP connection. PING, screenshots,
and status requests must remain usable during execution. Bound job slots,
retained output, per-poll work, and disk/memory use. Do not silently discard
old results to make room. Define agent restart, self-update, shutdown, and
job-retention behavior before deployment. Job IDs must not accidentally refer
to a different job after restart or slot reuse.

Win32 is the first target because it already retains a real process handle
and reads output without blocking on pipe EOF. Preserve compatibility with
Windows 95 and NT4: newer process-group/job-object APIs must be detected at
runtime, with honest limits when unavailable. The design must also ensure
that output is drained while no client is connected, rather than filling a
pipe and silently stalling the child. A background MCP task that merely keeps
the current EXEC connection open would not provide these native properties.

Before extending this model, fix Win16/OS2 shared-file overlap and OS/2's
false-success/implicit-reexecution paths. Use unique files per invocation,
retain outstanding invocation state, and report uncertain completion without
rerunning work. These fixes are useful even without a full job interface.

For plain DOS, consider a separate run-and-resume mode: acknowledge/stage work,
run it while the agent is unavailable, then reconnect and retrieve a persisted
result. That is not concurrent execution or remote cancellation. A TSR or
multitasking redesign would be a separate project. For NetWare, inspect the
specific console/NLM lifecycle before exposing any cancellation operation.

## Required verification

Test a quiet job longer than the current client timeout while independently
issuing PING/screenshots and retrieving status. Cover a chatty job exceeding
the output cap, a nonzero exit, binary output, concurrent jobs, full slot
capacity, a client disconnect immediately after launch, repeat queries,
malformed job IDs/offsets, cancellation failures, and a child that launches
another process. Check retained output and handle/file cleanup after each.

Live Win32 verification must include both 9x and NT, not just a modern host.
For Win16/OS2, test a second command after the original wait deadline and
verify that neither job's script/output/completion marker is reused. DOS and
NetWare must advertise their actual restrictions rather than inherit the
Win32 capability profile.

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
