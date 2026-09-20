# Collected binary notices

These files retain third-party terms; the root MIT license does not replace
them. They contain license/notice text, not SDK or runtime implementations.
Read [BINARY_RELEASE.md](../BINARY_RELEASE.md) before assembling a release.
Collection does not by itself satisfy every distribution condition.

| File | Origin and use |
|---|---|
| `MinGW-w64-runtime.txt` | Installed MSYS2 CRT package's `COPYING.MinGW-w64-runtime.txt`; include with Win32 binaries. |
| `GCC-Runtime-Exception.txt`, `GPL-3.0.txt` | Installed MSYS2 GCC library license files; include for covered GCC/Retro68 runtime components. |
| `Open-Watcom.txt` | Installed Watcom `license.txt`; DOS/Win16/OS2 runtime source/embedded-notice requirements remain open. |
| `Multiversal.txt` | `multiversal/COPYING` at the recorded submodule revision; Mac interface declarations/stubs. |
| `Retro68-runtime.txt` | Copyright/license comment from `libretro/start.c` at the recorded Retro68 revision. No implementation copied. |
| `newlib-4.6.0.txt` | `COPYING.NEWLIB` from the official 4.6.0.20260123 archive. Reconcile the Retro68 fork before release; this collection covers more than the selected Mac members. |
| `Watt-32-collected.txt` | Copyright/license comments from the mapped local Watt-32 source files, plus the common notice/restrictions, companion `inc/copying.bsd`, and advertising acknowledgment. Coverage gaps remain; this is not an exhaustive clearance. |
| `Font8x8.txt` | Upstream public-domain attribution for DOS/NetWare screenshot glyphs; revision and transformations are recorded in `THIRD_PARTY.md`. |

Text is stored as UTF-8 with LF line endings, tabs expanded, and trailing whitespace removed.
Notice wording is preserved, including historical spelling and URLs.
[binary-provenance.json](../binary-provenance.json) records the collected-file
hashes and exact observed dependency revisions. Novell/Apple SDK payloads and
their private preservation archives remain outside this repository.
