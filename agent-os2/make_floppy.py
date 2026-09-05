"""Create a 1.44MB FAT12 floppy with OS/2 agent deploy files (8.3 names)."""
from pathlib import Path
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
out = root / "llm_agent.flp"
FLOPPY_SIZE = 1474560

INI = """port=2222
token=REPLACE_WITH_UNIQUE_TOKEN
"""

README = """LLMAGENT for OS/2 2.11
=======================

Files:
  LLMAGENT.EXE - TCP agent (needs TCPIPDLL on LIBPATH)
  SOCKPING.EXE - one-shot listen/PONG proof (optional)
  LLMAGENT.INI - port= / token=
  README.TXT

1. TCP/IP must be up (ifconfig lan0 works).
2. Copy to hard disk, e.g.:
     MD C:\\LLM
     COPY A:\\*.* C:\\LLM\\
3. Proof (optional):
     C:\\LLM\\SOCKPING.EXE
   then from host:  python -c "import socket;s=socket.create_connection(('10.102.10.198',2222));print(s.recv(64))"
4. Agent:
     C:\\LLM\\LLMAGENT.EXE

machines.ini:
  [os2]
  host = 10.102.10.198
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
    agent = root / "llm_agent.exe"
    sockping = root / "_sockping.exe"
    if agent.is_file():
        files.append(("LLMAGENT.EXE", agent.read_bytes()))
    if sockping.is_file():
        files.append(("SOCKPING.EXE", sockping.read_bytes()))
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
        label="OS2AGENT",
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
