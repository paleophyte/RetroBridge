# Licensing and provenance

The original code, scripts, tests, and documentation in this repository are
licensed under [MIT](LICENSE). The copyright holder is Josh Anderson, as
recorded in the development history. Third-party SDKs, libraries, tools,
guest software, and excerpts from those works retain their own terms. The
root license does not grant rights to them, including when they are supplied
locally to build this project. Product names identify interoperability targets
and do not imply endorsement.

## Source publication and dependency setup

Apple SDK files, Novell SDK objects/import catalogs, Novell patch packages,
and raw QuicKeys research excerpts have been removed from the publication
history. Private preservation copies are not release artifacts. No vendor ZIP
is published by this project, and the setup tool does not download anything.

This cleanup changes historical commit IDs. Use the cleaned publication
checkout for publishing; existing development clones should be replaced or
carefully migrated. Do not merge an old/private preservation history back
into the publication branch. Recovery bundles and commit maps remain private.

[The file manifest](docs/vendor-manifest.json) records each removed vendor
file's former path, size, SHA-256, package group, and whether it is needed by a
current build. These hashes identify the copies used in this lab, not a
vendor signature, a claim that the files are unmodified upstream originals,
or permission to redistribute them. Exact original acquisition URLs and
licenses are incomplete for several older packages; unknowns remain explicit.

Supply files from an SDK you are entitled to use. The tool accepts either
an original package directory or an extracted private preservation directory
containing the former repository paths. It copies only the required files,
checks every input before writing, and refuses unexpected hashes or conflicting
destination files. Preserve the original package and its terms separately.

```text
python tools/prepare_dependencies.py --component mac-sdk --source-root /path/to/MacTCP-headers
python tools/prepare_dependencies.py --component netware-sdk --source-root C:\SDKs\novell-clib-devel-2007.10.02-1netware_windows
python tools/prepare_dependencies.py --component mac-sdk --check
python tools/prepare_dependencies.py --component netware-sdk --check
```

Default destinations are ignored `.deps/mac-sdk` and `.deps/netware-sdk`.
Use `--destination` to prepare a directory outside the checkout. Set CMake's
`MAC_TCP_SDK_DIR` or the NetWare build's `NLM_SDK_DIR` to that directory.
Paths with spaces are supported. A different SDK revision needs a deliberate
manifest review and build/runtime validation; there is no skip-verification
switch. OS/2 DLLs remain user-supplied through the platform README instructions.

## External material

| Component | Provenance evidence and use | Distribution status |
|---|---|---|
| `mac-sdk` | `MacTCP.h` and `AddressXlation.h` identify Apple MacTCP 2.0.6 / Universal Interfaces 2.1b1 (1995). Former vendor README describes extraction from an installed SDK. Both are build headers. `dnr.c` from that extraction is unused. | Apple copyright notices are present; a redistribution grant was not established. User-supplied; unused source is not installed. |
| `netware-sdk` | `novell-clib-devel-2007.10.02-1netware_windows`; `fetch_sdk.bat` previously downloaded it from the [October 2007 NDK archive](https://archive.org/details/novell-developer-kit-oct-2007). `prelude.obj` is linked; `clib.imp` is retained as a checked import catalog. The other four former SDK files are unused by current builds. | The locally preserved `SDK_LICENSE`, dated September 22, 2004, is the Novell Developer License Agreement. Sections 2–4 describe conditional Developer Product/Derivative Software grants, additional file terms, and restrictions. They do not establish unrestricted redistribution of this SDK bundle. User-supplied; applicability to shipped NLMs remains a release review item. |
| `netware-312ptd` | `312PTD.EXE`, accompanying text, and extracted loader/patch files; package text references TIDs 2934544 and 2934545. Used only for guest maintenance/loader installation. Original download URL was not recorded. | No redistribution permission established. Not needed to compile; obtain separately if your guest needs it. |
| `netware-clibaux` | `clibaux1.exe`, `lib312d.exe`, text, and extracted CLIB support modules. Original acquisition URL was not recorded. Guest dependencies, not compiler inputs. | No redistribution permission established. Obtain appropriate modules for your licensed guest separately. |
| `netware-stuffkey` | `stufkey5.exe`, STUFFKEY.NLM, accompanying text identifying TID 2948742, and a separately staged CLIBAUX.NLM. The CLIBAUX origin is not independently established by the StuffKey package text. | Optional guest keyboard automation. No redistribution permission established for this collected directory. |
| IBM TCP/IP DLLs | OS/2 guest copies of SO32DLL.DLL, TCP32DLL.DLL, or TCPIPDLL.DLL; import libraries generated locally with `wlib`. These DLLs were already excluded from Git. | User-supplied operating-system components. Generated import libraries and guest DLLs are not included in releases by default. |
| [Watt-32](https://github.com/gvanem/Watt-32) | External DOS networking library linked statically; build instructions use `WATT32`. | Mixed file-specific terms. `src/copyrigh.h` permits producing programs but restricts certain library distributions/modifications and explicitly limits its scope to including files. Inspect the actual linked revision and preserve applicable notices before distributing DOS binaries. The old README's application restrictions are not a blanket license for the library. |
| Open Watcom | External compiler, headers, and runtime for DOS, Win16, OS/2, and NetWare. | Review the installed `license.txt` and runtime terms for the actual build. The compiler's license does not mean every output is licensed identically. No compiler/runtime package is vendored here. |
| MinGW/GCC | External Win32 compiler, headers, import libraries, and possible runtime objects. | Review the chosen toolchain's component notices and runtime exceptions for binary releases; do not apply MIT to the whole toolchain. |
| [Retro68](https://github.com/autc04/Retro68) and Multiversal | External classic-Mac toolchain and Toolbox declarations. | Multiple component licenses; Retro68 includes [COPYING.RUNTIME](https://github.com/autc04/Retro68/blob/master/COPYING.RUNTIME), whose exception applies only to covered files and eligible builds. This does not license Apple SDK files. Preserve notices applicable to linked runtime code. |
| newlib | Retro68's Mac toolchain statically links newlib `libc.a`; the reviewed installation reports 4.6.0. | Multiple file-specific licenses, separate from the GCC runtime exception. A release-wide notice collection and the actual linked member lists are recorded in the binary review below; fork-specific reconciliation remains required. |
| Python `mcp`, Pillow, and transitive packages | Installed externally through `mcp-server/requirements.txt`. | Retain the exact installed distributions' licenses if packaging them. They are not copied into this source repository. |
| OS media, ROMs, drivers, utilities | User-supplied guest environment, including optional packet drivers, StuffIt, and QuicKeys used during investigation. | Not supplied or relicensed by this project. Floppy/VM creation scripts do not grant rights to their input files. |

## Project-maintained compatibility code

The DOS and NetWare screenshot glyphs are an explicitly attributed exception
to the project's original MIT code: public-domain data from Daniel Hepper's
[font8x8_basic.h at commit 8e279d2d864e79128e96188a6b9526cfa3fbfef9](https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h),
based on Marcel Sondaar / IBM public-domain VGA fonts. The project retains
U+0020–U+007F and reverses each byte's bit order for MSB-left rendering.
The manifest records the upstream file hash. This replaces the earlier
unattributed table described only as "public-domain style"; glyph shapes
differ in some characters. Public-domain status is the upstream declaration,
not a new copyright claim by this project.

`agent-mac-system7/vendor/AppleTalk.h` was written here as a minimal type
stub, according to its source comment and development notes. It is not the
Apple SDK's header. `agent-netware/vendor/nwsock.h` and both OS/2
`vendor/os2sock.h` files are project-maintained minimal API declarations;
the OS/2 1.3 header records verification against guest DLL exports. These
remain under MIT. They carry no upstream license notice, and this review
found no recorded wholesale upstream-header import for them. That evidence
is not a claim of a formal clean-room development process.

The Mac mouse implementation was informed by observing QuicKeys 3.5.3 and
System 7 behavior. Its origin is documented in the
[public investigation summary](agent-mac-system7/QUICKEYS_CLICK_INVESTIGATION.md).
The former raw disassembly/transcript and embedded diagnostic signatures
are kept privately, not relabeled MIT. The project implements the behavior
in its own C/assembly source; this review does not certify legal clearance
of reverse engineering in every jurisdiction.

## Before publishing compiled releases

The [binary release review](docs/BINARY_RELEASE.md) records the observed
toolchain revisions, linked runtime inputs, per-platform release tasks, and
[collected notices](docs/binary-notices/README.md). The accompanying
[machine-readable inventory](docs/binary-provenance.json) fingerprints the
diagnostic builds and dependencies reviewed on 2026-09-20. This replaces the
open-ended inventory task with specific remaining requirements; it does not
declare every binary cleared for release.

Source publication and binary redistribution are separate reviews. Record
the exact toolchain and dependency revisions, inspect the actual linked
objects/libraries, collect their required notices, and resolve applicable
redistribution conditions. In particular, NetWare embeds Novell's prelude,
DOS embeds Watt-32, and the other targets can embed compiler/runtime code.
Keep vendor installers, SDK archives, guest DLLs, ROMs, and OS images out of
source/release archives. No blanket binary redistribution clearance is
claimed by this source cleanup.
