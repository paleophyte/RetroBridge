#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <errno.h>
static char g_exedir[128]=".";
static char reply[70000];static unsigned used;static int launches,fail_launch;
static int send_all(const char *s,int n){if(used+(unsigned)n>=sizeof(reply))abort();memcpy(reply+used,s,n);used+=n;reply[used]=0;return 0;}
static int send_cstr(const char *s){return send_all(s,(int)strlen(s));}
static void fj_encode(char *s){(void)s;}
static int fj_launch(const char *dir){(void)dir;++launches;return !fail_launch;}
#define FJ_COMMAND_MAX 120
#include "../common/file_jobs.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void command(const char *s){char line[600];strcpy(line,s);used=0;handle_file_job(line);}
static void write_job(FileJob *j,const char *name,const void *bytes,unsigned n){char p[272];FILE *f;sprintf(p,"%s\\%s",j->dir,name);f=fopen(p,"wb");if(!f)abort();if(fwrite(bytes,1,n,f)!=n)abort();fclose(f);}
int main(void){
 const char *id="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";FileJob *j;FILE *f;char path[272];unsigned i;
 f=fopen("JOBRUN.EXE","wb");CHECK(f);fclose(f);
 command("JOBSTART aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa D command");CHECK(strstr(reply,"shell jobs only"));CHECK(!launches);
 command("JOBSTART aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa S echo hi");CHECK(strstr(reply,"state=unknown"));CHECK(launches==1);j=fj_find(id);CHECK(j&&fj_active());
 command("JOBSTART aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa S echo hi");CHECK(launches==1);
 command("JOBSTART aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa S echo different");CHECK(strstr(reply,"different input"));CHECK(launches==1);
 command("JOBCANCEL aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");CHECK(strstr(reply,"unsupported"));
 command("JOBRELEASE aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");CHECK(strstr(reply,"completion unknown"));
 command("JOBREAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 0 0");CHECK(!strcmp(reply,"SIZE:0\n"));
 command("JOBREAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 42949672960 3");CHECK(strstr(reply,"bounds"));
 command("JOBREAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 0 65537");CHECK(strstr(reply,"bounds"));
 command("JOBREAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 0 3 extra");CHECK(strstr(reply,"bounds"));
 write_job(j,"OUT.TMP","a\0bc",4);command("JOBREAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 3");CHECK(used==10&&!memcmp(reply,"SIZE:3\n\0bc",10));
 command("JOBSTART bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb S second");CHECK(launches==2);
 command("JOBSTART cccccccccccccccccccccccccccccccc S third");CHECK(strstr(reply,"slots full")&&launches==2);
 sprintf(path,"%s\\OUT.TMP",j->dir);f=fopen(path,"ab");CHECK(f);for(i=0;i<257;++i){memset(fj_buffer,'x',sizeof(fj_buffer));fwrite(fj_buffer,1,sizeof(fj_buffer),f);fwrite(fj_buffer,1,sizeof(fj_buffer),f);}fclose(f);
 command("JOBSTATUS aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");CHECK(strstr(reply,"truncated=1"));CHECK(j->size==FJ_CAP);
 write_job(j,"DONE.FLG","7 0\n",4);command("JOBSTATUS aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");CHECK(strstr(reply,"state=finished")&&strstr(reply,"exit_code=7"));
 command("JOBRELEASE aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");CHECK(!strcmp(reply,"OK\n")&&!fj_find(id));
 j=fj_find("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");write_job(j,"DONE.FLG","-1 2\n",5);command("JOBSTATUS bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");CHECK(strstr(reply,"state=unknown")&&strstr(reply,"exit_code=unknown")&&strstr(reply,"output_error=2"));
 command("JOBRELEASE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");CHECK(!strcmp(reply,"OK\n")&&!fj_active());
 fail_launch=1;command("JOBSTART cccccccccccccccccccccccccccccccc S fail");CHECK(strstr(reply,"launch failed")&&!fj_find("cccccccccccccccccccccccccccccccc"));
 remove("JOBRUN.EXE");puts("PASS file jobs");return 0;
}
