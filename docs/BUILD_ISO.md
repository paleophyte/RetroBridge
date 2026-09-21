# Build an all-agent deployment CD

[`tools/build_iso.py`](../tools/build_iso.py) compiles every production agent
and its helpers into one non-bootable ISO. Mount it in a VM's virtual CD drive,
then follow that platform's `README.TXT`. This is local deployment media;
[binary redistribution review](BINARY_RELEASE.md) remains separate from the
public source release.

## Build host setup

The orchestrator currently runs on Windows with Python 3.12+, Git, OpenSSH
`ssh`/`scp`, and these existing platform toolchains:

- Open Watcom at `C:\watcom` (the platform batch files use that location).
- MSYS2 i686 MinGW-w64 **MSVCRT**, not UCRT, plus GNU make. Defaults are
  `C:\msys64\mingw32\bin` and `C:\msys64\usr\bin\make.exe`.
- A configured [Watt-32 large-model build](../agent-dos/README.md), including
  `lib\wattcpwl.lib`, headers, and generated `inc\sys\watcom.err`.
- The locally supplied, verified [MacTCP and Novell inputs](../THIRD_PARTY.md)
  in `.deps/mac-sdk` and `.deps/netware-sdk`. Optional `mac_sdk` and
  `netware_sdk` JSON settings select alternate absolute directories.
- Guest-supplied `agent-os2/vendor/SO32DLL.DLL`, `TCP32DLL.DLL`, and
  `agent-os2-13/vendor/TCPIPDLL.DLL`. The build derives fresh import libraries;
  the guest DLLs are **not** placed on the ISO.
- A Linux SSH build host with Python 3, CMake, make, and a working
  [Retro68 toolchain](../agent-mac-system7/README.md#build-retro68-cross-toolchain-linux-host). Authenticate
  using an SSH key or agent. Verify its host key and establish `known_hosts`
  before running the builder; interactive passwords/prompts are disabled.

From the repository root, install the ISO library into your Python environment:

```powershell
python -m pip install -r tools/requirements-build.txt
Copy-Item tools/build_iso.example.json tools/build_iso.local.json
```

Edit `tools/build_iso.local.json` with your absolute toolchain paths and SSH
settings. The `mac.toolchain` path is on Linux. `mac.key`, `known_hosts`, and
`port` are optional; omitted values use OpenSSH's normal key/host-key defaults
and port 22. Hostnames/IPv4 addresses and plain usernames are supported.
No agent tokens or machine inventory are needed. The local JSON is ignored
by Git; it can instead live outside the checkout.

```powershell
python tools/build_iso.py --config tools/build_iso.local.json
```

The default output is `dist/RetroBridge.iso` (also ignored by Git). Use
`--output C:\media\RetroBridge.iso` to choose a different path. An existing
ISO is preserved unless `--force` is supplied; replacement happens only after
all builds, packaging, and file readbacks succeed.

The script snapshots **tracked working-tree files**, including staged edits,
into a new temporary workspace. Add new build inputs with `git add` first;
untracked source files are not included. A clean committed checkout gives
the clearest source identity. Existing local executables are never reused.
Build failures stop the whole operation; there is no partial-platform ISO.

The printed temporary workspace contains numbered build logs, source and
staged media, and is retained for diagnosis. The Mac compile uses a unique
`/tmp/retrobridge-iso-*` directory, transfers the same source/header snapshot,
fetches both applications, and removes that remote directory after success.
On failure, its printed path may remain for inspection. Neither build path
deploys to or restarts any guest.

## Contents and installation

The volume label is `RETROBRG`. ISO 9660 level 1 supplies uppercase 8.3 names
for old CD readers; Joliet exposes the same names to newer guests. All quick
guides, config examples and startup scripts use ASCII with CRLF endings.

| Directory | Binaries and helpers |
| --- | --- |
| `WIN32` | `LLMAGENT.EXE`, `UPDATE.EXE` |
| `DOS` | `LLMAGENT.EXE`, `LLMSTART.BAT`, `WATTCP.CFG` example |
| `WIN16` | `LLMAGENT.EXE`, `REDIR.EXE`, `RESTART.EXE`, `JOBRUN.EXE` |
| `OS2` | `LLMAGENT.EXE`, `UPDATE.EXE`, `REBOOT.EXE`, `JOBRUN.EXE` |
| `OS213` | `LLMAGENT.EXE`, `UPDATE.EXE`, `IORESET.EXE`, `IOSEG.DLL`, `JOBRUN.EXE` |
| `NW312` | NetWare 3.12 `LLMAGENT.NLM`, shared `UPDATE.NLM`, `CDLOAD.NCF` |
| `NW411` | NetWare 4.11 `LLMAGENT.NLM`, shared `UPDATE.NLM`, `CDLOAD.NCF` |
| `MAC7` | `AGENT.BIN`, `UPDATER.BIN` (MacBinary applications) |

Each folder contains `CONFIG.INI` with a placeholder token and its own
installation guide. Copy to a writable installation folder and rename the
config as directed; preserve a machine's existing configuration when updating.
Generate a unique token before starting a new installation. The CD contains
no real tokens, inventory, SDKs, packet drivers, OS installers, or guest DLLs.
Guest network stacks and CD drivers must already work.

**Win32:** Copy `LLMAGENT.EXE` as **`llm_agent.exe`**, and `CONFIG.INI` as
`llm_agent.ini`. That installed executable name is used by the updater and
is deliberately different from the ISO's 8.3 name. Other PC agents use
`LLMAGENT.EXE` / `LLMAGENT.INI`.

**Mac:** Copy both `.BIN` files to HFS and decode with an existing MacBinary
utility. This restores `llm_agent` and `llm_updater`, including their resource
forks. Keep both applications and `LLMAGENT.INI` together. Simply renaming a
`.BIN` file does not decode it. System 7 needs its CD driver and ISO 9660 File
Access; this ISO has no HFS partition or bootable Mac system.

`BUILD.JSN` is a JSON record of the source commit, dirty state, tracked-input
digest, supplied dependency hashes, and packaged artifact hashes. It is not
a complete toolchain or license attestation. `CHECKSUM.TXT` hashes all payload
files, including that record, but not itself. `NOTICES` retains the collected
third-party terms. `DOCS` contains selected UTF-8 Markdown references; their
relative links refer to the source repository.

## Loading the NetWare NLM

The preferred installation copies the matching folder's `LLMAGENT.NLM` and
`UPDATE.NLM` to `SYS:SYSTEM` using a NetWare client or an existing file utility.
Create `SYS:SYSTEM\LLMAGENT.INI` from `CONFIG.INI`, set its unique token, then
run `LOAD SYS:SYSTEM\LLMAGENT.NLM` after TCP/IP and CLIB are available.

To load directly from CD, first make the virtual CD visible to the server's
own controller/CD driver, then use:

```text
LOAD CDROM
CD DEVICE LIST
CD MOUNT RETROBRG
LOAD RETROBRG:\NW312\LLMAGENT.NLM
```

Use `NW411` instead for 4.11. Alternatively, invoke the matching
`RETROBRG:\NW312\CDLOAD.NCF` or `RETROBRG:\NW411\CDLOAD.NCF` after mounting.
These NCF files only load the agent; they do not configure networking, mount
the CD, create credentials, or stop an existing instance. Create the private
config in `SYS:SYSTEM` first, and ensure no other agent is loaded.

Novell documents the volume-name form of `CD MOUNT` in its
[3.12/4.x CDROM utility notes](https://support.novell.com/subscriptions/readmes/2940403.html).
Driver requirements vary; see its [3.12 CD-ROM setup guide](https://support.novell.com/docs/Tids/Solutions/10011408.html).
A DOS CD drive letter alone does not make the CD a NetWare volume. The ISO
does not provide Novell CD drivers, CLIB, CLIBAUX, or StuffKey.

Direct CD loading does **not** support self-update; install in `SYS:SYSTEM`
for that. Do not use `UNLOAD LLMAGENT` as a routine stop method: it has abended
on 3.12. Use a controlled server shutdown/restart to stop a CD-loaded copy.
See [NetWare recovery](../agent-netware/README.md#manual-recovery-after-interruption)
before replacing an existing installation. `UPDATE.NLM` is a replacement
helper, not the entry point for starting the agent.

## Validation

The builder checks binary signatures and MacBinary fork lengths, requires
every listed helper, verifies notice fingerprints, and reads every ISO file
back through both ISO 9660 and Joliet to compare hashes and the full inventory.
It does not claim the resulting CD has been mounted on every vintage guest.

The September 21, 2026 validation built all eight directories from fresh
sources, including both Mac applications over SSH. Windows mounted the ISO
as a `RETROBRG` CDFS volume, and all 59 entries in `CHECKSUM.TXT` matched through
the mounted filesystem. Vintage-guest CD mounting/loading was not repeated
as part of this packaging check.

Packaging regression tests require the ISO library installed above:

```powershell
python -m unittest discover -s tests -p test_build_iso.py -v
```
