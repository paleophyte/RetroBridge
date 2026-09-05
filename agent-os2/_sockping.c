/*
 * Proof binary: listen on TCP 2222, accept one client, send "PONG\n", exit.
 * 16-bit OS/2 + TCPIPDLL (Socket/MPTS).
 */
#include <stdio.h>
#include <string.h>
#include "os2sock.h"

#define PORT 2222

int main(void)
{
    int ls, cs;
    struct sockaddr_in addr;
    struct sockaddr_in peer;
    int peerlen;
    const char *msg = "PONG\n";
    int n;

    printf("sockping: sock_init...\n");
    fflush(stdout);
    if (sock_init() != 0) {
        printf("sock_init failed\n");
        return 1;
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("socket failed (rc=%d)\n", ls);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind failed\n");
        soclose(ls);
        return 1;
    }
    if (listen(ls, 1) < 0) {
        printf("listen failed\n");
        soclose(ls);
        return 1;
    }

    printf("sockping: listening on %d — connect once\n", PORT);
    fflush(stdout);

    peerlen = sizeof(peer);
    cs = accept(ls, (struct sockaddr *)&peer, &peerlen);
    if (cs < 0) {
        printf("accept failed\n");
        soclose(ls);
        return 1;
    }

    n = send(cs, (char *)msg, (int)strlen(msg), 0);
    printf("sockping: sent %d bytes, done\n", n);
    soclose(cs);
    soclose(ls);
    sock_term();
    return 0;
}
