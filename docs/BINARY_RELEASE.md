# Binary release provenance

Review date: 2026-09-20. The source remains MIT, with the exceptions in
[THIRD_PARTY.md](../THIRD_PARTY.md). Compiled agents also contain dependency
code. This review inventories that code and prepares notices; it does **not**
mark every platform ready for binary redistribution.

## Evidence and scope

[binary-provenance.json](binary-provenance.json) records the reviewed source
commit, tool versions, package revisions, SHA-256 fingerprints, linker inputs,
archive member names, diagnostic artifacts, and collected notices. Paths use
symbolic build/toolchain roots, not private machine locations. These fingerprints
identify observed files; they are not vendor signatures or reproducible-build
attestations. Empty archive-member lists identify libraries listed by Watcom
without ordinary object-module entries, typically import libraries. GNU maps
can list members whose sections are subsequently discarded.

All seven Windows-host build configurations passed: DOS, Win16, Win32,
OS/2 1.3, OS/2 32-bit, NetWare 3.12, and NetWare 4.11. Nineteen production
executables/helpers were inventoried. Maps were enabled in private build
copies. For the Win32 updater the diagnostic invocation also listed the
agent's import libraries; its PE import table shows which were actually used.
These builds were not deployed and are not release assets.

The two Mac application code images were successfully relinked with maps
using the existing build objects. Nine application source/build files matched
the current checkout. This was not a clean Mac rebuild or proof that every
transitive header matched; final release builds must capture those inputs too.
The Mac input libraries and member lists are recorded separately from the
nineteen Windows-host artifacts.

## Platform disposition

| Target | Code incorporated into the binary | What remains before a binary release |
|---|---|---|
| Win32 | MinGW-w64 CRT/startup, `libmingw32`, `libmingwex`, and selected `libgcc` members; system import stubs | Notice set prepared. Include it with final artifact hashes and retain the recorded toolchain inputs. No additional license blocker was identified for this observed linkage. |
| DOS | Watcom DOS large-model runtime and 35 Watt-32 source modules | Complete the Watcom source/notice steps and resolve the Watt-32 file-coverage gaps below. Do not package the modified networking library itself. |
| Win16 | Watcom Windows runtime; DOS runtime in `JOBRUN` and `REDIR` | Complete the Watcom source/notice steps for all helpers, not just the agent. Windows/Winsock DLLs remain guest prerequisites. |
| OS/2 1.3 and 32-bit | Watcom runtime, including the one runtime module reported for `IOSEG.DLL`; OS import libraries | Complete the Watcom source/notice steps. IBM networking DLLs and generated import libraries remain build/guest inputs, not payloads. |
| NetWare 3.12 and 4.11 | Novell `prelude.obj`; both agents and `UPDATE.NLM` import CLIB dynamically | A conditional Developer Product distribution path is supported by the SDK evidence below. Record the publisher's applicability/terms decision before shipping NLMs. No Watcom static runtime appears in these maps. |
| System 7 Mac | Retro68 runtime, GCC helpers, newlib, and Multiversal interface stubs | Notices collected. Finish the fork-specific newlib notice check and establish the applicable MacTCP SDK terms. The Apple headers are not licensed by the Retro68 exception. |
| MCP bridge | Python source; externally installed `mcp`, Pillow and dependencies | A source-only bridge does not bundle these packages. If creating a frozen app, installer or container, inventory that exact closure and include Python, wheel and native-library notices; the two recorded test environments are not release locks. |

## Verified versions

- Open Watcom: **2.0 beta, September 4, 2026**. The 16-bit compiler reports
  05:21:28 and the linker 05:16:04, both 64-bit host tools. A local installer
  was recovered and fingerprinted; all recorded installed tools, license and
  linked Watcom libraries matched its ZIP contents. The corresponding upstream
  source commit is still unknown. Installed-file hashes and the installer
  digest are recorded; a date or rolling `Current-build` tag is not a substitute
  for an immutable source revision.
- MinGW32: MSYS2 **GCC 16.1.0-5**, GCC libraries 16.1.0-5, binutils 2.46.1-2,
  CRT/headers **14.0.0.r150.g6b5798fd4-1**. Both PE files import guest
  `msvcrt.dll`; neither requires a distributed libgcc DLL. Other imports are
  listed per artifact. No Microsoft DLL is included in the notice bundle.
- Watt-32: **c727114d209138e3057c122134cf7c3f3e67c374**, with one tracked local
  change to `src/config.h`: large-model defaults disable `USE_DEBUG` and enable
  `USE_DHCP`. The linked `wattcpwl.lib` is fingerprinted. Its correspondence
  to this source/configuration was not independently established by rebuilding
  the library in this review.
- Retro68: **f99ecb5aeb6fbb004c647518ea760daba2b1f6bb**, clean tracked tree;
  Multiversal **ac0a2950ff72e29cf59a7d120d4d8f2546518525**, also clean.
  Compiler **GCC 16.1.0**; installed newlib headers report **4.6.0**. The
  resolved `libInterface.a` comes from Multiversal, not Apple's InterfaceLib.
  GCC, binutils and newlib source are inside the recorded Retro68 tree.
- Novell: **novell-clib-devel-2007.10.02-1netware_windows**. The SDK inputs
  match the existing vendor manifest. Apple headers retain the existing
  MacTCP 2.0.6 / Universal Interfaces 2.1b1 identification and hashes.

## Notices and remaining decisions

### MinGW and GCC

Include `MinGW-w64-runtime.txt`, `GCC-Runtime-Exception.txt`, and `GPL-3.0.txt`
from [binary-notices](binary-notices/README.md) with Win32 binaries. The installed
MinGW notice is specifically intended for statically linked applications.
The GCC exception permits covered runtime code to be combined with independent
modules under an eligible compilation process; it does not relicense every
toolchain component. The observed builds use ordinary C compilation/linking.
See the [FSF exception](https://www.gnu.org/licenses/gcc-exception-3.1.en.html).

### Open Watcom

The installed `license.txt` is Sybase Open Watcom Public License 1.0.
Sections 2.2(d) and 4 require attention for embedded runtime portions: provide
a prominent source-availability notice in the executable and accompanying
documentation, identifying how to obtain the covered source. Section 2.2(c)
addresses deployed modifications. Section 2.2(e) also constrains an alternative
license used for the covered object code. Preserve the runtime's own terms;
do not present the entire executable as exclusively MIT.

No separate runtime exception was located in the installed material reviewed.
The project's source being MIT does not discharge these runtime conditions.
Before releasing DOS, Win16 or OS/2 binaries:

1. Recover the matching toolchain source revision, or build with a deliberately
   pinned source snapshot and retain its runtime sources and local changes.
2. Add the source URL/version and runtime license notice to each runtime-bearing
   executable/helper and its accompanying documentation. This review collected
   the license but did not modify the executables to add that notice.
3. Include `Open-Watcom.txt` and identify the runtime as separately licensed.
   Preserve any additional file-specific terms found in the pinned runtime.

The [upstream license](https://github.com/open-watcom/open-watcom-v2/blob/master/license.txt)
is a reference; the collected installed copy is the evidence for this build.

### Watt-32

`copyrigh.h` permits applications in source/executable form but restricts
distribution of modified library source, objects and libraries. Its stated
scope is limited to files that include it. Application permission therefore
must not be generalized to every linked member or a redistributable SDK ZIP.

The linked `gettod.c`, `ip4_out.c`, `loopback.c`, `pcdhcp.c`, `pcqueue.c`, and
`udp_rev.c` contain a Gisle Vanem advertising acknowledgment requirement:

> This product includes software developed by Gisle Vanem
> Bergen, Norway.

Use that acknowledgment in DOS release materials describing the included
networking functionality, and carry the collected copyright/disclaimer text.
`Watt-32-collected.txt` preserves the notices found in the mapped source files.
It is explicitly incomplete as a clearance record: files such as `asmpkt.asm`,
`chksum.c`, `ip4_in.c`, `pcdbug.c`, `run.c`, `timer.c`, and `version.c` lack a
direct `copyrigh.h` include and a clear grant in their own source comments.
Check transitive notices and obtain an authoritative coverage explanation
where needed. Some other comments name upstream authors without supplying
complete terms. Pin/rebuild the library and resolve those gaps before claiming
the DOS bundle's notice review is complete. No library/source redistribution
permission is inferred from an application build succeeding.

The public `inc/tcp.h` also refers to `LICENSE.H`, which was not found in this
checkout. The common `wattcp.h`/`misc.h` headers do not supply a blanket grant.
The companion `inc/copying.bsd` notice is retained in the collection; it does
not alone establish coverage for every otherwise unmarked file.

Source reference: [reviewed Watt-32 revision](https://github.com/gvanem/Watt-32/tree/c727114d209138e3057c122134cf7c3f3e67c374).

### Retro68, Multiversal and newlib

Include the Retro68 runtime copyright notice, GPL and GCC exception, and
Multiversal's MIT notice. The reviewed `libretro` files expressly carry the
runtime exception. The mapped archives also include **newlib `libc.a`**,
which was missing from the earlier third-party table. It has multiple
file-specific licenses and is not covered by the GCC runtime exception merely
because GCC links it.

The installed newlib version is 4.6.0. Retro68's imported subtree references
`COPYING.NEWLIB` but omits that top-level file. A release-wide copy was obtained
from the [official 4.6.0.20260123 archive](https://sourceware.org/pub/newlib/newlib-4.6.0.20260123.tar.gz),
whose hash is recorded, and saved as `newlib-4.6.0.txt`. This is a useful notice
collection, not proof that the Retro68 fork matches that archive. Before a Mac
binary release, reconcile the linked fork's files/generated variants against
these notices and retain any additions. The two mapped member lists make that
review bounded. Separately recover the terms accompanying the Apple MacTCP
SDK used for the headers; their declaration-only role does not establish a
license grant. No Apple source, libraries or headers are bundled here.

### Novell

The preserved SDK README identifies its prelude files as NLM link inputs,
including compatibility guidance for older servers. Together with the
September 22, 2004 agreement, this supports treating the linked prelude as
Derivative Software in a Developer Product, rather than republishing an SDK.
Section 3(c) permits that distribution conditionally; section 4 preserves
additional file restrictions. The publisher still needs to establish those
conditions apply, including the product definition and developer obligations.
This review does not accept contractual commitments on the publisher's behalf.
See the [Novell agreement](https://www.novell.com/developer/novell_developer_license_agreement.html).

The private `SDK_LICENSE` and `SDK_README.html` hashes are recorded; retain
the original package/terms as evidence. Keep `prelude.obj`, import catalogs,
CLIB/CLIBAUX/STUFFKEY modules and patch installers out of binary bundles.
The NLMs contain the prelude; CLIB is a guest prerequisite. Label the Novell
portion separately from project MIT code and preserve applicable notices.

## Assemble a release after closing its platform's open items

Build from a clean checkout of the selected commit, with recorded SDK/toolchain
inputs. Generate maps again; refresh the inventory and notices when inputs or
linkage change. Archive compiler/link commands and private raw maps, and publish
sanitized evidence. Do not reuse this review's diagnostic hashes as hashes for
a later build: dates/timestamps and source changes can alter binaries.

Use an explicit file allowlist for each platform's agent and required helpers.
Include the root MIT license for project code, an accompanying third-party
notice document, the applicable notice files, version/source information and
SHA-256 checksums. Include the font attribution (`Font8x8.txt`) for DOS and
NetWare screenshot glyphs. Do not zip a working build directory: it can contain SDK
objects, guest DLLs, probes, logs and configured credentials. Include only
example configuration with a placeholder token. ROMs, OS/floppy/VM images,
packet drivers, installers, private backups and real configuration are excluded.

Verify the assembled ZIP contents and their hashes, scan for secrets, then
test installation from that ZIP on the intended guest. Binary publication is
still a separate action; this review created no GitHub release or download.
