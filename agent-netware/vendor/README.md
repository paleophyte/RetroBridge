# NetWare compatibility declarations

`nwsock.h` is the project's minimal API declaration header, covered by the
root MIT license. Novell SDK files and guest patch/utility packages are
user-supplied and excluded from Git. Do not put a public vendor archive here.

The agent and proof builds use checked SDK files from `.deps/netware-sdk`
at the repository root, or `NLM_SDK_DIR`. See
[THIRD_PARTY.md](../../THIRD_PARTY.md) for preparation and provenance.

`make_loader_floppy.py` is an optional guest-maintenance helper. If needed,
its historical `vendor/312ptd/out/312PTD/NATIVE/LOADER` input directory must
be supplied privately. Those patches are not compiler dependencies and are
not supplied or relicensed by this project.
