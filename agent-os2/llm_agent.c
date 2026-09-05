/*
 * llm_agent (OS/2): OS/2 2.x port of the legacy Windows / FreeDOS llm_agent.
 *
 * Same wire protocol as agent/llm_agent.c so bridge/server.py can drive an
 * OS/2 VM without a protocol fork.
 *
 * 32-bit OS/2 (LX) + IBM SO32DLL/TCP32DLL sockets + PM screen capture.
 *
 * Supported: auth, PING, QUIT, EXEC, PUT, GET, SYSINFO, SCREENSHOT
 * Unsupported: EXECDETACH, CLICK, WINLIST, CLIPSET, REG*, PSLIST,
 *              PSKILL, SHUTDOWN, KEY, TYPE, REBOOT
 */

#define INCL_DOS
#define INCL_DOSPROCESS
#define INCL_WIN
#define INCL_GPI
#define INCL_DEV
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <process.h>

#include <types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tcpustd.h>

/*
 * OS/2 2.x MPTS (SO32DLL) uses 4.3-style sockaddr_in (no sin_len).
 * Watcom's <netinet/in.h> is 4.4-style with sin_len — using that makes
 * bind() fail even on a quiet port after reboot.
 */
struct os2_sockaddr_in {
    short          sin_family;
    unsigned short sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

/* Avoid pulling Watcom's TCPIP32.DLL just for byte-swap helpers. */
#undef htons
#undef ntohs
#undef htonl
#undef ntohl
static unsigned short local_htons(unsigned short x) {
    return (unsigned short)(((x & 0xff) << 8) | ((x >> 8) & 0xff));
}
#define htons(x) local_htons((unsigned short)(x))

#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     512
#define READ_CHUNK       4096
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
    DosSleep(20);
}

static int send_all(const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = (int)send(g_client, buf + sent, (size_t)(len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_byte(char *out) {
    int n = (int)recv(g_client, out, 1, 0);
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

    strcpy(g_tmppath, OUT_TMP);
    if (strlen(cmdline) + strlen(g_tmppath) + 24 >= sizeof(g_cmd)) {
        send_cstr("ERR:command too long (use a .CMD)\n");
        return -1;
    }
    sprintf(g_cmd, "CMD.EXE /C \"%s > %s\"", cmdline, g_tmppath);
    rc = system(g_cmd);

    f = fopen(g_tmppath, "rb");
    if (!f) {
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
    ULONG majmin = 0;
    FSALLOCATE fs;

    DosQuerySysInfo(QSV_VERSION_MAJOR, QSV_VERSION_MINOR, &majmin, sizeof(majmin));
    /* DosQuerySysInfo one-at-a-time is clearer: */
    {
        ULONG major = 0, minor = 0;
        DosQuerySysInfo(QSV_VERSION_MAJOR, QSV_VERSION_MAJOR, &major, sizeof(major));
        DosQuerySysInfo(QSV_VERSION_MINOR, QSV_VERSION_MINOR, &minor, sizeof(minor));
        len += sprintf(buf + len, "os_family=os2\r\n");
        len += sprintf(buf + len, "os2_major=%lu\r\n", (unsigned long)major);
        len += sprintf(buf + len, "os2_minor=%lu\r\n", (unsigned long)minor);
    }

    memset(&fs, 0, sizeof(fs));
    if (DosQueryFSInfo(3, FSIL_ALLOC, &fs, sizeof(fs)) == 0) {
        unsigned long bps = fs.cbSector;
        unsigned long free_b = (unsigned long)fs.cUnitAvail * fs.cSectorUnit * bps;
        unsigned long total_b = (unsigned long)fs.cUnit * fs.cSectorUnit * bps;
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

/* ---- SCREENSHOT: PM desktop via WinGetScreenPS → 24-bit BMP ---- */

static int handle_screenshot(void) {
    HAB hab = NULLHANDLE;
    HMQ hmq = NULLHANDLE;
    HDC hdcMem = NULLHANDLE;
    HPS hpsScr = NULLHANDLE;
    HPS hpsMem = NULLHANDLE;
    HBITMAP hbm = NULLHANDLE;
    HBITMAP hbmOld = NULLHANDLE;
    SIZEL sizl = { 0, 0 };
    LONG cx, cy;
    BITMAPINFOHEADER2 bmih;
    BITMAPINFO2 *pbmi = NULL;
    PBYTE row = NULL;
    POINTL aptl[3];
    unsigned long rowBytes, imageSize, fileSize;
    char hdr[32];
    unsigned char fileHdr[14];
    unsigned char infoHdr[40];
    LONG py;
    int rc = -1;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return -1;
    }
    /* Message queue is optional for a one-shot screen grab from a VIO/
       WINDOWCOMPAT process. OS/2 2.x often returns NULL here even when
       WinGetScreenPS will still work. */
    hmq = WinCreateMsgQueue(hab, 0);

    cx = WinQuerySysValue(HWND_DESKTOP, SV_CXSCREEN);
    cy = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    if (cx <= 0 || cy <= 0 || cx > 4096 || cy > 4096) {
        send_cstr("ERR:bad screen size\n");
        goto done;
    }

    hpsScr = WinGetScreenPS(HWND_DESKTOP);
    if (!hpsScr) {
        send_cstr("ERR:WinGetScreenPS failed\n");
        goto done;
    }

    hdcMem = DevOpenDC(hab, OD_MEMORY, "*", 0L, NULL, NULLHANDLE);
    if (!hdcMem) {
        send_cstr("ERR:DevOpenDC failed\n");
        goto done;
    }
    hpsMem = GpiCreatePS(hab, hdcMem, &sizl,
                         PU_PELS | GPIF_DEFAULT | GPIT_MICRO | GPIA_ASSOC);
    if (!hpsMem) {
        send_cstr("ERR:GpiCreatePS failed\n");
        goto done;
    }

    memset(&bmih, 0, sizeof(bmih));
    bmih.cbFix = sizeof(bmih);
    bmih.cx = cx;
    bmih.cy = cy;
    bmih.cPlanes = 1;
    bmih.cBitCount = 24;

    hbm = GpiCreateBitmap(hpsMem, &bmih, 0L, NULL, NULL);
    if (!hbm) {
        send_cstr("ERR:GpiCreateBitmap failed\n");
        goto done;
    }
    hbmOld = GpiSetBitmap(hpsMem, hbm);

    aptl[0].x = 0;
    aptl[0].y = 0;
    aptl[1].x = cx - 1;
    aptl[1].y = cy - 1;
    aptl[2].x = 0;
    aptl[2].y = 0;
    if (GpiBitBlt(hpsMem, hpsScr, 3L, aptl, ROP_SRCCOPY, BBO_IGNORE) == GPI_ERROR) {
        send_cstr("ERR:GpiBitBlt failed\n");
        goto done;
    }

    pbmi = (BITMAPINFO2 *)calloc(1, sizeof(BITMAPINFO2) + 256 * sizeof(RGB2));
    if (!pbmi) {
        send_cstr("ERR:oom\n");
        goto done;
    }
    pbmi->cbFix = 16; /* minimal fixed header used by QueryBitmapBits */
    pbmi->cx = cx;
    pbmi->cy = cy;
    pbmi->cPlanes = 1;
    pbmi->cBitCount = 24;

    rowBytes = ((unsigned long)cx * 3UL + 3UL) & ~3UL;
    imageSize = rowBytes * (unsigned long)cy;
    fileSize = 14UL + 40UL + imageSize;
    row = (PBYTE)malloc(rowBytes);
    if (!row) {
        send_cstr("ERR:oom\n");
        goto done;
    }

    sprintf(hdr, "SIZE:%lu\n", fileSize);
    if (send_cstr(hdr) < 0) goto done;

    memset(fileHdr, 0, sizeof(fileHdr));
    fileHdr[0] = 'B'; fileHdr[1] = 'M';
    fileHdr[2] = (unsigned char)(fileSize);
    fileHdr[3] = (unsigned char)(fileSize >> 8);
    fileHdr[4] = (unsigned char)(fileSize >> 16);
    fileHdr[5] = (unsigned char)(fileSize >> 24);
    fileHdr[10] = 54;

    memset(infoHdr, 0, sizeof(infoHdr));
    infoHdr[0] = 40;
    infoHdr[4] = (unsigned char)(cx);
    infoHdr[5] = (unsigned char)(cx >> 8);
    infoHdr[8] = (unsigned char)(cy);
    infoHdr[9] = (unsigned char)(cy >> 8);
    infoHdr[12] = 1;
    infoHdr[14] = 24;
    infoHdr[20] = (unsigned char)(imageSize);
    infoHdr[21] = (unsigned char)(imageSize >> 8);
    infoHdr[22] = (unsigned char)(imageSize >> 16);
    infoHdr[23] = (unsigned char)(imageSize >> 24);

    if (send_all((const char *)fileHdr, 14) < 0) goto done;
    if (send_all((const char *)infoHdr, 40) < 0) goto done;

    /* Scanline 0 is the bottom of the PM bitmap = first row of bottom-up BMP. */
    for (py = 0; py < cy; py++) {
        LONG got = GpiQueryBitmapBits(hpsMem, py, 1L, row, pbmi);
        if (got == GPI_ALTERROR) {
            send_cstr("ERR:GpiQueryBitmapBits failed\n");
            goto done;
        }
        if (send_all((const char *)row, (int)rowBytes) < 0) goto done;
        if ((py & 15) == 0) cpu_idle();
    }
    rc = 0;

done:
    if (row) free(row);
    if (pbmi) free(pbmi);
    if (hpsMem && hbmOld != NULLHANDLE) GpiSetBitmap(hpsMem, hbmOld);
    if (hbm) GpiDeleteBitmap(hbm);
    if (hpsMem) GpiDestroyPS(hpsMem);
    if (hdcMem) DevCloseDC(hdcMem);
    if (hpsScr) WinReleasePS(hpsScr);
    if (hmq) WinDestroyMsgQueue(hmq);
    if (hab) WinTerminate(hab);
    return rc;
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
            handle_screenshot();
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
    struct os2_sockaddr_in addr;
    int one = 1;

    printf("llm_agent-os2: sock_init...\n");
    fflush(stdout);
    if (sock_init() != 0) {
        printf("sock_init failed — is SO32DLL/TCP32DLL on LIBPATH and INET up?\n");
        return 1;
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("socket() failed (rc=%d errno=%d)\n", ls, sock_errno());
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind(%u) failed (errno=%d)\n", (unsigned)g_port, sock_errno());
        soclose(ls);
        return 1;
    }
    if (listen(ls, 1) < 0) {
        printf("listen failed (errno=%d)\n", sock_errno());
        soclose(ls);
        return 1;
    }

    printf("llm_agent-os2: listening on port %u (32-bit + PM screenshot)\n", (unsigned)g_port);
    printf("token configured: %s\n", g_token[0] ? "yes" : "NO - set token= in LLMAGENT.INI");
    fflush(stdout);

    for (;;) {
        struct os2_sockaddr_in peer;
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
    return 0;
}

int main(int argc, char **argv) {
    load_config(argc > 0 ? argv[0] : NULL);
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "/?") == 0)) {
        printf("llm_agent-os2 - OS/2 exec/file/screenshot agent (SO32DLL)\n");
        printf("Usage: LLMAGENT.EXE\n");
        return 0;
    }
    return server_main();
}
