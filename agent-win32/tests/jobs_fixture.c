/* Native production job-engine tests. Safe Win95/NT4 compiler flags required.
 * No listener or installed agent is modified. Each test child is bounded.
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#define LINE_MAX_LEN 4096
static int is_windows_9x(void) { return (GetVersion()&0x80000000UL)!=0; }
static void build_shell_command(char *out,int capacity,const char *cmd) {
    const char *prefix=is_windows_9x()?"command.com /C ":"cmd.exe /C ";
    (void)capacity;strcpy(out,prefix);strcat(out,cmd);
}
#define JOBS_NO_WIRE
#include "../jobs.h"
static void say(const void *text,DWORD size) {
    DWORD wrote;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,size,&wrote,NULL);
}
static void check(int ok,const char *message) {
    if(!ok) { say(message,(DWORD)strlen(message));say("\r\n",2);jobs_shutdown();ExitProcess(99); }
}
static void wait_done(ExecJob *job,DWORD timeout) {
    DWORD start=GetTickCount();
    while(job->process && GetTickCount()-start<timeout) { jobs_poll();Sleep(5); }
    check(!job->process,"job did not finish within fixture deadline");
}
int main(int argc,char **argv) {
    char path[MAX_PATH],command[1024],id[33];ExecJob *job,*again,*slots[JOB_SLOTS];
    const char *error;unsigned i;DWORD start;
    GetModuleFileNameA(NULL,path,sizeof(path));
    if(argc>1 && !strcmp(argv[1],"--sleep")) {
        say("START\r\n",7);Sleep(20000);say("END\r\n",5);return 7;
    }
    if(argc>1 && !strcmp(argv[1],"--short")) {say("hello\0world",11);return 7;}
    if(argc>1 && !strcmp(argv[1],"--flood")) {
        unsigned char bytes[4096];for(i=0;i<sizeof(bytes);++i)bytes[i]=(unsigned char)i;
        for(i=0;i<300;++i)say(bytes,sizeof(bytes));
        return 13;
    }
    if(argc>1 && !strcmp(argv[1],"--spawn")) {
        STARTUPINFOA si;PROCESS_INFORMATION pi;char pid[32];
        ZeroMemory(&si,sizeof(si));ZeroMemory(&pi,sizeof(pi));si.cb=sizeof(si);
        si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError=si.hStdOutput;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        wsprintfA(command,"\"%s\" --sleep",path);
        if(!CreateProcessA(NULL,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi))return 9;
        wsprintfA(pid,"%lu\n",(unsigned long)pi.dwProcessId);say(pid,(DWORD)strlen(pid));
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
    }
    memset(id,'a',32);id[32]=0;
    wsprintfA(command,"\"%s\" --short",path);
    error=job_start(id,0,command,&job);check(!error,"short launch failed");
    check(!job_start(id,0,command,&again)&&again==job,"same ID was not idempotent");
    check(job_start(id,0,"different",&again)!=NULL,"ID collision accepted different command");
    wait_done(job,10000);check(job->exit_code==7&&job->size==11&&!memcmp(job->output,"hello\0world",11),"binary output/exit mismatch");
    check(!strcmp(job_state(job),"finished"),"wrong completion state");
    job_discard(job);
    wsprintfA(command,"\"%s\" --flood",path);check(!job_start(id,0,command,&job),"flood launch failed");
    wait_done(job,30000);check(job->exit_code==13&&job->truncated&&job->size==JOB_OUTPUT_CAP,"output bound failed");
    for(i=0;i<job->size;++i)check(job->output[i]==(unsigned char)i,"binary output changed");
    job_discard(job);
    wsprintfA(command,"\"%s\" --sleep",path);
    for(i=0;i<JOB_SLOTS;++i) {id[0]=(char)('a'+i);check(!job_start(id,0,command,&slots[i]),"slot launch failed");}
    id[0]='e';check(job_start(id,0,command,&job)!=NULL,"full slot set accepted job");
    for(i=0;i<JOB_SLOTS;++i) {check(!job_cancel(slots[i]),"cancel failed");wait_done(slots[i],10000);check(slots[i]->cancelled,"cancel not confirmed");job_discard(slots[i]);}
    wsprintfA(command,"\"%s\" --spawn",path);start=GetTickCount();check(!job_start(id,0,command,&job),"descendant fixture launch failed");
    wait_done(job,10000);check(GetTickCount()-start<10000&&job->size>0,"descendant pipe blocked completion");
    {
        DWORD pid=0;HANDLE child;
        for(i=0;i<job->size&&job->output[i]>='0'&&job->output[i]<='9';++i)pid=pid*10+job->output[i]-'0';
        check(pid!=0,"missing descendant PID");child=OpenProcess(PROCESS_TERMINATE,FALSE,pid);
        if(child) {TerminateProcess(child,1);CloseHandle(child);}
    }
    job_discard(job);
    check(job_start("bad",0,"anything",&job)!=NULL,"invalid ID accepted");
    check(job_start(id,0,"C:\\__NO_JOB_FIXTURE__\\NO.EXE",&job)!=NULL,"missing executable accepted");
    check(!jobs_running(),"fixture left active jobs");jobs_shutdown();say("PASS job engine\r\n",17);return 0;
}
