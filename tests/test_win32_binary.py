"""Fault-inject production Win32 GET/screenshot handlers and wire helpers.

Uses portable C stubs; requires Python and GCC (or CC). No real files,
network, desktop, or legacy machine are touched.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SOCKET, HANDLE, HDC, HBITMAP, BOOL;
typedef uint32_t DWORD;
typedef unsigned char BYTE;
typedef struct {
    DWORD biSize; int32_t biWidth, biHeight; uint16_t biPlanes, biBitCount;
    DWORD biCompression, biSizeImage; int32_t biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef BITMAPINFOHEADER BITMAPINFO;
#pragma pack(push, 2)
typedef struct { uint16_t bfType; DWORD bfSize; uint16_t bfReserved1, bfReserved2;
                 DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
#define READ_CHUNK 8
#define SOCKET_ERROR (-1)
#define SD_BOTH 2
#define NET_IO_SECONDS 30UL
#define NET_DEADLINE(s) 0UL
#define WSAEWOULDBLOCK 10035
static int net_failed, g_running=1;
static int net_expired(unsigned long d) { (void)d; return net_failed; }
static int net_fail(void) { net_failed=1; return -1; }
static int WSAGetLastError(void) { return 10054; }
static void network_idle(void) { assert(0); }
#define INVALID_HANDLE_VALUE (-1)
#define INVALID_FILE_SIZE UINT32_MAX
#define GENERIC_READ 1
#define FILE_SHARE_READ 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 4
#define HORZRES 5
#define VERTRES 6
#define SRCCOPY 7
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define ZeroMemory(p,n) memset(p,0,n)
#define wsprintfA sprintf
static BYTE wire[8192], file_data[1024];
static int used, limit, fail_after, fail_zero, stopped, sends;
static int available, reported, consumed, reads, read_failure, closes;
static int bitmap_deletes, dc_deletes, dc_releases, allocations, frees;
static void reset(int chunk) {
    int i;
    net_failed=0;
    used = stopped = sends = consumed = reads = closes = 0;
    bitmap_deletes = dc_deletes = dc_releases = allocations = frees = 0;
    limit = chunk; fail_after = -1; fail_zero = read_failure = 0;
    available = reported = 257;
    memset(wire, 0x7f, sizeof(wire));
    for (i=0; i<(int)sizeof(file_data); ++i) file_data[i]=(BYTE)i;
}
static int shutdown(SOCKET s, int how) {
    (void)s; assert(how == SD_BOTH); ++stopped; return 0;
}
static int send(SOCKET s, const char *p, int n, int flags) {
    int take = n < limit ? n : limit;
    (void)s; (void)flags;
    assert(!stopped); ++sends;
    if (fail_after >= 0 && used >= fail_after) return fail_zero ? 0 : SOCKET_ERROR;
    if (fail_after >= 0 && take > fail_after-used) take=fail_after-used;
    assert(take > 0 && used+take < (int)sizeof(wire));
    memcpy(wire+used,p,(unsigned)take); used+=take; return take;
}
static HANDLE CreateFileA(const char *p,int a,int b,void *c,int d,int e,void *f) {
    (void)p;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 1;
}
static DWORD GetFileSize(HANDLE h,void *p) {(void)h;(void)p;return (DWORD)reported;}
static int CloseHandle(HANDLE h) {assert(h==1);++closes;return 1;}
static BOOL ReadFile(HANDLE h,void *buf,DWORD want,DWORD *got,void *p) {
    DWORD n = (DWORD)(available-consumed);
    (void)h;(void)p; ++reads;
    if (read_failure && reads==2) return 0;
    if (n>want) n=want;
    memcpy(buf,file_data+consumed,n);consumed+=(int)n;*got=n;return 1;
}
static HDC GetDC(void *p) {(void)p;return 1;}
static int GetDeviceCaps(HDC h,int which) {(void)h;return which==HORZRES ? 3 : 2;}
static HDC CreateCompatibleDC(HDC h) {(void)h;return 2;}
static HBITMAP CreateCompatibleBitmap(HDC h,int w,int n) {(void)h;(void)w;(void)n;return 3;}
static HBITMAP SelectObject(HDC h,HBITMAP b) {(void)h;(void)b;return 4;}
static int BitBlt(HDC a,int b,int c,int d,int e,HDC f,int g,int h,int i) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return 1;
}
static int GetDIBits(HDC h,HBITMAP b,int start,int rows,void *p,BITMAPINFO *bi,int flags) {
    int i; (void)h;(void)b;(void)start;(void)bi;(void)flags;
    for(i=0;i<24;++i) ((BYTE *)p)[i]=(BYTE)(i*17);
    return rows;
}
static int DeleteObject(HBITMAP b) {(void)b;++bitmap_deletes;return 1;}
static int DeleteDC(HDC h) {(void)h;++dc_deletes;return 1;}
static int ReleaseDC(void *p,HDC h) {(void)p;(void)h;++dc_releases;return 1;}
static void *alloc_mem(size_t n) {++allocations;return malloc(n);}
static void free_mem(void *p) {++frees;free(p);}
#define malloc alloc_mem
#define free free_mem
'''

MAIN = r'''
static void capture_cleanup(void) {
    assert(bitmap_deletes==1 && dc_deletes==1 && dc_releases==1);
    assert(allocations==1 && frees==1);
}
int main(void) {
    BYTE expected[8192]; int total, i, mode, chunk;
    const int chunks[]={8192,1,2,7};
    for(chunk=0;chunk<4;++chunk) {
        reset(chunks[chunk]);
        assert(handle_get(0,"fixture")==0 && !stopped && closes==1);
        assert(used==9+257 && !memcmp(wire,"SIZE:257\n",9));
        assert(!memcmp(wire+9,file_data,257));
        assert(send_cstr(0,"PONG\n")==0 && !memcmp(wire+266,"PONG\n",5));
        reset(chunks[chunk]); reported=available=0;
        assert(handle_get(0,"empty")==0 && !stopped && reads==0 && closes==1);
        assert(used==7 && !memcmp(wire,"SIZE:0\n",7));
    }
    /* File grows after SIZE: only the advertised prefix belongs to this reply. */
    reset(7);available=500;
    assert(handle_get(0,"growing")==0 && used==266 && consumed==257);
    /* Premature EOF and I/O error after SIZE must not leave a reusable stream. */
    reset(7);available=13;
    assert(handle_get(0,"short")==-1 && stopped && closes==1 && used==22);
    reset(7);read_failure=1;
    assert(handle_get(0,"io-error")==-1 && stopped && closes==1 && used==17);
    for(mode=0;mode<2;++mode) for(i=0;i<266;++i) {
        reset(7);fail_after=i;fail_zero=mode;
        assert(handle_get(0,"fixture")==-1 && stopped && closes==1 && used==i);
    }
    reset(8192);
    assert(handle_screenshot(0)==0 && !stopped);capture_cleanup();
    total=used;assert(total==86 && !memcmp(wire,"SIZE:78\nBM",10));
    memcpy(expected,wire,(unsigned)total);
    for(chunk=1;chunk<4;++chunk) {
        reset(chunks[chunk]);
        assert(handle_screenshot(0)==0 && !stopped);capture_cleanup();
        assert(used==total && !memcmp(expected,wire,(unsigned)total));
        assert(send_cstr(0,"PONG\n")==0 && !memcmp(wire+total,"PONG\n",5));
    }
    /* Every offset includes text header, both BMP headers, and pixel payload. */
    for(mode=0;mode<2;++mode) for(i=0;i<total;++i) {
        reset(7);fail_after=i;fail_zero=mode;
        assert(handle_screenshot(0)==-1 && stopped && used==i);capture_cleanup();
    }
    reset(1);assert(send_all(0,"",0)==0 && sends==0 && !stopped);
    assert(send_all(0,"",-1)==-1 && stopped && sends==0);
    return 0;
}
'''


class Win32BinaryTests(unittest.TestCase):
    def test_no_unchecked_socket_writes(self):
        source = SOURCE.read_text(encoding="utf-8")
        helper = function(source, "static int send_all(")
        self.assertEqual(len(re.findall(r"\bsend\s*\(", source)), 1)
        self.assertRegex(helper, r"\bsend\s*\(")

    def test_transfer_and_capture_failures(self):
        source = SOURCE.read_text(encoding="utf-8")
        handlers = "\n".join(function(source, signature) for signature in (
            "static int send_all(", "static int send_cstr(",
            "static int handle_get(", "static int handle_screenshot("))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cfile = root / "binary.c"
            exe = root / ("binary.exe" if os.name == "nt" else "binary")
            cfile.write_text(STUBS + handlers + MAIN, encoding="utf-8")
            compiler = shlex.split(os.environ.get("CC", "gcc"))
            subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                                      str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
