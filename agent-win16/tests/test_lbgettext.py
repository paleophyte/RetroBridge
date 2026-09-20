"""Fault-inject the production LBGETTEXT handler using a host C compiler.

Run: python tests/test_lbgettext.py (CC selects the compiler, default: gcc).
Live Win16 tests are also required to validate far pointers and real controls.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class ListboxTests(unittest.TestCase):
    def test_lengths_types_and_failures(self):
        source = (Path(__file__).resolve().parents[1] / "llm_agent.c").read_text()
        start = source.index("#define LB_TEXT_MAX")
        end = source.index("/* ---- WH_JOURNALPLAYBACK", start)
        handler = source[start:end]
        stubs = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef unsigned HWND;
typedef long LONG;
typedef long LRESULT;
typedef intptr_t LPARAM;
typedef unsigned WPARAM;
typedef unsigned long DWORD;
typedef unsigned char *HGLOBAL;
typedef char *LPSTR;
enum { GWL_STYLE=-16, LBS_OWNERDRAWFIXED=16, LBS_OWNERDRAWVARIABLE=32,
       LBS_HASSTRINGS=64, LB_GETTEXTLEN=10, LB_GETTEXT=11,
       GMEM_MOVEABLE=2, GMEM_ZEROINIT=64 };
#define _fmemcpy memcpy
#define lstrcmpi strcasecmp
static char g_iobuf[4096], text[32769], output[33000];
static const char *class_name;
static long style, first_len, second_len, result_len;
static unsigned size, allocs, frees, locks, unlocks, length_calls, reads, writes;
static int window_exists, alloc_fail, lock_fail, send_fail_at, bad_nul;
static HGLOBAL block;
static int IsWindow(HWND w) { assert(w == 123); return window_exists; }
static int GetClassName(HWND w, char *out, int cap) {
    (void)w; assert(strlen(class_name) < (unsigned)cap);
    strcpy(out, class_name); return (int)strlen(out);
}
static LONG GetWindowLong(HWND w, int which) {
    (void)w; assert(which == GWL_STYLE); return style;
}
static HGLOBAL GlobalAlloc(unsigned flags, DWORD bytes) {
    assert(flags == (GMEM_MOVEABLE | GMEM_ZEROINIT));
    assert(bytes == (unsigned long)first_len + 1 && bytes <= 32768);
    ++allocs;
    if (alloc_fail) return NULL;
    size=(unsigned)bytes; block=calloc(1,size+16); assert(block);
    memset(block+size,0xA5,16); return block;
}
static LPSTR GlobalLock(HGLOBAL h) {
    assert(h == block); ++locks; return lock_fail ? NULL : (char *)h;
}
static int GlobalUnlock(HGLOBAL h) {
    assert(h == block && !lock_fail); ++unlocks; return 0;
}
static HGLOBAL GlobalFree(HGLOBAL h) {
    unsigned i;
    assert(h == block);
    for (i=0;i<16;i++) assert(h[size+i] == 0xA5);
    ++frees; free(h); block=NULL; return NULL;
}
static LRESULT SendMessage(HWND w, unsigned msg, WPARAM idx, LPARAM data) {
    assert(w == 123 && idx == 0);
    if (msg == LB_GETTEXTLEN) {
        assert(data == 0); ++length_calls;
        return length_calls == 1 ? first_len : second_len;
    }
    assert(msg == LB_GETTEXT && data == (LPARAM)block);
    ++reads;
    assert(length_calls == 2 && size > strlen(text));
    memcpy((void *)data,text,strlen(text)+1);
    if (bad_nul) block[strlen(text)]='!';
    return result_len;
}
static int send_all(const char *data, int len) {
    assert(len >= 0 && strlen(output)+(unsigned)len < sizeof(output));
    if (++writes == (unsigned)send_fail_at) return -1;
    strncat(output,data,(size_t)len); return 0;
}
static int send_cstr(const char *data) { return send_all(data,(int)strlen(data)); }
static void reset(unsigned len) {
    unsigned i;
    assert(block == NULL);
    for (i=0;i<len;i++) text[i]=(char)('A'+i%26);
    text[len]=0; output[0]=0;
    class_name="ListBox"; style=0; window_exists=1;
    first_len=second_len=result_len=len;
    allocs=frees=locks=unlocks=length_calls=reads=writes=0;
    alloc_fail=lock_fail=send_fail_at=bad_nul=0;
}
'''
        cases = r'''
static void expect_error(const char *args) {
    assert(handle_lbgettext(args) < 0);
    assert(strncmp(output,"ERR:",4) == 0);
    assert(block == NULL);
}
int main(void) {
    unsigned i;
    const unsigned lengths[]={0,1,159,160,161,4096,8193,32767};
    const char *bad[]={"", "123", "0 0", "-1 0", "65536 0", "123 -1",
        "123 32768", "123 0 junk", "123x 0", "123 0x1",
        "999999999999999999999999999 0", "123 999999999999999999999999"};
    for (i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        unsigned len=lengths[i]; reset(len);
        assert(handle_lbgettext("123 0") == 0);
        assert(strlen(output) == len+5 && !memcmp(output,"OK:",3));
        assert(!memcmp(output+3,text,len) && !strcmp(output+3+len,"\r\n"));
        assert(reads == 1 && allocs == 1 && frees == 1 && unlocks == 1);
    }
    for (i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        reset(3); expect_error(bad[i]); assert(!reads && !allocs);
    }
    reset(3); window_exists=0; expect_error("123 0"); assert(!length_calls);
    reset(3); class_name="Button"; expect_error("123 0"); assert(!length_calls);
    reset(3); style=LBS_OWNERDRAWFIXED; expect_error("123 0"); assert(!length_calls);
    reset(3); style=LBS_OWNERDRAWVARIABLE; expect_error("123 0"); assert(!length_calls);
    reset(3); style=LBS_OWNERDRAWFIXED|LBS_HASSTRINGS;
    assert(handle_lbgettext("123 0") == 0 && !strcmp(output,"OK:ABC\r\n"));
    reset(3); first_len=-1; expect_error("123 0"); assert(!allocs);
    reset(3); first_len=32768; expect_error("123 0"); assert(!allocs);
    reset(3); alloc_fail=1; expect_error("123 0"); assert(!reads && !frees);
    reset(3); lock_fail=1; expect_error("123 0"); assert(frees == 1 && !unlocks && !reads);
    reset(3); second_len=4; expect_error("123 0"); assert(!reads && frees == 1);
    reset(3); second_len=-1; expect_error("123 0"); assert(!reads && frees == 1);
    reset(3); first_len=8; assert(handle_lbgettext("123 0") == 0);
    reset(3); result_len=-1; expect_error("123 0"); assert(frees == 1);
    reset(3); result_len=4; expect_error("123 0"); assert(frees == 1);
    reset(3); bad_nul=1; expect_error("123 0"); assert(frees == 1);
    reset(3); text[1]='\n'; expect_error("123 0"); assert(frees == 1);
    reset(3); text[1]='\r'; expect_error("123 0"); assert(frees == 1);
    for (i=1;i<=5;i++) {
        reset(8193); send_fail_at=(int)i;
        assert(handle_lbgettext("123 0") < 0 && frees == 1 && unlocks == 1);
    }
    puts("LBGETTEXT: lengths, guard bytes, control types, parsing, allocation, mutation and send failures passed.");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cfile = root / "listbox.c"
            exe = root / ("listbox.exe" if os.name == "nt" else "listbox")
            cfile.write_text(stubs + handler + cases)
            compiler = shlex.split(os.environ.get("CC", "gcc"))
            subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                                      str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
