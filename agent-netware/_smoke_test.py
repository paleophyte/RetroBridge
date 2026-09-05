"""One-shot NetWare agent capability smoke test."""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from agent_client import AgentClient, AgentProtocolError  # noqa: E402

HOST = os.environ.get("NW_HOST", "192.168.56.30")
PORT = int(os.environ.get("NW_PORT", "2222"))
TOKEN = os.environ.get("NW_TOKEN", "REPLACE_WITH_UNIQUE_TOKEN")

results: list[tuple[str, str, str]] = []


def ok(name: str, detail: str = "") -> None:
    results.append(("PASS", name, detail))
    print(f"PASS  {name}" + (f" — {detail}" if detail else ""))


def fail(name: str, detail: str = "") -> None:
    results.append(("FAIL", name, detail))
    print(f"FAIL  {name}" + (f" — {detail}" if detail else ""))


def expect_err(name: str, fn) -> None:
    try:
        fn()
        fail(name, "expected ERR but succeeded")
    except AgentProtocolError as e:
        msg = str(e)
        if "ERR:" in msg or "not supported" in msg.lower() or "failed" in msg.lower():
            ok(name, msg[:120])
        else:
            fail(name, f"unexpected error: {msg[:120]}")
    except Exception as e:
        fail(name, f"{type(e).__name__}: {e}")


def main() -> int:
    c = AgentClient(HOST, PORT, TOKEN, timeout=45.0)

    try:
        assert c.ping() is True
        ok("PING", "PONG")
    except Exception as e:
        fail("PING", str(e))

    try:
        info = c.sysinfo()
        need = ["os_family", "agent"]
        missing = [k for k in need if k not in info]
        if missing:
            fail("SYSINFO", f"missing keys {missing}: {info}")
        elif info.get("os_family") != "netware":
            fail("SYSINFO", f"os_family={info.get('os_family')!r}")
        else:
            ok("SYSINFO", ", ".join(f"{k}={info[k]}" for k in sorted(info)))
    except Exception as e:
        fail("SYSINFO", str(e))

    # EXEC has no stdout on NetWare — just verify it returns without protocol error.
    try:
        r = c.exec("modules")
        ok("EXEC modules", f"exit={r.exit_code} out_bytes={len(r.output)}")
    except Exception as e:
        fail("EXEC modules", str(e))

    remote = r"SYS:SYSTEM\AGTEST.TMP"
    payload = b"hello-from-bridge-" + os.urandom(16).hex().encode() + b"\n"
    local_up = Path(tempfile.gettempdir()) / "agtest_nw_up.bin"
    local_dn = Path(tempfile.gettempdir()) / "agtest_nw_dn.bin"
    local_up.write_bytes(payload)
    try:
        n = c.put(local_up, remote)
        if n != len(payload):
            fail("PUT", f"wrote {n} expected {len(payload)}")
        else:
            ok("PUT", f"{n} bytes -> {remote}")
        n2 = c.get(remote, local_dn)
        got = local_dn.read_bytes()
        if got == payload:
            ok("GET roundtrip", f"{n2} bytes identical")
        else:
            fail("GET roundtrip", f"len {len(got)} vs {len(payload)} mismatch")
    except Exception as e:
        fail("PUT/GET", str(e))

    try:
        bmp = c.screenshot()
        if len(bmp) < 54 or bmp[0:2] != b"BM":
            fail("SCREENSHOT", f"not a BMP ({len(bmp)} bytes, head={bmp[:8]!r})")
        else:
            # BITMAPINFOHEADER width/height at +18/+22 (little-endian)
            w = int.from_bytes(bmp[18:22], "little", signed=True)
            h = abs(int.from_bytes(bmp[22:26], "little", signed=True))
            ok("SCREENSHOT", f"{len(bmp)} bytes BMP {w}x{h}")
            out = Path(tempfile.gettempdir()) / "agtest_nw_shot.bmp"
            out.write_bytes(bmp)
    except Exception as e:
        fail("SCREENSHOT", str(e))

    # KEY/TYPE: avoid KEY enter on System Console (submits the command line).
    try:
        screens = c.screens()
        detail = " | ".join(f"{i}:{'*' if d else ''}{n}" for i, d, n in screens)[:160]
        ok("SCREENS", detail or "(none)")
    except Exception as e:
        fail("SCREENS", str(e))
    try:
        c.key("up")
        ok("KEY up", "OK")
    except Exception as e:
        fail("KEY up", str(e))

    expect_err("EXECDETACH unsupported", lambda: c.exec_detach("echo hi"))
    expect_err("CLICK unsupported", lambda: c.click(10, 10))
    expect_err("WINLIST unsupported", lambda: c.winlist())
    expect_err("CLIPSET unsupported", lambda: c.clipboard_set("x"))
    expect_err("PSLIST unsupported", lambda: c.pslist())
    # SHUTDOWN is supported but would DOWN the lab server — do not call here.
    expect_err("REBOOT unsupported", lambda: c.reboot())

    print()
    passes = sum(1 for s, _, _ in results if s == "PASS")
    fails = sum(1 for s, _, _ in results if s == "FAIL")
    print(f"Summary: {passes} passed, {fails} failed out of {len(results)}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
