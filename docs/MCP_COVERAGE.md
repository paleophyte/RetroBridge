# MCP coverage and platform extensions

The bridge registers 38 tools. Restart an existing bridge process to load
new tools; this bridge change does not require rebuilding the native agents.
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

The Windows/OS2 and Win16 update tools retain their existing startup-identity,
SHA-256, and installed-file checks. The new Mac/NetWare tools deliberately
report **replacement NOT verified**, even after an OK acknowledgment.

The Mac client freezes the input bytes, validates the classic MacBinary APPL
header and padded fork lengths, then sends one size-delimited transfer. It
does not upload or replace `llm_updater`; deploy that companion separately.
OK means the agent staged the bytes and requested helper launch. The helper
performs its native fork verification and rollback procedure. Reconnect and
inspect SYSINFO and UPDATER.LOG; the current agent does not expose a loaded
resource-fork hash/startup identity sufficient for strong bridge verification.

The NetWare tool freezes both local inputs, uploads them as
`SYS:SYSTEM\LLMAGENT.NEW` and `SYS:SYSTEM\UPDATE.NLM`, and reads each back
byte-for-byte before sending UPDATE. It never sends console UNLOAD. OK is
sent before the native handler runs LOAD UPDATE, so it does not prove helper
launch. Inspect the UPDATE console output and installed NLM after reconnecting;
the current agent does not expose a loaded-image hash/startup identity.
The existing helper replaces its `.OLD` backup and provides less recovery
protection than the Mac updater. Keep independent backups for deployments.

Neither tool automatically retries an uncertain update transfer/handoff.
`legacy_wait_for_agent` only establishes reachability. It cannot turn
acceptance or a build label into verified replacement.

## Intentional exclusions and remaining limits

Connection `QUIT` is managed by the shared client. Mac `QUITAGENT` and OS/2
1.3 `SELFEXIT` remain lifecycle primitives without standalone MCP tools.
Mac `DRAG`, `DRAGSTAT`, and `DRAGRESET` remain experimental: dragging does
not reliably terminate. Disabled Mac CLIPGET/CLIPSET/WINLIST operations do
not acquire functionality through the bridge.

Non-ASCII ANSI/OEM/Mac text conversion, single-client execution occupancy,
and consistent native capability/startup identity fields remain separate
work. Read the [publication audit](PUBLICATION_AUDIT.md) and per-agent READMEs
for live validation and other platform limits.
