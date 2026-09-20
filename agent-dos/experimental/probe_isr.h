#ifndef RETRO_PROBE_ISR_H
#define RETRO_PROBE_ISR_H
extern tcp_Socket *probe_socket;
extern volatile unsigned probe_ticks,probe_writes,probe_partial;
extern volatile unsigned probe_ss,probe_sp;
extern volatile unsigned observer_ticks,indos_min,indos_max,critical_max;
extern volatile unsigned observer_enabled;
extern volatile unsigned char __far *probe_indos;
extern void (__interrupt __far *old_observer)(void);
void __interrupt __far observe_timer(void);
void W32_CALL probe_pulse(void);
#endif
