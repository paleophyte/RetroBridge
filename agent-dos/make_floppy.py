"""Create a 1.44MB FAT12 floppy image with llm_agent deploy files (8.3 names)."""
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
out = root / "llm_agent.flp"
exe = root / "llm_agent.exe"
pktdrv = root / "PCNTPK.COM"

FLOPPY_SIZE = 1474560  # 1.44MB

INI = """port=2222
token=REPLACE_WITH_UNIQUE_TOKEN
"""

WATTCP = """# FreeDOS/MS-DOS lab: use DHCP (Watt-32 must be built with USE_DHCP).
# pkt.vector must match how you loaded the packet driver (e.g. PCNTPK INT=0x60).
# Optional: SET WATTCP.CFG=C:\\LLMAGENT  (directory containing this file)

my_ip = dhcp
pkt.vector = 0x60
"""

AUTOEXEC = """@ECHO OFF
LH PCNTPK INT=0x60
SET WATTCP.CFG=C:\\LLMAGENT
C:\\LLMAGENT\\LLMAGENT.EXE
"""

README = """LLMAGENT for FreeDOS / MS-DOS 6.22 (8.3 names)
===============================================

Files on this disk:
  LLMAGENT.EXE  - agent
  LLMAGENT.INI  - port= / token=
  WATTCP.CFG    - Watt-32 (my_ip=dhcp)
  PCNTPK.COM    - Crynwr packet driver, AMD PCnet/LANCE (VMware vlance NIC)
  AUTOEXEC.BAT  - sample autostart (packet driver, then agent)
  README.TXT    - this file

1. Copy to hard disk:
     MKDIR C:\\LLMAGENT
     COPY A:\\*.* C:\\LLMAGENT\\
2. Append AUTOEXEC.BAT's lines to C:\\AUTOEXEC.BAT (or copy it over,
   if this is a fresh install with nothing else in it):
     LH PCNTPK INT=0x60
     SET WATTCP.CFG=C:\\LLMAGENT
     C:\\LLMAGENT\\LLMAGENT.EXE
   MS-DOS 6.22 needs UMBs set up for LH to work; add to CONFIG.SYS:
     DEVICE=C:\\DOS\\HIMEM.SYS
     DEVICE=C:\\DOS\\EMM386.EXE NOEMS
     DOS=HIGH,UMB
   (FreeDOS installs usually already have these.) Without them, drop
   LH and load PCNTPK low.
3. Reboot, or run AUTOEXEC.BAT's three lines by hand.

Bridge machines.ini:
  [freedos-1]
  host = <guest-dhcp-ip>
  exec_port = 2222
  exec_token = REPLACE_WITH_UNIQUE_TOKEN
"""


def patch_1440k_geometry(img_path: Path) -> None:
    """pyfatfs leaves SPT/heads at 0; FreeDOS divides by them → crash."""
    data = bytearray(img_path.read_bytes())
    # BPB_SecPerTrk @ 24, BPB_NumHeads @ 26 (FAT12/16 boot sector)
    data[24:26] = (18).to_bytes(2, "little")
    data[26:28] = (2).to_bytes(2, "little")
    img_path.write_bytes(data)


def main() -> None:
    if not exe.is_file():
        raise SystemExit(f"missing {exe} — build with build.bat first")
    if not pktdrv.is_file():
        raise SystemExit(f"missing {pktdrv} — copy PCNTPK.COM next to this script")

    if out.exists():
        out.unlink()

    # mkfs opens rb+; pre-create the image file.
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

    # preserve_case=False → pure 8.3 directory entries, no LFNs.
    fs = PyFatFS(str(out), preserve_case=False, read_only=False)
    try:
        fs.writebytes("LLMAGENT.EXE", exe.read_bytes())
        fs.writebytes("PCNTPK.COM", pktdrv.read_bytes())
        fs.writetext("LLMAGENT.INI", INI, encoding="ascii", errors="strict")
        fs.writetext("WATTCP.CFG", WATTCP, encoding="ascii", errors="strict")
        fs.writetext("AUTOEXEC.BAT", AUTOEXEC, encoding="ascii", errors="strict")
        fs.writetext("README.TXT", README, encoding="ascii", errors="strict")
        print("Files on floppy:", fs.listdir("/"))
    finally:
        fs.close()

    patch_1440k_geometry(out)

    b = out.read_bytes()
    spt = int.from_bytes(b[24:26], "little")
    heads = int.from_bytes(b[26:28], "little")
    if spt != 18 or heads != 2:
        raise SystemExit(f"geometry patch failed: spt={spt} heads={heads}")

    print(f"Created {out} ({out.stat().st_size} bytes), geometry 18 spt / 2 heads")


if __name__ == "__main__":
    main()
