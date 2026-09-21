"""Host regression checks for Win16 journal lifetime, mapping and text batching."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from test_uploads import function
ROOT=Path(__file__).resolve().parents[1]
STUBS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned UINT, WPARAM;
typedef uintptr_t LPARAM;
typedef long LRESULT;
typedef unsigned long DWORD;
typedef void *HHOOK, *FARPROC, *HOOKPROC;
typedef struct {UINT message,paramL,paramH;DWORD time;} EVENTMSG,*LPEVENTMSG;
typedef struct {int unused;} MSG;
#define _EXPORT
#define FAR
#define PASCAL
#define MAX_EVENTS 256
enum {HC_SKIP=2,HC_GETNEXT=1,WM_KEYDOWN=256,WM_KEYUP=257,VK_SHIFT=16,
 WM_MOUSEMOVE=512,WM_LBUTTONDOWN=513,WM_LBUTTONUP=514,
 WM_RBUTTONDOWN=516,WM_RBUTTONUP=517,WM_MBUTTONDOWN=519,WM_MBUTTONUP=520,
 KF_EXTENDED=256,KF_UP=32768,KF_REPEAT=16384,WH_JOURNALPLAYBACK=1,PM_REMOVE=1};
static EVENTMSG g_events[MAX_EVENTS];
static volatile int g_event_count,g_event_pos;
static HHOOK g_hook;
static void *g_hinst=(void*)3;
static unsigned tick,mode,allocs,frees,hooks,unhooks,delivered;
static char response[200];
LRESULT JournalPlaybackProc(int,WPARAM,LPARAM);
static LRESULT CallNextHookEx(HHOOK h,int c,WPARAM w,LPARAM l){return -123;}
static DWORD GetTickCount(void){return tick++;}
static FARPROC MakeProcInstance(FARPROC p,void *h){assert(h==g_hinst);++allocs;return mode==1?NULL:(void*)2;}
static void FreeProcInstance(FARPROC p){assert(p==(void*)2 && !g_hook);++frees;}
static HHOOK SetWindowsHookEx(int k,HOOKPROC p,void *h,int task){assert(p==(void*)2);++hooks;return mode==2?NULL:(void*)1;}
static int UnhookWindowsHookEx(HHOOK h){assert(h==(void*)1);++unhooks;return 1;}
static int PeekMessage(MSG *m,void *w,int lo,int hi,int flags){
 if(mode!=3 && g_event_pos<g_event_count){EVENTMSG e;
  assert(JournalPlaybackProc(HC_GETNEXT,0,(LPARAM)&e)==0);
  assert(e.paramL!=0);++delivered;JournalPlaybackProc(HC_SKIP,0,0);
 }
 return 0;
}
static void TranslateMessage(MSG *m){}
static void DispatchMessage(MSG *m){}
static void Yield(void){tick+=mode==3?100:1;}
static UINT VkKeyScan(UINT ch){
 if(ch=='~')return (UINT)-1;
 if(ch=='@')return 0x640; /* layout requires Ctrl+Alt: reject */
 if(ch==':')return 0x1BA;
 if(ch>='A' && ch<='Z')return ch|0x100;
 return ch;
}
static UINT MapVirtualKey(UINT vk,UINT kind){assert(kind==0);return vk;}
static int send_cstr(const char *s){strcpy(response,s);return 0;}
'''
MAIN=r'''
static void reset(void){mode=tick=allocs=frees=hooks=unhooks=delivered=0;g_event_count=g_event_pos=0;g_hook=NULL;response[0]=0;}
int main(void){
 unsigned vk,scan;int shift;char text[401];
 reset();assert(map_char(' ',&vk,&scan,&shift)==0 && vk==32 && !shift);
 assert(map_char(':',&vk,&scan,&shift)==0 && vk==186 && shift);
 assert(map_char('@',&vk,&scan,&shift)==-1);
 assert(map_char('~',&vk,&scan,&shift)==-1);
 assert(map_char(128,&vk,&scan,&shift)==-1);
 assert(push_char(':')==0 && g_event_count==4);
 assert(g_events[1].paramL==186 && g_events[2].message==WM_KEYUP);
 assert(run_journal_events()==0 && allocs==1 && frees==1 && unhooks==1 && delivered==4);
 reset();mode=1;push_char('x');assert(run_journal_events()==-1 && !hooks && !frees && !g_event_count);
 reset();mode=2;push_char('x');assert(run_journal_events()==-1 && frees==1 && !unhooks && !g_event_count);
 reset();mode=3;push_char('x');assert(run_journal_events()==-2 && unhooks==1 && frees==1 && !g_event_count && !g_hook);
 reset();memset(text,'x',400);text[400]=0;assert(handle_type(text)==0 && delivered==800 && hooks==4 && frees==4);
 assert(!strcmp(response,"OK\r\n"));
 reset();assert(handle_type("abc~")==-1 && !hooks && !delivered && !strncmp(response,"ERR:",4));
 reset();assert(handle_type("")==0 && !hooks);
 reset();assert(handle_click("12 34 2")==0 && g_events[1].message==WM_MBUTTONDOWN && g_events[2].message==WM_MBUTTONUP);
 reset();assert(handle_click("12 34 3")==0 && g_events[1].message==WM_RBUTTONDOWN && g_events[2].message==WM_RBUTTONUP);
 reset();assert(handle_click("12 34 9")==-1 && !hooks);
 puts("Win16 journal: instance thunk cleanup, install failure, timeout, punctuation mapping, full text batches and preflight passed");return 0;
}
'''
class Win16JournalTests(unittest.TestCase):
 def test_journal_lifetime_and_text(self):
  source=(ROOT/'agent-win16/llm_agent.c').read_text(encoding='utf-8')
  names=['LRESULT _EXPORT FAR PASCAL JournalPlaybackProc(int code', 'static void push_event(', 'static void push_key(', 'static int map_char(', 'static int push_char(', 'static int run_journal_events(', 'static void send_journal_error(', 'static int handle_type(', 'static int handle_click(']
  code=STUBS+'\n'+'\n'.join(function(source,n) for n in names)+MAIN
  with tempfile.TemporaryDirectory() as tmp:
   src=Path(tmp)/'journal.c';exe=Path(tmp)/'journal.exe';src.write_text(code)
   subprocess.run(shlex.split(os.environ.get('CC','gcc'))+['-std=c99','-O2',str(src),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':unittest.main()
