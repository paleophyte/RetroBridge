/* Shell-only file-backed jobs: marker completion, no inferred PID identity.
 * Caller supplies fj_launch(path), fj_encode(command), send_* and g_exedir.
 * Retrieval is capped; redirected disk output itself is NOT bounded.
 */
#ifndef RETRO_FILE_JOBS_H
#define RETRO_FILE_JOBS_H
#include <sys/stat.h>
#include "exec_spool.h"
#define FJ_SLOTS 2
#define FJ_CAP 1048576UL
typedef struct {
    char id[33],command[FJ_COMMAND_MAX+1],dir[120];
    unsigned long size;
    int complete,exit_code,error,truncated;
} FileJob;
static FileJob fj_jobs[FJ_SLOTS];
#ifdef FJ_SHARED_BUFFER
#define fj_buffer FJ_SHARED_BUFFER
#else
static char fj_buffer[2048];
#endif
static int fj_id(const char *id) {
    int i;if(strlen(id)!=32)return 0;
    for(i=0;i<32;++i)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return 0;
    return 1;
}
static FileJob *fj_find(const char *id) {
    int i;if(!fj_id(id))return NULL;
    for(i=0;i<FJ_SLOTS;++i)if(!strcmp(id,fj_jobs[i].id))return &fj_jobs[i];
    return NULL;
}
static void fj_poll(FileJob *j) {
    char path[272];struct stat st;FILE *f;int rc,err;
    if(!j->id[0])return;
    sprintf(path,"%s\\DONE.FLG",j->dir);
    if(!j->complete && (f=fopen(path,"rb"))!=NULL) {
        if(fscanf(f,"%d %d",&rc,&err)==2) {
            j->complete=1;j->exit_code=rc;j->error=err;
        }
        fclose(f);
    }
    sprintf(path,"%s\\OUT.TMP",j->dir);
    if(stat(path,&st)==0 && st.st_size>=0) {
        j->size=(unsigned long)st.st_size;
        if(j->size>FJ_CAP){j->size=FJ_CAP;j->truncated=1;}
    }
}
static int fj_active(void) {
    int i;for(i=0;i<FJ_SLOTS;++i) {
        fj_poll(&fj_jobs[i]);
        if(fj_jobs[i].id[0]&&!fj_jobs[i].complete)return 1;
    }return 0;
}
static int fj_error(const char *msg) {
    char buf[180];sprintf(buf,"ERR:%s\n",msg);return send_cstr(buf);
}
static int fj_reply(FileJob *j) {
    char text[512],header[32],code[24];fj_poll(j);
    if(j->complete && !j->error && j->exit_code>=0)sprintf(code,"%d",j->exit_code);
    else strcpy(code,"unknown");
    sprintf(text,"id=%s\nstate=%s\npid=unknown\nexit_code=%s\ncaptured_bytes=%lu\noutput_limit=%lu\ntruncated=%d\noutput_error=%d\ncancel_scope=unsupported\ncommand_mode=shell\noutput_storage=file_unbounded\ncompletion_tracking=marker\n",
            j->id,j->complete&&!j->error?"finished":"unknown",code,j->size,FJ_CAP,j->truncated,j->error);
    sprintf(header,"SIZE:%u\n",(unsigned)strlen(text));
    if(send_cstr(header)<0)return -1;
    return send_cstr(text);
}
static int fj_clean(FileJob *j) {
    static const char *names[]={"COMMAND.TXT","OUT.TMP","DONE.NEW","DONE.FLG"};
    int i,ok=1;char path[272];
    for(i=0;i<4;++i){sprintf(path,"%s\\%s",j->dir,names[i]);if(remove(path)!=0&&errno!=ENOENT)ok=0;}
    if(ok&&rmdir(j->dir)==0){memset(j,0,sizeof(*j));return 1;}
    return 0;
}
static int fj_start(const char *id,const char *command) {
    FileJob *j=fj_find(id);char path[272],encoded[512];FILE *f;int i,ok;
    if(!fj_id(id)||!*command||strlen(command)>FJ_COMMAND_MAX)return fj_error("bad job ID or command length");
    if(j) {
        if(strcmp(j->command,command))return fj_error("job ID already used for different input");
        return fj_reply(j);
    }
#ifdef FJ_SINGLE_ACTIVE
    if(fj_active())return fj_error("only one active DOS-box job supported; wait for completion");
#endif
    if(strlen(g_exedir)>100 || strpbrk(g_exedir," \t\r\n\""))return fj_error("jobs require a short installation path without spaces");
    sprintf(path,"%s\\JOBRUN.EXE",g_exedir);
    f=fopen(path,"rb");if(!f)return fj_error("JOBRUN.EXE is not installed");fclose(f);
    for(i=0;i<FJ_SLOTS;++i)if(!fj_jobs[i].id[0]){j=&fj_jobs[i];break;}
    if(!j)return fj_error("job slots full; release a completed job");
    if(exec_spool_create(g_exedir,j->dir,sizeof(j->dir))<0)return fj_error("cannot reserve job directory");
    strcpy(j->id,id);strcpy(j->command,command);strcpy(encoded,command);fj_encode(encoded);
    sprintf(path,"%s\\COMMAND.TXT",j->dir);f=fopen(path,"wb");
    if(!f){j->complete=1;j->error=1;fj_clean(j);return fj_error("cannot write job command; nothing launched");}
    ok=fwrite(encoded,1,strlen(encoded),f)==strlen(encoded);
    if(fflush(f)!=0)ok=0;
    if(fclose(f)!=0)ok=0;
    if(!ok){j->complete=1;j->error=1;fj_clean(j);return fj_error("cannot finish job command; nothing launched");}
    if(!fj_launch(j->dir)){j->complete=1;j->error=1;fj_clean(j);return fj_error("job helper launch failed");}
    return fj_reply(j);
}
static int fj_number(const char **p,unsigned long *value,unsigned long max) {
    unsigned long n=0,d;const char *start=*p;
    while(**p>='0'&&**p<='9') {
        d=(unsigned long)(**p-'0');if(n>max/10||(n==max/10&&d>max%10))return 0;
        n=n*10+d;++*p;
    }if(*p==start)return 0;*value=n;return 1;
}
static int handle_file_job(char *line) {
    char *args,*id,*mode,*space,header[32],path[272];
    const char *p;FileJob *j;FILE *f;unsigned long offset,count,left;unsigned n;int i;
    if(!strcmp(line,"JOBS")) {
        char list[FJ_SLOTS*34+1];list[0]=0;
        for(i=0;i<FJ_SLOTS;++i)if(fj_jobs[i].id[0]){strcat(list,fj_jobs[i].id);strcat(list,"\n");}
        sprintf(header,"SIZE:%u\n",(unsigned)strlen(list));
        if(send_cstr(header)<0)return -1;
        return send_cstr(list);
    }
    args=strchr(line,' ');if(!args)return fj_error("bad job command");*args++=0;id=args;
    if(!strcmp(line,"JOBSTART")) {
        mode=strchr(id,' ');if(!mode)return fj_error("bad JOBSTART syntax");*mode++=0;
        if(mode[0]!='S'||mode[1]!=' '||!mode[2])return fj_error("this platform supports shell jobs only");
        return fj_start(id,mode+2);
    }
    if(!strcmp(line,"JOBREAD")) {
        space=strchr(id,' ');if(!space)return fj_error("bad JOBREAD syntax");*space++=0;p=space;
        if(!fj_number(&p,&offset,FJ_CAP)||*p++!=' '||!fj_number(&p,&count,65536UL)||*p)return fj_error("bad JOBREAD bounds");
        j=fj_find(id);if(!j)return fj_error("unknown job ID");fj_poll(j);
        if(offset>j->size)return fj_error("offset exceeds captured output");
        if(count>j->size-offset)count=j->size-offset;
        if(!count)return send_cstr("SIZE:0\n");
        sprintf(path,"%s\\OUT.TMP",j->dir);f=fopen(path,"rb");
        if(!f)return fj_error("cannot read job output");
        if(fseek(f,(long)offset,SEEK_SET)!=0){fclose(f);return fj_error("cannot seek job output");}
        sprintf(header,"SIZE:%lu\n",count);
        if(send_cstr(header)<0){fclose(f);return -1;}
        left=count;
        while(left) {
            n=(unsigned)(left>sizeof(fj_buffer)?sizeof(fj_buffer):left);
            if(fread(fj_buffer,1,n,f)!=n || send_all(fj_buffer,(int)n)<0){fclose(f);return -1;}
            left-=n;
        }fclose(f);return 0;
    }
    j=fj_find(id);if(!j)return fj_error("unknown job ID");fj_poll(j);
    if(!strcmp(line,"JOBSTATUS"))return fj_reply(j);
    if(!strcmp(line,"JOBCANCEL"))return fj_error("cancellation unsupported; no retained safe process identity");
    if(!strcmp(line,"JOBRELEASE")) {
        if(!j->complete)return fj_error("completion unknown; cannot release files a child may use");
        if(!fj_clean(j))return fj_error("cannot remove job files; retained for cleanup retry");
        return send_cstr("OK\n");
    }return fj_error("unknown job command");
}
#endif
