/*
 * Minimal IBM TCP/IP (TCPIPDLL) declarations for 16-bit OS/2.
 * Entry points match exports from TCPIPDLL.DLL (sock_init, socket, â€¦, soclose).
 *
 * Verified against the real OS/2 1.3 guest's TCPIPDLL.DLL export table
 * (wdump -a) - every name below matches an EXPORTED entry point there,
 * including the so_cancel binding (the DLL exports "_sock_cancel", not
 * "_so_cancel" - fixed here after checking; so_cancel isn't actually
 * called anywhere in llm_agent.c, so this was latent, not a live bug).
 */
#ifndef OS2SOCK_H
#define OS2SOCK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned long  u_long;

#define AF_INET      2
#define SOCK_STREAM  1
#define INADDR_ANY   0UL
#define SOL_SOCKET   0xffff
#define SO_REUSEADDR 0x0004
#define FIONBIO      0x667e

struct in_addr {
    u_long s_addr;
};

struct sockaddr {
    u_short sa_family;
    char    sa_data[14];
};

struct sockaddr_in {
    short          sin_family;
    u_short        sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

#ifndef htons
#define htons(x) ((u_short)((((u_short)(x) & 0x00ffU) << 8) | \
                            (((u_short)(x) & 0xff00U) >> 8)))
#endif
#ifndef ntohs
#define ntohs(x) htons(x)
#endif
#ifndef htonl
#define htonl(x) ((u_long)((((u_long)(x) & 0x000000ffUL) << 24) | \
                           (((u_long)(x) & 0x0000ff00UL) <<  8) | \
                           (((u_long)(x) & 0x00ff0000UL) >>  8) | \
                           (((u_long)(x) & 0xff000000UL) >> 24)))
#endif
#ifndef ntohl
#define ntohl(x) htonl(x)
#endif

/* TCPIPDLL's _ioctl takes a 16-bit command and a far pointer, without
 * SO32DLL's fourth (length) argument. Verified against its entry point. */
int  socket_ioctl(int s, int cmd, char *arg);
/* Original IBM select: socket array, counts, timeout in milliseconds.
 * _select's entry point takes a far pointer, three ints, and a long. */
int  socket_select(int *sockets, int reads, int writes, int excepts, long timeout);
int  sock_init(void);
void sock_term(void);
int  socket(int domain, int type, int protocol);
int  bind(int s, struct sockaddr *name, int namelen);
int  listen(int s, int backlog);
int  accept(int s, struct sockaddr *name, int *anamelen);
int  connect(int s, struct sockaddr *name, int namelen);
int  send(int s, char *buf, int len, int flags);
int  recv(int s, char *buf, int len, int flags);
int  soclose(int s);
int  setsockopt(int s, int level, int optname, char *optval, int optlen);
int  getsockname(int s, struct sockaddr *name, int *namelen);
int  so_cancel(int s);
void cleanupsockets(void);

/*
 * TCPIPDLL is MSC-style cdecl (args on stack, AX return). Watcom's default
 * aux is register-based â€” that made socket() appear to fail after a good
 * sock_init().
 */
#pragma aux socket_ioctl   "_ioctl" parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux socket_select  "_select" parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux sock_init      "_sock_init"      parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux sock_term      "_sock_term"      parm caller [] modify [ax bx cx dx es]
#pragma aux socket         "_socket"         parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux bind           "_bind"           parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux listen         "_listen"         parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux accept         "_accept"         parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux connect        "_connect"        parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux send           "_send"           parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux recv           "_recv"           parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux soclose        "_soclose"        parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux setsockopt     "_setsockopt"     parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux getsockname    "_getsockname"    parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux so_cancel      "_sock_cancel"    parm caller [] value [ax] modify [ax bx cx dx es]
#pragma aux cleanupsockets "_cleanupsockets" parm caller [] modify [ax bx cx dx es]

#ifdef __cplusplus
}
#endif

#endif /* OS2SOCK_H */
