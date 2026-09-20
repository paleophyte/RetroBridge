# Text encodings

The bridge accepts Unicode strings, but the native agents use legacy byte
strings. Configure the guest's actual code pages in `machines.ini`. The bridge
does not infer a locale from the OS family or use the controller's locale.
This change only requires the updated Python bridge, not rebuilt agents.

| Per-machine setting | Applies to | Default |
|---|---|---|
| `text_encoding` | General command arguments and textual replies, including window titles, registry values, clipboard text and Win16 LBGETTEXT | `ascii` |
| `file_encoding` | GET/PUT remote filenames only | `text_encoding` |
| `exec_command_encoding` | EXEC command input only; EXECDETACH still uses `text_encoding` | `text_encoding` |
| `exec_encoding` | Captured EXEC output | `text_encoding` |

These are also keyword arguments to `AgentClient`. `legacy_capabilities`
reports the resolved settings. Existing bridge processes must be restarted
to load code or inventory changes; settings are cached for the process.
The inventory itself is UTF-8 (an optional BOM is accepted), and authentication
tokens remain ASCII. Binary uploads/downloads, screenshots, MacBinary forks,
and `AgentClient.exec().output` remain unchanged bytes.

## Choosing settings

For an English Windows NT/9x guest whose ANSI/OEM pages are 1252/437:

```ini
text_encoding = cp1252
exec_encoding = cp437
```

For the tested English Windows 3.11 agent, add `file_encoding = cp437`.
Its Watcom file APIs use DOS filenames, while window text and WinExec input
use ANSI. WinExec translates an ANSI command line for the DOS shell; sending
OEM bytes as EXEC input corrupted accented characters in the live test.
Leave `exec_command_encoding` inherited unless the actual command handler
requires another encoding. Use ASCII installation paths where possible:
native startup/update paths have not been audited across localized systems.

For DOS and OS/2, check the active code page before setting `text_encoding`.
The tested FreeDOS guest uses `cp858`, while two tested OS/2 guests use `cp437`.
These are observations, not defaults for every installation. An English
System 7 installation using the Roman script uses `text_encoding = mac_roman`.
Confirm the server locale for NetWare; do not assume a code page from its
version number. Entries without a confirmed encoding can retain ASCII.

On Windows, GetACP and GetOEMCP identify system ANSI/OEM pages, but console
applications can select other pages or produce another encoding. `chcp`
describes the current shell's console page. A program's redirected output
can still differ. See Microsoft's [GetACP](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getacp),
[GetOEMCP](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getoemcp),
and [console application encoding guidance](https://learn.microsoft.com/en-us/windows/console/console-application-issues).

For a program explicitly producing UTF-16LE, use
`legacy_exec(machine, command, output_encoding="utf-16-le")`. This per-call
override changes only the output decoder; it does not change the guest's
console, command input, or program behavior. A process emitting mixed
encodings cannot be decoded reliably with one codec; use raw output through
the Python client or redirect output to a file and download it as bytes.

## Strict conversion and limits

ASCII is now the safe default, replacing implicit UTF-8 commands and lossy
response decoding. Existing inventories need explicit settings for non-ASCII
text. Unrepresentable input fails before connecting, without sending a partial
command. Command limits count encoded bytes, including the verb and separators.
Invalid response bytes produce an explicit decoding error instead of replacement
characters. A wrong single-byte code page can still produce plausible but
incorrect characters; strict decoding cannot detect every mismatch.

If EXEC output cannot be decoded, the result includes **command already
executed** and its exit status. Do not rerun a command merely to recover its
output. Choose the correct decoder before executing commands with side effects.

Supported text codecs include ASCII, explicit UTF-8, Latin-1/ISO-8859
(1-11 and 13-16), Windows 1250-1258, OEM pages 437/720/737/775/850/852/855/
857/858/860/861/862/863/865/866/869, and Mac Roman/Latin2/Cyrillic/Greek/
Iceland/Turkish. Python codec aliases are accepted. UTF-8 is for agents and
applications known to accept it; it does not make stock legacy APIs Unicode.
EXEC output additionally accepts UTF-8 with BOM and UTF-16 variants. Stateful,
escape, transform, host-dependent `mbcs`, and DBCS codecs are rejected. The
native parsers and path/input routines have not been audited for DBCS.
See [Python's codec documentation](https://docs.python.org/3/library/codecs.html)
for codec names; the allowlist in `mcp-server/text_codec.py` is authoritative.

`TYPE` and `KEY` accept ASCII only. Native keyboard implementations may drop
unmapped characters or partially type a string before failing; selecting a
codec does not implement keyboard layout mapping. Use clipboard text where
CLIPSET is supported, or transfer a file with the application's expected bytes.
This restriction does not fix existing platform-specific keyboard injection
limitations. General text codecs also do not add missing native functionality.
