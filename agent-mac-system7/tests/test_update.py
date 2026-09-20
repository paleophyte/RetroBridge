"""Exercise production updater logic with two-fork File Manager fault injection.

Run with Python 3 and GCC, or set CC. Does not run Mac application code.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "llm_updater.c"

STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int OSErr;
typedef uint32_t OSType;
typedef struct { short vRefNum; int id; } FSSpec;
typedef struct { int launchBlockID, launchEPBLength, launchControlFlags;
                 FSSpec *launchAppSpec; } LaunchParamBlockRec;
enum { noErr=0, fnfErr=-43, fsWrPerm=2, fsRdPerm=1, smSystemScript=0,
       fsFromStart=1, fsFromMark=3, extendedBlock=1, extendedBlockLen=2, launchContinue=4 };
typedef struct { unsigned char data[2][512]; long len[2]; int exists; } File;
static File files[102]; /* 0=target, 1=stage, 2..100=saved files */
static FILE *stage;
static int fault, flushes, exchanges, launchCalls, launched, deletes, created;
static int srcOpen;
static int refFile, refFork, refMode, refOpen;
static long refPos;
static char logs[4096];
static void Log(const char *s) { assert(strlen(logs)+strlen(s)+2<sizeof(logs)); strcat(logs,s); strcat(logs,"\n"); }
static void LogErr(const char *s, OSErr e) { (void)e; Log(s); }
static OSErr FSMakeFSSpec(int v, int d, const unsigned char *p, FSSpec *s) {
    char name[256]; int n;
    assert(v==1 && d==678); memcpy(name,p+1,p[0]); name[p[0]]=0;
    if (!strcmp(name,"llm_agent")) { s->id=0; if (fault==27) return -36; }
    else if (!strcmp(name,"STAGED_AGENT.bin")) s->id=1;
    else { assert(sscanf(name,"llm_agent.saved.%d",&n)==1 && n>=1 && n<=99);
           if (fault==28) return -36; s->id=n+1; }
    s->vRefNum=1; return files[s->id].exists ? noErr : fnfErr;
}
static OSErr MacApplicationFile(const char *name, FSSpec *spec) {
    unsigned char p[32]; p[0]=(unsigned char)strlen(name); memcpy(p+1,name,p[0]);
    return FSMakeFSSpec(1,678,p,spec);
}
static OSErr FSpCreate(FSSpec *s, OSType c, OSType t, int script) {
    (void)c; (void)t; (void)script;
    assert(s->id>=2 && !files[s->id].exists);
    if (fault==3) return -36;
    files[s->id].exists=1; created=s->id; return 0;
}
static OSErr FSpDelete(FSSpec *s) {
    assert(s->id!=0); /* The installed application must NEVER be deleted. */
    if (fault==19 || fault==30) return -36;
    assert(files[s->id].exists); files[s->id].exists=0; ++deletes; return 0;
}
static OSErr open_fork(FSSpec *s, int mode, short *ref, int fork) {
    if (s->id==1) { if (fault==29) return -36; assert(!srcOpen); srcOpen=1; rewind(stage); *ref=2; return 0; }
    assert(!refOpen && s->id>=2 && files[s->id].exists);
    if ((fault==4 && !fork && mode==fsWrPerm) ||
        (fault==23 && fork && mode==fsWrPerm) ||
        (fault==24 && !fork && mode==fsRdPerm) ||
        (fault==25 && fork && mode==fsRdPerm)) return -36;
    refOpen=1; refFile=s->id; refFork=fork; refMode=mode; refPos=0; *ref=1; return 0;
}
static OSErr FSpOpenDF(FSSpec *s,int m,short *r) { return open_fork(s,m,r,0); }
static OSErr FSpOpenRF(FSSpec *s,int m,short *r) { return open_fork(s,m,r,1); }
static OSErr FSWrite(short ref,long *count,const void *buf) {
    assert(ref==1 && refOpen && refMode==fsWrPerm && refPos+*count<=512);
    if ((fault==5 || fault==30) && !refFork) return -36;
    if (fault==6 && refFork) --*count;
    memcpy(files[refFile].data[refFork]+refPos,buf,(size_t)*count);
    refPos+=*count; files[refFile].len[refFork]=refPos; return 0;
}
static OSErr FSRead(short ref,long *count,void *buf) {
    if (ref==2) { assert(srcOpen); long n=(long)fread(buf,1,(size_t)*count,stage); int ok=n==*count; *count=n; return ok?0:-39; }
    assert(ref==1 && refOpen && refMode==fsRdPerm);
    if (fault==22) { --*count; return -36; }
    memcpy(buf,files[refFile].data[refFork]+refPos,(size_t)*count); refPos+=*count;
    if ((fault==10 && !refFork) || (fault==11 && refFork)) ((char *)buf)[0]^=1;
    if (fault==31 && refFork) ((char *)buf)[48]^=1;
    if (fault==32 && refFork) ((char *)buf)[128]^=1;
    return 0;
}
static OSErr GetEOF(short ref,long *size) {
    if (ref==2) { long pos=ftell(stage); fseek(stage,0,SEEK_END); *size=ftell(stage); fseek(stage,pos,SEEK_SET); return 0; }
    assert(ref==1 && refOpen); *size=files[refFile].len[refFork];
    if (fault==12) ++*size; return 0;
}
static OSErr FSClose(short ref) {
    if (ref==2) { assert(srcOpen); srcOpen=0; return fault==13?-36:0; }
    assert(ref==1 && refOpen); refOpen=0;
    return ((fault==7 && !refFork && refMode==fsWrPerm) ||
            (fault==8 && refFork && refMode==fsWrPerm) ||
            (fault==26 && refMode==fsRdPerm)) ? -36 : 0;
}
static OSErr FlushVol(const unsigned char *n,short v) {
    assert(!n && v==1 && !refOpen); ++flushes;
    return ((fault==9 && flushes==1) || (fault==15 && flushes==2)) ? -36 : 0;
}
static OSErr FSpExchangeFiles(FSSpec *a,FSSpec *b) {
    File temp; assert(a->id==0 && b->id==created && !refOpen); ++exchanges;
    if (exchanges==1) {
        assert(flushes==1 && files[b->id].len[0]==9 && files[b->id].len[1]==320);
        assert(!memcmp(files[b->id].data[0],"NEW-DATA!",9));
        assert(!memcmp(files[b->id].data[1]+256,"NEW-RESOURCE",11));
    }
    if (fault==14 || (fault==17 && exchanges==2)) return -36;
    temp=files[a->id]; files[a->id]=files[b->id]; files[b->id]=temp; return 0;
}
static OSErr LaunchApplication(LaunchParamBlockRec *pb) {
    int id=pb->launchAppSpec->id;
    assert(!refOpen && files[id].exists && pb->launchBlockID==extendedBlock &&
           pb->launchEPBLength==extendedBlockLen && pb->launchControlFlags==launchContinue);
    ++launchCalls;
    if (((fault==16 || fault==17 || fault==18) && launchCalls==1) ||
        (fault==18 && launchCalls==2)) return -108;
    launched=id; return 0;
}
static OSErr SetFPos(short ref,int mode,long offset) { assert(ref==2 && srcOpen); return fseek(stage,offset,mode==fsFromStart?SEEK_SET:SEEK_CUR)==0?0:-36; }
'''

MAIN = r'''
#undef fclose
static void fixture(int scenario) {
    unsigned char blob[640]={0}; int i;
    memset(files,0,sizeof(files)); memset(logs,0,sizeof(logs));
    fault=scenario; flushes=exchanges=launchCalls=deletes=created=refOpen=srcOpen=0; launched=-1;
    files[0].exists=files[1].exists=1;
    memcpy(files[0].data[0],"OLD-DATA",8); files[0].len[0]=8;
    memcpy(files[0].data[1],"OLD-RESOURCE",12); files[0].len[1]=12;
    /* An earlier backup must survive every attempt. */
    files[2]=files[0];
    if (scenario==20) for(i=2;i<=100;i++) files[i]=files[0];
    blob[1]=9; memcpy(blob+2,"llm_agent",9); memcpy(blob+65,"APPL",4);
    blob[86]=9; blob[89]=1; blob[90]=64;
    memcpy(blob+128,"NEW-DATA!",9); blob[258]=1; /* data offset 256 */
    blob[262]=1; blob[263]=16; /* map offset 272 */
    blob[267]=16; blob[271]=48; /* data/map lengths */
    memcpy(blob+512,"NEW-RESOURCE",11);
    if (scenario==1) blob[0]=1;
    stage=tmpfile(); assert(stage); assert(fwrite(blob,1,scenario==2?300:640,stage)>0); rewind(stage);
}
int main(void) {
    int f,ok,i;
    unsigned char hdr[128]={0}; MacBinInfo info;
    for(f=0;f<=32;f++) {
        if(f==21) continue;
        fixture(f); ok=InstallUpdate();
        assert(!refOpen && !srcOpen && files[0].exists && files[2].exists);
        assert(!memcmp(files[2].data[0],"OLD-DATA",8));
        if (f==0 || f==19 || f==31) {
            assert(ok && exchanges==1 && launched==0);
            assert(!memcmp(files[0].data[0],"NEW-DATA!",9));
            assert(!memcmp(files[created].data[1],"OLD-RESOURCE",12));
            assert(files[1].exists==(f==19));
        } else {
            assert(!ok);
            if (f==17) {
                assert(exchanges==2 && launched==created);
                assert(!memcmp(files[created].data[0],"OLD-DATA",8));
                assert(strstr(logs,"ROLLBACK FAILED"));
            } else {
                assert(!memcmp(files[0].data[0],"OLD-DATA",8));
                assert(!memcmp(files[0].data[1],"OLD-RESOURCE",12));
                if (f!=18 && f!=27) assert(launched==0);
            }
            for(i=0;i<102;i++) if(files[i].exists) assert(files[i].len[0]<=512);
        }
        fclose(stage);
    }
    hdr[1]=1; memcpy(hdr+65,"APPL",4); hdr[89]=1; hdr[90]=0;
    assert(ParseMacBinaryHeader(hdr,&info));
    hdr[83]=0x80; assert(!ParseMacBinaryHeader(hdr,&info)); hdr[83]=0;
    hdr[120]=1; assert(!ParseMacBinaryHeader(hdr,&info)); hdr[120]=0;
    hdr[89]=0; assert(!ParseMacBinaryHeader(hdr,&info));
    puts("Updater: preparation faults, exchange/launch rollback, retained backups and cleanup passed");
    return 0;
}
'''


class UpdateTests(unittest.TestCase):
    def test_transaction(self):
        source = SOURCE.read_text(encoding="utf-8")
        constants = source[source.index("#define STAGED_PATH"):source.index("static void Log(")]
        helpers = source[source.index("static int ParseMacBinaryHeader("):source.index("int main(void)")]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cfile = root / "update.c"
            exe = root / ("update.exe" if os.name == "nt" else "update")
            cfile.write_text(STUBS + constants + helpers + MAIN, encoding="utf-8")
            subprocess.run(shlex.split(os.environ.get("CC", "gcc")) + [
                "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
