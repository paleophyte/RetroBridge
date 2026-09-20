/* Fixed, bounded foreground workloads; never accepts an arbitrary command. */
#include <stdio.h>
#include <string.h>
#include <i86.h>
static unsigned long ticks(void) {
    union REGS r;r.h.ah=0;int86(0x1a,&r,&r);return ((unsigned long)r.w.cx<<16)|r.w.dx;
}
int main(int argc,char **argv) {
    unsigned long start,now,elapsed;unsigned i;
    volatile unsigned accumulator=0;char buf[1024];FILE *f;
    int disk,wait;
    if(argc!=2)return 2;
    disk=!strcmp(argv[1],"disk");wait=!strcmp(argv[1],"wait");
    if(!disk&&!wait&&strcmp(argv[1],"cpu"))return 3;
    memset(buf,'x',sizeof(buf));start=ticks();
    do {
        if(disk){
            f=fopen("PROBE.TMP","wb");if(!f)return 4;
            for(i=0;i<32;++i)if(fwrite(buf,1,sizeof(buf),f)!=sizeof(buf)){fclose(f);return 5;}
            if(fclose(f)!=0)return 6;
            f=fopen("PROBE.TMP","rb");if(!f)return 4;
            while(fread(buf,1,sizeof(buf),f))++accumulator;
            fclose(f);
        } else if(wait) {
            /* BIOS keyboard poll with a timer limit, not an unbounded getch. */
            union REGS r;r.h.ah=1;int86(0x16,&r,&r);
            __asm {
                sti
                hlt
            }
        } else for(i=0;i<20000;++i)accumulator+=(unsigned)(i^0x51);
        now=ticks();elapsed=now>=start?now-start:now+0x1800b0UL-start;
    }while(elapsed<8UL*182/10);
    if(disk)remove("PROBE.TMP");
    return 7;
}
