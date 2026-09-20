/*
 * llm_agent (OS/2 1.3): 16-bit OS/2 1.x port of the legacy Windows / FreeDOS /
 * OS/2 2.x llm_agent.
 *
 * Same wire protocol as ../agent-os2/llm_agent.c so mcp-server/server.py can
 * drive an OS/2 1.3 VM without a protocol fork. OS/2 1.3 has no 32-bit
 * kernel at all (that arrived with 2.0), so this is a genuinely different
 * build: 16-bit NE via Open Watcom's os21x header set (-i=%WATCOM%\h\os21x),
 * not a recompile of ../agent-os2 for a smaller target.
 *
 * Uses IBM TCPIPDLL (16-bit Berkeley-sockets-workalike) for networking and
 * 16-bit Presentation Manager (Win.../Gpi... calls) for screenshot, click,
 * key, type, winlist. PM itself is not 32-bit-only - only Watcom's 2.x-era
 * os2.h header is (it hard-errors on a 16-bit compile); os21x is Watcom's
 * real 16-bit OS/2 1.x header set and does have PM declarations. See
 * README.md.
 *
 * Supported: auth, PING, QUIT, EXEC, EXECDETACH, PUT, GET, SYSINFO,
 *            SCREENSHOT, CLICK, KEY, TYPE, WINCLOSE, WINLIST, PSLIST (via
 *            C:\OS2\PSTAT.EXE - no DosQProcStatus equivalent in the 1.x
 *            kernel headers, but a real userspace utility does the same
 *            job - see handle_pslist), PSKILL, SELFEXIT (update.exe-only -
 *            see its handler for why it exists), REBOOT (delegated to
 *            IORESET.EXE + IOSEG.DLL, which reset the hardware from an
 *            I/O privilege segment at ring 2 - see handle_reboot and
 *            ioseg.c; both files must sit next to LLMAGENT.EXE)
 * Unsupported: CLIPSET (16-bit PM clipboard needs giveable-segment plumbing
 *              not yet validated live - see README), REG*, SHUTDOWN
 */

#define INCL_DOS
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#define INCL_WIN
#define INCL_WINSWITCHLIST
#define INCL_GPI
#define INCL_DEV
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <process.h>
#include <dos.h>

#include "os2sock.h"

#ifndef NULLHANDLE
#define NULLHANDLE ((LHANDLE)0)
#endif

#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     512
#define READ_CHUNK       4096
#define PID_FILE         "AGENT.PID"
#define BOOT_LOG         "AGTBOOT.LOG"
#define BOOT_LOG_MAX     32000L
#define ERR_NOSUP        "ERR:not supported on OS/2 1.3\n"
#define OS2_CMD_EXE      "C:\\OS2\\CMD.EXE"

static char g_token[128] = "";
static unsigned short g_port = DEFAULT_PORT;
static char g_exedir[128] = ".";
static int g_client = -1;

static char g_bootlog[160];

static char g_line[LINE_MAX_LEN];
static char g_cmd[LINE_MAX_LEN + 64];
static char g_iobuf[READ_CHUNK];
static char g_tmppath[160];

static void cpu_idle(void) {
    DosSleep(20);
}

static int network_wait(int writing) {
    /* Wake as soon as the socket is ready, with a bounded wait so the
       application deadline is checked even if the peer sends nothing.
       DosSleep rounds up to scheduler ticks and stalls small TCP windows. */
    int fd = g_client;
    return socket_select(&fd, writing ? 0 : 1, writing ? 1 : 0, 0, 20L);
}

static PGINFOSEG g_info;
static unsigned long network_ticks(void) { return g_info->msecs; }
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
        n = send(g_client, (char *)(buf + sent), len - sent, 0);
        if (n > 0) {
            sent += n;
            deadline = NET_DEADLINE(NET_IO_SECONDS);
        } else if (network_wait(1) < 0) return net_fail();
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
        if (network_wait(0) < 0) return net_fail();
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

/*
 * Startup/shutdown log, next to the .EXE.
 *
 * The agent's own messages go to its session window, and that window goes
 * away with the process - so an agent that starts and then stops leaves
 * no evidence at all of why. That is not hypothetical: an agent seen
 * exiting right after printing "listening on port 2222" could have been a
 * failed bind, a dead TCP stack, a crash, or a perfectly ordinary
 * SELFEXIT from update.exe, and from the console there was no way to
 * tell. Same reasoning as the Windows 9x agent's agent_boot.log - see
 * docs/ARCHITECTURE.md.
 *
 * Deliberately cheap and self-limiting: opened and closed per line (this
 * agent has to live within OS/2 1.x's 20-handle-per-process budget), and
 * restarted from scratch once it passes BOOT_LOG_MAX so an agent that
 * restarts in a loop can't fill the disk.
 */
static void boot_log(const char *fmt, ...) {
    va_list ap;
    DATETIME dt;
    FILE *f;
    long size = 0;

    if (!g_bootlog[0]) return;

    f = fopen(g_bootlog, "rb");
    if (f) {
        if (fseek(f, 0L, SEEK_END) == 0) size = ftell(f);
        fclose(f);
    }
    f = fopen(g_bootlog, size > BOOT_LOG_MAX ? "w" : "a");
    if (!f) return;

    memset(&dt, 0, sizeof(dt));
    DosGetDateTime(&dt);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ",
            (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
            (unsigned)dt.hours, (unsigned)dt.minutes, (unsigned)dt.seconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void load_config(const char *argv0) {
    char path[160];
    char line[256];
    FILE *f;

    dirname_of(argv0 ? argv0 : ".", g_exedir, sizeof(g_exedir));
    sprintf(g_bootlog, "%s\\%s", g_exedir, BOOT_LOG);
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

/* A spawnv relaunch (self-update, or just an odd CONFIG.SYS) can leave an
 * empty PATH/COMSPEC, which breaks Watcom's system() - it looks up CMD.EXE
 * via the environment. Same fix as the 2.x agent for the same reason. */
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
    PIDINFO pi;

    memset(&pi, 0, sizeof(pi));
    if (DosGetPID(&pi) != 0) return;
    sprintf(path, "%s\\%s", g_exedir, PID_FILE);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%u\n", (unsigned)pi.pid);
    fclose(f);
}

/*
 * Minimize our own session - NOT CURRENTLY CALLED (see server_main). Kept
 * for reference/future revisit, not deleted, because getting this far was
 * expensive: live-confirmed on os2-13 that WinCreateMsgQueue fails
 * immediately after listen() (the exact same WinInitialize/
 * WinCreateMsgQueue pattern works fine once a real client has connected -
 * handle_screenshot/handle_click/handle_key/handle_winlist all rely on
 * it), yet WinQuerySwitchHandle and WinSetWindowPos both still report
 * success even with a NULL message queue - and the window still never
 * visually minimizes. Retrying WinCreateMsgQueue for up to 15s made no
 * difference (still fails every time) and just delayed startup for
 * nothing, so that retry loop was removed along with the call site.
 * Leading theory, unconfirmed: a VIO session's switch-list entry isn't a
 * real PM frame window, so SWP_MINIMIZE may simply not apply to it -
 * WinSetWindowPos "succeeds" (doesn't error) without doing anything.
 * Revisit only with a plan to actually root-cause this rather than retry
 * further blind - see docs/ARCHITECTURE.md.
 */
static void minimize_self(void) {
    HAB hab;
    HMQ hmq = NULLHANDLE;
    PIDINFO pi;
    HSWITCH hsw;
    SWCNTRL swctl;

    memset(&pi, 0, sizeof(pi));
    if (DosGetPID(&pi) != 0) return;

    hab = WinInitialize(0);
    if (!hab) return;
    hmq = WinCreateMsgQueue(hab, 0);

    hsw = WinQuerySwitchHandle(NULLHANDLE, pi.pid);
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
 * Confirmed live: this VM is slow to release a file handle after the
 * CMD.EXE child that held it exits - a rapid-fire second EXEC landing on
 * the same fixed temp name before the first one's handle is truly gone
 * fails with SYS0032 ("being used by another process"), visible on the
 * agent's own console. Same underlying slow-release pattern already hit
 * during self-update testing (see docs/ARCHITECTURE.md), just here it's
 * consecutive EXEC calls instead of a rename. Fixed with a small rotating
 * pool of temp names instead of one fixed name, so back-to-back EXEC
 * calls (exactly what an LLM harness driving this agent would do) don't
 * collide even if OS/2 hasn't let go of the previous one yet.
 */
#define EXEC_TMP_SLOTS 8
static unsigned g_exec_slot = 0;

static int run_exec(const char *cmdline) {
    FILE *f;
    int rc;
    size_t n;
    char hdr[32];

    sprintf(g_tmppath, "LLMOUT%u.TMP", g_exec_slot);
    g_exec_slot = (g_exec_slot + 1) % EXEC_TMP_SLOTS;

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

/*
 * EXECDETACH: spawnv(P_NOWAIT, ...) instead of DosStartSession - the CRT
 * spawn functions are plain OS/2 kernel DosExecPgm underneath, return
 * immediately with the real child PID, and need none of DosStartSession's
 * hand-built STARTDATA/PgmInputs string. Command line may use "quoted"
 * program/arg tokens like the Windows/2.x agent.
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

#define MAX_ARGV_TOKENS 32

static int run_exec_detach(const char *cmdline) {
    static char toks[MAX_ARGV_TOKENS][160];
    const char *argv[MAX_ARGV_TOKENS + 1];
    const char *p = cmdline;
    int n = 0;
    int rc;
    char reply[64];

    while (n < MAX_ARGV_TOKENS && next_token(&p, toks[n], sizeof(toks[n])))
        n++;
    if (n == 0) {
        send_cstr("ERR:empty EXECDETACH command\n");
        return -1;
    }
    {
        int i;
        for (i = 0; i < n; i++) argv[i] = toks[i];
        argv[n] = NULL;
    }

    rc = spawnv(P_NOWAIT, argv[0], argv);
    if (rc < 0) {
        sprintf(reply, "ERR:spawnv failed (errno=%d)\n", errno);
        send_cstr(reply);
        return -1;
    }
    sprintf(reply, "OK pid=%d\n", rc);
    send_cstr(reply);
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

/*
 * PSLIST: no DosQProcStatus equivalent exists in the 1.x kernel headers
 * (see file header), but OS/2 1.3 ships a real, general-purpose process/
 * thread dump utility that solves the same problem from userspace:
 * C:\OS2\PSTAT.EXE, present since 1.0/1.1 (dated 11-14-91 on the real
 * os2-13 box - confirmed live, along with the exact output format below).
 * Bare invocation prints three space-separated sections: process/thread
 * table, semaphores, then shared memory/RTL library trees - we only need
 * the first.
 *
 * Table format (confirmed against real live output):
 *   " 0013      0007       10      CMD         01      0400     FFE00013  Blocked\n"
 *     PID(hex) PPID(hex) SessID(hex) Name(<=8ch)  TID  Priority  BlockID  State
 * Continuation lines for a process's 2nd+ thread repeat only the last four
 * columns, left-padded so column 1 (PID) is blank - that's exactly what
 * distinguishes a new process row from a thread continuation: a process
 * row's first whitespace-separated token is a 4-hex-digit PID, a
 * continuation row's first token is a 1-2 digit thread ID (or the row is
 * blank). The header/title lines ("Process and Thread Information",
 * column headers) fail that same 4-hex-digit check and fall out for free.
 */
#define PSTAT_EXE "C:\\OS2\\PSTAT.EXE"
#define PSTAT_TMP "PSTAT.TMP"

static int is_4hex(const char *s) {
    int i;
    if (strlen(s) != 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int handle_pslist(void) {
    /* Keep the large PSLIST buffer outside DGROUP so startup identity
       and SYSINFO retain the full 16 KiB stack within the 64 KiB limit. */
    static char __far out[16384];
    static char line[200];
    int len = 0;
    char hdr[32];
    FILE *f;
    char tmppath[160];
    char cmd[128];
    char *dir = g_exedir[0] ? g_exedir : ".";

    sprintf(tmppath, "%s\\%s", dir, PSTAT_TMP);
    remove(tmppath);
    sprintf(cmd, "CMD.EXE /C %s > %s", PSTAT_EXE, tmppath);
    system(cmd);

    f = fopen(tmppath, "rb");
    if (!f) {
        send_cstr("ERR:cannot run PSTAT.EXE\n");
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *tok[8];
        int ntok = 0;
        char *t;

        if (strstr(line, "System Semaphore") != NULL)
            break;

        t = strtok(line, " \t\r\n");
        while (t && ntok < 8) {
            tok[ntok++] = t;
            t = strtok(NULL, " \t\r\n");
        }
        if (ntok < 4 || !is_4hex(tok[0]))
            continue;

        if (len > (int)sizeof(out) - 160)
            break;
        len += sprintf(out + len, "%lu\t%s\r\n", strtoul(tok[0], NULL, 16), tok[3]);
    }
    fclose(f);
    remove(tmppath);

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || (len > 0 && send_all(out, len) < 0))
        return -1;
    return 0;
}

/* PSKILL <pid>: DosKillProcess - same trust model as EXEC. */
static void handle_pskill(const char *args) {
    PID pid;
    USHORT rc;
    char reply[64];

    while (*args == ' ') args++;
    pid = (PID)atoi(args);
    if (pid == 0) {
        send_cstr("ERR:bad PID\n");
        return;
    }

    rc = DosKillProcess(0 /* DKP_PROCESS */, pid);
    if (rc != 0) {
        sprintf(reply, "ERR:DosKillProcess rc=%u\n", (unsigned)rc);
        send_cstr(reply);
        return;
    }
    send_cstr("OK\n");
}

static int handle_sysinfo(void) {
    static char buf[1024];
    int len = 0;
    char hdr[32];
    USHORT ver = 0;
    unsigned major = 1, minor = 30;
    struct diskfree_t df;

    /* DosGetVersion byte layout, confirmed live against the real os2-13
     * box (kernel reports 1.30; EXEC "ver" agrees): low byte = minor
     * version as-is (30), high byte = major *generation* number times 10
     * (10, i.e. "1.x") - NOT the DOS int21h/AH=30h convention (AL=major,
     * AH=minor) this originally assumed, which gave major=30/minor=10
     * (backwards, and the family digit unscaled). Falls back to the
     * hardcoded 1.30 default if the call itself fails. */
    if (DosGetVersion(&ver) == 0) {
        minor = ver & 0xFF;
        major = ((ver >> 8) & 0xFF) / 10;
    }

    len += sprintf(buf + len, "agent_exe=%s\r\nagent_sha256=%s\r\nagent_started=%s\r\n",
                   g_update_exe, g_update_sha256, g_update_started);
    len += sprintf(buf + len, "os_family=os2\r\n");
    len += sprintf(buf + len, "os2_major=%u\r\n", major);
    len += sprintf(buf + len, "os2_minor=%u\r\n", minor);

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

    len += sprintf(buf + len, "agent=llm_agent-os2-13\r\n");

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

/* ---- SCREENSHOT: 16-bit PM desktop via WinGetScreenPS -> 24-bit BMP ---- */

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
    BITMAPINFOHEADER bmih;
    BITMAPINFO *pbmi = NULL;
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
    hmq = WinCreateMsgQueue(hab, 0);

    cx = WinQuerySysValue(HWND_DESKTOP, SV_CXSCREEN);
    cy = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    if (cx <= 0 || cy <= 0 || cx > 2048 || cy > 2048) {
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
    bmih.cx = (USHORT)cx;
    bmih.cy = (USHORT)cy;
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

    pbmi = (BITMAPINFO *)calloc(1, sizeof(BITMAPINFO));
    if (!pbmi) {
        send_cstr("ERR:oom\n");
        goto done;
    }
    pbmi->cbFix = sizeof(BITMAPINFOHEADER);
    pbmi->cx = (USHORT)cx;
    pbmi->cy = (USHORT)cy;
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

/* Close every top-level frame whose title matches (case-insensitive). */
static int close_frames_by_title(const char *want) {
    HWND h;
    int n = 0;
    char title[160];

    h = WinQueryWindow(HWND_DESKTOP, QW_TOP, FALSE);
    while (h != NULLHANDLE) {
        title[0] = '\0';
        WinQueryWindowText(h, sizeof(title), title);
        if (title[0] && stricmp(title, want) == 0) {
            WinSetActiveWindow(HWND_DESKTOP, h);
            WinPostMsg(h, WM_COMMAND, MPFROM2SHORT(2, 0), 0);
            WinPostMsg(h, WM_SYSCOMMAND, MPFROMSHORT(SC_CLOSE), 0);
            WinPostMsg(h, WM_CLOSE, 0, 0);
            n++;
        }
        h = WinQueryWindow(h, QW_NEXT, FALSE);
    }
    return n;
}

/*
 * CLICK <x> <y> <button>: wire coords are top-left origin (same as our BMP
 * screenshots). PM pointer coords are bottom-left - convert before use.
 */
static void handle_click(const char *args) {
    int x = 0, y = 0, button = 1;
    int n;
    HAB hab;
    HMQ hmq = NULLHANDLE;
    LONG cy;
    POINTL ptl, ptlScreen;
    HWND hwnd;
    USHORT msgDown, msgUp;

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
    ptlScreen.y = cy - y;
    ptl = ptlScreen;

    if (!WinSetPointerPos(HWND_DESKTOP, (SHORT)ptl.x, (SHORT)ptl.y)) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:WinSetPointerPos failed\n");
        return;
    }
    DosSleep(30);

    hwnd = WinWindowFromPoint(HWND_DESKTOP, &ptlScreen, TRUE, FALSE);
    if (hwnd == NULLHANDLE) {
        if (hmq) WinDestroyMsgQueue(hmq);
        WinTerminate(hab);
        send_cstr("ERR:no window under point\n");
        return;
    }

    WinSetActiveWindow(HWND_DESKTOP, hwnd);
    WinFocusChange(HWND_DESKTOP, hwnd, 0);

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

typedef struct { const char *name; USHORT vk; } Os2KeyName;

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

static void post_wm_char(HWND hwnd, USHORT fs, USHORT ch, USHORT vk) {
    MPARAM mp1, mp2;

    if (hwnd == NULLHANDLE) return;
    mp1 = MPFROM2SHORT(fs, 1);
    mp2 = MPFROM2SHORT(ch, vk);
    WinPostMsg(hwnd, WM_CHAR, mp1, mp2);
    WinPostMsg(hwnd, WM_VIOCHAR, mp1, mp2);
    DosSleep(10);
    mp1 = MPFROM2SHORT((USHORT)(fs | KC_KEYUP | KC_PREVDOWN), 1);
    WinPostMsg(hwnd, WM_CHAR, mp1, mp2);
    WinPostMsg(hwnd, WM_VIOCHAR, mp1, mp2);
}

static HWND focus_hwnd(void) {
    HWND hwnd = WinQueryFocus(HWND_DESKTOP, FALSE);
    if (hwnd == NULLHANDLE)
        hwnd = WinQueryActiveWindow(HWND_DESKTOP, FALSE);
    return hwnd;
}

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

    if (vk == VK_ESC) {
        close_frames_by_title("Search");
        WinPostMsg(hwnd, WM_COMMAND, MPFROM2SHORT(2, 0), 0);
    }

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

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
            continue;
        post_wm_char(hwnd, fs, ch, 0);
        DosSleep(15);
    }

    if (hmq) WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    send_cstr("OK\n");
}

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

static void scrub_field(char *s) {
    for (; *s; s++) {
        if (*s == '\t' || *s == '\r' || *s == '\n')
            *s = ' ';
    }
}

/*
 * WINLIST: same wire format as Windows/2.x agent -
 *   "<hwnd>\t<x>\t<y>\t<w>\t<h>\t<class>\t<title>\r\n"
 * Enumerate via WinQuerySwitchList (Window List entries), same as 2.x.
 */
static int handle_winlist(void) {
    static char buf[16384];
    HAB hab;
    HMQ hmq = NULLHANDLE;
    USHORT cb;
    USHORT nentries;
    PSWBLOCK psw = NULL;
    LONG cy;
    int len = 0;
    char hdr[32];
    char cls[128];
    USHORT i;
    int rc_out = -1;

    hab = WinInitialize(0);
    if (!hab) {
        send_cstr("ERR:WinInitialize failed\n");
        return -1;
    }
    hmq = WinCreateMsgQueue(hab, 0);

    cy = WinQuerySysValue(HWND_DESKTOP, SV_CYSCREEN);
    if (cy <= 0) cy = 480;

    nentries = WinQuerySwitchList(hab, NULL, 0);
    if (nentries == 0) {
        send_cstr("ERR:WinQuerySwitchList empty\n");
        goto done;
    }
    cb = (USHORT)(sizeof(USHORT) + (nentries + 2) * sizeof(SWENTRY));
    psw = (PSWBLOCK)malloc(cb);
    if (!psw) {
        send_cstr("ERR:out of memory\n");
        goto done;
    }
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
            WinMapWindowPoints(WinQueryWindow(hwnd, QW_PARENT, FALSE), HWND_DESKTOP,
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

/*
 * REBOOT. Hands off to IORESET.EXE, detached, and returns - the same
 * shape as the 2.x agent's REBOOT.EXE helper. OK is sent before any of
 * it, matching every other agent's REBOOT/SHUTDOWN convention.
 *
 * The reset itself needs direct hardware port access, so it lives in
 * IORESET.EXE + IOSEG.DLL (see ioreset.c / ioseg.c) rather than here:
 * OS/2 1.x only allows IN/OUT from a code segment marked IOPL at link
 * time, entered through a ring-2 call gate, and that constrains how the
 * whole module is compiled and linked. Keeping it in a separate helper
 * also means a missing or broken IOSEG.DLL can only break REBOOT instead
 * of stopping the agent from starting.
 *
 * What this replaced, and why none of it ever worked - all four earlier
 * attempts were real-mode DOS .COM stubs (keyboard-controller pulse
 * reset, Ctrl-Alt-Del scancode injection, BIOS warm-boot vector jump,
 * 0xCF9 chipset reset) spawned into a DOS box, plus OEMHLP$/DOS$ IOCTLs
 * ported from the 2.x agent:
 *
 *   - This machine's CONFIG.SYS has PROTECTONLY=YES, so it has no DOS box
 *     at all. Confirmed live: any DOS binary, .COM or COMMAND.COM itself,
 *     fails to start. Three of the four mechanisms never executed a single
 *     instruction, which is why they all "failed" identically despite
 *     being genuinely different mechanisms. spawnl()'s return code was
 *     discarded, so nothing ever said so.
 *   - The IOCTLs did run, and did nothing. OEMHLP$ is a 2.x device.
 *
 * The working mechanism is the mundane one - out 0xFE to port 0x64, the
 * 8042's pulse-reset line, exactly what a PC/AT does for Ctrl-Alt-Del -
 * just executed from protected mode at ring 2 instead of from a DOS box
 * that does not exist. CONFIG.SYS already had the IOPL=YES this needs.
 *
 * IORESET.EXE is invoked with FLUSH, so it calls DosShutdown() first.
 * That matters and is not optional here: this box runs HPFS386 with a
 * lazy-write cache, and resetting without it both left the volume dirty
 * enough for AUTOCHECK to run CHKDSK on every boot and, once, silently
 * shredded a file that had been written seconds earlier. See ioreset.c
 * for the ordering constraints that come with DosShutdown.
 */
static void handle_reboot(void) {
    char path[160];
    int rc;

    boot_log("REBOOT requested - handing off to IORESET.EXE");
    send_cstr("OK\n");

    sprintf(path, "%s\\IORESET.EXE", g_exedir[0] ? g_exedir : ".");
    rc = spawnl(P_NOWAIT, path, path, "KBD", "FLUSH", NULL);
    if (rc < 0) {
        /* OK already went out, so the client will never hear about this,
         * and IORESET.LOG won't exist either if the helper never started.
         * Leave the reason somewhere findable instead of discarding it,
         * which is precisely how the old .COM attempts stayed a mystery.
         * A handle-starved agent fails exactly here, too. */
        boot_log("REBOOT FAILED: could not spawn %s, errno=%d", path, errno);
    }
}

static void handle_shutdown(void) {
    send_cstr(ERR_NOSUP);
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
            send_cstr(ERR_NOSUP);
        } else if (strncmp(g_line, "REGGET\t", 7) == 0 || strncmp(g_line, "REGSET\t", 7) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "PING") == 0) {
            send_cstr("PONG\n");
        } else if (strcmp(g_line, "QUIT") == 0) {
            break;
        } else if (strcmp(g_line, "SELFEXIT") == 0) {
            /* Voluntary exit, used by update.exe instead of DosKillProcess:
             * OS/2 only allows killing descendants, and update.exe is a
             * *child* of this agent (EXECDETACH -> spawnv), so it can never
             * legally kill its own parent (DosKillProcess fails with
             * ERROR_NOT_DESCENDANT, rc=305 - confirmed live against the
             * real os2-13 box). Exiting ourselves on request sidesteps that
             * restriction entirely. */
            boot_log("EXIT: SELFEXIT requested by a client (normally "
                     "update.exe starting a self-update)");
            send_cstr("OK\n");
            exit(0);
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

    printf("llm_agent-os2-13: sock_init...\n");
    fflush(stdout);
    if (sock_init() != 0) {
        printf("sock_init failed - is TCPIPDLL on LIBPATH and INET up?\n");
        boot_log("EXIT: sock_init failed - TCPIPDLL missing from LIBPATH, "
                 "or the TCP/IP stack was not up yet");
        return 1;
    }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("socket() failed (rc=%d)\n", ls);
        boot_log("EXIT: socket() failed rc=%d", ls);
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    /*
     * Retry bind() for a while instead of failing immediately. Confirmed
     * live: right after a SELFEXIT-driven self-update, the just-exited old
     * agent's listening socket on this same port isn't released instantly
     * on this VM - a fresh instance spawned immediately afterward (as
     * update.exe does) can hit bind() before the old socket is actually
     * gone, even with SO_REUSEADDR already set. Same shape as the Windows
     * agent's own WSAStartup/bind retry loop for the analogous early-boot
     * RunServices race (see docs/ARCHITECTURE.md).
     */
    {
        int i;
        int bound = 0;
        for (i = 0; i < 30; i++) { /* up to ~60s */
            if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                bound = 1;
                break;
            }
            DosSleep(2000);
        }
        if (!bound) {
            printf("bind(%u) failed after retries\n", (unsigned)g_port);
            boot_log("EXIT: bind(%u) failed after %d attempts (~60s) - "
                     "another agent is almost certainly already running",
                     (unsigned)g_port, i);
            soclose(ls);
            return 1;
        }
        if (i > 0)
            boot_log("bind(%u) succeeded after %d retr%s",
                     (unsigned)g_port, i, i == 1 ? "y" : "ies");
    }
    if (listen(ls, 1) < 0) {
        printf("listen failed\n");
        boot_log("EXIT: listen() failed");
        soclose(ls);
        return 1;
    }

    printf("llm_agent-os2-13: listening on port %u\n", (unsigned)g_port);
    printf("token configured: %s\n", g_token[0] ? "yes" : "NO - set token= in LLMAGENT.INI");
    fflush(stdout);
    write_pid_file();
    {
        PIDINFO pi;
        memset(&pi, 0, sizeof(pi));
        DosGetPID(&pi);
        boot_log("LISTENING on port %u, pid=%u, token=%s",
                 (unsigned)g_port, (unsigned)pi.pid,
                 g_token[0] ? "yes" : "NO");
    }
    /* minimize_self() not called - see its own comment for why. */

    for (;;) {
        struct sockaddr_in peer;
        int peerlen = sizeof(peer);
        g_client = accept(ls, (struct sockaddr *)&peer, &peerlen);
        if (g_client < 0) {
            cpu_idle();
            continue;
        }
        {
            int nonblocking = 1;
            if (socket_ioctl(g_client, FIONBIO, (char *)&nonblocking) < 0) {
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
        SEL global_sel, local_sel;
        if (DosGetInfoSeg(&global_sel, &local_sel) != 0) return 1;
        g_info = MAKEPGINFOSEG(global_sel);
    }
    {
        PIDINFO pi;
        if (DosGetPID(&pi) == 0)
            update_identity_init(argc > 0 ? argv[0] : NULL, g_info->msecs, (unsigned long)pi.pid);
    }
    load_config(argc > 0 ? argv[0] : NULL);
    ensure_shell_env();
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "/?") == 0)) {
        printf("llm_agent-os2-13 - OS/2 1.3 exec/file/screenshot agent (TCPIPDLL)\n");
        printf("Usage: LLMAGENT.EXE\n");
        printf("Reads port/token from LLMAGENT.INI next to the exe.\n");
        return 0;
    }
    boot_log("=== starting: exedir=%s port=%u ===", g_exedir, (unsigned)g_port);
    return server_main();
}
