"""Create a 1.44MB FAT12 floppy with OS/2 1.3 agent deploy files (8.3 names)."""
from pathlib import Path
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
out = root / "llm_agent.flp"
FLOPPY_SIZE = 1474560

INI = """port=2222
token=REPLACE_WITH_UNIQUE_TOKEN
host=10.102.10.199
"""

README = """LLMAGENT for OS/2 1.3
======================

Files:
  LLMAGENT.EXE - TCP agent (needs TCPIPDLL.DLL on LIBPATH)
  UPDATE.EXE   - self-update helper (optional, for legacy_self_update)
  IORESET.EXE  - reboot helper, launched by the agent's REBOOT command
  IOSEG.DLL    - I/O privilege segment used by IORESET.EXE. Keep it in
                 the same directory as IORESET.EXE; it is loaded by path,
                 so it does NOT need to be on LIBPATH.
  LLMAGENT.INI - port= / token=
  README.TXT

1. TCP/IP must be up.
2. Copy to hard disk, e.g.:
     MD C:\\LLM
     COPY A:\\*.* C:\\LLM\\
3. Agent:
     C:\\LLM\\LLMAGENT.EXE

REBOOT needs IOPL=YES in CONFIG.SYS (the OS/2 1.3 default).

machines.ini:
  [os2-13]
  host = 10.102.10.199
  exec_port = 2222
  exec_token = REPLACE_WITH_UNIQUE_TOKEN
"""


def patch_1440k_geometry(img_path: Path) -> None:
    data = bytearray(img_path.read_bytes())
    data[24:26] = (18).to_bytes(2, "little")
    data[26:28] = (2).to_bytes(2, "little")
    img_path.write_bytes(data)


def main() -> None:
    files = []
    for remote, local in (
        ("LLMAGENT.EXE", "llm_agent.exe"),
        ("UPDATE.EXE", "update.exe"),
        # REBOOT is delegated to these two - see ioreset.c / ioseg.c.
        ("IORESET.EXE", "ioreset.exe"),
        ("IOSEG.DLL", "ioseg.dll"),
    ):
        path = root / local
        if path.is_file():
            files.append((remote, path.read_bytes()))
    if not files:
        raise SystemExit("nothing to put on floppy — build first")

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
        label="OS213AGT",
        media_type=0xF0,
    )
    fat.close()

    fs = PyFatFS(str(out), preserve_case=False, read_only=False)
    try:
        for name, data in files:
            fs.writebytes(name, data)
        fs.writetext("LLMAGENT.INI", INI, encoding="ascii", errors="strict")
        fs.writetext("README.TXT", README, encoding="ascii", errors="strict")
        print("Files on floppy:", fs.listdir("/"))
    finally:
        fs.close()

    patch_1440k_geometry(out)
    print(f"Wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
