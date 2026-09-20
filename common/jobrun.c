/* Companion for file-backed Win16/OS2 jobs. Never runs inside the agent.
 * Input/output live in a directory reserved by the authenticated agent.
 * Completion marker is renamed into place only after output is closed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <process.h>
#include <share.h>

int main(int argc,char **argv) {
    char path[256],command[512];
    const char *shell;
    FILE *f;
    int input=-1,output=-1,rc=-1,error=0;
    unsigned n;
    if(argc!=2 || strlen(argv[1])>230) return 1;
    sprintf(path,"%s\\COMMAND.TXT",argv[1]);
    f=fopen(path,"rb");
    if(!f) return 2;
    n=(unsigned)fread(command,1,sizeof(command)-1,f);
    if(ferror(f) || !feof(f) || !n) {fclose(f);return 3;}
    fclose(f);command[n]=0;
    if(strlen(command)!=n || strpbrk(command,"\r\n")) return 4;
    sprintf(path,"%s\\OUT.TMP",argv[1]);
    output=sopen(path,O_WRONLY|O_CREAT|O_EXCL|O_BINARY,SH_DENYNO,S_IREAD|S_IWRITE);
    input=open("NUL",O_RDONLY|O_BINARY);
    if(output<0 || input<0 || dup2(input,0)<0 || dup2(output,1)<0 || dup2(output,2)<0) error=1;
    if(input>=0) close(input);
    if(output>=0) close(output);
    if(!error) {
        shell=getenv("COMSPEC");
        if(!shell || !*shell) {
#ifdef JOBRUN_DOS
            shell="C:\\COMMAND.COM";
#else
            shell="C:\\OS2\\CMD.EXE";
#endif
        }
        rc=spawnl(P_WAIT,shell,shell,"/C",command,NULL);
        if(rc<0)error=2;
#ifdef JOBRUN_DOS
        /* COMMAND.COM does not preserve the inner command's ERRORLEVEL
         * through this /C launch. Completion is known, command exit is not. */
        rc=-1;
#endif
    }
    /* Closing our copies cannot close handles retained by descendants.
     * Jobs track the shell's completion, not an entire process tree. */
    if(fflush(stdout)!=0 || fflush(stderr)!=0)error=3;
    if(close(1)!=0)error=3;
    if(close(2)!=0)error=3;
    sprintf(path,"%s\\DONE.NEW",argv[1]);
    f=fopen(path,"wb");
    if(!f)return 5;
    if(fprintf(f,"%d %d\n",rc,error)<0 || fflush(f)!=0) {fclose(f);return 6;}
    if(fclose(f)!=0)return 6;
    sprintf(command,"%s\\DONE.FLG",argv[1]);
    return rename(path,command)==0?0:7;
}
