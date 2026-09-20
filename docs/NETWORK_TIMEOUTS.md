# Agent network deadlines

All seven agent ports share the policy in
[`common/session_timeout.h`](../common/session_timeout.h). Rebuild and deploy
the agent to enable it; restarting the MCP bridge alone does not update a
target's network behavior.

| Phase | Default deadline |
| --- | --- |
| Complete authentication line | 10 seconds |
| Complete next command line, including time spent idle | 60 seconds |
| Upload data | 30 seconds without receiving bytes |
| Reply data | 30 seconds without sending bytes |

Authentication and command deadlines cover the entire line. Sending one
character occasionally does not extend them. Binary transfers can take longer
than 30 seconds if they keep making progress. TCP send progress means the local
stack accepted bytes, not that the remote application consumed them.

Malformed lines also fail the session immediately. See
[command framing](COMMAND_FRAMING.md) for length limits, LF/CRLF endings,
and rejection of embedded NUL or misplaced CR bytes.

On a deadline, the agent abandons that connection and accepts another one.
It does not acknowledge a partial upload, append another reply to a partial
response, or execute queued commands from the failed session. A caller must
reconnect and authenticate again. Raw persistent clients should finish their
next command within 60 seconds; the shared Python client ordinarily opens a
connection per operation.

PUT can leave a created, truncated, or partially written destination after a
disconnect. It is not a transactional replacement. Mac UPDATE removes a failed
staging upload and keeps the installed application running.

## Platform implementation

- Win32 and Win16 use nonblocking Winsock calls. Only would-block errors retry;
  Win32 sleeps between attempts and Win16 pumps window messages and yields.
- DOS uses Watt-32's nonblocking `sock_fastread`/`sock_fastwrite` and its timeout
  API while continuing to pump TCP and yield the CPU.
- NetWare uses nonblocking CLIB sockets, cooperative thread switches, and
  `GetCurrentTicks`. Its tick resolution is approximately 1/18 second, per the
  [Novell CLIB reference](https://www.novell.com/documentation/developer/clib/pdfdoc/prog_enu/prog_enu.pdf).
- OS/2 uses nonblocking IBM sockets and the kernel's millisecond uptime clock.
  SO32DLL uses command `0x667e` with a four-byte flag and a length argument.
  The 16-bit TCPIPDLL interface takes a 16-bit command and a far pointer, with
  no length argument. These are not interchangeable with newer BSD ioctl
  declarations. The 1.3 port waits on IBM's bounded `select` call so it can
  wake on incoming data rather than waiting for another scheduler tick.
- MacTCP receives and ordinary sends run asynchronously. On timeout the agent
  aborts the stream, then waits for the outstanding request to complete before
  releasing its parameter block or buffers. Finder's final power-command reply
  retains its existing synchronous MacTCP send with a 30-second native timeout.

The IBM and NetWare send loops treat zero-byte writes as no progress and retry
within the deadline. Some legacy stacks return zero for a full send buffer.
DOS, OS/2, and NetWare upload bodies are read in chunks to avoid a clock query
for every payload byte. Win16 window-close handling leaves socket cleanup to
the server loop, avoiding duplicate closes while pumping messages.

Keep the repository's `common/` directory alongside the agent directories when
building. The defaults are compile-time values (`NET_AUTH_SECONDS`,
`NET_LINE_SECONDS`, `NET_IO_SECONDS`); there are no new INI settings.

## Remaining limits

These are network wait limits, not command cancellation or a total session
lifetime. An executing command, blocked disk or OS call, or application dialog
can still occupy the single command processor. In particular, DOS cannot pump
its network or receive cancellation while a synchronous `system()` child runs.
Win32 EXEC retains its output heartbeats and has no overall runtime limit.

An authenticated client that keeps issuing commands or steadily transferring
data can still retain the server. There is no fairness scheduler, minimum
transfer rate, connection rate limit, or per-client quota. TCP connection setup
and half-open connection handling remain the network stack's responsibility.
Keep the agents on a trusted network; these deadlines do not make the protocol
suitable for direct public exposure.

## Verification

`tests/test_session_timeouts.py` exercises the production network loops with
silent clients, trickled lines, stalled uploads and replies, timer wraparound,
sticky failure, progressing transfers, and long command execution. It also
checks that MacTCP's asynchronous request storage remains alive until an abort
completes. See the [publication audit](PUBLICATION_AUDIT.md) for live deployment
coverage and outstanding platform verification.
