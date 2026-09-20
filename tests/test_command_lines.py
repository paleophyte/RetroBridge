"""Exercise every production line reader with raw, length-delimited input.

No shared Python client is involved. Includes authentication smuggling,
buffer boundaries, failed-session suffixes, EOF, and binary-body boundaries.
"""
from pathlib import Path
import re
import unittest

from test_session_timeouts import compile_run
from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <assert.h>
#include <string.h>
typedef int SOCKET;
#define NET_DEADLINE(s) (s)
#define NET_EXPIRED(d) ((void)(d), 0)
HEADERS
static const unsigned char *wire;
static int size, pos, reads;
static int recv_byte(char *out) {
    ++reads;
    if (net_failed || pos == size) return -1;
    *out = (char)wire[pos++];
    return 0;
}
static int RecvByte(char *out) { return recv_byte(out); }
static int recv_some(SOCKET s, char *out, int n) {
    (void)s; assert(n == 1);
    return recv_byte(out) < 0 ? -1 : 1;
}
static void reset(const void *data, int count) {
    net_reset(); wire = (const unsigned char *)data; size = count;
    pos = reads = 0;
}
'''
MAIN = r'''
static void rejected(const void *data, int count) {
    struct { unsigned char before; char line[CAPACITY]; unsigned char after; } b;
    int saved;
    memset(&b, 0x7f, sizeof(b)); reset(data, count);
    assert(READ(b.line, sizeof(b.line)) < 0);
    assert(net_failed && b.line[0] == 0);
    assert(b.before == 0x7f && b.after == 0x7f);
    saved = reads;
    /* Neither the remainder nor a queued command may revive this session. */
    assert(READ(b.line, sizeof(b.line)) < 0 && reads == saved);
}
int main(void) {
    char line[CAPACITY], data[CAPACITY * 3], byte;
    int n, ending, max = CAPACITY - 2;
    const char *bad[] = {"to\rken\nPING\n", "token\r\r\nPING\n",
                         "PI\rNG\nPING\n", "PING\rX\nPING\n"};
    for (n = 0; n < 4; ++n) rejected(bad[n], (int)strlen(bad[n]));
    rejected("token\0suffix\nPING\n", sizeof("token\0suffix\nPING\n") - 1);
    rejected("PING\0suffix\nPING\n", sizeof("PING\0suffix\nPING\n") - 1);
    rejected("\0\nPING\n", sizeof("\0\nPING\n") - 1);
    rejected("PING\r\0\nPING\n", sizeof("PING\r\0\nPING\n") - 1);
    rejected("", 0);
    rejected("PING", 4);
    rejected("PING\r", 5);
    /* Every EOF prefix of a CRLF command is incomplete. */
    for (n = 0; n < 6; ++n) rejected("PING\r\n", n);

    for (ending = 0; ending < 2; ++ending) {
        for (n = max - 1; n <= max; ++n) {
            memset(data, 'x', (unsigned)n);
            strcpy(data + n, ending ? "\r\nPING\n" : "\nPING\n");
            reset(data, (int)strlen(data));
            assert(READ(line, sizeof(line)) == n && !net_failed);
            assert(line[n] == 0 && !memcmp(line, data, (unsigned)n));
            assert(pos == n + (ending ? 2 : 1));
            assert(READ(line, sizeof(line)) == 4 && !strcmp(line, "PING"));
        }
        memset(data, 'x', (unsigned)(max + 1));
        strcpy(data + max + 1, ending ? "\r\nPING\n" : "\nPING\n");
        rejected(data, (int)strlen(data));
        assert(pos == max + 1); /* reject immediately, without draining */
    }
    memset(data, 'x', sizeof(data));
    memcpy(data, "EXEC ", 5);
    rejected(data, sizeof(data));
    assert(pos == max + 1);
    rejected(data, max); /* even an exactly full legal prefix needs LF */
    rejected(data, max + 1);

    reset("token\r\nPING\n\r\n\nQUIT\r\n", sizeof("token\r\nPING\n\r\n\nQUIT\r\n") - 1);
    assert(READ(line, sizeof(line)) == 5 && !strcmp(line, "token"));
    assert(net_line_seconds == NET_LINE_SECONDS && !net_line_active);
    assert(READ(line, sizeof(line)) == 4 && !strcmp(line, "PING"));
    assert(READ(line, sizeof(line)) == 0);
    assert(READ(line, sizeof(line)) == 0);
    assert(READ(line, sizeof(line)) == 4 && !strcmp(line, "QUIT"));
    /* Tabs and non-ASCII bytes remain data; framing does not impose an encoding. */
    reset("TYPE \t\x80\xff\n", sizeof("TYPE \t\x80\xff\n") - 1);
    assert(READ(line, sizeof(line)) == 8);
    assert(!memcmp(line, "TYPE \t\x80\xff", 8));
    /* A complete PUT line stops exactly before its opaque body. */
    reset("PUT file 8\r\n\0\r\nPING\nPING\n",
          sizeof("PUT file 8\r\n\0\r\nPING\nPING\n") - 1);
    assert(READ(line, sizeof(line)) == 10 && pos == 12);
    for (n = 0; n < 8; ++n) {
        assert(recv_byte(&byte) == 0);
        assert((unsigned char)byte == (unsigned char)"\0\r\nPING\n"[n]);
    }
    assert(READ(line, sizeof(line)) == 4 && !strcmp(line, "PING"));
    /* New session resets the sticky failure. */
    rejected("PING\0\n", 6);
    reset("PING\n", 5);
    assert(READ(line, sizeof(line)) == 4 && !net_failed);
    return 0;
}
'''


class CommandLineTests(unittest.TestCase):
    def test_all_agent_readers(self):
        headers = ''.join((ROOT / 'common' / name).read_text()
                          for name in ('session_timeout.h', 'command_line.h'))
        for port in ('win32', 'win16', 'dos', 'netware', 'os2', 'os2-13', 'mac-system7'):
            with self.subTest(port=port):
                source = (ROOT / f'agent-{port}/llm_agent.c').read_text(encoding='utf-8')
                capacity = re.search(r'#define LINE_MAX_LEN\s+(\d+)', source)[1]
                name = 'RecvLine' if port == 'mac-system7' else 'recv_line'
                args = '0,p,n' if port == 'win32' else 'p,n'
                defines = f'\n#define CAPACITY {capacity}\n#define READ(p,n) {name}({args})\n'
                compile_run(STUBS.replace('HEADERS', headers)
                            + function(source, f'static int {name}(') + defines + MAIN)


if __name__ == '__main__':
    unittest.main()
