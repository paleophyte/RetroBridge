"""Rewrite Open Watcom NLM external refs to classic Novell NLMLINK layout.

Watcom emits each import as:
  0xFF + 255-byte field (name\\0 + junk) + u32 count + count*u32 fixups

NetWare 3.12 expects:
  u8 nameLen + name bytes + u32 count + count*u32 fixups

Without this fix the 3.12 loader treats 0xFF as length 255 and reports
garbage symbols (CODE, _TEXT, main, ...).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

MAGIC = b"NetWare Loadable Module\x1a"


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def put_u32(data: bytearray, off: int, val: int) -> None:
    struct.pack_into("<I", data, off, val)


def fix(path: Path) -> None:
    data = bytearray(path.read_bytes())
    if not data.startswith(MAGIC):
        raise SystemExit(f"{path}: not an NLM")

    # Fixed header fields (i386) — see binutils nlm/i386-ext.h
    # After magic(24)+version(4)+nameLen(1)+name(13) = offset 42? 
    # Watcom: name length at 28, name 14 bytes (13+NUL pad) → fields at 42
    # Confirm against known HELLO: codeImageOffset at file 0x2A area
    # magic(24)+ver(4)=28; nameLen(1)+name(14)=43? Let's locate by scanning
    # Known: version at 24. Module name length byte at 28.
    name_len = data[28]
    # Novell fixed header after signature+version: byte moduleNameLength,
    # then 13-byte name (docs say 14 including length?). Observed: 1 + 14 = 15
    # so first u32 (codeImageOffset) is at 28+15=43? HELLO had code at 0x121
    # From earlier hex: offset 42 (0x2A): 21 01 00 00 = 0x121. So:
    # 28 + 1 + 13 = 42. name field is 13 bytes after length byte.
    base = 28 + 1 + 13  # 42

    code_off = u32(data, base + 0)
    # offsets we must patch if they are >= ext_off (everything after imports)
    fields = {
        "codeImageOffset": base + 0,
        "codeImageSize": base + 4,
        "dataImageOffset": base + 8,
        "dataImageSize": base + 12,
        "uninitializedDataSize": base + 16,
        "customDataOffset": base + 20,
        "customDataSize": base + 24,
        "moduleDependencyOffset": base + 28,
        "numberOfModuleDependencies": base + 32,
        "relocationFixupOffset": base + 36,
        "numberOfRelocationFixups": base + 40,
        "externalReferencesOffset": base + 44,
        "numberOfExternalReferences": base + 48,
        "publicsOffset": base + 52,
        "numberOfPublics": base + 56,
        "debugInfoOffset": base + 60,
        "numberOfDebugRecords": base + 64,
    }

    ext_off = u32(data, fields["externalReferencesOffset"])
    ext_count = u32(data, fields["numberOfExternalReferences"])
    dep_off = u32(data, fields["moduleDependencyOffset"])
    pub_off = u32(data, fields["publicsOffset"])
    dbg_off = u32(data, fields["debugInfoOffset"])
    custom_off = u32(data, fields["customDataOffset"])

    if ext_count == 0:
        print(f"{path.name}: no externals")
        return

    if data[ext_off] != 0xFF:
        # Already Novell-style?
        if data[ext_off] < 0x80 and data[ext_off] > 0:
            print(f"{path.name}: externals look Novell already (first len={data[ext_off]})")
            return
        raise SystemExit(f"{path.name}: unexpected external table lead-in 0x{data[ext_off]:02X}")

    records: list[tuple[bytes, bytes]] = []
    pos = ext_off
    for i in range(ext_count):
        if data[pos] != 0xFF:
            raise SystemExit(f"{path.name}: import {i} at 0x{pos:X} not 0xFF (0x{data[pos]:02X})")
        pos += 1
        field = bytes(data[pos : pos + 255])
        pos += 255
        name = field.split(b"\0", 1)[0]
        if not name or len(name) > 254:
            raise SystemExit(f"{path.name}: bad import name {name!r}")
        fixup_count = u32(data, pos)
        pos += 4
        fixups = bytes(data[pos : pos + 4 * fixup_count])
        pos += 4 * fixup_count
        records.append((name, fixups))

    # Remainder of file after Watcom external blob (deps / publics / etc.)
    # pos should equal the earliest of dep/pub that sits at end of imports
    old_ext_end = pos
    if old_ext_end not in (dep_off, pub_off, dbg_off, custom_off):
        # still OK if trailing zeros before deps
        if dep_off < old_ext_end:
            raise SystemExit(
                f"{path.name}: parsed ext end 0x{old_ext_end:X} but dep at 0x{dep_off:X}"
            )

    new_ext = bytearray()
    for name, fixups in records:
        new_ext.append(len(name))
        new_ext.extend(name)
        new_ext.extend(struct.pack("<I", len(fixups) // 4))
        new_ext.extend(fixups)

    delta = len(new_ext) - (old_ext_end - ext_off)
    out = bytearray()
    out.extend(data[:ext_off])
    out.extend(new_ext)
    out.extend(data[old_ext_end:])

    def adj(off: int) -> int:
        if off >= old_ext_end:
            return off + delta
        return off

    # Patch offset fields that point at or after the old external blob end
    for key, foff in fields.items():
        if "Offset" not in key:
            continue
        cur = u32(data, foff)
        if cur == 0:
            continue
        if key == "externalReferencesOffset":
            continue  # unchanged start
        if cur >= old_ext_end or cur == dep_off or cur == pub_off:
            put_u32(out, foff, adj(cur))
        elif cur > ext_off:
            # offsets into the middle of old ext table shouldn't exist
            put_u32(out, foff, adj(cur))

    # Force dep/pub/debug/custom to follow new layout if they matched old_ext_end
    put_u32(out, fields["moduleDependencyOffset"], adj(dep_off))
    put_u32(out, fields["publicsOffset"], adj(pub_off))
    put_u32(out, fields["debugInfoOffset"], adj(dbg_off))
    put_u32(out, fields["customDataOffset"], adj(custom_off))

    path.write_bytes(out)
    names = b", ".join(n for n, _ in records).decode("ascii", "replace")
    print(
        f"{path.name}: rewrote {ext_count} imports "
        f"({old_ext_end - ext_off} -> {len(new_ext)} bytes) [{names}]"
    )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: fix_nlm_imports.py <file.nlm>...")
    for a in sys.argv[1:]:
        fix(Path(a))
