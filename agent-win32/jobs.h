/* Bounded, cooperatively polled jobs. No worker thread touches agent sockets.
 * Retained process handles prevent cancellation from targeting a reused PID.
 * Cancellation covers the direct child only (also on Win95/NT4).
 */
#ifndef RETRO_WIN32_JOBS_H
#define RETRO_WIN32_JOBS_H
#define JOB_SLOTS 4
#ifndef JOB_OUTPUT_CAP
#define JOB_OUTPUT_CAP (1024UL * 1024UL)
#endif
#define JOB_POLL_BYTES 16384UL

typedef struct {
    char id[33], command[LINE_MAX_LEN];
    HANDLE process, pipe;
    DWORD pid, exit_code, output_error;
    unsigned char *output;
    DWORD size;
    int shell, truncated, cancelled, cancel_requested, wait_error;
} ExecJob;
static ExecJob g_jobs[JOB_SLOTS];

static int job_id_valid(const char *id) {
    int i;
    if (strlen(id) != 32) return 0;
    for (i=0;i<32;++i)
        if (!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f'))) return 0;
    return 1;
}
static ExecJob *job_find(const char *id) {
    int i;
    if (!job_id_valid(id)) return NULL;
    for (i=0;i<JOB_SLOTS;++i) if (!strcmp(id,g_jobs[i].id)) return &g_jobs[i];
    return NULL;
}
static void job_discard(ExecJob *job) {
    if (job->pipe) CloseHandle(job->pipe);
    if (job->process) CloseHandle(job->process);
    if (job->output) HeapFree(GetProcessHeap(),0,job->output);
    ZeroMemory(job,sizeof(*job));
}
static void job_drain(ExecJob *job, DWORD budget) {
    unsigned char bytes[4096];
    while (job->pipe && budget) {
        DWORD available=0,got=0,want,keep;
        if (!PeekNamedPipe(job->pipe,NULL,0,NULL,&available,NULL)) {
            DWORD err=GetLastError();
            if (err!=ERROR_BROKEN_PIPE) job->output_error=err;
            CloseHandle(job->pipe);job->pipe=NULL;break;
        }
        if (!available) break;
        want=available<sizeof(bytes)?available:sizeof(bytes);
        if (want>budget) want=budget;
        if (!ReadFile(job->pipe,bytes,want,&got,NULL) || !got) {
            job->output_error=GetLastError();
            if (!job->output_error) job->output_error=ERROR_READ_FAULT;
            CloseHandle(job->pipe);job->pipe=NULL;break;
        }
        keep=JOB_OUTPUT_CAP-job->size;
        if (keep>got) keep=got;
        if (keep) memcpy(job->output+job->size,bytes,keep);
        job->size+=keep;
        if (keep!=got) job->truncated=1;
        budget-=got;
    }
}
static void jobs_poll(void) {
    int i;
    for (i=0;i<JOB_SLOTS;++i) {
        ExecJob *job=&g_jobs[i];
        DWORD wait;
        if (!job->process) continue;
        job_drain(job,JOB_POLL_BYTES);
        wait=WaitForSingleObject(job->process,0);
        if (wait==WAIT_OBJECT_0) {
            DWORD available=0;
            /* A descendant can retain/write the pipe indefinitely. Snapshot
             * a bounded final drain; completion tracks only our direct child. */
            job_drain(job,JOB_POLL_BYTES);
            if (job->pipe && PeekNamedPipe(job->pipe,NULL,0,NULL,&available,NULL) && available)
                job->truncated=1;
            if (!GetExitCodeProcess(job->process,&job->exit_code)) {
                job->wait_error=1;job->exit_code=0;
            } else job->wait_error=0;
            job->cancelled=job->cancel_requested;
            CloseHandle(job->process);job->process=NULL;
            if (job->pipe) { CloseHandle(job->pipe);job->pipe=NULL; }
        } else if (wait==WAIT_FAILED) job->wait_error=1;
    }
}
static const char *job_start(const char *id,int shell,const char *command,ExecJob **result) {
    ExecJob *job=job_find(id);
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE original=NULL,writer=NULL,input=INVALID_HANDLE_VALUE;
    char full[LINE_MAX_LEN+16];
    int i;
    *result=NULL;
    if (!job_id_valid(id) || !*command || strlen(command)>=LINE_MAX_LEN)
        return "bad job ID or command";
    if (job) {
        if (job->shell!=shell || strcmp(job->command,command)) return "job ID already used for different input";
        *result=job;return NULL;
    }
    for(i=0;i<JOB_SLOTS;++i) if (!g_jobs[i].id[0]) {job=&g_jobs[i];break;}
    if (!job) return "job slots full; release a completed job";
    job->output=(unsigned char *)HeapAlloc(GetProcessHeap(),0,JOB_OUTPUT_CAP);
    if (!job->output) return "cannot allocate job output buffer";
    ZeroMemory(&sa,sizeof(sa));sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
    if (!CreatePipe(&original,&writer,&sa,0)) goto failed;
    if (!DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&job->pipe,
                         0,FALSE,DUPLICATE_SAME_ACCESS)) goto failed;
    CloseHandle(original);original=NULL;
    input=CreateFileA("NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
                      OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (input==INVALID_HANDLE_VALUE) goto failed;
    ZeroMemory(&si,sizeof(si));si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=si.hStdError=writer;si.hStdInput=input;
    ZeroMemory(&pi,sizeof(pi));
    if (shell) build_shell_command(full,sizeof(full),command);
    else strcpy(full,command);
    if (!CreateProcessA(NULL,full,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) goto failed;
    CloseHandle(writer);CloseHandle(input);CloseHandle(pi.hThread);
    job->process=pi.hProcess;job->pid=pi.dwProcessId;job->shell=shell;
    strcpy(job->id,id);strcpy(job->command,command);*result=job;return NULL;
failed:
    if (original) CloseHandle(original);
    if (writer) CloseHandle(writer);
    if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
    job_discard(job);
    return "job launch failed; no child started";
}
static const char *job_state(const ExecJob *job) {
    if (job->wait_error) return "unknown";
    if (job->process) return job->cancel_requested?"cancelling":"running";
    return job->cancelled?"cancelled":"finished";
}
static const char *job_cancel(ExecJob *job) {
    if (!job->process || job->cancel_requested) return NULL;
    if (!TerminateProcess(job->process,1)) return "termination failed; job may still be running";
    job->cancel_requested=1;
    return NULL;
}
static int jobs_running(void) {
    int i;for(i=0;i<JOB_SLOTS;++i) if(g_jobs[i].process)return 1;return 0;
}
static void jobs_shutdown(void) {
    int i;
    for(i=0;i<JOB_SLOTS;++i) {
        if(g_jobs[i].process) TerminateProcess(g_jobs[i].process,1);
        job_discard(&g_jobs[i]);
    }
}
#ifndef JOBS_NO_WIRE
static int job_reply(SOCKET s, const ExecJob *job) {
    char text[512],header[32],code[32];
    if (job->process || job->wait_error) strcpy(code,"unknown");
    else wsprintfA(code,"%lu",(unsigned long)job->exit_code);
    wsprintfA(text,"id=%s\nstate=%s\npid=%lu\nexit_code=%s\ncaptured_bytes=%lu\noutput_limit=%lu\ntruncated=%d\noutput_error=%lu\ncancel_scope=process\ncommand_mode=%s\n",
              job->id,job_state(job),(unsigned long)job->pid,code,(unsigned long)job->size,
              (unsigned long)JOB_OUTPUT_CAP,job->truncated,(unsigned long)job->output_error,
              job->shell?"shell":"direct");
    wsprintfA(header,"SIZE:%lu\n",(unsigned long)strlen(text));
    if(send_cstr(s,header)<0)return -1;
    return send_cstr(s,text);
}
static int job_error(SOCKET s,const char *error) {
    char line[160];wsprintfA(line,"ERR:%s\n",error);return send_cstr(s,line);
}
static int job_number(const char **p,DWORD *value,DWORD maximum) {
    DWORD n=0;const char *start=*p;
    while(**p>='0'&&**p<='9') {
        DWORD digit=(DWORD)(**p-'0');
        if(n>maximum/10 || (n==maximum/10 && digit>maximum%10))return 0;
        n=n*10+digit;++*p;
    }
    if(*p==start)return 0;
    *value=n;return 1;
}
static int handle_job(SOCKET s,char *line) {
    ExecJob *job;
    char *args=strchr(line,' '),*id,*mode;
    const char *error;
    jobs_poll();
    if(!strcmp(line,"JOBS")) {
        char text[JOB_SLOTS*34+1],header[32];int i;
        text[0]=0;for(i=0;i<JOB_SLOTS;++i)if(g_jobs[i].id[0]){strcat(text,g_jobs[i].id);strcat(text,"\n");}
        wsprintfA(header,"SIZE:%lu\n",(unsigned long)strlen(text));
        if(send_cstr(s,header)<0)return -1;
        return send_cstr(s,text);
    }
    if(!args)return job_error(s,"bad job command");
    *args++=0;id=args;
    if(!strcmp(line,"JOBSTART")) {
        mode=strchr(id,' ');if(!mode)return job_error(s,"bad JOBSTART syntax");
        *mode++=0;
        if((mode[0]!='S'&&mode[0]!='D')||mode[1]!=' '||!mode[2])return job_error(s,"bad JOBSTART mode/command");
        error=job_start(id,mode[0]=='S',mode+2,&job);
        return error?job_error(s,error):job_reply(s,job);
    }
    if(!strcmp(line,"JOBREAD")) {
        const char *p;char *space=strchr(id,' ');DWORD offset,count;
        char header[32];
        if(!space)return job_error(s,"bad JOBREAD syntax");
        *space++=0;p=space;
        if(!job_number(&p,&offset,JOB_OUTPUT_CAP)||*p++!=' '||
           !job_number(&p,&count,65536UL)||*p)return job_error(s,"bad JOBREAD bounds");
        job=job_find(id);if(!job)return job_error(s,"unknown job ID");
        if(offset>job->size)return job_error(s,"offset exceeds captured output");
        if(count>job->size-offset)count=job->size-offset;
        wsprintfA(header,"SIZE:%lu\n",(unsigned long)count);
        if(send_cstr(s,header)<0)return -1;
        return send_all(s,(const char *)job->output+offset,(int)count);
    }
    job=job_find(id);if(!job)return job_error(s,"unknown job ID");
    if(!strcmp(line,"JOBSTATUS"))return job_reply(s,job);
    if(!strcmp(line,"JOBCANCEL")) {
        error=job_cancel(job);return error?job_error(s,error):job_reply(s,job);
    }
    if(!strcmp(line,"JOBRELEASE")) {
        if(job->process)return job_error(s,"job is still running; cannot release");
        job_discard(job);return send_cstr(s,"OK\n");
    }
    return job_error(s,"unknown job command");
}
#endif
#endif
