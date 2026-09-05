"""One-shot FreeDOS agent capability smoke test. Not for permanent check-in."""
from __future__ import annotations

import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from agent_client import AgentClient, AgentProtocolError  # noqa: E402

HOST = "10.102.10.224"
PORT = 2222
TOKEN = "REPLACE_WITH_UNIQUE_TOKEN"

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

    # 1. PING
    try:
        assert c.ping() is True
        ok("PING", "PONG")
    except Exception as e:
        fail("PING", str(e))

    # 2. SYSINFO
    try:
        info = c.sysinfo()
        need = ["os_family", "agent", "dos_major", "disk_c_free_mb"]
        missing = [k for k in need if k not in info]
        if missing:
            fail("SYSINFO", f"missing keys {missing}: {info}")
        elif info.get("os_family") != "dos":
            fail("SYSINFO", f"os_family={info.get('os_family')!r}")
        else:
            ok("SYSINFO", ", ".join(f"{k}={info[k]}" for k in sorted(info)))
    except Exception as e:
        fail("SYSINFO", str(e))

    # 3. EXEC ver
    try:
        r = c.exec("ver")
        out = r.output.decode("ascii", "replace")
        if out.strip():
            ok("EXEC ver", f"exit={r.exit_code} out={out.strip()[:80]!r}")
        else:
            fail("EXEC ver", f"empty exit={r.exit_code}")
    except Exception as e:
        fail("EXEC ver", str(e))

    # 4. EXEC dir
    try:
        r = c.exec("dir C:\\")
        out = r.output.decode("ascii", "replace")
        if len(out) > 20:
            ok("EXEC dir", f"exit={r.exit_code} bytes={len(r.output)} sample={out.strip()[:60]!r}")
        else:
            fail("EXEC dir", f"exit={r.exit_code} out={out!r}")
    except Exception as e:
        fail("EXEC dir", str(e))

    # 5. PUT + GET roundtrip
    remote = r"C:\AGTEST.TMP"
    payload = b"hello-from-bridge-" + os.urandom(16).hex().encode() + b"\n"
    local_up = Path(tempfile.gettempdir()) / "agtest_up.bin"
    local_dn = Path(tempfile.gettempdir()) / "agtest_dn.bin"
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

    # 6. GET missing file
    expect_err("GET missing", lambda: c.get(r"C:\NOPEZZZ.TXT", local_dn))

    # 7. SCREENSHOT BMP
    try:
        bmp = c.screenshot()
        if len(bmp) < 54 or bmp[:2] != b"BM":
            fail("SCREENSHOT", f"not BMP magic, len={len(bmp)} head={bmp[:8]!r}")
        else:
            bf_size = struct.unpack_from("<I", bmp, 2)[0]
            bi_width, bi_height = struct.unpack_from("<ii", bmp, 18)
            bi_bit_count = struct.unpack_from("<H", bmp, 28)[0]
            detail = f"len={len(bmp)} bfSize={bf_size} {bi_width}x{abs(bi_height)} {bi_bit_count}bpp"
            if bi_width == 640 and abs(bi_height) == 200 and bi_bit_count == 24:
                ok("SCREENSHOT", detail)
            else:
                fail("SCREENSHOT", f"unexpected geometry: {detail}")
            out_bmp = Path(tempfile.gettempdir()) / "dos_screenshot.bmp"
            out_bmp.write_bytes(bmp)
            print(f"      saved {out_bmp}")
    except Exception as e:
        fail("SCREENSHOT", str(e))

    # 8. TYPE / KEY (protocol OK; visual only if guest is at COMMAND.COM)
    try:
        c.type_text("rem smoke")
        ok("TYPE", "OK")
    except Exception as e:
        fail("TYPE", str(e))

    try:
        c.key("enter")
        ok("KEY enter", "OK")
    except Exception as e:
        fail("KEY enter", str(e))

    try:
        c.key("esc")
        ok("KEY esc", "OK")
    except Exception as e:
        fail("KEY esc", str(e))

    # Unsupported — should ERR clearly
    expect_err("EXECDETACH unsupported", lambda: c.exec_detach("echo hi"))
    expect_err("CLICK unsupported", lambda: c.click(10, 10))
    expect_err("WINLIST unsupported", lambda: c.winlist())
    expect_err("CLIPSET unsupported", lambda: c.clipboard_set("x"))
    expect_err("PSLIST unsupported", lambda: c.pslist())
    expect_err("SHUTDOWN unsupported", lambda: c.shutdown())

    # cleanup remote best-effort
    try:
        c.exec(r"del C:\AGTEST.TMP")
    except Exception:
        pass

    print()
    passes = sum(1 for s, _, _ in results if s == "PASS")
    fails = sum(1 for s, _, _ in results if s == "FAIL")
    print(f"Summary: {passes} passed, {fails} failed out of {len(results)}")
    print("Skipped: REBOOT (would reset the guest)")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
