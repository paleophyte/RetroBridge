"""Create a 1.44MB FAT12 floppy image with the Win16 llm_agent deploy files."""
from pathlib import Path

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

root = Path(__file__).resolve().parent
out = root / "llm_agent_win16.flp"
exe = root / "llm_agent.exe"
redir_exe = root / "redir.exe"
restart_exe = root / "restart.exe"

FLOPPY_SIZE = 1474560  # 1.44MB

INI = """port=2222
token=REPLACE_WITH_UNIQUE_TOKEN
"""

README = """LLMAGENT for Windows for Workgroups 3.11 (8.3 names)
=====================================================

Files on this disk:
  LLMAGENT.EXE  - agent (Win16, needs WFW's own Winsock/NET START up)
  REDIR.EXE     - DOS-target helper EXEC uses to capture command output;
                  must sit next to LLMAGENT.EXE (see agent-win16/README.md's
                  "EXEC's redirection workaround" section for why this
                  exists -- EXEC will fail without it)
  RESTART.EXE   - helper UPDATE uses to relaunch a fresh copy after a
                  self-update; must also sit next to LLMAGENT.EXE (see
                  the "UPDATE: self-update without a full REBOOT" section
                  -- UPDATE will not bring the agent back without it)
  LLMAGENT.INI  - port= / placeholder token=
  README.TXT    - this file

JOBRUN.EXE is not included by this builder. Copy the DOS-target helper
from build.bat separately to use tracked jobs.

1. Copy to hard disk from File Manager (or a DOS prompt, if you have
   one open):
     MKDIR C:\\LLMWIN
     COPY A:\\LLMAGENT.EXE C:\\LLMWIN\\
     COPY A:\\REDIR.EXE C:\\LLMWIN\\
     COPY A:\\RESTART.EXE C:\\LLMWIN\\
     COPY A:\\LLMAGENT.INI C:\\LLMWIN\\
   Edit LLMAGENT.INI to set a unique token and match the bridge inventory.
2. Make sure NET START succeeded for this boot (WFW's own NDIS network
   stack -- see agent-dos/README.md's boot menu section) before starting
   the agent; it calls WSAStartup() and will show an error dialog if the
   network isn't up yet.
3. Start it: File Manager, double-click C:\\LLMWIN\\LLMAGENT.EXE (or
   Program Manager, File > Run). A small status window appears with the
   listening port, IP address, and token status -- leave it running.

Bridge machines.ini:
  [wfw-1]
  host = <this guest's IP, shown in the agent's own window>
  exec_port = 2222
  exec_token = REPLACE_WITH_UNIQUE_TOKEN
"""


def patch_1440k_geometry(img_path: Path) -> None:
    """pyfatfs leaves SPT/heads at 0; real DOS/Windows divides by them -> crash."""
    data = bytearray(img_path.read_bytes())
    # BPB_SecPerTrk @ 24, BPB_NumHeads @ 26 (FAT12/16 boot sector)
    data[24:26] = (18).to_bytes(2, "little")
    data[26:28] = (2).to_bytes(2, "little")
    img_path.write_bytes(data)


def main() -> None:
    if not exe.is_file():
        raise SystemExit(f"missing {exe} — build with build.bat first")
    if not redir_exe.is_file():
        raise SystemExit(f"missing {redir_exe} — build with build_redir.bat first")
    if not restart_exe.is_file():
        raise SystemExit(f"missing {restart_exe} — build with build_restart.bat first")

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
        label="LLMWIN",
        media_type=0xF0,
    )
    fat.close()

    # preserve_case=False -> pure 8.3 directory entries, no LFNs.
    fs = PyFatFS(str(out), preserve_case=False, read_only=False)
    try:
        fs.writebytes("LLMAGENT.EXE", exe.read_bytes())
        fs.writebytes("REDIR.EXE", redir_exe.read_bytes())
        fs.writebytes("RESTART.EXE", restart_exe.read_bytes())
        fs.writetext("LLMAGENT.INI", INI, encoding="ascii", errors="strict")
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
