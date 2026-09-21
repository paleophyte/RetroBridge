"""Create a 1.44MB FAT12 floppy image with NetWare llm_agent deploy files.

Ships LLMAGENT.INI from LLMAGENT.INI.example so LOAD A:LLMAGENT works when
SYS: is unavailable (lab rescue). Prefer SYS:SYSTEM\\LLMAGENT.INI in normal use.
"""
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
out = root / "llm_agent.flp"
nlm = root / "LLMAGENT.NLM"
hello = root / "HELLO.NLM"
sockping = root / "SOCKPING.NLM"
update = root / "UPDATE.NLM"
stuffkey = root / "STUFFKEY.NLM"
clibaux = root / "CLIBAUX.NLM"
ini_example = root / "LLMAGENT.INI.example"

FLOPPY_SIZE = 1474560

README = """LLMAGENT for NetWare 3.12+
==========================

Files on this floppy:
  LLMAGENT.NLM  - agent
  LLMAGENT.INI  - placeholder config from the example (when present)
  UPDATE.NLM    - remote swap helper
  STUFFKEY.NLM  - keystroke helper (INSTALL menus)
  CLIBAUX.NLM   - required by StuffKey on 3.12
  HELLO.NLM / SOCKPING.NLM - proofs
  README.TXT    - this file

Optional helpers/utilities are included only when present locally.
Set a unique private token before loading; never use the placeholder.
This is private lab media, not a redistributable release bundle.

Rescue boot (SYS: not mounted):
  LOAD A:CLIBAUX
  LOAD A:LLMAGENT

Normal (SYS: up) - copy to SYS:SYSTEM then:
  LOAD LLMAGENT

Prefer token on SYS:SYSTEM\\LLMAGENT.INI once SYS is healthy.
"""


def patch_1440k_geometry(img_path: Path) -> None:
    data = bytearray(img_path.read_bytes())
    data[24:26] = (18).to_bytes(2, "little")
    data[26:28] = (2).to_bytes(2, "little")
    img_path.write_bytes(data)


def main() -> None:
    if not nlm.is_file():
        raise SystemExit(f"missing {nlm} - build with build.bat first")

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
        label="LLMAGENT",
        media_type=0xF0,
    )
    fat.close()
    patch_1440k_geometry(out)

    fs = PyFatFS(str(out))
    try:
        with fs.openbin("LLMAGENT.NLM", "wb") as dest:
            dest.write(nlm.read_bytes())
        if ini_example.is_file():
            with fs.openbin("LLMAGENT.INI", "wb") as dest:
                dest.write(ini_example.read_bytes())
        if hello.is_file():
            with fs.openbin("HELLO.NLM", "wb") as dest:
                dest.write(hello.read_bytes())
        if sockping.is_file():
            with fs.openbin("SOCKPING.NLM", "wb") as dest:
                dest.write(sockping.read_bytes())
        if update.is_file():
            with fs.openbin("UPDATE.NLM", "wb") as dest:
                dest.write(update.read_bytes())
        if stuffkey.is_file():
            with fs.openbin("STUFFKEY.NLM", "wb") as dest:
                dest.write(stuffkey.read_bytes())
        if clibaux.is_file():
            with fs.openbin("CLIBAUX.NLM", "wb") as dest:
                dest.write(clibaux.read_bytes())
        with fs.openbin("README.TXT", "wb") as dest:
            dest.write(README.encode("ascii"))
    finally:
        fs.close()

    print(f"Wrote {out} ({out.stat().st_size} bytes) from {nlm.name} ({nlm.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
