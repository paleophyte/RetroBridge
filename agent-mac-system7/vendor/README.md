# Project stub and external MacTCP headers

Apple's `MacTCP.h` and `AddressXlation.h` are no longer distributed here.
Supply the MacTCP 2.0.6 / Universal Interfaces 2.1b1 (1995) files locally
using `tools/prepare_dependencies.py` from the repository root. The default
destination is `.deps/mac-sdk`, outside this source directory. The build
checks both input hashes. See [THIRD_PARTY.md](../../THIRD_PARTY.md).

`AppleTalk.h` is **not** vendored — it's a small stub written for this
project. MacTCP.h only needs the `AddrBlock` type from Apple's real
AppleTalk.h, so the stub defines just that instead of pulling in real
AppleTalk/EtherTalk support this agent never uses.

The former Apple `dnr.c` was unused and is no longer included or installed.
`AppleTalk.h` remains project source under the root MIT license.
