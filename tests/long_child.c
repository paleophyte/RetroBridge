/* Native bounded live-test child. Watcom DOS/OS2; no listener/config changes.
 * Usage: LONGCHLD <seconds up to 120> <tag>. Prints start/end, exits 7.
 */
#ifdef CHILD_OS2
#define INCL_DOSPROCESS
#include <os2.h>
#else
#include <i86.h>
static long bios_ticks(void) {
    union REGS r;
    r.h.ah=0;int86(0x1a,&r,&r);
    return ((long)r.w.cx<<16)|r.w.dx;
}
#endif
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    unsigned seconds=argc>1?(unsigned)atoi(argv[1]):1;
    const char *tag=argc>2?argv[2]:"fixture";
    if(seconds>120)return 2;
    printf("%s START\n",tag);fflush(stdout);
#ifdef CHILD_OS2
    DosSleep((unsigned long)seconds*1000UL);
#else
    {
        long start,now;unsigned long elapsed;
        start=bios_ticks();
        do {
            __asm {
                sti
                hlt
            }
            now=bios_ticks();
            elapsed=now>=start?(unsigned long)(now-start):(unsigned long)(now+0x1800b0L-start);
        } while(elapsed<(unsigned long)seconds*182UL/10UL);
    }
#endif
    printf("%s END\n",tag);fflush(stdout);
    return 7;
}
