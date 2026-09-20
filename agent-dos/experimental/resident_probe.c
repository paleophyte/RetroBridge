/* EXPERIMENT ONLY. Parent stays resident while a bounded child runs.
 * Never install as LLMAGENT.EXE or change the regular boot default.
 * Reuse foreground configuration/auth/network code; no command dispatcher
 * is called from an interrupt. This is not a production TSR.
 */
#define main unused_agent_main
#include "../llm_agent.c"
#undef main

#include "probe_isr.h"
static void (W32_CALL *probe_previous_chain)(void);
static int probe_vectors_installed;
static void probe_restore_vectors(void) {
    if(!probe_vectors_installed)return;
    wintr_disable();wintr_chain=probe_previous_chain;wintr_shutdown();
    _dos_setvect(0x1c,old_observer);probe_vectors_installed=0;
}
static unsigned long probe_clock(void) {
    union REGS r;r.h.ah=0;int86(0x1a,&r,&r);
    return ((unsigned long)r.w.cx<<16)|r.w.dx;
}


int main(int argc,char **argv) {
    static const char *modes[]={"cpu","cpu","disk","wait"};
    volatile unsigned canary[32];
    unsigned i,k,ss_before=0,sp_before=0;
    unsigned long deadline;
    int rc=1,child_rc;
    char child[160],logpath[160],line[200];FILE *log;
    void (__interrupt __far *old_timer)(void);
    union REGS regs;struct SREGS segs;
    load_config(argc>0?argv[0]:NULL);
    if(!g_token[0])return 2;
    sprintf(child,"%s\\PROBECH.EXE",g_exedir);
    sprintf(logpath,"%s\\PROBE.LOG",g_exedir);
    log=fopen(logpath,"wt");if(!log)return 3;
    if(sock_init()!=0 || !my_ip_addr){fclose(log);sock_exit();return 4;}
    if(!tcp_listen(&g_sock,g_port,0L,0,NULL,0))goto done;
    deadline=NET_DEADLINE(45);
    while(!sock_established((sock_type *)&g_sock)) {
        if(NET_EXPIRED(deadline)||!tcp_tick((sock_type *)&g_sock))goto done;
        __asm {
            sti
            hlt
        }
    }
    net_reset();
    if(recv_line(g_line,sizeof(g_line))<0 || strcmp(g_line,g_token))goto done;
    if(send_cstr("OK\n")<0 || recv_line(g_line,sizeof(g_line))<0 || strcmp(g_line,"PROBE"))goto done;
    probe_socket=&g_sock;
    old_timer=_dos_getvect(8);probe_previous_chain=wintr_chain;
    regs.h.ah=0x34;int86x(0x21,&regs,&regs,&segs);
    probe_indos=(volatile unsigned char __far *)MK_FP(segs.es,regs.x.bx);
    old_observer=_dos_getvect(0x1c);_dos_setvect(0x1c,observe_timer);
    probe_vectors_installed=1;atexit(probe_restore_vectors);
    wintr_init();wintr_chain=probe_pulse;
    __asm {
        mov ax,ss
        mov ss_before,ax
        mov ax,sp
        mov sp_before,ax
    }
    /* Establish that the configured timer works in the parent before
     * attributing child-time starvation to DOS's busy flags. */
    probe_ticks=probe_writes=0;
    send_cstr("PHASE control timer=1\n");
    deadline=probe_clock();wintr_enable();
    while(probe_clock()-deadline<36UL) {
        __asm {
            sti
            hlt
        }
    }
    wintr_disable();
    sprintf(line,"CONTROL ticks=%u writes=%u\n",probe_ticks,probe_writes);
    send_cstr(line);fputs(line,log);
    for(i=0;i<4;++i) {
        for(k=0;k<32;++k)canary[k]=(unsigned)(0x5a00+k);
        probe_ticks=probe_writes=probe_partial=probe_ss=probe_sp=0;
        observer_ticks=indos_max=critical_max=0;indos_min=255;
        sprintf(line,"PHASE %u %s timer=%u\n",i,modes[i],i!=0);send_cstr(line);
        fputs(line,log);fflush(log);
        if(i)wintr_enable();
        observer_enabled=1;
        child_rc=spawnl(P_WAIT,child,child,modes[i],NULL);
        observer_enabled=0;
        wintr_disable();
        for(k=0;k<32;++k)if(canary[k]!=(unsigned)(0x5a00+k))break;
        sprintf(line,"DONE %u rc=%d ticks=%u writes=%u canary=%u parent=%04x:%04x callback=%04x:%04x\n",
                i,child_rc,probe_ticks,probe_writes,k==32,ss_before,sp_before,probe_ss,probe_sp);
        /* Complete an interrupted heartbeat before foreground metadata. */
        if(probe_partial)send_all("BUSY\n"+probe_partial,5-probe_partial);
        send_cstr(line);fputs(line,log);fflush(log);
        sprintf(line,"FLAGS %u timer_ticks=%u indos_min=%u indos_max=%u critical_max=%u\n",i,observer_ticks,indos_min,indos_max,critical_max);
        send_cstr(line);fputs(line,log);fflush(log);
        if(child_rc!=7 || k!=32)break;
    }
    probe_restore_vectors();
    rc=_dos_getvect(8)==old_timer && _dos_getvect(0x1c)==old_observer?0:8;
    sprintf(line,"END vector_restored=%u\n",rc==0);send_cstr(line);fputs(line,log);
done:
    fclose(log);sock_close((sock_type *)&g_sock);
    deadline=NET_DEADLINE(2);
    while(!NET_EXPIRED(deadline)&&tcp_tick((sock_type *)&g_sock))delay(10);
    sock_exit();return rc;
}
