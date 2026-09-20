/*
 * llm_agent (OS/2): OS/2 2.x port of the legacy Windows / FreeDOS llm_agent.
 *
 * Same wire protocol as agent-win32/llm_agent.c so mcp-server/server.py can drive an
 * OS/2 VM without a protocol fork.
 *
 * 32-bit OS/2 (LX) + IBM SO32DLL/TCP32DLL sockets + PM screen capture.
 *
 * Supported: auth, PING, QUIT, EXEC, EXECDETACH, PUT, GET, SYSINFO,
 *            SCREENSHOT, CLICK, KEY, TYPE, WINCLOSE, WINLIST, PSLIST,
 *            PSKILL, CLIPSET, REBOOT
 * Unsupported: REG*, SHUTDOWN
 */

#define INCL_DOS
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#define INCL_DOSMEMMGR
#define INCL_DOSQUEUES
#define INCL_WIN
#define INCL_WINSWITCHLIST
#define INCL_WINCLIPBOARD
#define INCL_GPI
#define INCL_DEV
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <process.h>

/*
 * DosQProcStatus (DOSCALLS.154): undocumented OS/2 2.x process table dump.
 * Layout from Rick Fishman / Hobbes procstat.h (32-bit PROCS.EXE).
 */
#define PROCESS_END_INDICATOR 3

#pragma pack(1)
typedef struct _THREADINFO {
    ULONG  ulRecType;
    USHORT tidWithinProcess;
    USHORT usSlot;
    ULONG  ulBlockId;
    ULONG  ulPriority;
    ULONG  ulSysTime;
    ULONG  ulUserTime;
    UCHAR  uchState;
    UCHAR  uchPad;
    USHORT usPad;
} THREADINFO, *PTHREADINFO;

typedef struct _PROCESSINFO {
    ULONG       ulEndIndicator;
    PTHREADINFO ptiFirst;
    USHORT      pid;
    USHORT      pidParent;
    ULONG       ulType;
    ULONG       ulStatus;
    ULONG       idSession;
    USHORT      hModRef;
    USHORT      usThreadCount;
    ULONG       ulReserved;
    PVOID       pvReserved;
    USHORT      usSem16Count;
    USHORT      usDllCount;
    USHORT      usShrMemHandles;
    USHORT      usReserved;
    PUSHORT     pusSem16TableAddr;
    PUSHORT     pusDllTableAddr;
    PUSHORT     pusShrMemTableAddr;
} PROCESSINFO, *PPROCESSINFO;

typedef struct _MODINFO {
    struct _MODINFO *pNext;
    USHORT hMod;
    USHORT usModType;
    ULONG  ulModRefCount;
    ULONG  ulSegmentCount;
    ULONG  ulDontKnow1;
    PSZ    szModName;
    USHORT usModRef[1];
} MODINFO, *PMODINFO;

typedef struct _BUFFHEADER {
    PVOID         psumm;
    PPROCESSINFO  ppi;
    PVOID         psi;
    PVOID         pDontKnow1;
    PVOID         psmi;
    PMODINFO      pmi;
    PVOID         pDontKnow2;
    PVOID         pDontKnow3;
} BUFFHEADER, *PBUFFHEADER;
#pragma pack()

USHORT APIENTRY16 DosQProcStatus(PVOID pBuf, USHORT cbBuf);

#include <types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tcpustd.h>
#include <sys/ioctl.h>
/* SO32DLL's original IBM interface uses the 16-bit command number;
 * Watcom's newer BSD FIONBIO includes direction/size bits it rejects. */
#define IBM_FIONBIO 0x667eUL

/*
 * OS/2 2.x MPTS (SO32DLL) uses 4.3-style sockaddr_in (no sin_len).
 * Watcom's <netinet/in.h> is 4.4-style with sin_len â€” using that makes
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
#define OUT_TMP          "EXEC_OUT.TMP"
#define PID_FILE         "AGENT.PID"
#define ERR_NOSUP        "ERR:not supported on OS/2\n"
#define OS2_CMD_EXE      "C:\\OS2\\CMD.EXE"

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

static unsigned long network_ticks(void) {
    ULONG ticks = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ticks, sizeof(ticks));
    return ticks;
}
#define NET_DEADLINE(sec) (network_ticks() + (sec) * 1000UL)
#define NET_EXPIRED(d) ((long)(network_ticks() - (d)) >= 0)
#include "../common/session_timeout.h"
#include "../common/command_line.h"
#include "../common/update_identity.h"

static int send_all(const char *buf, int len) {
    int sent = 0;
    unsigned long deadline = NET_DEADLINE(NET_IO_SECONDS);
    while (sent < len) {
        int n;
        if (net_expired(deadline)) return net_fail();
        n = send(g_client, buf + sent, (size_t)(len - sent), 0);
        if (n > 0) {
            sent += n;
            deadline = NET_DEADLINE(NET_IO_SECONDS);
        } else cpu_idle(); /* old IBM stacks can report zero progress */
    }
    return net_failed ? -1 : 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_some(char *out, int len) {
    unsigned long deadline = NET_DEADLINE(NET_IO_SECONDS);
    for (;;) {
        int n;
        if (net_expired(deadline)) return net_fail();
        n = recv(g_client, out, len, 0);
        if (n > 0) return n;
        if (n == 0) return net_fail();
        cpu_idle();
    }
}

static int recv_byte(char *out) {
    return recv_some(out, 1) == 1 ? 0 : -1;
}

static int recv_line(char *out, int outlen) {
    int length = 0, saw_cr = 0, result;
    char c;
    if (outlen < 2) return net_fail();
    out[0] = '\0';
    if (net_failed) return -1;
    net_begin_line();
    for (;;) {
        if (recv_byte(&c) < 0) break;
        result = command_line_byte(out, outlen, &length, &saw_cr,
                                   (unsigned char)c);
        if (result < 0) break;
        if (result > 0) {
            net_end_line();
            return length;
        }
    }
    /* Never expose a partial command or consume a failed session's suffix. */
    out[0] = '\0';
    return net_fail();
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

/* DosStartSession relaunch often inherits an empty PATH/COMSPEC, which
 * breaks Watcom system() (it looks up CMD.EXE via the environment). */
static void ensure_shell_env(void) {
    char *comspec = getenv("COMSPEC");
    char *path = getenv("PATH");
    if (!comspec || !comspec[0])
        putenv("COMSPEC=C:\\OS2\\CMD.EXE");
    if (!path || !path[0])
        putenv("PATH=C:\\OS2;C:\\OS2\\SYSTEM;C:\\OS2\\MDOS;C:\\");
}

static void write_pid_file(void) {
    char path[160];
    FILE *f;
    PTIB ptib = NULL;
    PPIB ppib = NULL;
    PID pid = 0;

    if (DosGetInfoBlocks(&ptib, &ppib) == 0 && ppib) {
        pid = ppib->pib_ulpid;
    }
    sprintf(path, "%s\\%s", g_exedir, PID_FILE);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%lu\n", (unsigned long)pid);
    fclose(f);
}

/* Collapse our VIO session to the Minimized Window Viewer so a normal
 * boot/self-update doesn't leave a console covering the desktop. */
static void minimize_self(void) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    PTIB ptib = NULL;
    PPIB ppib = NULL;
    PID pid;
    HSWITCH hsw;
    SWCNTRL swctl;

    if (DosGetInfoBlocks(&ptib, &ppib) != 0 || !ppib) return;
    pid = ppib->pib_ulpid;

    hab = WinInitialize(0);
    if (!hab) return;
    hmq = WinCreateMsgQueue(hab, 0);

    DosSleep(200); /* let the frame finish creating before we poke it */

    hsw = WinQuerySwitchHandle(NULLHANDLE, pid);
    if (hsw != NULLHANDLE) {
        memset(&swctl, 0, sizeof(swctl));
        if (WinQuerySwitchEntry(hsw, &swctl) == 0 && swctl.hwnd != NULLHANDLE) {
            WinSetWindowPos(swctl.hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                            SWP_MINIMIZE | SWP_DEACTIVATE);
        }
    }

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
}

/*
 * EXECDETACH: independent session via DosStartSession so the child
 * survives when we later kill this agent (needed for UPDATE.EXE).
 * Command line may use "quoted" program/arg tokens like the Windows agent.
 */
static int next_token(const char **p, char *buf, int buflen) {
    int i = 0;
    while (**p == ' ' || **p == '\t') (*p)++;
    if (**p == '\0') {
        buf[0] = '\0';
        return 0;
    }
    if (**p == '"') {
        (*p)++;
        while (**p && **p != '"' && i < buflen - 1) buf[i++] = *(*p)++;
        if (**p == '"') (*p)++;
    } else {
        while (**p && **p != ' ' && **p != '\t' && i < buflen - 1) buf[i++] = *(*p)++;
    }
    buf[i] = '\0';
    return 1;
}

static int run_exec_detach(const char *cmdline) {
    char pgm[260];
    char inputs[LINE_MAX_LEN];
    const char *p = cmdline;
    STARTDATA sd;
    ULONG sess_id = 0;
    PID pid = 0;
    APIRET rc;
    char reply[64];
    char obj[260];

    if (!next_token(&p, pgm, sizeof(pgm)) || pgm[0] == '\0') {
        send_cstr("ERR:empty EXECDETACH command\n");
        return -1;
    }

    /* DosStartSession wants PgmInputs starting with a blank. */
    inputs[0] = ' ';
    inputs[1] = '\0';
    {
        int n = (int)strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        if (n > 0 && n < (int)sizeof(inputs) - 2) {
            memcpy(inputs + 1, p, n);
            inputs[1 + n] = '\0';
        }
    }

    memset(&sd, 0, sizeof(sd));
    sd.Length = sizeof(sd);
    sd.Related = SSF_RELATED_INDEPENDENT;
    sd.FgBg = SSF_FGBG_BACK;
    sd.TraceOpt = SSF_TRACEOPT_NONE;
    sd.PgmTitle = (PSZ)"llm_detach";
    sd.PgmName = (PSZ)pgm;
    sd.PgmInputs = (PBYTE)inputs;
    sd.TermQ = NULL;
    sd.Environment = NULL;
    sd.InheritOpt = SSF_INHERTOPT_PARENT;
    sd.SessionType = SSF_TYPE_WINDOWABLEVIO;
    sd.IconFile = NULL;
    sd.PgmHandle = 0;
    sd.PgmControl = SSF_CONTROL_MINIMIZE;
    sd.ObjectBuffer = obj;
    sd.ObjectBuffLen = sizeof(obj);

    rc = DosStartSession(&sd, &sess_id, &pid);
    if (rc != 0) {
        sprintf(reply, "ERR:DosStartSession rc=%lu\n", (unsigned long)rc);
        send_cstr(reply);
        return -1;
    }
    sprintf(reply, "OK pid=%lu\n", (unsigned long)pid);
    send_cstr(reply);
    return 0;
}

#define OS2_CMD_EXE      "C:\\OS2\\CMD.EXE"
#define EXEC_CMD_NAME    "LLMEXEC.CMD"
#define EXEC_DONE_NAME   "LLMDONE.FLG"

static int run_exec(const char *cmdline) {
    FILE *f;
    int exit_code = 0;
    size_t n;
    char hdr[80];
    char inputs[64];
    char cmdpath[160];
    char donepath[160];
    char obj[128];
    STARTDATA sd;
    ULONG sess_id = 0;
    PID pid = 0;
    APIRET arc;
    int i;
    char *dir = g_exedir[0] ? g_exedir : "C:\\llmagent";

    sprintf(g_tmppath, "%s\\%s", dir, OUT_TMP);
    sprintf(cmdpath, "%s\\%s", dir, EXEC_CMD_NAME);
    sprintf(donepath, "%s\\%s", dir, EXEC_DONE_NAME);

    if (strlen(cmdline) + strlen(g_tmppath) + 32 >= 1000) {
        send_cstr("ERR:command too long (use a .CMD)\n");
        return -1;
    }

    remove(g_tmppath);
    remove(donepath);

    /*
     * DosExecPgm/system() are unreliable after DosStartSession relaunch.
     * Independent StartSession works (see EXECDETACH) but returns pid=0 and
     * TermQ never fires here â€” so wrap the command in a .CMD that writes a
     * done flag when finished, then poll for that flag.
     */
    f = fopen(cmdpath, "wb");
    if (!f) {
        send_cstr("ERR:cannot write LLMEXEC.CMD\n");
        return -1;
    }
    fprintf(f, "%s > %s\r\n", cmdline, g_tmppath);
    fprintf(f, "echo done > %s\r\n", donepath);
    fclose(f);

    sprintf(inputs, " /C %s", cmdpath);

    memset(&sd, 0, sizeof(sd));
    sd.Length = sizeof(sd);
    sd.Related = SSF_RELATED_INDEPENDENT;
    sd.FgBg = SSF_FGBG_BACK;
    sd.TraceOpt = SSF_TRACEOPT_NONE;
    sd.PgmTitle = (PSZ)"llm_exec";
    sd.PgmName = (PSZ)OS2_CMD_EXE;
    sd.PgmInputs = (PBYTE)inputs;
    sd.TermQ = NULL;
    sd.Environment = NULL;
    sd.InheritOpt = SSF_INHERTOPT_PARENT;
    sd.SessionType = SSF_TYPE_WINDOWABLEVIO;
    sd.PgmControl = SSF_CONTROL_MINIMIZE | SSF_CONTROL_INVISIBLE;
    sd.ObjectBuffer = obj;
    sd.ObjectBuffLen = sizeof(obj);

    arc = DosStartSession(&sd, &sess_id, &pid);
    if (arc != 0) {
        sprintf(hdr, "ERR:DosStartSession EXEC rc=%lu\n", (unsigned long)arc);
        send_cstr(hdr);
        return -1;
    }

    for (i = 0; i < 600; i++) { /* ~60s */
        f = fopen(donepath, "rb");
        if (f) {
            fclose(f);
            DosSleep(150);
            break;
        }
        DosSleep(100);
    }
    /* Leave LLMEXEC.CMD for diagnosis if output missing; always clear done. */
    remove(donepath);

    f = fopen(g_tmppath, "rb");
    if (!f) {
        sprintf(hdr, "LEN:0\nEXIT:%d\n", exit_code);
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
    sprintf(hdr, "EXIT:%d\n", exit_code);
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
        int got = recv_some(g_iobuf, want);
        if (got <= 0) {
            openFailed = 1;
            break;
        }
        if (got > 0 && !openFailed) {
            if ((int)fwrite(g_iobuf, 1, got, f) != got) openFailed = 1;
        }
        remaining -= got;
    }
    if (f) {
        /* Buffered disk errors may surface only at flush/close. */
        if (fflush(f) != 0 || ferror(f)) openFailed = 1;
        if (fclose(f) != 0) openFailed = 1;
    }
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

/* Basename of a path; also scrub tabs for the wire format. */
static void path_basename(char *dst, size_t dstsz, const char *src) {
    const char *base = src;
    const char *p;
    size_t n;
    char *d;

    if (!src) src = "";
    for (p = src; *p; p++) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    n = strlen(base);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, base, n);
    dst[n] = '\0';
    for (d = dst; *d; d++) {
        if (*d == '\t' || *d == '\r' || *d == '\n')
            *d = ' ';
    }
}

/*
 * PSLIST: "<pid>\t<name>\r\n" via DosQProcStatus (same wire format as
 * Windows). Names come from the module table matched by hModRef.
 */
static int handle_pslist(void) {
    static char out[32768];
    char *raw = NULL;
    PBUFFHEADER pbh;
    PPROCESSINFO ppi;
    PMODINFO pmi;
    USHORT rc;
    int len = 0;
    char hdr[32];
    char name[260];
    int i, nproc;
    struct {
        USHORT pid;
        USHORT hMod;
        char name[128];
    } *tbl = NULL;

    raw = (char *)malloc(0xFFFF);
    if (!raw) {
        send_cstr("ERR:out of memory\n");
        return -1;
    }
    memset(raw, 0, 0xFFFF);

    rc = DosQProcStatus(raw, 0xFFFF);
    if (rc != 0) {
        free(raw);
        sprintf(out, "ERR:DosQProcStatus rc=%u\n", (unsigned)rc);
        send_cstr(out);
        return -1;
    }

    pbh = (PBUFFHEADER)raw;
    if (!pbh->ppi) {
        free(raw);
        send_cstr("ERR:DosQProcStatus empty process list\n");
        return -1;
    }

    nproc = 0;
    ppi = pbh->ppi;
    while (ppi->ulEndIndicator != PROCESS_END_INDICATOR) {
        nproc++;
        ppi = (PPROCESSINFO)(ppi->ptiFirst + ppi->usThreadCount);
    }
    if (nproc <= 0) {
        free(raw);
        send_cstr("SIZE:0\n");
        return 0;
    }

    tbl = (void *)calloc(nproc, sizeof(*tbl));
    if (!tbl) {
        free(raw);
        send_cstr("ERR:out of memory\n");
        return -1;
    }

    ppi = pbh->ppi;
    for (i = 0; i < nproc; i++) {
        tbl[i].pid = ppi->pid;
        tbl[i].hMod = ppi->hModRef;
        sprintf(tbl[i].name, "pid%u", (unsigned)ppi->pid);
        ppi = (PPROCESSINFO)(ppi->ptiFirst + ppi->usThreadCount);
    }

    for (pmi = pbh->pmi; pmi != NULL; pmi = pmi->pNext) {
        if (!pmi->szModName)
            continue;
        for (i = 0; i < nproc; i++) {
            if (tbl[i].hMod == pmi->hMod) {
                path_basename(tbl[i].name, sizeof(tbl[i].name),
                              (const char *)pmi->szModName);
            }
        }
    }

    for (i = 0; i < nproc; i++) {
        if (len > (int)sizeof(out) - 160)
            break;
        len += sprintf(out + len, "%u\t%s\r\n",
                       (unsigned)tbl[i].pid, tbl[i].name);
    }

    free(tbl);
    free(raw);

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || (len > 0 && send_all(out, len) < 0))
        return -1;
    return 0;
}

/* PSKILL <pid>: DosKillProcess â€” same trust model as EXEC. */
static void handle_pskill(const char *args) {
    PID pid;
    APIRET rc;
    char reply[64];

    while (*args == ' ') args++;
    pid = (PID)atol(args);
    if (pid == 0) {
        send_cstr("ERR:bad PID\n");
        return;
    }

    rc = DosKillProcess(DKP_PROCESS, pid);
    if (rc != 0) {
        sprintf(reply, "ERR:DosKillProcess rc=%lu\n", (unsigned long)rc);
        send_cstr(reply);
        return;
    }
    send_cstr("OK\n");
}

static int handle_sysinfo(void) {
    static char buf[1024];
    int len = 0;
    char hdr[32];
    ULONG majmin = 0;
    FSALLOCATE fs;

    DosQuerySysInfo(QSV_VERSION_MAJOR, QSV_VERSION_MINOR, &majmin, sizeof(majmin));
    /* DosQuerySysInfo one-at-a-time is clearer: */
    {
        ULONG major = 0, minor = 0, disp_major, disp_minor;
        DosQuerySysInfo(QSV_VERSION_MAJOR, QSV_VERSION_MAJOR, &major, sizeof(major));
        DosQuerySysInfo(QSV_VERSION_MINOR, QSV_VERSION_MINOR, &minor, sizeof(minor));

        /* QSV_VERSION_MAJOR/MINOR are raw kernel values, not the marketing
         * version CMD.EXE's "ver" prints. Major is always 20 for every
         * 32-bit OS/2 release; the generation lives in minor instead:
         * minor 0/10/11 -> OS/2 2.00/2.10/2.11, but from Warp 3 onward
         * (minor >= 30) the tens digit becomes the displayed major and the
         * ones digit *10 becomes the displayed minor (30 -> "3.00", 40 ->
         * "4.00", 45 -> "4.50" for Warp 4.5). Confirmed against the real
         * os2-3 box: kernel reports 20.45, "ver" prints 4.50. */
        if (minor >= 30) {
            disp_major = minor / 10;
            disp_minor = (minor % 10) * 10;
        } else {
            disp_major = major / 10;
            disp_minor = minor;
        }

        len += sprintf(buf + len, "agent_exe=%s\r\nagent_sha256=%s\r\nagent_started=%s\r\n",
                       g_update_exe, g_update_sha256, g_update_started);
        len += sprintf(buf + len, "os_family=os2\r\n");
        len += sprintf(buf + len, "os2_major=%lu\r\n", (unsigned long)major);
        len += sprintf(buf + len, "os2_minor=%lu\r\n", (unsigned long)minor);
        len += sprintf(buf + len, "os2_version=%lu.%02lu\r\n", (unsigned long)disp_major, (unsigned long)disp_minor);
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

/* Scrub tabs/CR/LF so WINLIST wire lines stay 7 tab-separated fields. */
static void scrub_field(char *s) {
    for (; *s; s++) {
        if (*s == '\t' || *s == '\r' || *s == '\n')
            *s = ' ';
    }
}

/*
 * WINLIST: same wire format as Windows agent â€”
 *   "<hwnd>\t<x>\t<y>\t<w>\t<h>\t<class>\t<title>\r\n"
 * Coords are top-left origin (match SCREENSHOT / CLICK).
 *
 * Enumerate via WinQuerySwitchList (Window List entries) â€” a plain
 * WinBeginEnumWindows walk from a VIO agent sees no titled PM frames.
 */
static int handle_winlist(void) {
    static char buf[32768];
    HAB hab;
    HMQ hmq = NULLHANDLE;
    ULONG cb;
    ULONG nentries;
    PSWBLOCK psw = NULL;
    LONG cy;
    int len = 0;
    char hdr[32];
    char cls[128];
    ULONG i;
    int rc_out = -1;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return -1;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    cy = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    if (cy <= 0) cy = 480;

    /* First call returns entry COUNT (not bytes). HAB may be 0. */
    nentries = WinQuerySwitchList(NULLHANDLE, NULL, 0);
    if (nentries == 0)
        nentries = WinQuerySwitchList(hab, NULL, 0);
    if (nentries == 0) {
        send_cstr("ERR:WinQuerySwitchList empty\n");
        goto done;
    }
    cb = sizeof(ULONG) + (nentries + 2) * sizeof(SWENTRY);
    psw = (PSWBLOCK)malloc(cb);
    if (!psw) {
        send_cstr("ERR:out of memory\n");
        goto done;
    }
    nentries = WinQuerySwitchList(NULLHANDLE, psw, cb);
    if (nentries == 0)
        nentries = WinQuerySwitchList(hab, psw, cb);
    if (nentries == 0 || psw->cswentry == 0) {
        send_cstr("ERR:WinQuerySwitchList failed\n");
        goto done;
    }

    for (i = 0; i < psw->cswentry; i++) {
        SWCNTRL *sw = &psw->aswentry[i].swctl;
        HWND hwnd = sw->hwnd;
        RECTL rcl;
        int x, y, w, h;
        char title[MAXNAMEL + 8];

        if (sw->uchVisibility == SWL_INVISIBLE)
            continue;

        strncpy(title, sw->szSwtitle, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        if (title[0] == '\0' && hwnd != NULLHANDLE)
            WinQueryWindowText(hwnd, sizeof(title), title);
        if (title[0] == '\0')
            continue;

        cls[0] = '\0';
        if (hwnd != NULLHANDLE)
            WinQueryClassName(hwnd, sizeof(cls), cls);
        if (cls[0] == '\0')
            strcpy(cls, "switch");

        memset(&rcl, 0, sizeof(rcl));
        x = y = w = h = 0;
        if (hwnd != NULLHANDLE && WinQueryWindowRect(hwnd, &rcl)) {
            /* Rect is relative to parent; map to desktop for top-level. */
            WinMapWindowPoints(WinQueryWindow(hwnd, QW_PARENT), HWND_DESKTOP,
                               (PPOINTL)&rcl, 2);
            x = (int)rcl.xLeft;
            w = (int)(rcl.xRight - rcl.xLeft);
            h = (int)(rcl.yTop - rcl.yBottom);
            y = (int)(cy - rcl.yTop);
            if (w < 0) w = 0;
            if (h < 0) h = 0;
        }

        scrub_field(title);
        scrub_field(cls);

        if (len > (int)sizeof(buf) - 512)
            break;

        len += sprintf(buf + len, "%lu\t%d\t%d\t%d\t%d\t%s\t%s\r\n",
                       (unsigned long)hwnd, x, y, w, h, cls, title);
    }

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || (len > 0 && send_all(buf, len) < 0))
        goto done;
    rc_out = 0;

done:
    if (psw) free(psw);
    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return rc_out;
}

/* ---- SCREENSHOT: PM desktop via WinGetScreenPS â†’ 24-bit BMP ---- */

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

/*
 * Soft reboot: detach REBOOT.EXE (OEMHLP/DOS$ IOCTL, then kbd .COM).
 */
static void handle_reboot(void) {
    char path[160];
    STARTDATA sd;
    ULONG sess_id = 0;
    PID pid = 0;
    APIRET rc;
    char obj[128];
    char reply[96];

    sprintf(path, "%s\\REBOOT.EXE", g_exedir[0] ? g_exedir : "C:\\llmagent");

    memset(&sd, 0, sizeof(sd));
    sd.Length = sizeof(sd);
    sd.Related = SSF_RELATED_INDEPENDENT;
    sd.FgBg = SSF_FGBG_BACK;
    sd.TraceOpt = SSF_TRACEOPT_NONE;
    sd.PgmTitle = "REBOOT";
    sd.PgmName = path;
    sd.PgmInputs = NULL;
    sd.TermQ = NULL;
    sd.Environment = NULL;
    sd.InheritOpt = SSF_INHERTOPT_PARENT;
    sd.SessionType = SSF_TYPE_WINDOWABLEVIO;
    sd.PgmControl = SSF_CONTROL_MINIMIZE | SSF_CONTROL_INVISIBLE;
    sd.ObjectBuffer = obj;
    sd.ObjectBuffLen = sizeof(obj);

    rc = DosStartSession(&sd, &sess_id, &pid);
    if (rc != 0) {
        sprintf(reply, "ERR:DosStartSession REBOOT.EXE rc=%lu\n", (unsigned long)rc);
        send_cstr(reply);
        return;
    }
    send_cstr("OK\n");
}

static void handle_shutdown(void) {
    send_cstr(ERR_NOSUP);
}

/* CLIPSET <text>: put plain text on the PM clipboard (pair with KEY for paste). */
static void handle_clipset(const char *text) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    char *mem = NULL;
    size_t len;
    ULONG alloc;
    APIRET rc;
    ULONG flags;

    while (*text == ' ') text++;
    len = strlen(text) + 1;
    alloc = (ULONG)((len + 4095U) & ~4095U);
    if (alloc == 0)
        alloc = 4096;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    /* Giveable+gettable so other processes can map the block. */
    flags = PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE | OBJ_GETTABLE;
    rc = DosAllocSharedMem((PPVOID)&mem, NULL, alloc, flags);
    if (rc != 0 || !mem) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:DosAllocSharedMem failed\n");
        return;
    }
    memset(mem, 0, alloc);
    memcpy(mem, text, len);

    if (!WinOpenClipbrd(hab)) {
        DosFreeMem(mem);
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:WinOpenClipbrd failed\n");
        return;
    }
    WinEmptyClipbrd(hab);
    if (!WinSetClipbrdData(hab, (ULONG)mem, CF_TEXT, CFI_POINTER)) {
        WinCloseClipbrd(hab);
        DosFreeMem(mem);
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:WinSetClipbrdData failed\n");
        return;
    }
    /* System owns mem after successful Set with CFI_POINTER. */
    WinCloseClipbrd(hab);

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

/*
 * CLICK <x> <y> <button>: wire coords are top-left origin (same as our BMP
 * screenshots). PM pointer coords are bottom-left â€” convert before use.
 *
 * Cross-session VIOâ†’PM: also BM_CLICK the window under the pointer
 * (do not walk parents â€” that hits a dialog's default pushbutton).
 */
static void handle_click(const char *args) {
    int x = 0, y = 0, button = 1;
    int n;
    HAB hab;
    HMQ hmq = NULLHANDLE;
    LONG cy;
    POINTL ptl, ptlScreen;
    HWND hwnd;
    ULONG msgDown, msgUp;

    n = sscanf(args, "%d %d %d", &x, &y, &button);
    if (n < 2) {
        send_cstr("ERR:bad CLICK syntax\n");
        return;
    }
    if (n < 3) button = 1;
    if (button < 1 || button > 3) {
        send_cstr("ERR:bad button (use 1/2/3)\n");
        return;
    }

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    cy = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    if (cy <= 0) cy = 480;
    ptlScreen.x = x;
    /* Match screenshot top-left â†” PM bottom-left (no off-by-one). */
    ptlScreen.y = cy - y;
    ptl = ptlScreen;

    if (!WinSetPointerPos(HWND_DESKTOP, ptl.x, ptl.y)) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:WinSetPointerPos failed\n");
        return;
    }
    DosSleep(30);

    hwnd = WinWindowFromPoint(HWND_DESKTOP, &ptlScreen, TRUE);
    if (hwnd == NULLHANDLE) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:no window under point\n");
        return;
    }

    WinSetActiveWindow(HWND_DESKTOP, hwnd);
    WinFocusChange(HWND_DESKTOP, hwnd, 0);

    /* Only BM_CLICK the window under the pointer â€” walking parents hits
     * the dialog's default pushbutton (e.g. Search instead of Cancel). */
    WinPostMsg(hwnd, BM_CLICK, 0, 0);

    ptl = ptlScreen;
    WinMapWindowPoints(HWND_DESKTOP, hwnd, &ptl, 1);
    if (button == 1) {
        msgDown = WM_BUTTON1DOWN;
        msgUp = WM_BUTTON1UP;
    } else if (button == 2) {
        msgDown = WM_BUTTON2DOWN;
        msgUp = WM_BUTTON2UP;
    } else {
        msgDown = WM_BUTTON3DOWN;
        msgUp = WM_BUTTON3UP;
    }
    WinPostMsg(hwnd, msgDown, MPFROM2SHORT((SHORT)ptl.x, (SHORT)ptl.y), 0);
    DosSleep(40);
    WinPostMsg(hwnd, msgUp, MPFROM2SHORT((SHORT)ptl.x, (SHORT)ptl.y), 0);

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

/* Close every top-level frame whose title matches (case-insensitive). */
static int close_frames_by_title(const char *want) {
    HWND h;
    int n = 0;
    char title[160];

    h = WinQueryWindow(HWND_DESKTOP, QW_TOP);
    while (h != NULLHANDLE) {
        title[0] = '\0';
        WinQueryWindowText(h, sizeof(title), title);
        if (title[0] && stricmp(title, want) == 0) {
            WinSetActiveWindow(HWND_DESKTOP, h);
            /* Dialogs often want DID_CANCEL; frames want SC_CLOSE/WM_CLOSE. */
            WinPostMsg(h, WM_COMMAND, MPFROM2SHORT(2, 0), 0);
            WinPostMsg(h, WM_SYSCOMMAND, MPFROMSHORT(SC_CLOSE), 0);
            WinPostMsg(h, WM_CLOSE, 0, 0);
            n++;
        }
        h = WinQueryWindow(h, QW_NEXT);
    }
    return n;
}

typedef struct { const char *name; USHORT vk; } Os2KeyName;

/* Named keys for KEY <keyspec> â€” OS/2 VK_* codes (not Win32). */
static const Os2KeyName OS2_KEY_NAMES[] = {
    {"enter", VK_ENTER}, {"return", VK_ENTER},
    {"esc", VK_ESC}, {"escape", VK_ESC},
    {"tab", VK_TAB}, {"space", VK_SPACE},
    {"backspace", VK_BACKSPACE}, {"bksp", VK_BACKSPACE},
    {"delete", VK_DELETE}, {"del", VK_DELETE},
    {"insert", VK_INSERT}, {"ins", VK_INSERT},
    {"home", VK_HOME}, {"end", VK_END},
    {"pageup", VK_PAGEUP}, {"pgup", VK_PAGEUP},
    {"pagedown", VK_PAGEDOWN}, {"pgdn", VK_PAGEDOWN},
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
    {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
    {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
    {"pause", VK_PAUSE},
    {NULL, 0}
};

static int lookup_os2_key(const char *tok, USHORT *vkOut) {
    int i;
    for (i = 0; OS2_KEY_NAMES[i].name; i++) {
        if (stricmp(tok, OS2_KEY_NAMES[i].name) == 0) {
            *vkOut = OS2_KEY_NAMES[i].vk;
            return 1;
        }
    }
    return 0;
}

/*
 * Post a WM_CHAR (and WM_VIOCHAR) down/up pair to hwnd.
 * mp1 = flags | (repeat/scancode); mp2 = char | virtual-key.
 */
static void post_wm_char(HWND hwnd, USHORT fs, USHORT ch, USHORT vk) {
    MPARAM mp1, mp2;

    if (hwnd == NULLHANDLE) return;
    mp1 = MPFROM2SHORT(fs, 1); /* repeat count 1, scancode 0 */
    mp2 = MPFROM2SHORT(ch, vk);
    WinPostMsg(hwnd, WM_CHAR, mp1, mp2);
    WinPostMsg(hwnd, WM_VIOCHAR, mp1, mp2);
    DosSleep(10);
    mp1 = MPFROM2SHORT((USHORT)(fs | KC_KEYUP | KC_PREVDOWN), 1);
    WinPostMsg(hwnd, WM_CHAR, mp1, mp2);
    WinPostMsg(hwnd, WM_VIOCHAR, mp1, mp2);
}

static HWND focus_hwnd(void) {
    HWND hwnd = WinQueryFocus(HWND_DESKTOP);
    if (hwnd == NULLHANDLE)
        hwnd = WinQueryActiveWindow(HWND_DESKTOP);
    return hwnd;
}

/*
 * KEY <keyspec>: same grammar as Windows agent â€” "enter", "a", "shift-a",
 * "ctrl-c", "alt-f4", "esc". Injects WM_CHAR to the focus window.
 * Esc also tries dialog Cancel / close titled Search (legacy helper).
 */
static void handle_key(const char *keyspec) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    HWND hwnd;
    char buf[128];
    char tokens[8][32];
    int ntok = 0, i;
    int ctrl = 0, alt = 0, shift = 0;
    char *tok;
    USHORT vk = 0;
    USHORT ch = 0;
    USHORT fs = 0;
    const char *base;

    strncpy(buf, keyspec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    /* trim trailing CR/spaces */
    {
        int n = (int)strlen(buf);
        while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\r' || buf[n - 1] == '\n'))
            buf[--n] = '\0';
    }

    tok = strtok(buf, "-");
    while (tok && ntok < 8) {
        strncpy(tokens[ntok], tok, sizeof(tokens[0]) - 1);
        tokens[ntok][sizeof(tokens[0]) - 1] = '\0';
        ntok++;
        tok = strtok(NULL, "-");
    }
    if (ntok == 0) {
        send_cstr("ERR:empty key\n");
        return;
    }

    for (i = 0; i < ntok - 1; i++) {
        if (stricmp(tokens[i], "ctrl") == 0 || stricmp(tokens[i], "control") == 0)
            ctrl = 1;
        else if (stricmp(tokens[i], "alt") == 0)
            alt = 1;
        else if (stricmp(tokens[i], "shift") == 0)
            shift = 1;
        else {
            send_cstr("ERR:unknown modifier\n");
            return;
        }
    }

    base = tokens[ntok - 1];
    if (lookup_os2_key(base, &vk)) {
        fs = KC_VIRTUALKEY;
        ch = 0;
        if (vk == VK_ENTER || vk == VK_NEWLINE) {
            fs |= KC_CHAR;
            ch = '\r';
        } else if (vk == VK_TAB) {
            fs |= KC_CHAR;
            ch = '\t';
        } else if (vk == VK_SPACE) {
            fs |= KC_CHAR;
            ch = ' ';
        } else if (vk == VK_BACKSPACE) {
            fs |= KC_CHAR;
            ch = 0x08;
        } else if (vk == VK_ESC) {
            fs |= KC_CHAR;
            ch = 0x1b;
        }
    } else if (strlen(base) == 1) {
        ch = (USHORT)(unsigned char)base[0];
        fs = KC_CHAR;
        vk = 0;
        if (ch >= 'A' && ch <= 'Z')
            shift = 1;
    } else {
        send_cstr("ERR:unknown key name\n");
        return;
    }

    if (ctrl) fs |= KC_CTRL;
    if (alt) fs |= KC_ALT;
    if (shift) fs |= KC_SHIFT;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    hwnd = focus_hwnd();
    if (hwnd == NULLHANDLE) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:no focus window\n");
        return;
    }

    post_wm_char(hwnd, fs, ch, vk);

    /* Esc: also nudge modal dialogs that ignore synthetic WM_CHAR. */
    if (vk == VK_ESC) {
        close_frames_by_title("Search");
        WinPostMsg(hwnd, WM_COMMAND, MPFROM2SHORT(2, 0), 0);
    }

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

/* TYPE <text>: post each character as WM_CHAR to the focus window. */
static void handle_type(const char *text) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    HWND hwnd;
    const char *p;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    hwnd = focus_hwnd();
    if (hwnd == NULLHANDLE) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:no focus window\n");
        return;
    }

    for (p = text; *p; p++) {
        USHORT ch = (USHORT)(unsigned char)*p;
        USHORT fs = KC_CHAR;
        if (ch >= 'A' && ch <= 'Z')
            fs |= KC_SHIFT;
        if (ch == '\n' || ch == '\r')
            continue; /* use KEY enter */
        post_wm_char(hwnd, fs, ch, 0);
        DosSleep(15);
    }

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

/* WINCLOSE <title>: close top-level window(s) with that title. */
static void handle_winclose(const char *title) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    int n;

    while (*title == ' ') title++;
    if (!*title) {
        send_cstr("ERR:need window title\n");
        return;
    }

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    n = close_frames_by_title(title);

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    if (n)
        send_cstr("OK\n");
    else
        send_cstr("ERR:no matching window\n");
}

static void handle_client(void) {
    net_reset();
    if (recv_line(g_line, sizeof(g_line)) < 0) return;
    if (g_token[0] == '\0' || strcmp(g_line, g_token) != 0) {
        send_cstr("FAIL\n");
        return;
    }
    send_cstr("OK\n");

    for (;;) {
        if (recv_line(g_line, sizeof(g_line)) < 0) break;

        if (strncmp(g_line, "EXECDETACH ", 11) == 0) {
            run_exec_detach(g_line + 11);
        } else if (strncmp(g_line, "EXEC ", 5) == 0) {
            run_exec(g_line + 5);
        } else if (strncmp(g_line, "PUT ", 4) == 0) {
            handle_put(g_line + 4);
        } else if (strncmp(g_line, "GET ", 4) == 0) {
            handle_get(g_line + 4);
        } else if (strcmp(g_line, "SCREENSHOT") == 0) {
            handle_screenshot();
        } else if (strncmp(g_line, "CLICK ", 6) == 0) {
            handle_click(g_line + 6);
        } else if (strncmp(g_line, "KEY ", 4) == 0) {
            handle_key(g_line + 4);
        } else if (strncmp(g_line, "WINCLOSE ", 9) == 0) {
            handle_winclose(g_line + 9);
        } else if (strncmp(g_line, "TYPE ", 5) == 0) {
            handle_type(g_line + 5);
        } else if (strcmp(g_line, "PSLIST") == 0) {
            handle_pslist();
        } else if (strncmp(g_line, "PSKILL ", 7) == 0) {
            handle_pskill(g_line + 7);
        } else if (strcmp(g_line, "SYSINFO") == 0) {
            handle_sysinfo();
        } else if (strcmp(g_line, "REBOOT") == 0) {
            handle_reboot();
            break;
        } else if (strcmp(g_line, "SHUTDOWN") == 0) {
            handle_shutdown();
        } else if (strcmp(g_line, "WINLIST") == 0) {
            handle_winlist();
        } else if (strncmp(g_line, "CLIPSET ", 8) == 0) {
            handle_clipset(g_line + 8);
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
        printf("sock_init failed â€” is SO32DLL/TCP32DLL on LIBPATH and INET up?\n");
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
    write_pid_file();
    minimize_self();

    for (;;) {
        struct os2_sockaddr_in peer;
        int peerlen = sizeof(peer);
        g_client = accept(ls, (struct sockaddr *)&peer, &peerlen);
        if (g_client < 0) {
            cpu_idle();
            continue;
        }
        {
            int nonblocking = 1;
            if (os2_ioctl(g_client, IBM_FIONBIO, (char *)&nonblocking,
                             sizeof(nonblocking)) < 0) {
                soclose(g_client);
                g_client = -1;
                continue;
            }
        }
        handle_client();
        soclose(g_client);
        g_client = -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    {
        PTIB tib; PPIB pib; ULONG ticks = 0;
        if (DosGetInfoBlocks(&tib, &pib) == 0 && pib &&
            DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ticks, sizeof(ticks)) == 0)
            update_identity_init(argc > 0 ? argv[0] : NULL, ticks, pib->pib_ulpid);
    }
    load_config(argc > 0 ? argv[0] : NULL);
    /* DosStartSession often leaves cwd as \, which breaks EXEC's LLMOUT.TMP */
    if (g_exedir[0]) DosSetCurrentDir(g_exedir);
    ensure_shell_env();
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "/?") == 0)) {
        printf("llm_agent-os2 - OS/2 exec/file/screenshot agent (SO32DLL)\n");
        printf("Usage: LLMAGENT.EXE\n");
        return 0;
    }
    return server_main();
}
