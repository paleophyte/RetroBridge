"""Exercise NetWare LLMAGENT remote UPDATE flow against a live server."""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from agent_client import AgentClient, AgentProtocolError  # noqa: E402

HOST = "10.102.10.197"
PORT = 2222
TOKEN = "REPLACE_WITH_UNIQUE_TOKEN"
root = Path(__file__).resolve().parent
update_nlm = root / "UPDATE.NLM"
agent_nlm = root / "LLMAGENT.NLM"


def main() -> int:
    if not update_nlm.is_file() or not agent_nlm.is_file():
        print("missing UPDATE.NLM or LLMAGENT.NLM — build first")
        return 1

    c = AgentClient(HOST, PORT, TOKEN, timeout=60.0)

    print("1) PING before update...")
    assert c.ping(), "ping failed"
    info = c.sysinfo()
    print(f"   server={info.get('server_name')} agent={info.get('agent')}")

    print("2) PUT SYS:SYSTEM\\UPDATE.NLM ...")
    n = c.put(update_nlm, r"SYS:SYSTEM\UPDATE.NLM")
    print(f"   wrote {n} bytes")

    print("3) PUT SYS:SYSTEM\\LLMAGENT.NEW ...")
    n = c.put(agent_nlm, r"SYS:SYSTEM\LLMAGENT.NEW")
    print(f"   wrote {n} bytes")

    print("4) UPDATE command...")
    sock = c._connect()
    try:
        sock.sendall(b"UPDATE\n")
        line = c._recv_line(sock)
        print(f"   response: {line!r}")
        if line.startswith("ERR:"):
            print("UPDATE failed")
            return 1
    finally:
        try:
            sock.close()
        except OSError:
            pass

    print("5) waiting for unload/reload...")
    c2 = None
    for i in range(40):
        time.sleep(1)
        try:
            c2 = AgentClient(HOST, PORT, TOKEN, timeout=5.0)
            if c2.ping():
                info2 = c2.sysinfo()
                print(
                    f"   up after {i + 1}s — agent={info2.get('agent')} "
                    f"server={info2.get('server_name')}"
                )
                break
        except Exception as e:
            print(f"   [{i + 1}] not ready: {type(e).__name__}: {e}")
            c2 = None

    if c2 is None:
        print("agent did not come back within 40s")
        return 1

    print("6) check LLMAGENT.OLD ...")
    try:
        out = root / "_got_llmagent.old"
        n = c2.get(r"SYS:SYSTEM\LLMAGENT.OLD", out)
        print(f"   LLMAGENT.OLD present ({n} bytes)")
    except AgentProtocolError as e:
        print(f"   LLMAGENT.OLD: {e}")

    print("UPDATE flow OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
