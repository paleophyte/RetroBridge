"""One-shot OS/2 1.3 agent capability smoke test. Not for permanent check-in."""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from agent_client import AgentClient, AgentProtocolError  # noqa: E402

HOST = "10.102.10.199"
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

    try:
        assert c.ping() is True
        ok("PING", "PONG")
    except Exception as e:
        fail("PING", str(e))

    try:
        info = c.sysinfo()
        need = ["os_family", "agent", "os2_major", "disk_c_free_mb"]
        missing = [k for k in need if k not in info]
        if missing:
            fail("SYSINFO", f"missing keys {missing}: {info}")
        elif info.get("os_family") != "os2":
            fail("SYSINFO", f"os_family={info.get('os_family')!r}")
        else:
            ok("SYSINFO", ", ".join(f"{k}={info[k]}" for k in sorted(info)))
    except Exception as e:
        fail("SYSINFO", str(e))

    try:
        r = c.exec("ver")
        out = r.output.decode("ascii", "replace")
        if out.strip():
            ok("EXEC ver", f"exit={r.exit_code} out={out.strip()[:80]!r}")
        else:
            fail("EXEC ver", f"empty exit={r.exit_code}")
    except Exception as e:
        fail("EXEC ver", str(e))

    try:
        r = c.exec("dir C:\\")
        out = r.output.decode("ascii", "replace")
        if len(out) > 20:
            ok("EXEC dir", f"exit={r.exit_code} bytes={len(r.output)} sample={out.strip()[:60]!r}")
        else:
            fail("EXEC dir", f"exit={r.exit_code} out={out!r}")
    except Exception as e:
        fail("EXEC dir", str(e))

    remote = r"C:\AGTEST.TMP"
    payload = b"hello-from-bridge-" + os.urandom(16).hex().encode() + b"\n"
    local_up = Path(tempfile.gettempdir()) / "agtest_os213_up.bin"
    local_dn = Path(tempfile.gettempdir()) / "agtest_os213_dn.bin"
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

    expect_err("GET missing", lambda: c.get(r"C:\NOPEZZZ.TXT", local_dn))

    try:
        pid_result = c.exec_detach("CMD.EXE /C dir C:\\ > C:\\AGTEST2.TMP")
        ok("EXECDETACH", pid_result.reply)
    except Exception as e:
        fail("EXECDETACH", str(e))

    expect_err("CLIPSET unsupported", lambda: c.clipboard_set("smoke-clip"))
    expect_err("SHUTDOWN unsupported", lambda: c.shutdown())

    try:
        procs = c.pslist()
        if len(procs) < 1:
            fail("PSLIST", "empty")
        else:
            sample = ", ".join(f"{p}:{n}" for p, n in procs[:8])
            ok("PSLIST", f"{len(procs)} procs: {sample}")
    except Exception as e:
        fail("PSLIST", str(e))

    try:
        c.key("esc")
        ok("KEY esc", "OK")
    except Exception as e:
        fail("KEY esc", str(e))

    try:
        c.type_text("x")
        ok("TYPE", "OK")
    except Exception as e:
        fail("TYPE", str(e))

    try:
        wins = c.winlist()
        ok("WINLIST", f"{len(wins)} windows: " + ", ".join(
            f"{w.title!r}@({w.x},{w.y},{w.width}x{w.height})" for w in wins[:8]
        ))
    except Exception as e:
        fail("WINLIST", str(e))

    # SCREENSHOT — PM desktop capture as BMP
    try:
        bmp = c.screenshot()
        out = Path(__file__).resolve().parent / "last_screenshot.bmp"
        out.write_bytes(bmp)
        if len(bmp) < 54 or bmp[:2] != b"BM":
            fail("SCREENSHOT", f"not BMP, len={len(bmp)}")
        else:
            import struct
            w, h = struct.unpack_from("<ii", bmp, 18)
            bpp = struct.unpack_from("<H", bmp, 28)[0]
            ok("SCREENSHOT", f"len={len(bmp)} {w}x{abs(h)} {bpp}bpp -> {out.name}")
    except Exception as e:
        fail("SCREENSHOT", str(e))

    try:
        c.exec(r"del C:\AGTEST.TMP")
        c.exec(r"del C:\AGTEST2.TMP")
    except Exception:
        pass

    print()
    passes = sum(1 for s, _, _ in results if s == "PASS")
    fails = sum(1 for s, _, _ in results if s == "FAIL")
    print(f"Summary: {passes} passed, {fails} failed out of {len(results)}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
