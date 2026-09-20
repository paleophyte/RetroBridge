"""Run production network loops against stalled/trickling fake sockets.

No real guest is needed. Time advances deterministically, including across
the 32-bit uptime wrap. Requires GCC (or CC).
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef int SOCKET, sock_type;
typedef unsigned char BYTE;
static uint32_t ticks;
#define NET_DEADLINE(s) ((uint32_t)(ticks + (s)*1000UL))
#define NET_EXPIRED(d) ((int32_t)(ticks - (uint32_t)(d)) >= 0)
HEADER
static int g_client, g_sock, g_running=1, g_shutdown;
static int mode, pos, sent, calls, stopped;
static const char *input;
#define SOCKET_ERROR (-1)
#define WSAEWOULDBLOCK 10035
static void cpu_idle(void) { ticks += 1000; }
static void network_idle(void) { cpu_idle(); }
static int network_wait(int writing) { (void)writing; cpu_idle(); return 0; }
static int WSAGetLastError(void) { return WSAEWOULDBLOCK; }
static int shutdown(int s, int how) { (void)s; assert(how==2); stopped=1; return 0; }
static int recv(int s, char *p, int n, int flags) {
    (void)s; (void)n; (void)flags; ++calls;
    if (mode==0) return -1;
    if (mode==3) return 0;
    if (!input[pos]) return -1;
    if (mode==2) ticks+=2000; /* one byte every two seconds */
    *p=input[pos++]; return 1;
}
static int send(int s, const char *p, int n, int flags) {
    (void)s; (void)p; (void)n; (void)flags; ++calls;
    if (mode==0) return -1;
    if (mode==3) return 0;
    if (mode==2) ticks+=10000; /* progress permits a long transfer */
    ++sent; return 1;
}
static int sock_alive(void) { return 1; }
static int sock_established(sock_type *s) { (void)s; return 1; }
static int sock_dataready(sock_type *s) { (void)s; return mode!=0; }
static void sock_flushnext(sock_type *s) { (void)s; }
static int sock_fastread(sock_type *s, BYTE *p, int n) {
    (void)s; return recv(0,(char *)p,n,0);
}
static int sock_fastwrite(sock_type *s, const BYTE *p, int n) {
    int r; (void)s; r=send(0,(const char *)p,n,0); return r<0?0:r;
}
static void reset(int m, const char *data) {
    net_reset(); ticks=0; mode=m; input=data;
    pos=sent=calls=stopped=0;
}
'''
MAIN = r'''
int main(void) {
    char line[512]; int saved;
    reset(0, "");
    assert(RLINE < 0 && ticks==10000 && net_failed); /* silent auth */
    saved=calls; mode=1; input="token\nPING\n";
    assert(RLINE < 0 && calls==saved); /* queued work cannot revive it */
    reset(2, "1234567890123456789012345678901234567890\n");
    assert(RLINE < 0 && ticks==10000); /* trickle does not renew auth */
    reset(1, "token\n");
    assert(RLINE==5 && !strcmp(line,"token"));
    mode=0;
    assert(RLINE<0 && ticks==60000); /* idle command */
    reset(1, "token\n"); assert(RLINE==5);
    mode=2; pos=0; input="1234567890123456789012345678901234567890\n";
    assert(RLINE<0 && ticks==60000); /* command trickle */
    reset(1, "token\nPING\n"); assert(RLINE==5); assert(RLINE==4);
    mode=0;
    assert(RBYTE<0 && ticks==30000); /* stalled binary body */
    reset(0, ""); ticks=UINT32_MAX-4999;
    assert(RLINE<0 && ticks==5000); /* uptime wrap */
    reset(0, "");
    assert(SEND("abcdef",6)<0 && ticks==30000 && net_failed);
    saved=calls; mode=1;
    assert(SEND("PONG\n",5)<0 && calls==saved); /* no trailing reply */
    reset(2, "");
    assert(SEND("abcdefghijkl",12)==0 && ticks==120000 && sent==12);
    reset(3, "");
    assert(SEND("abc",3)<0 && net_failed);
    assert(ticks==(ZERO_SEND_WAITS ? 30000 : 0));
    reset(1, "token\n"); assert(RLINE==5);
    ticks+=120000; /* a command may legitimately execute for two minutes */
    pos=0; input="PING\n"; assert(RLINE==4);
    return 0;
}
'''


def compile_run(source):
    with tempfile.TemporaryDirectory() as directory:
        c = Path(directory) / 'timeouts.c'
        exe = Path(directory) / ('timeouts.exe' if os.name == 'nt' else 'timeouts')
        c.write_text(source, encoding='utf-8')
        subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) +
                       ['-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


class SessionTimeoutTests(unittest.TestCase):
    def test_socket_ports(self):
        header = (ROOT / 'common/session_timeout.h').read_text()
        header += (ROOT / 'common/command_line.h').read_text()
        for port in ('win32', 'win16', 'dos', 'netware', 'os2', 'os2-13'):
            with self.subTest(port=port):
                src = (ROOT / f'agent-{port}/llm_agent.c').read_text(encoding='utf-8')
                helpers = function(src, 'static int send_all(')
                helpers += function(src, 'static int recv_some(')
                if port != 'win32':
                    helpers += function(src, 'static int recv_byte(')
                helpers += function(src, 'static int recv_line(')
                if port == 'win32':
                    defines = '\n#define RLINE recv_line(0,line,sizeof(line))\n#define RBYTE recv_some(0,line,1)\n#define SEND(p,n) send_all(0,p,n)\n'
                else:
                    defines = '\n#define RLINE recv_line(line,sizeof(line))\n#define RBYTE recv_byte(line)\n#define SEND(p,n) send_all(p,n)\n'
                defines += '\n#define ZERO_SEND_WAITS ' + str(int(not port.startswith('win'))) + '\n'
                compile_run(PRELUDE.replace('HEADER', header) + helpers + defines + MAIN)

    def test_mac_async_abort_waits_for_completion(self):
        src = (ROOT / 'agent-mac-system7/llm_agent.c').read_text(encoding='utf-8')
        header = (ROOT / 'common/session_timeout.h').read_text()
        header += (ROOT / 'common/command_line.h').read_text()
        stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef int OSErr, Boolean;
typedef struct { int ioCRefNum, csCode; void *ioCompletion;
                 volatile int ioResult; int tcpStream; } TCPiopb;
typedef TCPiopb *ParmBlkPtr;
enum { false=0,true=1,noErr=0,userCanceledErr=-128,inProgress=1,
       TCPRcv=1,TCPSend=2,TCPPassiveOpen=3,TCPAbort=4 };
static uint32_t ticks;
#define NET_DEADLINE(s) ((uint32_t)(ticks+(s)*1000UL))
#define NET_EXPIRED(d) ((int32_t)(ticks-(uint32_t)(d))>=0)
HEADER
static int gQuitRequested, gMacTCPRefNum, aborted, polls, complete_at;
static TCPiopb *pending;
static int PBControlAsync(ParmBlkPtr pb) { pending=pb; return noErr; }
static int TCPControlSync(TCPiopb *pb, short code) {
    assert(code==TCPAbort && pb->tcpStream==pending->tcpStream);
    ++aborted; return noErr;
}
static void Idle(void) {
    ticks+=1000;
    /* Deliberately finish two polls AFTER abort. PB storage must live. */
    if (aborted && ++polls==2) pending->ioResult=userCanceledErr;
    else if (complete_at && ticks >= (unsigned)complete_at) pending->ioResult=noErr;
}
'''.replace('HEADER', header)
        main = r'''
int main(void) {
    TCPiopb pb; memset(&pb,0,sizeof(pb)); pb.tcpStream=123;
    net_reset(); net_begin_line();
    assert(TCPControlWait(&pb,TCPRcv)==userCanceledErr);
    assert(net_failed && aborted==1 && polls==2 && ticks==12000);
    net_reset(); aborted=polls=ticks=0;
    assert(TCPControlWait(&pb,TCPSend)==userCanceledErr);
    assert(net_failed && aborted==1 && ticks==32000);
    net_reset(); aborted=polls=ticks=0; complete_at=120000;
    assert(TCPControlWait(&pb,TCPPassiveOpen)==noErr);
    assert(!aborted && !net_failed); /* listener has no client deadline */
    return 0;
}
'''
        compile_run(stubs + function(src, 'static OSErr TCPControlWait(') + main)


if __name__ == '__main__':
    unittest.main()
