"""Build loader-update floppy for NetWare 3.12 (DOS partition, no client needed)."""
import argparse
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
default_loader_dir = root / "vendor" / "312ptd" / "out" / "312PTD" / "NATIVE" / "LOADER"
hello = root / "HELLO.NLM"

FLOPPY_SIZE = 1474560

README = """NetWare 3.12 LOADER update (from 312PTD)
=======================================

Optional loader update from 312PTD. This does not replace the NLM import
normalization performed by this project's build scripts. Updating the
loader alone does not fix Watcom's padded import-name format.

DO THIS FROM DOS (no NetWare client needed):

1. At the server console:
     DOWN
     EXIT
2. You are now at the DOS prompt on the server boot partition.
3. Find where SERVER.EXE lives (often C:\\SERVER.312 or C:\\NWSERVER).
     DIR C:\\SERVER.EXE /S
4. Insert this floppy. Backup then replace the loader:

     COPY C:\\SERVER.312\\LOADER.EXE C:\\SERVER.312\\LOADER.OLD
     COPY A:\\LOADER.EXE C:\\SERVER.312\\LOADER.EXE
     COPY A:\\LSWAP.EXE C:\\SERVER.312\\LSWAP.EXE

   (Adjust path if your SERVER.EXE is elsewhere.)

5. Optional but recommended - run LSWAP to wire the new loader:
     C:
     CD \\SERVER.312
     LSWAP

6. Restart the server:
     SERVER

7. After boot, with CLIB loaded:
     LOAD A:HELLO

   You should see: hello netware nlm
   Then try: LOAD A:LLMAGENT  (from the agent floppy)

Files on this disk: LOADER.EXE, LSWAP.EXE, LSWAP.NLM, HELLO.NLM, README.TXT
"""


def patch_1440k_geometry(img_path: Path) -> None:
    data = bytearray(img_path.read_bytes())
    data[24:26] = (18).to_bytes(2, "little")
    data[26:28] = (2).to_bytes(2, "little")
    img_path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader-dir", type=Path, default=default_loader_dir,
                        help="Directory containing locally supplied LOADER.EXE, LSWAP.EXE and LSWAP.NLM")
    parser.add_argument("--output", type=Path, default=root / "loader312.flp",
                        help="Output floppy image (default: agent-netware/loader312.flp)")
    args = parser.parse_args()
    loader_dir, out = args.loader_dir, args.output
    for name in ("LOADER.EXE", "LSWAP.EXE", "LSWAP.NLM"):
        if not (loader_dir / name).is_file():
            raise SystemExit(f"missing {loader_dir / name} - extract 312PTD and pass --loader-dir")

    if out.exists():
        out.unlink()
    with open(out, "wb") as f:
        f.truncate(FLOPPY_SIZE)

    fat = PyFat()
    fat.mkfs(
        str(out),
        fat_type=PyFat.FAT_TYPE_FAT12,
        size=FLOPPY_SIZE,
        sector_size=512,
        label="LOADER312",
        media_type=0xF0,
    )
    fat.close()
    patch_1440k_geometry(out)

    fs = PyFatFS(str(out))
    try:
        for name in ("LOADER.EXE", "LSWAP.EXE", "LSWAP.NLM"):
            with fs.openbin(name, "wb") as dest:
                dest.write((loader_dir / name).read_bytes())
        if hello.is_file():
            with fs.openbin("HELLO.NLM", "wb") as dest:
                dest.write(hello.read_bytes())
        with fs.openbin("README.TXT", "wb") as dest:
            dest.write(README.encode("ascii"))
    finally:
        fs.close()
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
