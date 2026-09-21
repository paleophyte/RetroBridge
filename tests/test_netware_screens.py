"""Exercise NetWare screen-ID conversion and selection with a host C compiler."""
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
#include <stdint.h>
typedef unsigned short WORD;
typedef long LONG;
#define SCR_MAX_ROWS 50
#define SCR_MAX_COLS 132
static int g_debug, current=7, mode, attached, copies, installs;
static int ids[] = { 0x123456, (int)0xF1234567, 0x765432 };
static int GetScreenInfo(int id, char *name, LONG *a) {
    (void)name; (void)a;
    assert(id==ids[0] || id==ids[1] || id==ids[2]);
    if(id==ids[0]) return attached ? 11 : 0;
    return id==ids[1] ? 12 : 13;
}
static int CreateScreen(const char *id, unsigned char flags) {
    assert((int)(intptr_t)id==ids[0] && flags==0);
    attached=1; return mode==6 ? 0 : 11;
}
static int ScanScreens(int previous,char *name,LONG *a) {
    int index=previous==0?0:previous==ids[0]?1:previous==ids[1]?2:3;
    if(index==3 || (index==2 && mode!=4)) return 0;
    strcpy(name,index==0?"System Console":index==1?"Monitor":"Install Screen");
    *a=0;return ids[index];
}
static int SetCurrentScreen(int handle) {
    assert(handle==7 || handle==11 || handle==12);
    if(mode!=3 || handle==7) current=handle;
    return 7; /* Some historical CLIBs return the previous handle. */
}
static int GetCurrentScreen(void) {return current;}
static int CheckIfScreenDisplayed(int handle,long wait) {
    assert(handle>=11 && handle<=13 && wait==0);
    if(mode==5) return -1; /* An error must not mean displayed. */
    return handle==12;
}
static int GetSizeOfScreen(WORD *r,WORD *c) {
    *r=25;*c=80;return mode==7?-1:0;
}
static void CopyFromScreenMemory(WORD r,WORD c,unsigned char *out,WORD x,WORD y) {
    unsigned i;assert(x==0 && y==0 && r==25 && c==80);++copies;
    for(i=0;i<r*c;i++){out[i*2]=' ';out[i*2+1]=7;}
    if(mode==1 || (mode==2 && current==12)) return;
    out[0]=current==11?'C':'M';
}
static void ThreadSwitchWithDelay(void) {}
static int capture_install_via_stuffkey(const char *name,unsigned char *out,WORD *r,WORD *c) {
    assert(!strcmp(name,"Install Screen"));++installs;*r=1;*c=80;
    memset(out,0,160);out[0]='I';out[1]=7;return 1;
}
'''

MAIN = r'''
int main(void) {
    unsigned char cells[SCR_MAX_ROWS*SCR_MAX_COLS*2];WORD r,c;
    memset(cells,' ',sizeof(cells));assert(count_printable(cells,2000)==0);
    assert(capture_console_text(cells,&r,&c)>0 && cells[0]=='M');
    assert(attached && current==7 && r==25 && c==80);
    mode=1;assert(capture_console_text(cells,&r,&c)==0 && current==7);
    mode=2;assert(capture_console_text(cells,&r,&c)>0 && cells[0]=='C');
    mode=3;copies=0;assert(capture_console_text(cells,&r,&c)==0 && copies==0 && current==7);
    mode=4;assert(capture_console_text(cells,&r,&c)>0 && cells[0]=='I' && installs==1 && current==7);
    mode=5;assert(!screen_displayed(ids[0]));
    assert(capture_console_text(cells,&r,&c)>0 && cells[0]=='C');
    mode=6;attached=0;assert(screen_handle(ids[0])==-1);
    mode=7;assert(capture_console_text(cells,&r,&c)==0 && current==7);
    assert(screen_handle(0)==-1);
    puts("NetWare screens: IDs/handles, negative IDs, blank/error rejection, selection, Install isolation and restoration passed");
    return 0;
}
'''

class NetwareScreensTests(unittest.TestCase):
    def test_capture_context_and_failures(self):
        source=(ROOT/'agent-netware/llm_agent.c').read_text(encoding='utf-8')
        names=['static int screen_name_ignored(const char *name) {',
               'static int is_system_console_name(const char *name) {',
               'static int count_printable(', 'static int screen_handle(',
               'static int screen_displayed(', 'static void restore_operator_screen(',
               'static int try_copy_screen(', 'static int capture_console_text(']
        code=STUBS+'\n'+'\n'.join(function(source,n) for n in names)+MAIN
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'screens.c';exe=Path(tmp)/'screens.exe'
            src.write_text(code)
            subprocess.run(shlex.split(os.environ.get('CC','gcc'))+['-std=c99','-O2',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=10)

if __name__=='__main__': unittest.main()
