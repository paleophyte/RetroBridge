/* Interrupt-only module: build with -s, matching Watt-32. Watcom
 * foreground stack probes cannot validate an interrupt/private stack.
 * Do not add CRT, DOS, allocation, blocking calls, or agent dispatch here. */
#include <tcp.h>
#include <i86.h>
#include "probe_isr.h"
tcp_Socket *probe_socket;
volatile unsigned probe_ticks,probe_writes,probe_partial;
volatile unsigned probe_ss,probe_sp;
volatile unsigned observer_ticks,indos_min,indos_max,critical_max;
volatile unsigned observer_enabled;
volatile unsigned char __far *probe_indos;
void (__interrupt __far *old_observer)(void);

void __interrupt __far observe_timer(void) {
    unsigned n;
    if(observer_enabled) {
        n=*probe_indos;++observer_ticks;
        if(n<indos_min)indos_min=n;
        if(n>indos_max)indos_max=n;
        n=*(probe_indos-1);if(n>critical_max)critical_max=n;
    }
    (*old_observer)();
}
void W32_CALL probe_pulse(void) {
    static const BYTE heartbeat[]="BUSY\n";
    int n;
    /* Only fixed memory and one nonblocking write attempt. Watt-32 itself
     * then polls TCP on its private timer stack. No DOS/stdio/allocation. */
    __asm {
        mov ax,ss
        mov probe_ss,ax
        mov ax,sp
        mov probe_sp,ax
    }
    ++probe_ticks;
    if(probe_ticks%18!=0 && !probe_partial)return;
    sock_flushnext((sock_type *)probe_socket);
    n=sock_fastwrite((sock_type *)probe_socket,heartbeat+probe_partial,5-probe_partial);
    if(n>0){probe_partial+=(unsigned)n;if(probe_partial==5){probe_partial=0;++probe_writes;}}
}
