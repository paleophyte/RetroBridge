"""Compile the Mac configuration loader and application-file helpers with faults."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int OSErr;
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
typedef struct { long highLongOfPSN, lowLongOfPSN; } ProcessSerialNumber;
typedef struct { long processInfoLength; void *processName; FSSpec *processAppSpec; } ProcessInfoRec;
typedef struct { void *ioNamePtr, *ioCompletion; short ioVRefNum; long ioWDDirID; } WDPBRec;
enum { noErr=0, paramErr=-50, fnfErr=-43, fsRdPerm=1, fsWrPerm=2,
       fsFromLEOF=2, smSystemScript=0 };
static int fault, closes, lookups;
static short currentVol=9;
static long currentDir=42;
static const char *input="token=fixture\rport=2233\r";
static OSErr GetCurrentProcess(ProcessSerialNumber *p) {
    p->lowLongOfPSN=7; return fault==1?-1:0;
}
static OSErr GetProcessInformation(ProcessSerialNumber *p, ProcessInfoRec *i) {
    assert(p->lowLongOfPSN==7 && i->processInfoLength==sizeof(*i) && !i->processName);
    i->processAppSpec->vRefNum=-3; i->processAppSpec->parID=678;
    memcpy(i->processAppSpec->name,"\x09LLM_AGENT",10);
    if(fault==3) i->processAppSpec->parID=0;
    return fault==2?-1:0;
}
static OSErr PBHSetVolSync(WDPBRec *p) {
    assert(!p->ioNamePtr && !p->ioCompletion && p->ioVRefNum==-3 && p->ioWDDirID==678);
    if(fault==4) return -1;
    currentVol=p->ioVRefNum; currentDir=p->ioWDDirID; return 0;
}
static OSErr FSMakeFSSpec(short v,long d,const unsigned char *p,FSSpec *s) {
    assert(v==-3 && d==678 && p[0]<=31); ++lookups;
    s->vRefNum=v; s->parID=d; memcpy(s->name,p,p[0]+1);
    return fault==5?fnfErr:0;
}
static OSErr FSpOpenDF(FSSpec *s,int mode,short *r) {
    assert(s->vRefNum==-3 && s->parID==678 && mode==fsRdPerm); *r=12;
    return fault==6?-1:0;
}
static OSErr GetEOF(short r,long *n) {
    assert(r==12); *n=(long)strlen(input);
    if(fault==8) *n=4097;
    if(fault==9) *n=0;
    return fault==7?-1:0;
}
static OSErr FSRead(short r,long *n,void *buf) {
    assert(r==12); if(fault==11) --*n; memcpy(buf,input,(size_t)*n);
    return fault==10?-1:0;
}
static OSErr FSClose(short r) { assert(r==12); ++closes; return fault==12?-1:0; }
/* Log dependencies; native logging is exercised by the companion updater. */
static OSErr FSpCreate(FSSpec *s,unsigned long c,unsigned long t,int script) {
    (void)s;(void)c;(void)t;(void)script; return 0;
}
static OSErr SetFPos(short r,int m,long p) { (void)r;(void)m;(void)p;return 0; }
static OSErr FSWrite(short r,long *n,const void *p) { (void)r;(void)n;(void)p;return 0; }
static OSErr FlushVol(void *p,short v) { (void)p;(void)v;return 0; }
#include "app_files.h"
#include "config.h"
static char gToken[MAC_TOKEN_MAX];
static unsigned short gAgentPort=MAC_DEFAULT_PORT;
'''

MAIN = r'''
static void bad(const char *s, long len) {
    MacAgentConfig c,before; memset(&c,0x5A,sizeof(c)); before=c;
    assert(MacParseConfig(s,len,&c)); assert(!memcmp(&c,&before,sizeof(c)));
}
int main(void) {
    MacAgentConfig c; FSSpec spec; int i; char text[4200];
    const char *invalid[]={"", "port=2222", "token=", "token=a b", "token=a\tt",
      "token=REPLACE_WITH_UNIQUE_TOKEN", "token=a\ntoken=b", "token=a\nport=1\nport=2",
      "token=a\nport=0", "token=a\nport=65536", "token=a\nport=999999999999999999999",
      "token=a\nport=-1", "token=a\nport=+1", "token=a\nport=12x", "token=a\nport=",
      "token=a\nunknown=b", "token=a\nToken=b", "[agent]\ntoken=a", "token=a\nport 2"};
    for(i=0;i<(int)(sizeof(invalid)/sizeof(*invalid));i++) bad(invalid[i],(long)strlen(invalid[i]));
    bad("token=a\0b",9); bad("token=\x80",7);
    assert(!MacParseConfig("token=a",7,&c) && !strcmp(c.token,"a") && c.port==2222);
    strcpy(text," # comment\r;comment\n\r\n token \t= a=b! \r\n port=65535\t\r");
    assert(!MacParseConfig(text,(long)strlen(text),&c) && c.port==65535 && !strcmp(c.token,"a=b!"));
    strcpy(text,"port=00001\ntoken=a\n"); assert(!MacParseConfig(text,(long)strlen(text),&c) && c.port==1);
    strcpy(text,"token="); memset(text+6,'x',255); text[261]=0;
    assert(!MacParseConfig(text,261,&c) && strlen(c.token)==255);
    text[261]='x'; text[262]=0; bad(text,262);
    memset(text,' ',4096); memcpy(text,"token=a\n",8);
    for(i=511;i<4096;i+=511) text[i]='\n';
    assert(!MacParseConfig(text,4096,&c)); bad(text,4097);
    memset(text,'#',512); bad(text,512);
    assert(MacApplicationFile("LLMAGENT.INI",&spec)==paramErr && !lookups);
    for(i=1;i<=3;i++) { fault=i; assert(MacLocateApplication()!=noErr);
        assert(MacApplicationFile("LLMAGENT.INI",&spec)==paramErr && !lookups); }
    fault=0; assert(MacLocateApplication()==noErr);
    assert(MacApplicationHasName("llm_agent") && !MacApplicationHasName("other"));
    assert(MacApplicationFile("other:file",&spec)==paramErr);
    assert(MacApplicationFile("",&spec)==paramErr);
    assert(MacApplicationFile("12345678901234567890123456789012",&spec)==paramErr);
    /* An unrelated launch/default directory cannot redirect configuration. */
    assert(currentVol==9 && currentDir==42 && !LoadConfig());
    assert(!strcmp(gToken,"fixture") && gAgentPort==2233);
    fault=4; assert(MacSelectApplicationDirectory()!=noErr && currentDir==42);
    fault=0; assert(MacSelectApplicationDirectory()==noErr && currentVol==-3 && currentDir==678);
    for(i=5;i<=12;i++) {
        fault=i; closes=0; strcpy(gToken,"unchanged"); gAgentPort=1111;
        assert(LoadConfig() && !strcmp(gToken,"unchanged") && gAgentPort==1111);
        assert(closes==(i>=7));
    }
    fault=0; input="token=fixture\rport=invalid";
    assert(LoadConfig() && !strcmp(gToken,"unchanged") && gAgentPort==1111);
    puts("Mac config: parser boundaries, native file errors and application-folder isolation passed");
    return 0;
}
'''


class MacConfigTests(unittest.TestCase):
    def test_config_and_file_location(self):
        source = (ROOT / "agent-mac-system7/llm_agent.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            cfile = Path(directory) / "config.c"
            exe = Path(directory) / ("config.exe" if os.name == "nt" else "config")
            cfile.write_text(STUBS + function(source, "static const char *LoadConfig(") + MAIN)
            subprocess.run(shlex.split(os.environ.get("CC", "gcc")) + [
                "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-I", str(ROOT / "agent-mac-system7"), str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
