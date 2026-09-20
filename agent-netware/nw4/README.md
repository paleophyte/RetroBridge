# NetWare 4.x+ build (Novell CLIB)

This builds a format-v4 NLM using Novell's prelude and explicit CLIB imports.
It shares the agent source and runtime strategy with the 3.12 build, with
separate outputs. The corrected build has been loaded on NetWare 4.11 and
passed authenticated PING, SYSINFO, staged UPDATE, and full binary readback.
See the [publication audit](../../docs/PUBLICATION_AUDIT.md) for additional
network-deadline coverage and remaining limits.

## Build

Run `build.bat` in this directory with Open Watcom installed at `C:\watcom`.
It produces `LLMAGENT.NLM`. Keep the repository's `common/` directory beside
the agent directories because the source includes the shared timeout policy.

Requires verified local `clib.imp` and `prelude.obj`; run
`..\fetch_sdk.bat "C:\path\to\extracted-sdk"` to prepare the inputs in
`..\..\.deps\netware-sdk`, or set `NLM_SDK_DIR` for an external location.
The helper performs no download; see [THIRD_PARTY.md](../../THIRD_PARTY.md).
The post-link import rewrite
uses `mcp-server\.venv\Scripts\python.exe` when available, otherwise
`python` on PATH. Both NetWare build paths normalize Watcom's padded imports
to classic Novell length-prefixed names. Leave the NLM header at version 4;
changing it to version 3 is not the import-table fix.

## Deployment

With TCP/IP running, put the NLM and a configured `LLMAGENT.INI` in
`SYS:SYSTEM`, then use `LOAD LLMAGENT`. Shared helpers such as `UPDATE.NLM`,
`STUFFKEY.NLM`, and `CLIBAUX.NLM` also belong there. Follow the
[parent README](../README.md) for configuration and staged updates.

The shared network deadlines apply to this build: 10 seconds for the complete
authentication line, 60 seconds for the next command line, and 30 seconds
without binary I/O progress. These do not cancel executing commands.

## Earlier runtime and loader failures

The earlier static-runtime build could answer PING but crash during SYSINFO
on NetWare 4.11. Its link map resolved `sprintf` to Watcom's `clib3s.lib`
wrapper and `vsprintf` to Novell CLIB. Their `va_list` representations differ,
so the wrapper passed an incompatible argument list to the formatter.
The corrected build uses Novell CLIB throughout, including formatting,
file I/O, and startup. It neither links `clib3s.lib` nor imports `vsprintf`.
The real CLIB `ferror` function is used instead of Watcom's FILE-layout macro.

After an abend, restart the guest before loading a corrected image; do not
hot-update a suspended agent. Verify PING, SYSINFO, and another PING.

A loader error containing section names such as CODE or _TEXT, or private
variables such as `g_running`, indicates malformed import-name fields.
Those names are not additional NLM dependencies. Rebuild with the current
batch file and replace the deployment media before retrying.

The tests cover NetWare 4.11; other 4.x releases and CLIB/TCP/IP revisions
still need their own runtime checks. Console input, power operations, and
forced module unload are not covered by the network-deadline tests.
