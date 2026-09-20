"""Run legacy production EXEC handlers with controlled child lifetimes."""
from pathlib import Path
import unittest
from test_uploads import function
from test_session_timeouts import compile_run

ROOT = Path(__file__).resolve().parents[1]
BASE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
HEADER
static char g_exedir[144]=".", g_tmppath[160], g_cmd[576];
static char g_iobuf[4096],g_execbuf[8192];
#define EXEC_CAP 8192
static char wire[8192], output_path[256], script_path[256], done_path[256];
static int used, launches, mode, stopped;
static unsigned long ticks;
static int send_all(const char *p,int n) { assert(used+n<8192);memcpy(wire+used,p,n);used+=n;wire[used]=0;return stopped?-1:0; }
static int send_cstr(const char *p) { return send_all(p,(int)strlen(p)); }
static void file(const char *p,const char *s) { FILE *f=fopen(p,"wb");assert(f);fputs(s,f);assert(!fclose(f)); }
static void reset(int m) { mode=m;used=launches=stopped=0;wire[0]=0;ticks=0; }
static void clean(const char *p) { char dir[256],*slash;strcpy(dir,p);slash=strrchr(dir,'\\');assert(slash);*slash=0;remove(p);rmdir(dir); }
'''
WIN16 = r'''
typedef unsigned long DWORD;
typedef unsigned UINT,HINSTANCE;
typedef int MSG;
#define SW_SHOWNORMAL 1
#define PM_REMOVE 1
static UINT WinExec(const char *cmd,int show) {
 const char *p=strchr(cmd,' '),*q;unsigned n;(void)show;assert(p);q=strchr(++p,' ');assert(q);n=(unsigned)(q-p);memcpy(output_path,p,n);output_path[n]=0;
 ++launches;if(mode==2)return 5;file(output_path,"fixture");return 42;
}
static int GetModuleUsage(HINSTANCE h) { assert(h==42);return mode==1; }
static DWORD GetTickCount(void) { return ticks; }
static int PeekMessage(MSG *m,void *a,int b,int c,int d) { (void)m;(void)a;(void)b;(void)c;(void)d;return 0; }
static void TranslateMessage(MSG *m) {(void)m;}
static void DispatchMessage(MSG *m) {(void)m;}
static void Yield(void) { ticks+=1000; }
'''
OS2 = r'''
typedef unsigned long ULONG,PID,APIRET;
typedef char *PSZ,*PBYTE;
typedef struct { unsigned Length;int Related,FgBg,TraceOpt,InheritOpt,SessionType,PgmControl;char *PgmTitle,*PgmName,*PgmInputs,*ObjectBuffer;void *TermQ,*Environment;unsigned ObjectBuffLen;} STARTDATA;
#define SSF_RELATED_INDEPENDENT 0
#define SSF_FGBG_BACK 0
#define SSF_TRACEOPT_NONE 0
#define SSF_INHERTOPT_PARENT 0
#define SSF_TYPE_WINDOWABLEVIO 0
#define SSF_CONTROL_MINIMIZE 0
#define SSF_CONTROL_INVISIBLE 0
#define OS2_CMD_EXE "CMD.EXE"
static APIRET DosStartSession(STARTDATA *sd,ULONG *sid,PID *pid) {
 FILE *f;char line[1024],*p;(void)sid;(void)pid;
 ++launches;strcpy(script_path,sd->PgmInputs+4);f=fopen(script_path,"rb");assert(f);
 assert(fgets(line,sizeof(line),f));p=strstr(line," > ");assert(p);strcpy(output_path,p+3);output_path[strcspn(output_path,"\r\n")]=0;
 assert(fgets(line,sizeof(line),f));p=strstr(line," > ");assert(p);strcpy(done_path,p+3);done_path[strcspn(done_path,"\r\n")]=0;fclose(f);
 if(mode==2)return 5;
 file(output_path,"fixture");if(mode==0)file(done_path,"done\r\n");return 0;
}
static void DosSleep(unsigned n) { ticks+=n; }
static unsigned long network_ticks(void) {return ticks;}
#define NET_DEADLINE(s) (ticks+(s)*1000UL)
#define NET_EXPIRED(d) ((long)(ticks-(d))>=0)
'''
MAIN = r'''
int main(void) {
 char old[256],oldscript[256],olddone[256],longcmd[512];FILE *f;
 reset(0);assert(run_exec("echo hi")==0);assert(launches==1&&strstr(wire,"EXIT:0"));assert(!fopen(output_path,"rb"));
 reset(1);assert(run_exec("wait")<0);assert(launches==1);assert(strstr(wire,"LEN:0\n"));assert(strstr(wire,"not cancelled"));assert(!strstr(wire,"EXIT:"));
 strcpy(old,output_path);strcpy(oldscript,script_path);strcpy(olddone,done_path);f=fopen(old,"rb");assert(f);fclose(f);
 reset(0);assert(run_exec("echo second")==0);assert(strcmp(old,output_path));f=fopen(old,"rb");assert(f);fclose(f);
 if(*oldscript)remove(oldscript);
 if(*olddone)remove(olddone);
 clean(old);
 reset(2);assert(run_exec("launch failure")<0);assert(launches==1&&!strstr(wire,"EXIT:0"));
 reset(1);stopped=1;assert(run_exec("disconnected")<0);assert(ticks<10000);if(*script_path)remove(script_path);if(*done_path)remove(done_path);clean(output_path);
 return 0;
}
'''


class ExecLifetimeTests(unittest.TestCase):
    def test_wait_expiry_disconnect_and_output_isolation(self):
        header = (ROOT / 'common/exec_spool.h').read_text()
        for platform, stubs in [('agent-win16', WIN16), ('agent-os2', OS2)]:
            with self.subTest(platform=platform):
                source = (ROOT / platform / 'llm_agent.c').read_text(encoding='utf-8')
                compile_run(BASE.replace('HEADER', header) + stubs + function(source, 'static int run_exec(') + MAIN)

    def test_os2_13_does_not_repeat_side_effects_on_missing_output(self):
        source = (ROOT / 'agent-os2-13/llm_agent.c').read_text(encoding='utf-8')
        stubs = r'''
static int exec_once(const char *cmd) { (void)cmd;++launches;return 7; }
#define system exec_once
'''
        main = r'''
int main(void) {
 reset(0);assert(run_exec("side effect")<0);assert(launches==1);
 assert(strstr(wire,"status 7")&&strstr(wire,"not retried"));
 assert(!strstr(wire,"EXIT:0"));return 0;
}
'''
        compile_run(BASE.replace('HEADER', (ROOT / 'common/exec_spool.h').read_text()) + stubs + function(source, 'static int run_exec(') + main)

    def test_spool_does_not_reuse_existing_directories(self):
        main = r'''
int main(void) {
 char a[64],b[64];FILE *f;
 assert(!EXEC_MKDIR(".\\LX000000"));file(".\\LX000000\\OUT.TMP","old job");
 assert(!exec_spool_create(".",a,sizeof(a)));assert(strcmp(a,".\\LX000000"));
 assert(!exec_spool_create(".",b,sizeof(b)));assert(strcmp(a,b));
 f=fopen(".\\LX000000\\OUT.TMP","rb");assert(f&&fgetc(f)=='o');fclose(f);
 assert(exec_spool_create(".",a,4)<0);
 remove(".\\LX000000\\OUT.TMP");rmdir(".\\LX000000");rmdir(b);
 return 0;
}
'''
        compile_run(BASE.replace('HEADER', (ROOT / 'common/exec_spool.h').read_text()) + main)


if __name__ == '__main__':
    unittest.main()
