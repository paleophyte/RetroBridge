# Vendored MacTCP headers

`MacTCP.h` and `AddressXlation.h` are copied from a real MacTCP 2.0.6 /
Universal Interfaces 2.1 installation (Apple, 1995). Retro68's own
"Multiversal" header reimplementation deliberately excludes MacTCP.h for
licensing reasons, so these are needed to build against MacTCP at all.

`AppleTalk.h` is **not** vendored — it's a small stub written for this
project. MacTCP.h only needs the `AddrBlock` type from Apple's real
AppleTalk.h, so the stub defines just that instead of pulling in real
AppleTalk/EtherTalk support this agent never uses.

`dnr.c` is vendored from the same MacTCP SDK extraction and is currently
unused (not compiled into the build).
