# MCP coverage and platform extensions

The bridge registers 43 tools. Restart an existing bridge process to load
new tools. The five background-job tools require a native build advertising
`exec_jobs=1`; the other coverage additions below use existing native commands.

`legacy_job_start/status/output/cancel/release` expose tracked commands with
explicit retention, byte offsets, and platform cancellation limits. See
[long-running commands](LONG_RUNNING_COMMANDS.md) for wire syntax, lifecycle,
output limits, and deployment evidence. Old agents are rejected before a
job command is sent. Update tools refuse retained jobs, including completed
results, until their output has been collected and they have been released.
The installed agents must already implement the requested command.

`legacy_capabilities(machine)` reads SYSINFO and returns a JSON report of
the inferred platform, reported identity, applicable tools, and limitations.
It uses source-tree profiles, not a new capability-negotiation command.
Older or custom builds can differ. Unknown profiles receive no inferred
platform operations, and unrelated SYSINFO fields are not copied into the
report. NT autologon helpers are omitted for Windows 9x.

New platform-specific tools check SYSINFO before sending their operation.
They return an explanatory error for the wrong family or an inventory-only
entry. Existing general tools still rely on agent replies for unsupported
operations. No raw-command escape hatch is exposed.

| Platform | MCP tool | Native operation / important behavior |
|---|---|---|
| Win16 | `legacy_winlist(machine, parent_hwnd=None)` | `WINLIST [hwnd]`; no parent lists top-level windows, a parent lists immediate child controls |
| Win16 | `legacy_winmsg(machine, hwnd, msg, wparam=0, lparam=0)` | Synchronous `WINMSG`; returns numeric LRESULT; can occupy the agent while a modal dialog is open |
| Win16 | `legacy_postmsg(machine, hwnd, msg, wparam=0, lparam=0)` | Queued `POSTMSG`; acknowledgment is not proof of UI completion |
| Win16 | `legacy_lbgettext(machine, hwnd, index)` | `LBGETTEXT`; zero-based item in a string-backed LISTBOX, full text up to the agent's 32767-byte limit |
| OS/2 1.3 and 2.x | `legacy_winclose(machine, title)` | `WINCLOSE`; close/cancel requests to every exact title match, ignoring case; apps may prompt or refuse |
| NetWare | `legacy_screens(machine)` | `SCREENS`; JSON rows `[id, displayed, name]`, without switching screens |
| NetWare | `legacy_autoexec(machine)` | `AUTOEXEC`; may add startup loads and retain LLMAUTO.BAK; ambiguous/reversed entries return errors |
| NetWare | `legacy_debug(machine, enabled=None)` | Omit `enabled` to query; enabling truncates LLMAGENT.LOG |
| NetWare | `legacy_netware_self_update(machine, new_agent_local_path, update_nlm_local_path)` | Read-back-checked staging in fixed SYS:SYSTEM paths, followed by bare `UPDATE` |
| Mac | `legacy_double_click(machine, x, y, button=1)` | `DBLCLICK`; global signed 16-bit coordinates, left button only |
| Mac | `legacy_mouse_position(machine)` | `MOUSEPOS`; JSON object with `x`, `y`, and `button` (0 or 1) |
| Mac | `legacy_mac_self_update(machine, new_agent_local_path)` | `UPDATE <size>` followed by a complete MacBinary application |

Win16 messages use **Win16 constants**, which can differ from Win32 control
messages. For example, the Win16 headers define LB_GETCOUNT as WM_USER+12.
Only scalar numeric values are passed: strings, buffers, and pointers are
not marshalled into the target application's address space. Use LBGETTEXT
for listbox strings rather than passing a pointer-bearing message. Journal
mouse/keyboard injection remains unreliable; these wrappers do not repair it.

## Update result semantics

All four update tools wait for verified replacement by default. Passing
`wait_for_agent=False` reports acceptance only. A quiet settling interval
precedes verification, and older replacement builds without identity fields
remain explicitly unverified.

Windows/OS2, Win16, and NetWare require a new startup instance, the expected
startup executable SHA-256, installed executable readback, and an unchanged
identity after readback. NetWare uses the loader-supplied, volume-qualified
NLM path; an unavailable path/hash cannot verify. Its tool freezes both
inputs, reads back staged `SYS:SYSTEM\LLMAGENT.NEW` and `UPDATE.NLM`, then
sends UPDATE. It never sends console UNLOAD. The helper's weaker recovery
and replacement of its `.OLD` backup remain separate limitations.

The Mac tool freezes and validates a classic MacBinary APPL, then compares
both its forks against startup and fresh disk fingerprints from a new
instance at the same application location. The resource hash uses explicit
`sha256-zero-system-16-127-v1` semantics: bytes 16 through 127 are normalized
to zero because System 7 writes directory metadata there during installation.
The 16-byte layout header, application-owned area from byte 128 onward,
resource data, and map remain covered. Header bounds are checked before
normalization. The data fork uses ordinary SHA-256. See the
[Mac update details](../agent-mac-system7/README.md).
The separately installed `llm_updater` is not replaced by this tool.

These are startup-file fingerprints, not memory-image attestation or code
signatures. They detect an unchanged instance, wrong file, rollback, failed
reads, and identity changes during verification under the existing trusted
lab-agent model; they do not protect against a malicious guest.

Neither tool automatically retries an uncertain update transfer/handoff.
`legacy_wait_for_agent` only establishes reachability. It cannot turn
acceptance or a build label into verified replacement.

## Intentional exclusions and remaining limits

Connection `QUIT` is managed by the shared client. Mac `QUITAGENT` and OS/2
1.3 `SELFEXIT` remain lifecycle primitives without standalone MCP tools.
Mac `DRAG`, `DRAGSTAT`, and `DRAGRESET` remain experimental: dragging does
not reliably terminate. Disabled Mac CLIPGET/CLIPSET/WINLIST operations do
not acquire functionality through the bridge.

Explicit ANSI/OEM/Mac conversion is supported through per-machine settings;
`legacy_capabilities` reports the selected codecs, and `legacy_exec` accepts
an optional output decoder. See [text encodings](TEXT_ENCODINGS.md). Keyboard
input remains ASCII only; DBCS and automatic locale negotiation are unsupported.
Single-client execution occupancy and consistent native capability/startup
identity fields remain separate work. Read the [publication audit](PUBLICATION_AUDIT.md) and per-agent READMEs
for live validation and other platform limits.
