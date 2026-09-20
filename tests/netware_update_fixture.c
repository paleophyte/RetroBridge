/* In-memory filesystem fault injection around the production updater. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <setjmp.h>

typedef struct { char path[128]; unsigned char data[4096]; unsigned size; int dir; } Entry;
typedef struct { Entry *entry; unsigned pos; int open, error; char mode; } FakeFile;
static Entry entries[100];
static FakeFile handles[20];
static int fault, loaded, loads, triggered, delay_calls;
static Entry *find_entry(const char *p) {
    int i;for(i=0;i<100;i++)if(!strcmp(entries[i].path,p))return &entries[i];return NULL;
}
static Entry *make_entry(const char *p) {
    int i;Entry *e=find_entry(p);if(e)return e;
    for(i=0;i<100;i++)if(!entries[i].path[0]){strcpy(entries[i].path,p);return &entries[i];}
    assert(0);return NULL;
}
static void seed(const char *p,const char *text) {
    Entry *e=make_entry(p);e->size=(unsigned)strlen(text);memcpy(e->data,text,e->size);
}
static int equals(const char *p,const char *text) {
    Entry *e=find_entry(p);return e && e->size==strlen(text) && !memcmp(e->data,text,e->size);
}
static FakeFile *fs_open(const char *p,const char *mode) {
    Entry *e=find_entry(p);int i;
    if(fault==1 && strstr(p,"PLAN.TXT") && *mode=='w')return NULL;
    if(fault==18 && strstr(p,"READY.TXT") && *mode=='r')return NULL;
    if(*mode=='r' && !e)return NULL;
    if(!e)e=make_entry(p);
    assert(!e->dir);
    if(*mode=='w')e->size=0;
    for(i=0;i<20;i++)if(!handles[i].open){FakeFile *f=&handles[i];memset(f,0,sizeof(*f));f->entry=e;f->open=1;f->mode=*mode;f->pos=*mode=='a'?e->size:0;return f;}
    assert(0);return NULL;
}
static size_t fs_read(void *p,size_t size,size_t count,FakeFile *f) {
    unsigned n=(unsigned)(size*count), available=f->entry->size-f->pos;
    if(fault==19 && strstr(f->entry->path,"OLD.NLM")){f->error=1;return 0;}
    if(n>available)n=available;
    memcpy(p,f->entry->data+f->pos,n);f->pos+=n;return n/size;
}
static size_t fs_write(const void *p,size_t size,size_t count,FakeFile *f) {
    unsigned n=(unsigned)(size*count);
    if((fault==2 && strstr(f->entry->path,"PREPARED.TXT")) || (fault==9 && strstr(f->entry->path,"RESTORE.NLM"))) {if(n)n--;}
    assert(f->pos+n<=4096);memcpy(f->entry->data+f->pos,p,n);f->pos+=n;if(f->pos>f->entry->size)f->entry->size=f->pos;return n/size;
}
static int fs_flush(FakeFile *f) {return (fault==10 && strstr(f->entry->path,"RESTORE.NLM"))?-1:0;}
static int fs_close(FakeFile *f) {f->open=0;return (fault==11 && strstr(f->entry->path,"RESTORE.NLM"))?-1:0;}
static int fs_error(FakeFile *f) {return f->error;}
static int fs_seek(FakeFile *f,long pos,int origin) {assert(origin==SEEK_END && pos==0);f->pos=f->entry->size;return 0;}
static long fs_tell(FakeFile *f) {return f->pos;}
static int fs_access(const char *p,int mode) {
    (void)mode;if(fault==17 && !strcmp(p,"SYS:SYSTEM\\LLMUPD")){errno=EACCES;return -1;}
    if(find_entry(p))return 0;
    errno=ENOENT;return -1;
}
static int fs_mkdir(const char *p) {
    if(find_entry(p)){errno=EEXIST;return -1;}
    if(fault==17 && strstr(p,"\\LU")){errno=EACCES;return -1;}
    make_entry(p)->dir=1;return 0;
}
static int fs_remove(const char *p) {Entry *e=find_entry(p);if(!e)return -1;e->path[0]=0;return 0;}
static int fs_rename(const char *a,const char *b) {
    Entry *e=find_entry(a);int i;size_t n=strlen(a);char path[128];
    if((fault==3 && strstr(a,"LLMAGENT.NEW")) || (fault==5 && strstr(a,"LLMAGENT.NLM") && strstr(b,"OLD.NLM")) ||
       (fault==12 && strstr(a,"RESTORE.NLM")) || (fault==13 && strstr(b,"BAD.NLM")) ||
       (fault==15 && !strcmp(a,"SYS:SYSTEM\\LLMUPD")))return -1;
    if(!e || find_entry(b))return -1;
    if(e->dir)for(i=0;i<100;i++)if(!strncmp(entries[i].path,a,n)&&entries[i].path[n]=='\\'){
        strcpy(path,b);strcat(path,entries[i].path+n);strcpy(entries[i].path,path);
    }
    strcpy(e->path,b);
    if(fault==7 && strstr(a,"LLMAGENT.NEW"))e->data[0]^=1;
    return 0;
}
static unsigned int FindNLMHandle(char *name) {
    if(fault>=30) {assert(!strcmp(name,"UPDATE.NLM"));return loaded?42:0;}
    assert(!strcmp(name,"LLMAGENT.NLM"));
    if(fault==6 && !triggered && find_entry("SYS:SYSTEM\\LLMUPD\\PREPARED.TXT")){
        triggered=1;seed("SYS:SYSTEM\\LLMAGENT.NEW","changed");
    }
    return loaded?42:0;
}
static long GetCurrentTicks(void) {return 123 + delay_calls * 2;}
static void ConsolePrintf(const char *format,...) {(void)format;}
static void delay(unsigned int ms) {(void)ms;delay_calls++;}
static void ThreadSwitchWithDelay(void) {}
static int mock_system(const char *command);
#define FILE FakeFile
#define fopen fs_open
#define fread fs_read
#define fwrite fs_write
#define fflush fs_flush
#define fclose fs_close
#define ferror fs_error
#define fseek fs_seek
#define ftell fs_tell
#define access fs_access
#define mkdir fs_mkdir
#define remove fs_remove
#define rename fs_rename
#define system mock_system
#define main updater_main
PRODUCTION
#undef main
static int mock_system(const char *command) {
    char receipt[132], staged[65];int replacement;
    if(fault>=30) {
        assert(!strcmp(command,"LOAD SYS:SYSTEM\\UPDATE.NLM"));loads++;
        if(fault==30)return 0;
        loaded=1;make_entry(NW_WORK)->dir=1;
        assert(!nw_hash(NW_STAGE,staged));
        sprintf(receipt,"%s\n%s\n",g_update_sha256,staged);
        if(fault==31)receipt[0]^=1;
        seed(NW_PREPARED,receipt);return 0;
    }
    assert(!strcmp(command,"LOAD SYS:SYSTEM\\LLMAGENT.NLM"));loads++;
    replacement=!equals(NW_AGENT,"original executable");
    if(replacement && (fault==4 || (fault>=8 && fault<=13) || fault==16 || fault==19)){loaded=0;return 0;}
    if(!replacement && fault==16){loaded=0;return 0;}
    loaded=1;
    if(fault==14)return 0;
    sprintf(receipt,"%s\n01234567-00000042\n",replacement?new_hash:old_hash);
    seed(NW_READY,receipt);
    return -1; /* Return status alone never establishes startup or failure. */
}
static void reset_case(int value) {
    memset(entries,0,sizeof(entries));memset(handles,0,sizeof(handles));
    fault=value;loaded=loads=triggered=delay_calls=0;
    seed(NW_AGENT,"original executable");seed(NW_STAGE,"replacement executable");
    seed("SYS:SYSTEM\\LLMAGENT.OLD","older backup");
    make_entry("SYS:SYSTEM\\LU00007b")->dir=1;
    seed("SYS:SYSTEM\\LU00007b\\KEEP","prior history");
    if(value==20){make_entry(NW_WORK)->dir=1;seed(NW_PLAN,"unresolved");}
    if(value==21)loaded=1;
}
static jmp_buf exited;
static int g_allow_unload,g_running,g_client,g_listen,g_our_screen,closed;
static char response[256];
static int send_cstr(const char *p) {strcpy(response,p);return fault==34?-1:0;}
static int fake_close(int fd) {(void)fd;closed++;return 0;}
static void fake_exit(int code) {assert(code==0);longjmp(exited,1);}
#define close fake_close
#define exit fake_exit
HANDLER
#undef exit
#undef close
static void handler_cases(void) {
    int i;
    for(i=30;i<=36;i++) {
        reset_case(i);g_running=1;g_client=1;g_listen=2;closed=0;g_allow_unload=0;
        response[0]=0;
        update_identity_init(NW_AGENT,123,42);
        seed("SYS:SYSTEM\\UPDATE.NLM",NW_HELPER_TAG);
        if(i==33)make_entry(NW_WORK)->dir=1;
        if(i==35)strcpy(g_update_exe,"SYS:OTHER\\LLMAGENT.NLM");
        if(i==36)seed("SYS:SYSTEM\\UPDATE.NLM","old incompatible helper");
        if(setjmp(exited)==0) {
            handle_update();assert(i!=32);assert(g_running && !g_allow_unload && !closed);
            if(i!=34)assert(!strncmp(response,"ERR:",4));
        } else {assert(i==32);assert(!g_running && g_allow_unload && closed==2);assert(!strcmp(response,"OK\n"));}
        if(i==33 || i==35 || i==36)assert(!loads);
    }
}
int main(void) {
    int i,result;char saved[128];
    for(i=0;i<=21;i++) {
        reset_case(i);result=updater_main();
        assert(equals("SYS:SYSTEM\\LLMAGENT.OLD","older backup"));
        assert(equals("SYS:SYSTEM\\LU00007b\\KEEP","prior history"));
        if(i==0){assert(result==0 && loaded && loads==1);assert(equals(NW_AGENT,"replacement executable"));assert(!find_entry(NW_WORK));}
        else if(i==3 || i==4 || i==7 || i==8){assert(result==1 && loaded);assert(equals(NW_AGENT,"original executable"));assert(!find_entry(NW_WORK));}
        else if(i==5 || i==6 || i==21){assert(result==2);assert(equals(NW_AGENT,"original executable"));}
        else {assert(result==3);assert(find_entry(NW_WORK));}
        if(i==14 || i==18){assert(loads==1 && loaded);assert(equals(NW_AGENT,"replacement executable"));assert(equals(NW_OLD,"original executable"));}
        if(i==9 || i==10 || i==11 || i==12 || i==13 || i==16){assert(equals(NW_OLD,"original executable"));}
        if(i==0 || i==3 || i==4 || i==7 || i==8){sprintf(saved,"%s\\RESULT\\OLD.NLM",archive_dir);assert(equals(saved,"original executable"));}
        if(i==20){assert(equals(NW_PLAN,"unresolved"));assert(loads==0);}
    }
    handler_cases();
    puts("NetWare updater: 22 transaction faults and 7 handoff cases passed");return 0;
}
