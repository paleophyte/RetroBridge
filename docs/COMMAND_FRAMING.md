# Agent command-line framing

All seven agent ports use `common/command_line.h` for authentication and
command lines. This validation runs on the agent, including for raw TCP
clients that bypass the Python bridge. Rebuild and deploy the agent to
enable it.

| Agent | Maximum line content, in bytes |
| --- | --- |
| Win32 | 4094 |
| DOS, Win16, both OS/2 ports, NetWare, Mac System 7 | 510 |

The terminator is LF or CRLF and does not count toward that limit. These
limits match the shared client's existing `max_command_bytes` settings.
They count encoded bytes, not characters. Individual commands, configured
tokens, and legacy command interpreters can have smaller limits.

A command is dispatched only after the complete line arrives. An overlong
line, embedded NUL, or CR followed by anything except LF fails the session.
The agent closes the connection without a protocol reply, discards the
partial line, and does not execute queued commands from that connection.
It does not try to recover by interpreting the offending line's remainder.
EOF before LF also fails the session. A fresh connection must authenticate.

A CR waiting for LF remains subject to the existing absolute line deadline;
it cannot keep the server occupied indefinitely. See
[network deadlines](NETWORK_TIMEOUTS.md) for timing and cleanup behavior.

Tabs and non-ASCII bytes remain ordinary line content. This change does not
add encoding conversion or validate each command's arguments. An empty line
still reaches the existing authentication/command handler. Separate complete
commands may still be sent on one authenticated connection.

Binary PUT and Mac UPDATE bodies are read by their existing binary readers.
Their declared bytes may include NUL, CR, LF, or text resembling commands.
Framing checks neither modify those bytes nor treat them as new lines.
Likewise, binary replies keep their existing SIZE/LEN framing.

## Verification

`tests/test_command_lines.py` compiles the production line readers for all
seven ports. It tests authentication/command smuggling, malformed terminators,
EOF, exact limits and overflow, buffer guards, sticky failure, valid pipelined
lines, and the transition from a PUT header to an opaque binary body.
The separate timeout suite exercises the production network loops and line
deadlines. Live deployment coverage is recorded in the
[publication audit](PUBLICATION_AUDIT.md).
