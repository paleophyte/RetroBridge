"""Compile Win32 wire helpers and literal reply call sites with a fake socket.

Run with Python and GCC (or set CC). No Windows API or real socket is used,
so error replies from power/registry/input handlers are safe to exercise.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

from test_uploads import function

SOURCE = Path(__file__).resolve().parents[1] / "agent-win32" / "llm_agent.c"

STUBS = r'''
#include <assert.h>
#include <string.h>
typedef int SOCKET;
#define SOCKET_ERROR (-1)
#define SD_BOTH 2
/* These tests inject fatal writes; deadline/retry behavior has its own
 * production-loop tests in test_session_timeouts.py. */
#define NET_IO_SECONDS 30UL
#define NET_DEADLINE(s) 0UL
#define WSAEWOULDBLOCK 10035
static int net_failed, g_running=1;
static int net_expired(unsigned long d) { (void)d; return net_failed; }
static int net_fail(void) { net_failed=1; return -1; }
static int WSAGetLastError(void) { return 10054; }
static void network_idle(void) { assert(0); }
static int shutdown(SOCKET s, int how) {
    (void)s; assert(how == SD_BOTH); return 0;
}
static char wire[8192];
static int used, chunk, fail_mode, calls;
static void reset(int limit, int fail) {
    net_failed=0;
    memset(wire, 0x7f, sizeof(wire));
    used = calls = 0; chunk = limit; fail_mode = fail;
}
static int send(SOCKET s, const char *data, int len, int flags) {
    int n = len < chunk ? len : chunk;
    (void)s; (void)flags;
    ++calls;
    if (fail_mode && calls == 3) return fail_mode == 1 ? SOCKET_ERROR : 0;
    assert(n > 0 && used + n < (int)sizeof(wire));
    memcpy(wire + used, data, (unsigned)n); used += n;
    return n;
}
static void verify_reply(const char *expected) {
    int len = (int)strlen(expected);
    assert(used == len + 5);
    assert(!memcmp(wire, expected, (unsigned)len));
    assert(!memcmp(wire + len, "PONG\n", 5));
}
'''


class Win32FramingTests(unittest.TestCase):
    def test_literal_replies_and_short_sends(self):
        source = SOURCE.read_text(encoding="utf-8")
        # Preserve the actual call expression, including any raw send length.
        # Against the old source, the byte-count assertion catches the NULs
        # even when send() succeeds in one call.
        replies = list(re.finditer(
            r'\b(?:send|send_cstr)\(s,\s*("(?:[^"\\]|\\.)*")'
            r'(?:,\s*\d+,\s*0)?\);', source))
        self.assertTrue(replies)
        self.assertTrue(any("ERR:unknown command" in m[0] for m in replies))
        self.assertTrue(any("SHExitWindowsEx" in m[0] for m in replies))
        checks = []
        for match in replies:
            checks.append("reset(limits[i], 0); " + match[0]
                          + ' send_cstr(s, "PONG\\n"); verify_reply('
                          + match[1] + ");")
        main = r'''
int main(void) {
    SOCKET s = 0;
    int i, fail;
    const int limits[] = {8192, 1, 2, 7};
    for (i = 0; i < 4; ++i) {
        CHECKS
    }
    for (fail = 1; fail <= 2; ++fail) {
        reset(2, fail);
        assert(send_cstr(s, "ERR:socket test\n") == -1);
        assert(calls == 3 && used == 4);
    }
    reset(1, 0);
    assert(send_cstr(s, "") == 0 && calls == 0);
    return 0;
}
'''.replace("CHECKS", "\n".join(checks))
        helpers = function(source, "static int send_all(") + "\n" + function(source, "static int send_cstr(")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cfile = root / "framing.c"
            exe = root / ("framing.exe" if os.name == "nt" else "framing")
            cfile.write_text(STUBS + helpers + main, encoding="utf-8")
            compiler = shlex.split(os.environ.get("CC", "gcc"))
            subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                                      str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
