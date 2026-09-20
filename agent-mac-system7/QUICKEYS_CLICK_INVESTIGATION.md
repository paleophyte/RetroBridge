# Mac mouse input: provenance and findings

The mouse-input investigation used QuicKeys 3.5.3 (CE Software) on System
7.5.3 as a behavioral reference. It inspected event-queue activity and
low-memory mouse state through emulator debugging and guest diagnostics.
This was reverse engineering, not a formal clean-room implementation.

The useful observation was that cursor position and event position must
agree: update the relevant low-memory mouse globals, allow cursor tracking
to consume the change, then post mouse-down/up queue entries with the
intended position and modifiers. The resulting agent implementation is in
`SetMouseTo`, `PPostEventTrap`, and `PostMouseEvent` in `llm_agent.c`. It
includes bounded cursor-update waiting and checks event-posting failures.

QuicKeys is not required at runtime. Clicks, double-clicks, and typing work;
dragging still has the tracking-loop limitation described in the README.
Successful event injection does not establish that every Toolbox tracking
loop responds to synthetic input.

The earlier handoff document included raw disassembly, resource signatures,
debugger transcripts, and private lab setup details. It is preserved
privately and removed from the publication history. This summary records
the implementation's origin without reproducing those excerpts. The
generic `findsig.py` diagnostic now takes signatures supplied by its user.
See [THIRD_PARTY.md](../THIRD_PARTY.md) for license scope and remaining
binary-release review.
