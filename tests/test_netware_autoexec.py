"""Exercise the actual NCF planner and AUTOEXEC handler, including disk faults."""
from pathlib import Path
import unittest

from test_session_timeouts import compile_run
from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "agent-netware/llm_agent.c").read_text(encoding="utf-8")
HEADER = (ROOT / "agent-netware/autoexec.h").read_text(encoding="utf-8")
HANDLER = "\n".join(function(SOURCE, name) for name in (
    "static int file_exists_rb(", "static int autoexec_read(", "static int handle_autoexec("))

PARSER_CASES = r'''
static void expect(const char *input, int result, const char *output) {
    char out[8192]; int n=-1, rc;
    memset(out,0xcc,sizeof(out));
    rc=autoexec_plan(input,(int)strlen(input),out,sizeof(out)-1,&n);
    assert(rc==result);
    if (result>0) { assert(n==(int)strlen(output)); assert(!memcmp(out,output,n)); }
    if (result==0) { assert(n==(int)strlen(input)); assert((unsigned char)out[0]==0xcc); }
}
static void parser_cases(void) {
    char out[8192]; int n;
    expect("",1,"LOAD CLIBAUX\r\nLOAD LLMAGENT\r\n");
    expect("# LOAD LLMAGENT\n;load clibaux\n ReM load llmagent\n",1,
           "# LOAD LLMAGENT\n;load clibaux\n ReM load llmagent\nLOAD CLIBAUX\r\nLOAD LLMAGENT\r\n");
    expect("LOAD LLMAGENT_OLD.NLM\nLOAD CLIBAUX2\nECHO llmagent clibaux\n",1,
           "LOAD LLMAGENT_OLD.NLM\nLOAD CLIBAUX2\nECHO llmagent clibaux\nLOAD CLIBAUX\r\nLOAD LLMAGENT\r\n");
    expect("\tLoAd\tSYS:SYSTEM/ClibAux.Nlm\n load \"SYS:SYSTEM\\LLMAGENT.NLM\" -debug\n",0,0);
    expect("LOAD CLIBAUX # comment\rLOAD LLMAGENT ; comment\r",0,0);
    expect("LOAD CLIBAUX\nLOAD LLMAGENT.old\n",2,
           "LOAD CLIBAUX\nLOAD LLMAGENT.old\nLOAD LLMAGENT\r\n");
    expect("LOAD CLIBAUX",2,"LOAD CLIBAUX\r\nLOAD LLMAGENT\r\n");
    expect("LOAD TCPIP\n  LOAD LLMAGENT.NLM options\nREM tail  ",3,
           "LOAD TCPIP\nLOAD CLIBAUX\r\n  LOAD LLMAGENT.NLM options\nREM tail  ");
    expect("REM tail\032",1,"REM tail\r\nLOAD CLIBAUX\r\nLOAD LLMAGENT\r\n\032");
    expect("LOAD CLIBAUX\nLOAD LLMAGENT\032",0,0);
    expect("LOAD LLMAGENT\nLOAD CLIBAUX\n",-3,0);
    expect("LOAD CLIBAUX\nLOAD LLMAGENT\nLOAD TCPIP\n",-3,0);
    expect("LOAD CLIBAUX\nLOAD LLMAGENT\nBIND IP board\n",-3,0);
    expect("LOAD CLIBAUX\nLOAD LLMAGENT\nLOAD LLMAGENT\n",-2,0);
    expect("LOAD CLIBAUX\nLOAD CLIBAUX\n",-2,0);
    expect("?N LOAD LLMAGENT\n",-2,0);
    expect("? LOAD CLIBAUX\n",-2,0);
    expect("LOAD CLIBAUX\nUNLOAD CLIBAUX\n",-2,0);
    expect("LLMAGENT.NLM\n",-2,0);
    expect("LOAD PROTECTED LLMAGENT\n",-2,0);
    expect("LOAD \"LLMAGENT\n",-1,0);
    expect("LOAD \"LLMAGENT\"suffix\n",-1,0);
    expect("LOAD\n",-1,0);
    expect("REM tail\032LOAD LLMAGENT\n",-1,0);
    assert(autoexec_plan("LOAD\0CLIBAUX",12,out,sizeof(out),&n)==-1);
    assert(autoexec_plan("",0,out,5,&n)==-4);
}
'''

FAULT_STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define AUTOEXEC_MAX 8192
#define AUTOEXEC_NCF "original"
#define AUTOEXEC_NEW "staged"
#define AUTOEXEC_BAK "backup"
static int g_debug;
static char reply[512];
static int send_cstr(const char *s) { strcpy(reply,s); return 0; }
static void debug_puts(const char *s) { (void)s; }
typedef struct { unsigned char data[9000]; int n, exists, pos, writing; } DiskFile;
static DiskFile disk[3];
static int fault, writes, moves, closes, readbacks;
static int index_of(const char *p) { return !strcmp(p,AUTOEXEC_NCF)?0:!strcmp(p,AUTOEXEC_NEW)?1:2; }
static FILE *fake_open(const char *p,const char *m) {
    int i=index_of(p); DiskFile *d=&disk[i];
    if (!strcmp(m,"rb")) {
        if (!d->exists || (fault==1 && i==0)) return NULL;
        if (i==1) readbacks++;
        d->writing=0;
    } else {
        assert(i==1); /* The original must NEVER be opened for writing. */
        if (fault==4) return NULL;
        writes++; d->n=0; d->exists=1; d->writing=1;
    }
    d->pos=0; return (FILE *)d;
}
static size_t fake_read(void *p,size_t size,size_t n,FILE *f) {
    DiskFile *d=(DiskFile *)f; size_t take=(size_t)(d->n-d->pos);
    assert(size==1); if (take>n) take=n;
    if (fault==2 && d==disk) take=take?take-1:0;
    memcpy(p,d->data+d->pos,take);d->pos+=(int)take;
    if (fault==8 && d==&disk[1] && take) ((char *)p)[0]^=1;
    return take;
}
static size_t fake_write(const void *p,size_t size,size_t n,FILE *f) {
    DiskFile *d=(DiskFile *)f; assert(d==&disk[1] && size==1);
    if (fault==5) n--;
    memcpy(d->data,p,n); d->n=(int)n; return n;
}
static int fake_error(FILE *f) { return fault==2 && (DiskFile *)f==disk; }
static int fake_flush(FILE *f) { (void)f; return fault==6?-1:0; }
static int fake_close(FILE *f) {
    DiskFile *d=(DiskFile *)f;closes++;
    return (fault==3 && d==disk) || (fault==7 && d->writing) ? -1:0;
}
static int fake_remove(const char *p) { int i=index_of(p); assert(i==1);disk[i].exists=0; return 0; }
static int fake_rename(const char *a,const char *b) {
    int i=index_of(a), j=index_of(b);moves++;
    if ((fault==9 && i==0) || ((fault==10 || fault==11) && i==1) || (fault==11 && i==2)) return -1;
    assert(disk[i].exists && !disk[j].exists);disk[j]=disk[i];disk[i].exists=0;return 0;
}
#define fopen fake_open
#define fread fake_read
#define fwrite fake_write
#define ferror fake_error
#define fflush fake_flush
#define fclose fake_close
#define remove fake_remove
#define rename fake_rename
'''


class NetWareAutoexecTests(unittest.TestCase):
    def test_command_syntax_order_and_preservation(self):
        compile_run('#include <assert.h>\n#include <string.h>\n' + HEADER + PARSER_CASES +
                    '\nint main(void) { parser_cases(); return 0; }')

    def test_transaction_and_io_failures_preserve_recovery(self):
        compile_run(FAULT_STUBS + HEADER + HANDLER + r'''
static void reset(const char *text) {
    memset(disk,0,sizeof(disk));disk[0].exists=1;disk[0].n=(int)strlen(text);
    memcpy(disk[0].data,text,disk[0].n);writes=moves=closes=readbacks=0;reply[0]=0;
}
int main(void) {
    const char *before="LOAD TCPIP\nREM llmagent clibaux\n";
    const char *after="LOAD TCPIP\nREM llmagent clibaux\nLOAD CLIBAUX\r\nLOAD LLMAGENT\r\n";
    for (fault=0;fault<=11;fault++) {
        reset(before);handle_autoexec();
        if (!fault) {
            assert(!strcmp(reply,"OK autoexec=added\n"));
            assert(disk[0].n==(int)strlen(after) && !memcmp(disk[0].data,after,disk[0].n));
            assert(disk[2].exists && !memcmp(disk[2].data,before,strlen(before)));
            assert(!disk[1].exists && writes==1 && readbacks==1);
            handle_autoexec(); assert(!strcmp(reply,"OK autoexec=present\n") && writes==1);
        } else {
            assert(!strncmp(reply,"ERR:",4));
            if (fault==11) { assert(!disk[0].exists && disk[2].exists); }
            else assert(disk[0].exists && !memcmp(disk[0].data,before,strlen(before)));
        }
    }
    fault=0;reset("LOAD LLMAGENT\nLOAD CLIBAUX\n");handle_autoexec();
    assert(strstr(reply,"dependency order") && !writes && !moves);
    reset(before);disk[2].exists=1;handle_autoexec();assert(strstr(reply,"recovery files") && !writes);
    reset(before);disk[1].exists=1;handle_autoexec();assert(strstr(reply,"recovery files") && !writes);
    reset(before);memset(disk[0].data,'x',8192);disk[0].n=8192;
    handle_autoexec();assert(strstr(reply,"too large") && !writes);
    reset(before);memset(disk[0].data,'x',8191);disk[0].n=8191;
    handle_autoexec();assert(strstr(reply,"too large") && !writes);
    return 0;
}
''')
