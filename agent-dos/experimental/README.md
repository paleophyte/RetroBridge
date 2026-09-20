# Resident monitor experiment — not a production agent or TSR

**Status: deferred after the feasibility experiment (2026-09-20).** This is
not a prerequisite for source publication. The [DOS resident monitor design](../../docs/LONG_RUNNING_COMMANDS.md#dos-resident-monitor-feasibility)
records the original TSR concept, the parent-resident prototype approach,
and why a tiny shim that loads the full agent still needs safe DOS access,
available conventional memory, and a single owner of network state.

The source in this directory implements only the probe described below.
No stay-resident installation, background command dispatcher, or safe child
cancellation was implemented. Before resuming network work, isolate timer
delivery with a minimal resident counter, explain the missing child-time
callbacks, and reproduce the MS-DOS disk stall with the smallest possible
test. Both DOS variants must pass before expanding the scope. A foreground
run-and-resume design with persisted results remains an alternative proposal;
it would restore remote access after the child exits, not during execution.

`DOSPOLL.EXE` keeps a parent process in memory while fixed DOS children run.
It tests Watt-32's existing timer poller and a tiny heartbeat callback before
attempting a full resident agent. **The MS-DOS 6.22 disk test stalled and
required resetting the VM. Do not install this as the normal agent or make
it an unattended boot default.** Use a disposable test guest with console
recovery available. The normal DOS agent and its startup menu are unchanged.

Build with `build.bat` using the same Watcom/Watt-32 prerequisites as the DOS
agent. This produces `DOSPOLL.EXE` and `PROBECH.EXE`. Keep them in an isolated
8.3 directory with a copy of the test agent's `LLMAGENT.INI`, and set
`WATTCP.CFG` to the existing network configuration directory. Only one
Watt-32 process should own this network setup: stop the normal agent first.
Run from the experiment directory because the child uses `PROBE.TMP` there.

The experiment listens on the configured agent port for up to 45 seconds.
It accepts the usual token line, replies `OK`, then requires the literal
`PROBE` command. It does not accept arbitrary commands. The controller must
keep that connection open with a read timeout over eight seconds. Responses
are experiment-specific ASCII lines, not normal EXEC or job framing:

- A parent-only timer control emits `BUSY` heartbeats and a `CONTROL` count.
- Four phases run a CPU child without polling, then CPU, disk, and BIOS
  keyboard-poll children with polling requested. Each targets eight seconds;
  that is not a hard bound if DOS I/O stops returning or timer progress stops.
- `DONE` records the child result, callback/write counts, a parent-stack
  canary check, and stack addresses. `FLAGS` samples timer delivery and the
  InDOS/critical-error bytes from an INT 1Ch observer during the child.
- `END vector_restored=1` confirms restoring the saved timer/observer vectors.
  `PROBE.LOG` retains foreground results, though a reset can lose buffered
  file metadata. There is no cancellation or in-experiment reconnect.

The callback and observer live in `probe_isr.c`, built separately with
Watcom `-s`, as Watt-32's interrupt code is. Ordinary foreground stack probes
compare against the wrong stack in that context; an initial control run
reported Stack Overflow and required recovery. Only that module disables
compiler stack checks. The callback uses fixed memory and one nonblocking
write attempt; it performs no DOS calls, file I/O, allocation, command
dispatch, or child launch. Watt-32 handles its private stack and DOS-busy
guard. Its full TCP poll is not a proven bounded, interrupt-safe scheduler.
The small parent canary does not certify the poller's private-stack capacity.
Normal exits restore vectors; a fatal interrupt/driver failure can still
require resetting the guest.

## Live results, 2026-09-20

Tested through a one-shot wrapper that restored the original boot file
before entering the experiment. Production binaries/configuration remained
unchanged. The dual-boot machine's Windows default was also restored first.

| Environment | Parent control | Child workloads | Recovery |
|---|---|---|---|
| FreeDOS 1.4 | 36 callbacks, two live heartbeats in about two seconds | CPU, disk, and keyboard-poll children completed with exit 7 and intact parent canaries, but zero poller callbacks/heartbeats during every child | Timer vectors restored; normal agent resumed; original boot/config verified; test files removed |
| MS-DOS 6.22 | 36 callbacks and two live heartbeats | CPU children completed, but no poller callbacks occurred while they ran; the disk phase then stopped making progress | Reset required; original Windows boot restored and authenticated Win16 agent verified; test files removed |

On FreeDOS, the independent observer saw 145 timer ticks per child. InDOS
was zero during CPU/keyboard polling and one during the disk workload; the
sampled critical-error byte stayed zero. Thus DOS-busy gating alone does
not explain the absence of network polling during CPU children. The exact
interaction with child execution remains unresolved. MS-DOS CPU observations
were similar; its disk/remaining phases were not completed successfully.

This is evidence against enabling the poller in the production EXEC path
yet. Next work should isolate why the native poller stops invoking its
callback across child execution, then investigate the disk stall with a
minimal ISR and bounded network work. Revalidate on both DOS variants before
adding resident status, queued commands, or any TSR installation mechanism.
