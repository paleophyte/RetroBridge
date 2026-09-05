/*
 * Proof NLM: listen on TCP 2222, accept one client, send "PONG\n", exit.
 * Requires CLIB + TCP/IP (BSD sockets) on the NetWare server.
 *
 * Uses non-blocking accept + ThreadSwitchWithDelay so a waiting NLM does
 * not freeze the server (blocking accept holds the CLIB thread).
 */
#include <string.h>
#include "nwsock.h"

#define PORT 2222

int main(void)
{
    int ls, cs;
    struct sockaddr_in addr;
    struct sockaddr_in peer;
    int peerlen;
    const char *msg = "PONG\n";
    int n;
    int one = 1;

    ConsolePrintf("sockping: socket...\r\n");
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ConsolePrintf("sockping: socket failed (rc=%d)\r\n", ls);
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
    if (ioctl(ls, FIONBIO, &one) < 0) {
        ConsolePrintf("sockping: FIONBIO failed\r\n");
        close(ls);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ConsolePrintf("sockping: bind failed\r\n");
        close(ls);
        return 1;
    }
    if (listen(ls, 1) < 0) {
        ConsolePrintf("sockping: listen failed\r\n");
        close(ls);
        return 1;
    }

    ConsolePrintf("sockping: listening on %d - connect once (Alt-Esc for console)\r\n",
                  PORT);
    peerlen = sizeof(peer);
    for (;;) {
        cs = accept(ls, (struct sockaddr *)&peer, &peerlen);
        if (cs >= 0) break;
        ThreadSwitchWithDelay();
    }

    n = send(cs, (char *)msg, (int)strlen(msg), 0);
    ConsolePrintf("sockping: sent %d bytes, done\r\n", n);
    close(cs);
    close(ls);
    return 0;
}
