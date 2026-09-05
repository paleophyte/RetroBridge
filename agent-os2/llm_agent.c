/*
 * llm_agent (OS/2): OS/2 2.x port of the legacy Windows / FreeDOS llm_agent.
 *
 * Same wire protocol as agent/llm_agent.c so bridge/server.py can drive an
 * OS/2 VM without a protocol fork. Uses IBM TCP/IP TCPIPDLL (16-bit).
 *
 * Supported: auth, PING, QUIT, EXEC, PUT, GET, SYSINFO
 * Unsupported: EXECDETACH, CLICK, WINLIST, CLIPSET, REG*, PSLIST,
 *              PSKILL, SHUTDOWN, SCREENSHOT, KEY, TYPE, REBOOT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <process.h>
#include <dos.h>

#include "os2sock.h"

#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     512
#define READ_CHUNK       1024
#define OUT_TMP          "LLMOUT.TMP"
#define ERR_NOSUP        "ERR:not supported on OS/2\n"

static char g_token[128] = "";
static unsigned short g_port = DEFAULT_PORT;
static char g_exedir[128] = ".";
static int g_client = -1;

static char g_line[LINE_MAX_LEN];
static char g_cmd[LINE_MAX_LEN + 64];
static char g_iobuf[READ_CHUNK];
static char g_tmppath[160];

static void cpu_idle(void) {
    /* Accept loop only — keep it cheap; blocking accept/recv do the real wait. */
    volatile unsigned i;
    for (i = 0; i < 2000U; i++)
        ;
}

static int send_all(const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(g_client, (char *)(buf + sent), len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_byte(char *out) {
    int n = recv(g_client, out, 1, 0);
    if (n == 1) return 0;
    return -1;
}

static int recv_line(char *out, int outlen) {
    int i = 0;
    char c;
    while (i < outlen - 1) {
        if (recv_byte(&c) < 0) return -1;
        if (c == '\n') break;
        if (c != '\r') out[i++] = c;
    }
    out[i] = '\0';
    return i;
}

static void dirname_of(const char *path, char *out, int outlen) {
    int i, last = -1;
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\' || path[i] == '/') last = i;
    }
    if (last < 0) {
        strncpy(out, ".", outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    if (last >= outlen - 1) last = outlen - 2;
    memcpy(out, path, last);
    out[last] = '\0';
    if (out[0] == '\0') {
        out[0] = path[0];
        out[1] = path[1] == ':' ? ':' : '\0';
        if (out[1] == ':') { out[2] = '\\'; out[3] = '\0'; }
    }
}

static void load_config(const char *argv0) {
    char path[160];
    char line[256];
    FILE *f;

    dirname_of(argv0 ? argv0 : ".", g_exedir, sizeof(g_exedir));
    sprintf(path, "%s\\LLMAGENT.INI", g_exedir);

    f = fopen(path, "r");
    if (!f) {
        f = fopen("LLMAGENT.INI", "r");
        if (!f) return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0) g_port = (unsigned short)p;
        } else if (strncmp(line, "token=", 6) == 0) {
            strncpy(g_token, line + 6, sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = '\0';
        }
    }
    fclose(f);
}

static int run_exec(const char *cmdline) {
    FILE *f;
    int rc;
    size_t n;
    char hdr[32];

    /* Prefer a plain temp name in the agent's cwd. ".\LLMOUT.TMP" and
       paths built from argv0="." caused SYS0003 on OS/2 CMD. */
    strcpy(g_tmppath, OUT_TMP);

    /* Watcom system() can look for the wrong shell; call CMD.EXE explicitly.
       Quote the /C argument so "dir C:\" style tails don't break parsing. */
    if (strlen(cmdline) + strlen(g_tmppath) + 24 >= sizeof(g_cmd)) {
        send_cstr("ERR:command too long (use a .CMD)\n");
        return -1;
    }
    sprintf(g_cmd, "CMD.EXE /C \"%s > %s\"", cmdline, g_tmppath);
    rc = system(g_cmd);

    f = fopen(g_tmppath, "rb");
    if (!f) {
        /* Fallback without nested quotes (some CMD builds dislike them). */
        sprintf(g_cmd, "CMD.EXE /C %s > %s", cmdline, g_tmppath);
        rc = system(g_cmd);
        f = fopen(g_tmppath, "rb");
    }
    if (!f) {
        sprintf(hdr, "LEN:0\nEXIT:%d\n", rc);
        send_cstr(hdr);
        return 0;
    }
    while ((n = fread(g_iobuf, 1, sizeof(g_iobuf), f)) > 0) {
        sprintf(hdr, "LEN:%u\n", (unsigned)n);
        if (send_cstr(hdr) < 0 || send_all(g_iobuf, (int)n) < 0) {
            fclose(f);
            remove(g_tmppath);
            return -1;
        }
    }
    fclose(f);
    remove(g_tmppath);
    sprintf(hdr, "EXIT:%d\n", rc);
    send_cstr(hdr);
    return 0;
}

static int handle_put(char *args) {
    char *lastSpace = strrchr(args, ' ');
    long size;
    FILE *f;
    long remaining;
    int openFailed = 0;

    if (!lastSpace) {
        send_cstr("ERR:bad PUT syntax\n");
        return -1;
    }
    size = atol(lastSpace + 1);
    *lastSpace = '\0';
    if (size < 0) {
        send_cstr("ERR:bad size\n");
        return -1;
    }

    f = fopen(args, "wb");
    if (!f) openFailed = 1;

    remaining = size;
    while (remaining > 0) {
        int want = remaining < (long)sizeof(g_iobuf) ? (int)remaining : (int)sizeof(g_iobuf);
        int got = 0;
        while (got < want) {
            char c;
            if (recv_byte(&c) < 0) {
                openFailed = 1;
                remaining = 0;
                break;
            }
            g_iobuf[got++] = c;
        }
        if (got > 0 && !openFailed) {
            if ((int)fwrite(g_iobuf, 1, got, f) != got) openFailed = 1;
        }
        remaining -= got;
    }
    if (f) fclose(f);
    if (openFailed) {
        send_cstr("ERR:write failed\n");
        return -1;
    }
    send_cstr("OK\n");
    return 0;
}

static int handle_get(const char *path) {
    FILE *f;
    long size;
    char hdr[32];
    size_t n;

    f = fopen(path, "rb");
    if (!f) {
        send_cstr("ERR:cannot open file\n");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_cstr("ERR:cannot stat file\n");
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        send_cstr("ERR:cannot stat file\n");
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    sprintf(hdr, "SIZE:%ld\n", size);
    send_cstr(hdr);
    while ((n = fread(g_iobuf, 1, sizeof(g_iobuf), f)) > 0) {
        if (send_all(g_iobuf, (int)n) < 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int handle_sysinfo(void) {
    static char buf[512];
    int len = 0;
    char hdr[32];
    struct diskfree_t df;
    unsigned maj = 2, min = 11;

    /* Best-effort version; OS/2 2.11 lab target. */
    len += sprintf(buf + len, "os_family=os2\r\n");
    len += sprintf(buf + len, "os2_major=%u\r\n", maj);
    len += sprintf(buf + len, "os2_minor=%u\r\n", min);

    if (_dos_getdiskfree(3, &df) == 0) {
        unsigned long bps = (unsigned long)df.bytes_per_sector;
        unsigned long free_b = (unsigned long)df.avail_clusters *
                              (unsigned long)df.sectors_per_cluster * bps;
        unsigned long total_b = (unsigned long)df.total_clusters *
                               (unsigned long)df.sectors_per_cluster * bps;
        len += sprintf(buf + len, "disk_c_total_mb=%lu\r\n", total_b / (1024UL * 1024UL));
        len += sprintf(buf + len, "disk_c_free_mb=%lu\r\n", free_b / (1024UL * 1024UL));
    } else {
        len += sprintf(buf + len, "disk_c_total_mb=?\r\n");
        len += sprintf(buf + len, "disk_c_free_mb=?\r\n");
    }

    len += sprintf(buf + len, "agent=llm_agent-os2\r\n");

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

static void handle_client(void) {
    if (recv_line(g_line, sizeof(g_line)) < 0) return;
    if (g_token[0] == '\0' || strcmp(g_line, g_token) != 0) {
        send_cstr("FAIL\n");
        return;
    }
    send_cstr("OK\n");

    for (;;) {
        if (recv_line(g_line, sizeof(g_line)) < 0) break;

        if (strncmp(g_line, "EXECDETACH ", 11) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "EXEC ", 5) == 0) {
            run_exec(g_line + 5);
        } else if (strncmp(g_line, "PUT ", 4) == 0) {
            handle_put(g_line + 4);
        } else if (strncmp(g_line, "GET ", 4) == 0) {
            handle_get(g_line + 4);
        } else if (strcmp(g_line, "SCREENSHOT") == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "CLICK ", 6) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "KEY ", 4) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "TYPE ", 5) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "PSLIST") == 0 || strncmp(g_line, "PSKILL ", 7) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "SYSINFO") == 0) {
            handle_sysinfo();
        } else if (strcmp(g_line, "REBOOT") == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "SHUTDOWN") == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "WINLIST") == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "CLIPSET ", 8) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "REGGET\t", 7) == 0 || strncmp(g_line, "REGSET\t", 7) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "PING") == 0) {
            send_cstr("PONG\n");
        } else if (strcmp(g_line, "QUIT") == 0) {
            break;
        } else if (g_line[0] == '\0') {
            /* ignore */
        } else {
            send_cstr("ERR:unknown command\n");
        }
    }
}

static int server_main(void) {
    int ls;
    struct sockaddr_in addr;
    int one = 1;

    printf("llm_agent-os2: sock_init...\n");
    fflush(stdout);
    if (sock_init() != 0) {
        printf("sock_init failed — is TCPIPDLL on LIBPATH and INET up?\n");
        return 1;
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("socket() failed (rc=%d)\n", ls);
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind(%u) failed\n", (unsigned)g_port);
        soclose(ls);
        return 1;
    }
    if (listen(ls, 1) < 0) {
        printf("listen failed\n");
        soclose(ls);
        return 1;
    }

    printf("llm_agent-os2: listening on port %u\n", (unsigned)g_port);
    printf("token configured: %s\n", g_token[0] ? "yes" : "NO - set token= in LLMAGENT.INI");
    fflush(stdout);

    for (;;) {
        struct sockaddr_in peer;
        int peerlen = sizeof(peer);
        g_client = accept(ls, (struct sockaddr *)&peer, &peerlen);
        if (g_client < 0) {
            cpu_idle();
            continue;
        }
        handle_client();
        soclose(g_client);
        g_client = -1;
    }
    /* not reached */
    /* soclose(ls); sock_term(); */
    return 0;
}

int main(int argc, char **argv) {
    load_config(argc > 0 ? argv[0] : NULL);
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "/?") == 0)) {
        printf("llm_agent-os2 - OS/2 exec/file agent (TCPIPDLL)\n");
        printf("Usage: LLMAGENT.EXE\n");
        printf("Reads port/token from LLMAGENT.INI next to the exe.\n");
        return 0;
    }
    return server_main();
}
