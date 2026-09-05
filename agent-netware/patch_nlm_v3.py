# Patch Open Watcom NLM header file-format version from 4 -> 3 so NetWare 3.12
# can parse the module. OW always emits version 4; 3.12 misreads v4 headers.
from pathlib import Path
import sys

MAGIC = b"NetWare Loadable Module\x1a"

def patch(path: Path) -> None:
    data = bytearray(path.read_bytes())
    if not data.startswith(MAGIC):
        raise SystemExit(f"{path}: not an NLM (bad magic)")
    ver_off = len(MAGIC)
    ver = int.from_bytes(data[ver_off:ver_off + 4], "little")
    if ver == 3:
        print(f"{path.name}: already format version 3")
        return
    if ver != 4:
        raise SystemExit(f"{path.name}: unexpected format version {ver}")
    data[ver_off:ver_off + 4] = (3).to_bytes(4, "little")
    path.write_bytes(data)
    print(f"{path.name}: patched format version 4 -> 3")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: patch_nlm_v3.py <file.nlm>...")
    for a in sys.argv[1:]:
        patch(Path(a))
