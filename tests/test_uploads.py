"""Compile production upload handlers with injected disk/network failures.

Run with Python 3 and GCC (or set CC). No real files or sockets are used.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    # These handlers contain no unmatched braces inside comments/strings.
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int SOCKET;
typedef int HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;
typedef int OSErr;
typedef struct { short vRefNum; int kind; } FSSpec;
typedef struct { int launchBlockID, launchEPBLength, launchControlFlags;
                 FSSpec *launchAppSpec; } LaunchParamBlockRec;
enum { TRUE=1, FALSE=0, true=1, noErr=0, INVALID_HANDLE_VALUE=-1,
       GENERIC_WRITE=1, CREATE_ALWAYS=2, FILE_ATTRIBUTE_NORMAL=4,
       paramErr=-50, fnfErr=-43, fsWrPerm=2, smSystemScript=0, extendedBlock=1, extendedBlockLen=2, launchContinue=4 };
#define READ_CHUNK 8
static char g_iobuf[READ_CHUNK], gIOBuf[READ_CHUNK];
static int g_client, gQuitRequested;
static char incoming[80], stored[80], response[160];
static int declared, available, consumed, stored_len;
static int fault, opens, writes, flushes, closes, removes, launches;
static FILE *file_token;
/* fault: 1=open, 2=short write, 3=write error, 4=flush,
 * 5=close, 6=sticky error, 7=network interruption, 8=staging cleanup */
static int input(char *out, int want) {
    int n=want;
    if (consumed >= available) return 0;
    if (n > available-consumed) n=available-consumed;
    memcpy(out,incoming+consumed,(size_t)n); consumed+=n; return n;
}
static int recv(SOCKET s, char *out, int want, int flags) {
    (void)s; (void)flags; return input(out,want);
}
static int recv_byte(char *out) { return input(out,1) == 1 ? 0 : -1; }
static int RecvExact(char *out, long n) { return input(out,(int)n) == n ? 0 : -1; }
static void tcp_pump(void) {}
static void ThreadSwitch(void) {}
static int send_cstr(const char *s) {
    /* A local failure must drain before acknowledging, even when open fails. */
    assert(consumed == available || declared < 0);
    assert(strlen(response)+strlen(s)<sizeof(response)); strcat(response,s); return 0;
}
static int SendCStr(const char *s) { return send_cstr(s); }
static int send_cstr_socket(SOCKET sock, const char *s) {
    (void)sock; return send_cstr(s);
}
static int send(SOCKET sock, const char *s, int n, int flags) {
    (void)sock; (void)flags;
    assert(n == (int)strlen(s)); send_cstr(s); return n;
}
static FILE *mock_fopen(const char *name, const char *mode) {
    (void)name; assert(!strcmp(mode,"wb")); ++opens;
    return fault == 1 ? NULL : file_token;
}
static size_t mock_fwrite(const void *data, size_t unit, size_t count, FILE *f) {
    size_t n=count;
    assert(f == file_token && unit == 1); ++writes;
    if (fault == 2 || fault == 8) n=count-1;
    if (fault == 3) n=0;
    assert(stored_len+(int)n <= declared);
    memcpy(stored+stored_len,data,n); stored_len+=(int)n; return n;
}
static int mock_fflush(FILE *f) { assert(f == file_token); ++flushes; return fault == 4 ? -1 : 0; }
static int mock_ferror(FILE *f) { assert(f == file_token); return fault == 6; }
static int mock_fclose(FILE *f) { assert(f == file_token); ++closes; return fault == 5 ? -1 : 0; }
static int mock_remove(const char *path) {
    assert(!strcmp(path,"STAGED_AGENT.bin")); ++removes; return fault == 8 ? -1 : 0;
}
#define fopen mock_fopen
#define fwrite mock_fwrite
#define fflush mock_fflush
#define ferror mock_ferror
#define fclose mock_fclose
#define remove mock_remove
static HANDLE CreateFileA(const char *name, int access, int share, void *sec,
                          int disposition, int attrs, void *template_file) {
    (void)name; (void)access; (void)share; (void)sec; (void)disposition;
    (void)attrs; (void)template_file; ++opens;
    return fault == 1 ? INVALID_HANDLE_VALUE : 42;
}
static BOOL WriteFile(HANDLE h, const void *data, DWORD n, DWORD *written, void *overlapped) {
    (void)overlapped; assert(h == 42);
    *written=(DWORD)mock_fwrite(data,1,(size_t)n,file_token);
    return fault == 3 ? FALSE : TRUE;
}
static BOOL FlushFileBuffers(HANDLE h) { assert(h == 42); ++flushes; return fault != 4; }
static BOOL CloseHandle(HANDLE h) { assert(h == 42); ++closes; return fault != 5; }
static OSErr FSMakeFSSpec(int a, int b, unsigned char *name, FSSpec *spec) {
    (void)a; (void)b; spec->vRefNum=1; spec->kind=2;
    if (name[0]==11 && !memcmp(name+1,"llm_updater",11)) {
        spec->kind=1;
        assert((fault == 0 || fault == 9 || fault == 10) && consumed == declared && closes == 1 && flushes == 1);
        if (fault == 10) return fnfErr;
    }
    if (fault == 12) return fnfErr;
    return noErr;
}
static OSErr FSpCreate(FSSpec *s, unsigned long creator, unsigned long type, int script) {
    (void)s; (void)creator; (void)type; (void)script; return fault==12?-1:noErr;
}
static OSErr FSpOpenDF(FSSpec *s,int mode,short *ref) {
    assert(s->kind==2 && mode==fsWrPerm); ++opens; *ref=42; return fault==1?-1:0;
}
static OSErr SetEOF(short ref,long n) { assert(ref==42 && n==0); return fault==11?-1:noErr; }
static OSErr FSWrite(short ref,long *n,const void *p) {
    assert(ref==42); *n=(long)mock_fwrite(p,1,(size_t)*n,file_token); return fault==3?-1:0;
}
static OSErr FSClose(short ref) { assert(ref==42); ++closes; return fault==5?-1:0; }
static OSErr FlushVol(const unsigned char *name,short vol) {
    assert(!name && vol==1); ++flushes; return fault==4 || fault==6?-1:0;
}
static OSErr FSpDelete(FSSpec *spec) { assert(spec->kind==2); return mock_remove("STAGED_AGENT.bin"); }
static OSErr LaunchApplication(LaunchParamBlockRec *pb) {
    (void)pb; assert((fault == 0 || fault == 9 || fault == 10) && closes == 1 && flushes == 1); if (fault == 9) return -1; ++launches; return noErr;
}
static void reset(int scenario, int n) {
    int i;
    fault=scenario; declared=n; available=n; consumed=stored_len=0;
    opens=writes=flushes=closes=removes=launches=gQuitRequested=0;
    response[0]=0; file_token=(FILE *)&file_token;
    for(i=0;i<n;i++) incoming[i]=(char)('a'+i%26);
    memcpy(incoming+n,"PING\n",5);
    if (fault == 7) available=10;
}
'''

MAIN = r'''
int main(void) {
    int fail, n;
    char args[400];
    for (n=0;n<=23;n+=23) {
      for (fail=0;fail<=10;fail++) {
        if (!n && (fail == 2 || fail == 3 || fail == 7 || fail == 8)) continue;
        if (fail >= 9 && !IS_UPDATE) continue;
        if (IS_WIN32 && fail == 6) continue; /* stdio-only sticky error */
        if (IS_UPDATE && !n) continue;
        reset(fail,n);
        sprintf(args,IS_UPDATE ? "%d" : "sample file.bin %d",n);
        CALL_HANDLER;
        assert(consumed == available && !memcmp(incoming+n,"PING\n",5));
        assert(closes == (fail == 1 ? 0 : 1));
        assert(flushes == (fail == 1 ? 0 : 1));
        if (fail) {
          assert(!strncmp(response,"ERR:",4) && !strstr(response,"OK"));
          assert(!launches && !gQuitRequested);
          if (fail == 2 || fail == 3 || fail == 8) assert(writes == 1);
          if (IS_UPDATE) assert(removes == (fail == 1 || fail >= 9 ? 0 : 1));
        } else {
          assert(!strcmp(response,IS_WIN16 ? "OK\r\n" : "OK\n"));
          assert(stored_len == n && !memcmp(stored,incoming,(size_t)n));
          assert(launches == IS_UPDATE && gQuitRequested == IS_UPDATE);
        }
      }
    }
    EXTRA_CHECKS
    puts("upload success, short/error writes, flush/close errors, draining and update gating passed");
    return 0;
}
'''


class UploadTests(unittest.TestCase):
    def test_all_ports(self):
        ports = ["dos", "os2", "os2-13", "netware", "win16", "win32", "mac-system7"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for port in ports:
                source = (ROOT / ("agent-" + port) / "llm_agent.c").read_text(encoding="utf-8")
                for update in ([False, True] if port == "mac-system7" else [False]):
                    with self.subTest(port=port, update=update):
                        win32 = port == "win32"
                        mac = port == "mac-system7"
                        signature = ("static void HandleUpdate(" if update else
                                     "static void HandlePut(" if mac else "static int handle_put(")
                        handler = function(source, signature)
                        if win32:
                            handler = handler.replace("send_cstr(", "send_cstr_socket(")
                            handler = handler.replace("recv_some(s, buf, want)", "input(buf, want)")
                        elif not mac:
                            handler = handler.replace("recv_some(g_iobuf, want)", "input(g_iobuf, want)")
                        if mac:
                            handler = function(source, "static OSErr OpenUpload(") + "\n" + function(source, "static int ReceiveUpload(") + "\n" + handler
                        call = ("HandleUpdate(args)" if update else "HandlePut(args)" if mac else
                                "handle_put(0,args)" if win32 else "handle_put(args)")
                        main = MAIN.replace("IS_WIN32", str(int(win32))).replace(
                            "IS_WIN16", str(int(port == "win16"))).replace(
                            "IS_UPDATE", str(int(update))).replace("CALL_HANDLER", call)
                        extra = ""
                        if mac:
                            extra = r'''
    reset(0,0); declared=-1;
    strcpy(args,NEGATIVE_ARGS); CALL;
    assert(!opens && !closes && !launches && !gQuitRequested);
    assert(!strncmp(response,"ERR:",4));
'''.replace("NEGATIVE_ARGS", '"-1"' if update else '"sample.bin -1"').replace("CALL", call)
                            if update:
                                extra += r'''
    reset(0,0); strcpy(args,"0"); HandleUpdate(args);
    assert(!opens && !launches && !gQuitRequested);
    assert(!strncmp(response,"ERR:",4));
'''
                            else:
                                extra += r'''
    reset(0,23); memset(args,'p',256); strcpy(args+256," 23");
    HandlePut(args);
    assert(!opens && !closes && consumed == declared);
    assert(!strncmp(response,"ERR:",4));
'''
                        if mac:
                            extra += r'''
    for (fail=11;fail<=12;fail++) {
        reset(fail,23); strcpy(args,OPEN_ARGS); CALL;
        assert(consumed==declared && !strncmp(response,"ERR:",4));
        assert(!launches && !gQuitRequested && !flushes && !removes);
        assert(opens==(fail==11) && closes==(fail==11));
    }
'''.replace("OPEN_ARGS", '"23"' if update else '"sample.bin 23"').replace("CALL", call)
                        main = main.replace("EXTRA_CHECKS", extra)
                        cfile = root / "upload.c"
                        exe = root / ("upload.exe" if os.name == "nt" else "upload")
                        cfile.write_text(STUBS + handler + main, encoding="utf-8")
                        compiler = shlex.split(os.environ.get("CC", "gcc"))
                        subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                                                   "-Wno-unused-function", "-Wno-unused-variable",
                                                   str(cfile), "-o", str(exe)], check=True)
                        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
