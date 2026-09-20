"""Fault-inject the production Win32 EXEC handler and checked wire helpers."""
from pathlib import Path
import unittest

from test_session_timeouts import compile_run
from test_uploads import function

SOURCE = Path(__file__).resolve().parents[1] / 'agent-win32/llm_agent.c'
STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int SOCKET, BOOL;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef struct { DWORD nLength; int bInheritHandle; void *lpSecurityDescriptor; } SECURITY_ATTRIBUTES;
typedef struct { DWORD cb, dwFlags; HANDLE hStdOutput, hStdError, hStdInput; } STARTUPINFOA;
typedef struct { HANDLE hProcess, hThread; } PROCESS_INFORMATION;
#define TRUE 1
#define HANDLE_FLAG_INHERIT 1
#define STARTF_USESTDHANDLES 1
#define STD_INPUT_HANDLE 0
#define CREATE_NO_WINDOW 1
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define READ_CHUNK 8
#define LINE_MAX_LEN 4096
#define ZeroMemory(p,n) memset(p,0,n)
#define wsprintfA sprintf
#define SOCKET_ERROR (-1)
#define WSAEWOULDBLOCK 10035
#define NET_IO_SECONDS 30UL
#define NET_DEADLINE(s) 0UL
static int net_failed, g_running=1;
static int net_expired(unsigned long d) { (void)d; return net_failed; }
static int net_fail(void) { net_failed=1; return -1; }
static int WSAGetLastError(void) { return 10054; }
static void network_idle(void) { assert(0); }
static unsigned char wire[4096];
static int used, chunk, fail_at, stopped, mode, win9x, pipe_calls, process_calls;
static int live[5], closes[5], consumed, quiet_waits;
static DWORD last_error, injected_error, child_exit, ticks;
static const char output[] = "hello\0world\n";
static int is_windows_9x(void) { return win9x; }
static DWORD GetLastError(void) { return last_error; }
static DWORD GetTickCount(void) { return ticks; }
static HANDLE GetStdHandle(int which) { (void)which; return NULL; }
static int shutdown(SOCKET s, int how) { (void)s; assert(how==2); ++stopped; return 0; }
static int send(SOCKET s, const char *p, int n, int flags) {
    int take=n<chunk?n:chunk; (void)s; (void)flags;
    if (fail_at>=0) {
        if (used>=fail_at) return SOCKET_ERROR;
        if (take>fail_at-used) take=fail_at-used;
    }
    assert(used+take<(int)sizeof(wire)); memcpy(wire+used,p,(unsigned)take); used+=take;
    return take;
}
static BOOL CreatePipe(HANDLE *r, HANDLE *w, SECURITY_ATTRIBUTES *sa, DWORD size) {
    (void)size; assert(sa->bInheritHandle); ++pipe_calls;
    if (mode==1) { last_error=injected_error; return 0; }
    *r=(HANDLE)(uintptr_t)1; *w=(HANDLE)(uintptr_t)2; live[1]=live[2]=1; return 1;
}
static BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) {
    assert(h==(HANDLE)(uintptr_t)1 && mask==HANDLE_FLAG_INHERIT && flags==0); return 1;
}
static BOOL CloseHandle(HANDLE h) {
    unsigned i=(unsigned)(uintptr_t)h; assert(i>0 && i<5 && live[i]);
    live[i]=0; ++closes[i]; last_error=999; return 1;
}
static BOOL CreateProcessA(void *app, char *cmd, void *pa, void *ta, BOOL inherit,
                           DWORD flags, void *env, void *cwd, STARTUPINFOA *si,
                           PROCESS_INFORMATION *pi) {
    (void)app; (void)pa; (void)ta; (void)env; (void)cwd;
    assert(inherit && flags==CREATE_NO_WINDOW && si->dwFlags==STARTF_USESTDHANDLES);
    assert(si->hStdOutput==(HANDLE)(uintptr_t)2 && si->hStdError==si->hStdOutput);
    assert(!strncmp(cmd, win9x?"command.com /C ":"cmd.exe /C ",win9x?15:11));
    ++process_calls;
    if (mode==2) { last_error=injected_error; return 0; }
    pi->hProcess=(HANDLE)(uintptr_t)3; pi->hThread=(HANDLE)(uintptr_t)4;
    live[3]=live[4]=1; return 1;
}
static BOOL PeekNamedPipe(HANDLE h, void *buf, DWORD n, void *read, DWORD *avail, void *left) {
    (void)buf; (void)n; (void)read; (void)left; assert(h==(HANDLE)(uintptr_t)1);
    *avail=quiet_waits?0:(DWORD)(sizeof(output)-1-consumed); return 1;
}
static BOOL ReadFile(HANDLE h, char *buf, DWORD n, DWORD *got, void *overlapped) {
    (void)overlapped; assert(h==(HANDLE)(uintptr_t)1);
    assert(n<=sizeof(output)-1-(unsigned)consumed);
    memcpy(buf,output+consumed,n); consumed+=(int)n; *got=n; return 1;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    assert(h==(HANDLE)(uintptr_t)3); ticks+=ms;
    if (quiet_waits) { --quiet_waits; return WAIT_TIMEOUT; }
    return WAIT_OBJECT_0;
}
static BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {
    assert(h==(HANDLE)(uintptr_t)3); *code=child_exit; return 1;
}
static BOOL TerminateProcess(HANDLE h, unsigned code) {
    assert(h==(HANDLE)(uintptr_t)3 && code==1); child_exit=1; return 1;
}
static void reset(int failure, int limit, int family) {
    int i; for(i=1;i<5;++i)assert(!live[i]);
    net_failed=used=stopped=pipe_calls=process_calls=consumed=quiet_waits=0;
    ticks=0; last_error=0; child_exit=7; injected_error=8;
    memset(closes,0,sizeof(closes)); memset(wire,0x7f,sizeof(wire));
    mode=failure; chunk=limit; win9x=family; fail_at=-1;
}
'''
MAIN = r'''
int main(void) {
    int family, failure, i, length, pos, n, bytes, exit_code;
    char message[128], expected[256], line[64], combined[128];
    const DWORD errors[]={2,5,8,193,0xffffffffUL};
    for(family=0;family<2;++family)for(failure=1;failure<=2;++failure) {
        for(i=0;i<5;++i) {
            reset(failure,1,family); injected_error=errors[i];
            assert(run_exec(0,"do-not-echo-sensitive-argument")<0);
            sprintf(message,"EXEC launch failed: %s (Win32 error %lu)\r\n",
                    failure==1?"CreatePipe":"CreateProcessA",errors[i]);
            sprintf(expected,"LEN:%lu\n%sEXIT:-1\n",(unsigned long)strlen(message),message);
            length=(int)strlen(expected);
            assert(used==length && !memcmp(wire,expected,(unsigned)length));
            assert(pipe_calls==1 && process_calls==(failure==2));
            assert(closes[1]==(failure==2) && closes[2]==(failure==2));
            assert(closes[3]==0 && closes[4]==0);
            assert(!net_failed && !stopped);
            assert(send_cstr(0,"PONG\n")==0);
            assert(used==length+5 && !memcmp(wire+length,"PONG\n",5));
            /* The next EXEC on the same session can still start a child. */
            mode=0; assert(run_exec(0,"echo hello")==0 && !net_failed);
        }
        /* Fail at every byte boundary of header, message, and EXIT. */
        reset(failure,4096,family); assert(run_exec(0,"secret")<0);
        length=used; memcpy(expected,wire,(unsigned)length);
        for(i=0;i<length;++i) {
            reset(failure,3,family); fail_at=i;
            assert(run_exec(0,"secret")<0 && net_failed && stopped==1);
            assert(used==i && !memcmp(wire,expected,(unsigned)i));
            assert(send_cstr(0,"PONG\n")<0 && used==i);
            assert(!live[1] && !live[2] && !live[3] && !live[4]);
        }
    }
    /* Successful binary output, nonzero child exit, and quiet heartbeats. */
    reset(0,2,0); quiet_waits=27; assert(run_exec(0,"echo hello")==0);
    pos=bytes=0; exit_code=-1;
    while(pos<used) {
        n=0; while(wire[pos]!='\n') { assert(n<63); line[n++]=(char)wire[pos++]; }
        ++pos; line[n]=0;
        if(sscanf(line,"LEN:%d",&n)==1) {
            assert(bytes+n<(int)sizeof(combined)); memcpy(combined+bytes,wire+pos,(unsigned)n);
            bytes+=n; pos+=n;
        } else { assert(sscanf(line,"EXIT:%d",&exit_code)==1); assert(pos==used); }
    }
    assert(bytes==sizeof(output)-1 && !memcmp(combined,output,sizeof(output)-1));
    assert(exit_code==7 && !net_failed);
    for(i=1;i<5;++i)assert(closes[i]==1 && !live[i]);
    return 0;
}
'''


class Win32ExecTests(unittest.TestCase):
    def test_launch_failures_cleanup_and_persistent_framing(self):
        source = SOURCE.read_text(encoding='utf-8')
        helpers = ''.join(function(source, signature) for signature in (
            'static int send_all(', 'static int send_cstr(',
            'static void build_shell_command(', 'static int exec_launch_error(',
            'static int run_exec('))
        compile_run(STUBS + helpers + MAIN)


if __name__ == '__main__':
    unittest.main()
