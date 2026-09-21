/*
 * llm_agent (NetWare NLM): NetWare 3.12+ port of the legacy llm_agent.
 *
 * Same wire protocol as agent-win32/llm_agent.c so mcp-server/server.py can drive a
 * NetWare server without a protocol fork. Uses CLIB BSD sockets (TCP/IP
 * Transport must already be loaded).
 *
 * Supported: auth, PING, QUIT, EXEC, PUT, GET, SYSINFO, SCREENSHOT,
 *            SHUTDOWN, UPDATE, DEBUG, KEY, TYPE, SCREENS
 * Unsupported: EXECDETACH, CLICK, WINLIST, CLIPSET, REG*, PSLIST,
 *              PSKILL, REBOOT
 *
 * EXEC: CLIB system() â€” typically a console command; no stdout capture
 * (LEN:0 + EXIT:rc). Prefer PUT of an .NCF/.NLM and EXEC that by name.
 * SCREENSHOT: Install via StuffKey DUMP only (no CopyFromScreenMemory â€”
 *             that hangs/abends on NWSNUT). DUMP has no color attrs so
 *             menu highlight is not visible. Else System Console copy.
 * KEY/TYPE: ungetch via ScanScreens handle + SetCurrentScreen (no
 *           CreateScreen bind of System Console/Install â€” that GPFs on UNLOAD).
 * SCREENS: list ScanScreens names (SIZE: text) for menu discovery.
 * SHUTDOWN: OK then DownFileServer(1) (force down â€” lab use).
 * UPDATE: prepare protocol-2 helper, acknowledge, then self-exit for a recoverable swap.
 * AUTOEXEC: ensure SYS:SYSTEM\AUTOEXEC.NCF has LOAD CLIBAUX + LOAD LLMAGENT.
 * DEBUG: DEBUG / DEBUG 0 / DEBUG 1 â€” runtime verbose flag; when on, also
 *        appends to SYS:SYSTEM\LLMAGENT.LOG (truncated on DEBUG 1).
 * Config: prefer SYS:SYSTEM\LLMAGENT.INI (A: is fallback only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Watcom's macro inspects its private FILE layout. The 3.12 build uses
 * Novell CLIB FILEs instead, so use the runtime function at the ABI boundary.
 * Both builds resolve this function in Novell CLIB. */
#undef ferror

#include "nwsock.h"
#include "../common/update_identity.h"
#include "update_state.h"
#include "font8.h"

#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     512
#define READ_CHUNK       1024
#define ERR_NOSUP        "ERR:not supported on NetWare\n"
#define SCR_MAX_COLS     80
#define SCR_MAX_ROWS     50
#define LOG_PATH         "SYS:SYSTEM\\LLMAGENT.LOG"
#define STUFFKEY_NLM     "SYS:SYSTEM\\STUFFKEY.NLM"
#define CLIBAUX_NLM      "SYS:SYSTEM\\CLIBAUX.NLM"
/* Script + tiny NCF wrapper â€” system() on 3.12 truncates ~32 chars. */
#define STUFFKEY_SCRIPT  "SYS:SYSTEM\\L.SK"
#define STUFFKEY_NCF     "SYS:SYSTEM\\SK.NCF"
#define STUFFKEY_RUN     "SK"
#define SKDUMP_LOG       "SYS:SYSTEM\\MD.TXT"
#define AUTOEXEC_NCF     "SYS:SYSTEM\\AUTOEXEC.NCF"
#define AUTOEXEC_MAX     8192
#define AUTOEXEC_NEW     "SYS:SYSTEM\\LLMAUTO.NEW"
#define AUTOEXEC_BAK     "SYS:SYSTEM\\LLMAUTO.BAK"
#include "autoexec.h"
/* CreateScreen flags (nwconio.h) â€” must not steal the operator console. */
#define NW_DONT_AUTO_ACTIVATE  0x01
#define NW_DONT_SWITCH_SCREEN  0x02
#define NW_AUTO_DESTROY_SCREEN 0x20 /* no "Press any key to close screen" */

static char g_token[128] = "";
static unsigned short g_port = DEFAULT_PORT;
static int g_client = -1;
static int g_listen = -1;
static int g_our_screen = -1;
static volatile int g_running = 1;
static int g_debug = 0; /* INI debug= or runtime DEBUG command */
static volatile int g_allow_unload = 0; /* set before UPDATE so check won't prompt */

static char g_line[LINE_MAX_LEN];
static char g_iobuf[READ_CHUNK];

static int stuffkey_present(void);
static int ensure_clibaux(void);
static int screen_name_ignored(const char *name);
static int is_system_console_name(const char *name);

static int file_exists_rb(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void on_unload(void) {
    g_running = 0;
    if (g_client >= 0) {
        close(g_client);
        g_client = -1;
    }
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
    /*
     * Do not DestroyScreen here â€” NetWare 3.12 has abended in the console
     * command process when UNLOAD runs AtUnload that tears down screens.
     * The loader frees NLM-owned screens when the module exits.
     */
    g_our_screen = -1;
}

/* Never block UNLOAD with a prompt â€” automated UPDATE / lab ops need silence.
 * (Returning nonzero yields "Unload module anyway?" and can strand UPDATE.)
 */
int llm_agent_check(void) {
    (void)g_allow_unload;
    (void)g_client;
    return 0;
}

static void cpu_idle(void) {
    ThreadSwitchWithDelay();
}

/* Verbose console + append to LLMAGENT.LOG when g_debug is on. */
static void debug_puts(const char *msg) {
    FILE *f;
    if (!g_debug || !msg) return;
    ConsolePrintf("%s", msg);
    f = fopen(LOG_PATH, "a");
    if (f) {
        fputs(msg, f);
        fclose(f);
    }
}

#define NET_DEADLINE(sec) ((unsigned long)GetCurrentTicks() + (sec) * 18UL)
#define NET_EXPIRED(d) ((long)((unsigned long)GetCurrentTicks() - (d)) >= 0)
#include "../common/session_timeout.h"
#include "../common/command_line.h"

static int send_all(const char *buf, int len) {
    int sent = 0;
    unsigned long deadline = NET_DEADLINE(NET_IO_SECONDS);
    while (sent < len) {
        int n;
        if (net_expired(deadline) || !g_running) return net_fail();
        n = send(g_client, (char *)(buf + sent), len - sent, 0);
        if (n > 0) {
            sent += n;
            deadline = NET_DEADLINE(NET_IO_SECONDS);
        } else cpu_idle(); /* zero and negative results make no progress */
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
        if (net_expired(deadline) || !g_running) return net_fail();
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

static void load_config(void) {
    static const char *paths[] = {
        "SYS:SYSTEM\\LLMAGENT.INI",
        "SYS:ETC\\LLMAGENT.INI",
        "A:LLMAGENT.INI",
        "A:\\LLMAGENT.INI",
        "LLMAGENT.INI",
        NULL
    };
    char line[256];
    char msg[160];
    FILE *f = NULL;
    const char *used = NULL;
    int i;

    for (i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (f) {
            used = paths[i];
            break;
        }
    }
    if (!f) {
        /* g_debug still 0 â€” quiet unless already on somehow */
        return;
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
        } else if (strncmp(line, "debug=", 6) == 0) {
            g_debug = atoi(line + 6) ? 1 : 0;
        }
    }
    fclose(f);
    if (g_debug && used) {
        sprintf(msg, "LLMAGENT: config %s\r\n", used);
        debug_puts(msg);
    }
}

/*
 * NetWare has no shell redirection. system() runs a console-style command;
 * we report exit status only (LEN:0).
 */
static int run_exec(const char *cmdline) {
    int rc;
    char hdr[32];

    if (!cmdline || !cmdline[0]) {
        send_cstr("ERR:empty command\n");
        return -1;
    }
    if (strlen(cmdline) >= LINE_MAX_LEN) {
        send_cstr("ERR:command too long\n");
        return -1;
    }

    if (g_debug) {
        char msg[300];
        sprintf(msg, "LLMAGENT: EXEC %.200s\r\n", cmdline);
        debug_puts(msg);
    }
    rc = system(cmdline);
    sprintf(hdr, "LEN:0\nEXIT:%d\n", rc);
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
        ThreadSwitch();
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
        ThreadSwitch();
    }
    fclose(f);
    return 0;
}

static int handle_sysinfo(void) {
    static char buf[1280];
    char last_update[192];
    int len = 0;
    char hdr[32];
    FILE_SERV_INFO si;
    char volName[32];
    WORD totalBlocks = 0, sectorsPerBlock = 0, availBlocks = 0;
    WORD totalDir = 0, availDir = 0, removable = 0;

    memset(&si, 0, sizeof(si));
    len += sprintf(buf + len, "os_family=netware\r\n");

    if (GetServerInformation((int)sizeof(si), &si) == 0) {
        len += sprintf(buf + len, "server_name=%s\r\n", si.serverName);
        len += sprintf(buf + len, "netware_major=%u\r\n", (unsigned)si.netwareVersion);
        len += sprintf(buf + len, "netware_minor=%u\r\n", (unsigned)si.netwareSubVersion);
        len += sprintf(buf + len, "netware_revision=%u\r\n", (unsigned)si.revisionLevel);
        len += sprintf(buf + len, "connections_in_use=%u\r\n", (unsigned)si.connectionsInUse);
        len += sprintf(buf + len, "max_connections=%u\r\n", (unsigned)si.maxConnectionsSupported);
    } else {
        len += sprintf(buf + len, "server_name=?\r\n");
        len += sprintf(buf + len, "netware_major=?\r\n");
        len += sprintf(buf + len, "netware_minor=?\r\n");
    }

    volName[0] = '\0';
    if (GetVolumeInfoWithNumber(0, volName, &totalBlocks, &sectorsPerBlock,
                                &availBlocks, &totalDir, &availDir, &removable) == 0) {
        unsigned long bps = (unsigned long)sectorsPerBlock * 512UL;
        unsigned long total_b = (unsigned long)totalBlocks * bps;
        unsigned long free_b = (unsigned long)availBlocks * bps;
        len += sprintf(buf + len, "sys_volume=%s\r\n", volName[0] ? volName : "SYS");
        len += sprintf(buf + len, "sys_total_mb=%lu\r\n", total_b / (1024UL * 1024UL));
        len += sprintf(buf + len, "sys_free_mb=%lu\r\n", free_b / (1024UL * 1024UL));
    } else {
        len += sprintf(buf + len, "sys_volume=?\r\n");
        len += sprintf(buf + len, "sys_total_mb=?\r\n");
        len += sprintf(buf + len, "sys_free_mb=?\r\n");
    }

    len += sprintf(buf + len, "agent=llm_agent-netware\r\n");
    len += sprintf(buf + len, "agent_build=%s %s\r\n", __DATE__, __TIME__);
    len += sprintf(buf + len, "agent_exe=%s\r\nagent_started=%s\r\nagent_sha256=%s\r\n",
                   g_update_exe, g_update_started, g_update_sha256);
    len += sprintf(buf + len, "update_protocol=2\r\nupdate_state=%s\r\n",
                   FindNLMHandle("UPDATE.NLM") ? "busy" :
                   (nw_path_state(NW_WORK) == 0 ? "idle" : "recovery-required"));
    if (nw_read_text(NW_LAST, last_update, sizeof(last_update)) >= 0 && !strpbrk(last_update, "\r\n"))
        len += sprintf(buf + len, "update_last=%s\r\n", last_update);
    len += sprintf(buf + len, "stuffkey=%d\r\n", stuffkey_present());
    len += sprintf(buf + len, "clibaux=%d\r\n", file_exists_rb(CLIBAUX_NLM));
    len += sprintf(buf + len, "debug=%d\r\n", g_debug);

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

static void cga_rgb(unsigned char attr, unsigned char *r, unsigned char *g, unsigned char *b) {
    static const unsigned char pal[16][3] = {
        {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
        {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
        {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
        {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255}
    };
    unsigned char fg = (unsigned char)(attr & 0x0F);
    *r = pal[fg][0];
    *g = pal[fg][1];
    *b = pal[fg][2];
}

static void bg_rgb(unsigned char attr, unsigned char *r, unsigned char *g, unsigned char *b) {
    static const unsigned char pal[16][3] = {
        {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
        {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
        {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
        {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255}
    };
    unsigned char bg = (unsigned char)((attr >> 4) & 0x07);
    *r = pal[bg][0];
    *g = pal[bg][1];
    *b = pal[bg][2];
}

/* Spaces are not evidence of captured content. */
static int count_printable(const unsigned char *cells, int ncells) {
    int i, n = 0;
    for (i = 0; i < ncells; i++) {
        unsigned char ch = cells[i * 2];
        if (ch > 32 && ch < 127) n++;
    }
    return n;
}

/* ScanScreens returns OS IDs (possibly negative), not CLIB handles.
 * Only IDs obtained by scanning may be passed to GetScreenInfo: probing
 * arbitrary integers can abend the server. CreateScreen can attach a
 * CLIB handle to an existing OS ID without creating a new screen. */
static int screen_handle(int id) {
    int handle;
    if (id == 0) return -1;
    handle = GetScreenInfo(id, NULL, NULL);
    if (handle == 0) handle = CreateScreen((const char *)id, 0);
    return handle == 0 ? -1 : handle;
}

static int screen_displayed(int id) {
    int handle = screen_handle(id);
    return handle != -1 && CheckIfScreenDisplayed(handle, 0) == 1;
}

/* Restore the thread group's I/O context, not the operator's display. */
static void restore_operator_screen(int prefer_handle) {
    int id = 0;
    char name[64];
    LONG attr = 0;
    if (prefer_handle != 0 && prefer_handle != -1) {
        SetCurrentScreen(prefer_handle);
        return;
    }
    while ((id = ScanScreens(id, name, &attr)) != 0) {
        if (is_system_console_name(name)) {
            int handle = screen_handle(id);
            if (handle != -1) SetCurrentScreen(handle);
            return;
        }
    }
}

static int try_copy_screen(int id, unsigned char *cells, WORD *rowsP, WORD *colsP) {
    WORD rows = 25, cols = 80;
    int handle = screen_handle(id);
    if (handle == -1) return -1;
    SetCurrentScreen(handle);
    /* CLIB revisions differ in the return value; verify the actual context. */
    if (GetCurrentScreen() != handle) return -1;
    if (GetSizeOfScreen(&rows, &cols) != 0 || rows == 0 || cols == 0)
        return -1;
    if (rows > SCR_MAX_ROWS) rows = SCR_MAX_ROWS;
    if (cols > SCR_MAX_COLS) cols = SCR_MAX_COLS;
    memset(cells, 0, (unsigned)rows * (unsigned)cols * 2);
    CopyFromScreenMemory(rows, cols, cells, 0, 0);
    *rowsP = rows;
    *colsP = cols;
    return count_printable(cells, (int)rows * (int)cols);
}

/*
 * CopyFromScreenMemory hangs/abends on Install/NWSNUT on this 3.12 box.
 * StuffKey <DUMP> already works â€” parse that into a text cell buffer.
 */
static int parse_skdump_to_cells(unsigned char *cells, WORD *rowsP, WORD *colsP) {
    FILE *f;
    char line[128];
    long last_pos = -1;
    long pos;
    int row;
    int cols = 80;
    int rows = 25;
    int i;

    f = fopen(SKDUMP_LOG, "rb");
    if (!f) return -1;

    /* Find the last "Screen:" header (StuffKey may append). */
    pos = 0;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        if (strncmp(line, "Screen:", 7) == 0)
            last_pos = pos;
        pos = ftell(f);
    }
    if (last_pos < 0) {
        fclose(f);
        return -1;
    }
    fseek(f, last_pos, SEEK_SET);

    /* Skip Screen: line, then dashed separator. */
    if (fgets(line, (int)sizeof(line), f) == NULL) {
        fclose(f);
        return -1;
    }
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        if (line[0] == '-' || line[0] == '=')
            break;
    }

    memset(cells, 0, (unsigned)rows * (unsigned)cols * 2);
    row = 0;
    while (row < rows && fgets(line, (int)sizeof(line), f) != NULL) {
        int len;
        if (line[0] == '-' && line[1] == '-')
            break; /* next dump header */
        if (strncmp(line, "Screen:", 7) == 0)
            break;
        len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        for (i = 0; i < cols; i++) {
            unsigned char ch = (i < len) ? (unsigned char)line[i] : (unsigned char)' ';
            cells[(row * cols + i) * 2] = ch;
            cells[(row * cols + i) * 2 + 1] = 0x07;
        }
        row++;
    }
    fclose(f);
    if (row <= 0) return -1;
    *rowsP = (WORD)row;
    *colsP = (WORD)cols;
    return count_printable(cells, row * cols);
}

static int ensure_clibaux(void);

/* Write SK.NCF and run it â€” avoids system() 32-char truncation of LOAD lines. */
static int run_stuffkey_ncf(const char *flags) {
    FILE *f;
    int i;
    f = fopen(STUFFKEY_NCF, "wb");
    if (!f) return -1;
    /* Full path + flags live in the NCF, not the system() argv. */
    fprintf(f, "LOAD STUFFKEY SYS:SYSTEM/L.SK %s\r\n", flags ? flags : "/sr /d=80");
    fclose(f);
    system(STUFFKEY_RUN);
    /*
     * StuffKey exits after the script, but rapid LOAD/UNLOAD leaves zombies
     * and has abended TCP/IP here ("how many zombies?"). Cool down before
     * the next LOAD STUFFKEY.
     */
    for (i = 0; i < 20; i++) {
        delay(100);
        ThreadSwitchWithDelay();
    }
    return 0;
}

static int capture_install_via_stuffkey(const char *name, unsigned char *cells, WORD *rowsP, WORD *colsP) {
    FILE *f;

    if (!stuffkey_present()) return -1;
    if (ensure_clibaux() != 0) return -1;

    f = fopen(STUFFKEY_SCRIPT, "wb");
    if (!f) return -1;
    fprintf(f, "<SCREEN=%s>\n", name);
    fprintf(f, "<LOG NEW=SYS:SYSTEM/MD.TXT>\n");
    fprintf(f, "<DUMP>\n");
    fclose(f);

    if (g_debug)
        debug_puts("LLMAGENT: SCREENSHOT via StuffKey DUMP (Install)\r\n");

    if (run_stuffkey_ncf("/sr /d=50") != 0) return -1;
    restore_operator_screen(-1);
    return parse_skdump_to_cells(cells, rowsP, colsP);
}

static int capture_console_text(unsigned char *cells, WORD *rowsP, WORD *colsP) {
    int saved = GetCurrentScreen();
    int id = 0, best_score = 0, best_priority = -1;
    WORD best_rows = 25, best_cols = 80;
    char name[64];
    LONG attr = 0;
    static unsigned char tmp[SCR_MAX_ROWS * SCR_MAX_COLS * 2];
    FILE *dbgf = g_debug ? fopen("SYS:SYSTEM\\LLMSCR.DBG", "w") : NULL;

    while ((id = ScanScreens(id, name, &attr)) != 0) {
        WORD r = 25, c = 80;
        int score, priority;
        if (screen_name_ignored(name)) continue;
        /* NWSNUT/Install must use StuffKey: direct copying can abend. */
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL) {
            score = capture_install_via_stuffkey(name, tmp, &r, &c);
            priority = 3;
        } else {
            priority = screen_displayed(id) ? 2 :
                       (is_system_console_name(name) ? 1 : 0);
            score = try_copy_screen(id, tmp, &r, &c);
        }
        if (dbgf) fprintf(dbgf, "screen %d '%s' content=%d priority=%d %ux%u\n",
                          id, name, score, priority, (unsigned)r, (unsigned)c);
        /* Never turn a blank/failed capture into a success with a priority bonus. */
        if (score > 0 && (priority > best_priority ||
            (priority == best_priority && score > best_score))) {
            best_score = score;
            best_priority = priority;
            best_rows = r;
            best_cols = c;
            memcpy(cells, tmp, (unsigned)r * (unsigned)c * 2);
        }
        ThreadSwitchWithDelay();
    }
    restore_operator_screen(saved);
    if (dbgf) fclose(dbgf);
    if (best_score <= 0) return 0;
    *rowsP = best_rows;
    *colsP = best_cols;
    return best_score;
}

static int handle_screenshot(void) {
    WORD rows = 25, cols = 80;
    const int cw = 8, ch = 8;
    int width, height;
    unsigned long rowBytes, imageSize, fileSize;
    char hdr[32];
    unsigned char fileHdr[14];
    unsigned char infoHdr[40];
    static unsigned char cells[SCR_MAX_ROWS * SCR_MAX_COLS * 2];
    static unsigned char linebuf[SCR_MAX_COLS * 8 * 3 + 4];
    int py;
    int score;

    memset(cells, 0, sizeof(cells));
    score = capture_console_text(cells, &rows, &cols);
    if (score <= 0) {
        send_cstr("ERR:screenshot empty (no console text)\n");
        return -1;
    }

    width = (int)cols * cw;
    height = (int)rows * ch;
    rowBytes = ((unsigned long)width * 3UL + 3UL) & ~3UL;
    imageSize = rowBytes * (unsigned long)height;
    fileSize = 14UL + 40UL + imageSize;

    sprintf(hdr, "SIZE:%lu\n", (unsigned long)fileSize);
    if (send_cstr(hdr) < 0) return -1;

    memset(fileHdr, 0, sizeof(fileHdr));
    fileHdr[0] = 'B'; fileHdr[1] = 'M';
    fileHdr[2] = (unsigned char)(fileSize);
    fileHdr[3] = (unsigned char)(fileSize >> 8);
    fileHdr[4] = (unsigned char)(fileSize >> 16);
    fileHdr[5] = (unsigned char)(fileSize >> 24);
    fileHdr[10] = 54;
    memset(infoHdr, 0, sizeof(infoHdr));
    infoHdr[0] = 40;
    infoHdr[4] = (unsigned char)(width);
    infoHdr[5] = (unsigned char)(width >> 8);
    infoHdr[8] = (unsigned char)(height);
    infoHdr[9] = (unsigned char)(height >> 8);
    infoHdr[12] = 1;
    infoHdr[14] = 24;
    infoHdr[20] = (unsigned char)(imageSize);
    infoHdr[21] = (unsigned char)(imageSize >> 8);
    infoHdr[22] = (unsigned char)(imageSize >> 16);
    infoHdr[23] = (unsigned char)(imageSize >> 24);

    if (send_all((const char *)fileHdr, 14) < 0) return -1;
    if (send_all((const char *)infoHdr, 40) < 0) return -1;

    for (py = height - 1; py >= 0; py--) {
        int cellRow = py / ch;
        int glyphRow = py % ch;
        int px;

        for (px = 0; px < width; px++) {
            int cellCol = px / cw;
            int glyphCol = px % cw;
            unsigned offset = (unsigned)(cellRow * (int)cols + cellCol) * 2;
            unsigned char chv = cells[offset];
            unsigned char a = cells[offset + 1];
            unsigned char fr, fg, fb, br, bg, bb;
            unsigned char bits;
            int on;
            unsigned char ascii = chv;
            unsigned long i;

            cga_rgb(a, &fr, &fg, &fb);
            bg_rgb(a, &br, &bg, &bb);
            if (ascii < 32 || ascii > 127) ascii = 32;
            bits = FONT8[ascii - 32][glyphRow];
            on = (bits >> (7 - glyphCol)) & 1;
            i = (unsigned long)px * 3UL;
            if (on) {
                linebuf[i] = fb; linebuf[i + 1] = fg; linebuf[i + 2] = fr;
            } else {
                linebuf[i] = bb; linebuf[i + 1] = bg; linebuf[i + 2] = br;
            }
        }
        {
            unsigned long used = (unsigned long)width * 3UL;
            while (used < rowBytes) linebuf[used++] = 0;
            if (send_all((const char *)linebuf, (int)rowBytes) < 0) return -1;
        }
        if ((py & 7) == 0) ThreadSwitchWithDelay();
    }
    return 0;
}

/* ---- KEY / TYPE: ungetch into operator-displayed screen ---- */

typedef struct {
    const char *name;
    int code;
} NwKeyName;

static const NwKeyName NW_KEY_NAMES[] = {
    {"enter", '\r'}, {"return", '\r'},
    {"esc", 0x1B}, {"escape", 0x1B},
    {"tab", '\t'}, {"space", ' '},
    {"backspace", 0x08}, {"bksp", 0x08},
    {"delete", 0x5300}, {"del", 0x5300},
    {"insert", 0x5200}, {"ins", 0x5200},
    {"home", 0x4700}, {"end", 0x4F00},
    {"pageup", 0x4900}, {"pgup", 0x4900},
    {"pagedown", 0x5100}, {"pgdn", 0x5100},
    {"up", 0x4800}, {"down", 0x5000},
    {"left", 0x4B00}, {"right", 0x4D00},
    {"f1", 0x3B00}, {"f2", 0x3C00}, {"f3", 0x3D00}, {"f4", 0x3E00},
    {"f5", 0x3F00}, {"f6", 0x4000}, {"f7", 0x4100}, {"f8", 0x4200},
    {"f9", 0x4300}, {"f10", 0x4400},
    {NULL, 0}
};

static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/*
 * Prefer a non-debugger operator screen (INSTALL after LOAD INSTALL).
 * Else bind System Console. Only DisplayScreen for non-console screens â€”
 * Novell scrhand.c stuffs System Console with SetCurrentScreen alone.
 *
 * NOTE: On this CLIB, SetCurrentScreen's return is not a reliable errno
 * (NDK samples treat it as the previous screen handle). Never use
 * `if (SetCurrentScreen(x) != 0)` as a failure check.
 */
static int screen_name_ignored(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "LLMAGENT") != NULL) return 1;
    if (strstr(name, "Debugger") != NULL || strstr(name, "DEBUGGER") != NULL)
        return 1;
    return 0;
}

static int is_system_console_name(const char *name) {
    if (!name || !name[0]) return 1;
    if (strstr(name, "System Console") != NULL) return 1;
    if (strstr(name, "SYSTEM CONSOLE") != NULL) return 1;
    return 0;
}

static int resolve_input_screen(char *nameOut, int nameOutLen) {
    int id = 0;
    char name[64];
    LONG attr = 0;
    int install_id = 0;
    int displayed = 0;
    int any_other = 0;
    char install_name[64];
    char displayed_name[64];
    char any_name[64];

    install_name[0] = '\0';
    displayed_name[0] = '\0';
    any_name[0] = '\0';
    if (nameOut && nameOutLen > 0) nameOut[0] = '\0';
    /* StuffKey only needs the name â€” avoid CreateScreen("LLMAGENT"). */

    while ((id = ScanScreens(id, name, &attr)) != 0) {
        if (screen_name_ignored(name)) continue;
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL) {
            install_id = id;
            strncpy(install_name, name, sizeof(install_name) - 1);
            install_name[sizeof(install_name) - 1] = '\0';
        }
        if (any_other == 0) {
            any_other = id;
            strncpy(any_name, name, sizeof(any_name) - 1);
            any_name[sizeof(any_name) - 1] = '\0';
        }
        if (screen_displayed(id)) {
            displayed = id;
            strncpy(displayed_name, name, sizeof(displayed_name) - 1);
            displayed_name[sizeof(displayed_name) - 1] = '\0';
            if (!is_system_console_name(name))
                break;
        }
    }

    /* Prefer Install Screen when present (menu driving). */
    if (install_id != 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, install_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return install_id;
    }
    if (displayed != 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, displayed_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return displayed;
    }
    if (any_other != 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, any_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return any_other;
    }
    return 0;
}

static int begin_input_stuff(int *savedP) {
    char name[64];
    int target = resolve_input_screen(name, (int)sizeof(name));

    if (target == 0) return -1;
    target = screen_handle(target);
    if (target == -1) return -1;
    *savedP = GetCurrentScreen();
    /*
     * CLIB ungetch only feeds getch/getche screens. INSTALL uses NWSNUT
     * (C-Worthy) and ignores it â€” DisplayScreen does not fix that and has
     * correlated with UNLOAD GPFs. Keep SetCurrentScreen + ungetch for
     * System Console stuffing; INSTALL needs StuffKey (separate helper).
     */
    SetCurrentScreen(target);
    if (GetCurrentScreen() != target) {
        restore_operator_screen(*savedP);
        return -1;
    }
    ThreadSwitchWithDelay();
    if (g_debug) {
        char msg[140];
        sprintf(msg, "LLMAGENT: KEY/TYPE SetCurrent '%s' id=%d\r\n", name, target);
        debug_puts(msg);
    }
    return 0;
}

static void end_input_stuff(int saved) {
    if (saved >= 0 && saved != g_our_screen)
        restore_operator_screen(saved);
    else
        restore_operator_screen(-1);
}

static int stuff_code(int code) {
    int rc = ungetch(code);
    ThreadSwitchWithDelay();
    ThreadSwitchWithDelay();
    if (rc < 0) return -1;
    return 0;
}

/* After stuffing, optionally dump a line of the current screen into the debug log. */
static void debug_dump_input_row(void) {
    unsigned char row[160];
    char line[96];
    int i, n = 0;
    FILE *f;
    if (!g_debug) return;
    memset(row, 0, sizeof(row));
    CopyFromScreenMemory(1, 80, row, 0, 0);
    for (i = 0; i < 80 && n < (int)sizeof(line) - 1; i++) {
        unsigned char ch = row[i * 2];
        line[n++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
    }
    line[n] = '\0';
    f = fopen(LOG_PATH, "a");
    if (f) {
        fprintf(f, "LLMAGENT: screen row0 [%s]\r\n", line);
        fclose(f);
    }
}

static int lookup_key_code(const char *base, int *codeP) {
    int i;
    for (i = 0; NW_KEY_NAMES[i].name; i++) {
        if (name_eq_ci(base, NW_KEY_NAMES[i].name)) {
            *codeP = NW_KEY_NAMES[i].code;
            return 0;
        }
    }
    if (base[0] && base[1] == '\0') {
        *codeP = (unsigned char)base[0];
        return 0;
    }
    return -1;
}

/* StuffKey token names (C-Worthy / INSTALL need this, not CLIB ungetch). */
static int lookup_stuffkey_token(const char *base, char *out, int outlen) {
    static const struct { const char *name; const char *tok; } map[] = {
        {"enter", "CR"}, {"return", "CR"},
        {"esc", "ESC"}, {"escape", "ESC"},
        {"tab", "TAB"}, {"space", " "},
        {"backspace", "BS"}, {"bksp", "BS"},
        {"delete", "DEL"}, {"del", "DEL"},
        {"insert", "INS"}, {"ins", "INS"},
        {"home", "HOME"}, {"end", "END"},
        {"pageup", "PGUP"}, {"pgup", "PGUP"},
        {"pagedown", "PGDN"}, {"pgdn", "PGDN"},
        {"up", "UP"}, {"down", "DN"},
        {"left", "LEFT"}, {"right", "RIGHT"},
        {"f1", "F1"}, {"f2", "F2"}, {"f3", "F3"}, {"f4", "F4"},
        {"f5", "F5"}, {"f6", "F6"}, {"f7", "F7"}, {"f8", "F8"},
        {"f9", "F9"}, {"f10", "F10"},
        {NULL, NULL}
    };
    int i;
    for (i = 0; map[i].name; i++) {
        if (name_eq_ci(base, map[i].name)) {
            if (map[i].tok[0] == ' ' && map[i].tok[1] == '\0') {
                strncpy(out, " ", (unsigned)outlen - 1);
            } else {
                sprintf(out, "<%s>", map[i].tok);
            }
            out[outlen - 1] = '\0';
            return 0;
        }
    }
    if (base[0] && base[1] == '\0') {
        /* Escape StuffKey meta chars. */
        if (base[0] == '<' || base[0] == '\\')
            sprintf(out, "\\%c", base[0]);
        else {
            out[0] = base[0];
            out[1] = '\0';
        }
        return 0;
    }
    return -1;
}

static int stuffkey_present(void) {
    return file_exists_rb(STUFFKEY_NLM);
}

/* StuffKey on 3.12 needs CLIBAUX (TID 2948742); without it LOAD fails silently. */
static int g_clibaux_tried = 0;
static int ensure_clibaux(void) {
    if (!file_exists_rb(CLIBAUX_NLM)) return -1;
    if (!g_clibaux_tried) {
        system("LOAD CLIBAUX");
        g_clibaux_tried = 1;
    }
    return 0;
}

static int needs_stuffkey_screen(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL)
        return 1;
    if (strstr(name, "Monitor") != NULL || strstr(name, "MONITOR") != NULL)
        return 1;
    /* Any non-console / non-debugger menu-ish screen */
    if (!is_system_console_name(name) && !screen_name_ignored(name))
        return 1;
    return 0;
}

/*
 * Write L.SK and run via SK.NCF (system() truncates long LOAD lines on 3.12).
 */
static int run_stuffkey(const char *screen, const char *body) {
    FILE *f;

    if (!stuffkey_present()) {
        send_cstr("ERR:missing SYS:SYSTEM\\STUFFKEY.NLM\n");
        return -1;
    }
    if (ensure_clibaux() != 0) {
        send_cstr("ERR:missing SYS:SYSTEM\\CLIBAUX.NLM (required by StuffKey on 3.12)\n");
        return -1;
    }
    /* Binary write â€” text mode turns \n into \r\r\n and breaks the parser. */
    f = fopen(STUFFKEY_SCRIPT, "wb");
    if (!f) {
        send_cstr("ERR:cannot write L.SK\n");
        return -1;
    }
    fprintf(f, "<SCREEN=%s>\n%s\n", screen, body);
    fclose(f);

    if (g_debug) {
        char msg[200];
        sprintf(msg, "LLMAGENT: STUFFKEY '%s' body=%.60s\r\n", screen, body);
        debug_puts(msg);
    }

    return run_stuffkey_ncf("/sr /d=80");
}

/*
 * Strip ctrl-/alt-/shift- prefixes from one key token.
 * Returns pointer into mutated string at the base key name.
 */
static char *strip_key_modifiers(char *tok) {
    char *dash;
    for (;;) {
        dash = strstr(tok, "-");
        if (!dash || dash == tok) break;
        *dash = '\0';
        if (!name_eq_ci(tok, "ctrl") && !name_eq_ci(tok, "control") &&
            !name_eq_ci(tok, "alt") && !name_eq_ci(tok, "shift")) {
            *dash = '-';
            return NULL;
        }
        tok = dash + 1;
    }
    return tok;
}

/*
 * KEY up
 * KEY up up down enter     â€” one StuffKey run (comma or whitespace separators)
 *
 * Batching matters: one LOAD STUFFKEY per arrow key abends 3.12 with zombies.
 */
static int handle_key(const char *keyspec) {
    char buf[256];
    char body[500];
    char token[32];
    char *p;
    char *tok;
    char *base;
    char *save;
    int code = 0;
    int saved = -1;
    int nkeys = 0;
    int bn = 0;
    int tlen;
    char screen[64];

    if (!keyspec || !keyspec[0]) {
        send_cstr("ERR:empty key\n");
        return -1;
    }
    strncpy(buf, keyspec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (p = buf; *p; p++) {
        if (*p == ',') *p = ' ';
    }

    resolve_input_screen(screen, (int)sizeof(screen));
    if (screen[0] == '\0')
        strncpy(screen, "System Console", sizeof(screen) - 1);

    /* Prefer StuffKey whenever present â€” required for INSTALL/NWSNUT. */
    if (stuffkey_present()) {
        body[0] = '\0';
        bn = 0;
        tok = buf;
        while (*tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (!*tok) break;
            save = tok;
            while (*tok && *tok != ' ' && *tok != '\t') tok++;
            if (*tok) {
                *tok = '\0';
                tok++;
            }
            base = strip_key_modifiers(save);
            if (!base || !base[0]) {
                send_cstr("ERR:unknown modifier\n");
                return -1;
            }
            if (lookup_stuffkey_token(base, token, (int)sizeof(token)) != 0) {
                send_cstr("ERR:unknown key name\n");
                return -1;
            }
            tlen = (int)strlen(token);
            if (bn > 0) {
                if (bn + 1 >= (int)sizeof(body)) {
                    send_cstr("ERR:too many keys\n");
                    return -1;
                }
                body[bn++] = '\n';
            }
            if (bn + tlen >= (int)sizeof(body)) {
                send_cstr("ERR:too many keys\n");
                return -1;
            }
            memcpy(body + bn, token, (unsigned)tlen);
            bn += tlen;
            body[bn] = '\0';
            nkeys++;
        }
        if (nkeys == 0) {
            send_cstr("ERR:empty key\n");
            return -1;
        }
        if (run_stuffkey(screen, body) != 0) return -1;
        send_cstr("OK\n");
        return 0;
    }
    if (needs_stuffkey_screen(screen)) {
        send_cstr("ERR:missing SYS:SYSTEM\\STUFFKEY.NLM (needed for INSTALL menus)\n");
        return -1;
    }

    /* ungetch path: only the first key (console fallback). */
    tok = buf;
    while (*tok == ' ' || *tok == '\t') tok++;
    save = tok;
    while (*tok && *tok != ' ' && *tok != '\t') tok++;
    *tok = '\0';
    if (!save[0]) {
        send_cstr("ERR:empty key\n");
        return -1;
    }
    base = strip_key_modifiers(save);
    if (!base) {
        send_cstr("ERR:unknown modifier\n");
        return -1;
    }
    if (lookup_key_code(base, &code) != 0) {
        send_cstr("ERR:unknown key name\n");
        return -1;
    }
    if (begin_input_stuff(&saved) != 0) {
        send_cstr("ERR:no input screen\n");
        return -1;
    }
    if (stuff_code(code) != 0) {
        end_input_stuff(saved);
        send_cstr("ERR:ungetch failed\n");
        return -1;
    }
    debug_dump_input_row();
    end_input_stuff(saved);
    send_cstr("OK\n");
    return 0;
}

static int handle_type(const char *text) {
    const char *p;
    int saved = -1;
    char screen[64];
    char body[400];
    int n = 0;

    if (!text) {
        send_cstr("ERR:empty text\n");
        return -1;
    }

    resolve_input_screen(screen, (int)sizeof(screen));
    if (screen[0] == '\0')
        strncpy(screen, "System Console", sizeof(screen) - 1);

    if (stuffkey_present()) {
        for (p = text; *p && n < (int)sizeof(body) - 3; p++) {
            if (*p == '<' || *p == '\\') {
                body[n++] = '\\';
                body[n++] = *p;
            } else {
                body[n++] = *p;
            }
        }
        body[n] = '\0';
        if (run_stuffkey(screen, body) != 0) return -1;
        send_cstr("OK\n");
        return 0;
    }
    if (needs_stuffkey_screen(screen)) {
        send_cstr("ERR:missing SYS:SYSTEM\\STUFFKEY.NLM (needed for INSTALL menus)\n");
        return -1;
    }

    if (begin_input_stuff(&saved) != 0) {
        send_cstr("ERR:no input screen\n");
        return -1;
    }
    for (p = text; *p; p++) {
        if (stuff_code((unsigned char)*p) != 0) break;
    }
    end_input_stuff(saved);
    send_cstr("OK\n");
    return 0;
}

static int handle_screens(void) {
    static char buf[2048];
    int len = 0;
    int id = 0;
    char name[64];
    LONG attr = 0;
    char hdr[32];

    /* No CreateScreen â€” listing must not invent an LLMAGENT Ctrl+Esc entry. */
    while ((id = ScanScreens(id, name, &attr)) != 0) {
        int n;
        int disp = screen_name_ignored(name) ? 0 : screen_displayed(id);
        if (len >= (int)sizeof(buf) - 96) break;
        n = sprintf(buf + len, "%d\t%d\t%s\r\n", id, disp, name);
        if (n < 0) break;
        len += n;
    }
    if (len == 0) {
        len = sprintf(buf, "(no screens)\r\n");
    }
    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

static int handle_shutdown(void) {
    ConsolePrintf("LLMAGENT: SHUTDOWN â€” DownFileServer(1)\r\n");
    if (send_cstr("OK\n") < 0) return -1;
    g_running = 0;
    DownFileServer(1);
    return 0;
}

/* Prepare the helper while still serving; only self-exit after its checked
   readiness-to-swap record matches the current and staged fingerprints. */
static int handle_update(void) {
    char wanted[65], current[65], prepared[132], expected[132];
    int i, prepared_ok = 0;
    unsigned long started;
    if (!nw_is_target(g_update_exe) || strlen(g_update_sha256) != 64) {
        send_cstr("ERR:update requires identified SYS:SYSTEM agent\n"); return -1;
    }
    if (FindNLMHandle("UPDATE.NLM") || nw_path_state(NW_WORK) != 0) {
        send_cstr("ERR:updater busy or unresolved SYS:SYSTEM\\LLMUPD recovery\n"); return -1;
    }
    if (nw_hash(NW_AGENT, current) || strcmp(current, g_update_sha256) || nw_hash(NW_STAGE, wanted)) {
        send_cstr("ERR:update source unreadable or installed file differs from startup\n"); return -1;
    }
    if (!nw_helper_supported()) {
        send_cstr("ERR:update requires readable protocol-2 UPDATE.NLM; agent retained\n"); return -1;
    }
    sprintf(expected, "%s\n%s\n", current, wanted);
    system("LOAD SYS:SYSTEM\\UPDATE.NLM");
    started = (unsigned long)GetCurrentTicks();
    for (i = 0; i < 80 && (((unsigned long)GetCurrentTicks() - started) & 0xffffffffUL) < 144UL; i++) {
        if (FindNLMHandle("UPDATE.NLM") &&
            nw_read_text(NW_PREPARED, prepared, sizeof(prepared)) == 130 && !strcmp(prepared, expected)) { prepared_ok = 1; break; }
        delay(100); ThreadSwitchWithDelay();
    }
    if (!prepared_ok) {
        send_cstr("ERR:updater did not prepare; agent retained; inspect SYS:SYSTEM\\LLMUPD\n"); return -1;
    }
    if (send_cstr("OK\n") < 0) return -1;
    g_allow_unload = 1;
    g_running = 0;
    if (g_client >= 0) { close(g_client); g_client = -1; }
    if (g_listen >= 0) { close(g_listen); g_listen = -1; }
    /* Do not DestroyScreen or use console UNLOAD: both have abended on 3.12. */
    g_our_screen = -1;
    exit(0);
    return 0;
}

/* AUTOEXEC edits only recognized, unconditional loads; see autoexec.h. */
static int autoexec_read(const char *path, char *buf, int *n) {
    FILE *f = fopen(path, "rb");
    int failed;
    if (!f) return -1;
    *n = (int)fread(buf, 1, AUTOEXEC_MAX, f);
    failed = ferror(f);
    if (fclose(f) != 0) failed = 1;
    if (failed) return -1;
    return *n == AUTOEXEC_MAX ? -2 : 0;
}

static int handle_autoexec(void) {
    static char original[AUTOEXEC_MAX], updated[AUTOEXEC_MAX];
    FILE *f;
    int n, planned, check, result, failed;
    const char *reply;

    result = autoexec_read(AUTOEXEC_NCF, original, &n);
    if (result) return send_cstr(result == -2 ? "ERR:AUTOEXEC.NCF too large\n" :
                                "ERR:cannot completely read/close AUTOEXEC.NCF\n");
    result = autoexec_plan(original, n, updated, AUTOEXEC_MAX - 1, &planned);
    if (result == 0) return send_cstr("OK autoexec=present\n");
    if (result < 0) {
        if (result == -2) reply = "ERR:AUTOEXEC ambiguous/duplicate/optional/unload entries; review manually\n";
        else if (result == -3) reply = "ERR:AUTOEXEC dependency order; CLIBAUX and network setup must precede LLMAGENT\n";
        else if (result == -4) reply = "ERR:AUTOEXEC.NCF too large\n";
        else reply = "ERR:AUTOEXEC malformed text or command; review manually\n";
        return send_cstr(reply);
    }
    /* Do not overwrite recovery artifacts from an interrupted transaction.
       A later edit needs the retained backup moved aside by the operator. */
    if (file_exists_rb(AUTOEXEC_NEW) || file_exists_rb(AUTOEXEC_BAK))
        return send_cstr("ERR:AUTOEXEC recovery files exist; review LLMAUTO.NEW/BAK\n");
    f = fopen(AUTOEXEC_NEW, "wb");
    if (!f) return send_cstr("ERR:cannot stage AUTOEXEC update\n");
    failed = (int)fwrite(updated, 1, (size_t)planned, f) != planned;
    if (fflush(f) != 0 || ferror(f)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (!failed) {
        if (autoexec_read(AUTOEXEC_NEW, original, &check) != 0 || check != planned)
            failed = 1;
        else {
            int i;
            for (i = 0; i < planned; i++) if (original[i] != updated[i]) { failed = 1; break; }
        }
    }
    if (failed) {
        remove(AUTOEXEC_NEW);
        return send_cstr("ERR:AUTOEXEC staging write/flush/close/readback failed; original unchanged\n");
    }
    if (rename(AUTOEXEC_NCF, AUTOEXEC_BAK) != 0) {
        remove(AUTOEXEC_NEW);
        return send_cstr("ERR:cannot back up AUTOEXEC.NCF; original unchanged\n");
    }
    if (rename(AUTOEXEC_NEW, AUTOEXEC_NCF) != 0) {
        if (rename(AUTOEXEC_BAK, AUTOEXEC_NCF) != 0)
            return send_cstr("ERR:AUTOEXEC restore failed; recover LLMAUTO.BAK locally\n");
        remove(AUTOEXEC_NEW);
        return send_cstr("ERR:AUTOEXEC install failed; original restored\n");
    }
    if (g_debug) debug_puts("LLMAGENT: AUTOEXEC updated; backup LLMAUTO.BAK\r\n");
    return send_cstr(result == 1 ? "OK autoexec=added\n" :
                     result == 2 ? "OK autoexec=added-llmagent\n" : "OK autoexec=added-clibaux\n");
}

/*
 * DEBUG / DEBUG 0 / DEBUG 1 â€” runtime verbose toggle.
 * DEBUG 1 truncates SYS:SYSTEM\LLMAGENT.LOG so a following GET is a clean session.
 */
static int handle_debug(const char *args) {
    char reply[32];
    FILE *f;

    while (args && *args == ' ') args++;
    if (args && args[0]) {
        if (strcmp(args, "1") == 0 || strcmp(args, "on") == 0 || strcmp(args, "ON") == 0) {
            g_debug = 1;
            f = fopen(LOG_PATH, "w");
            if (f) {
                fputs("LLMAGENT: debug on\r\n", f);
                fclose(f);
            }
            ConsolePrintf("LLMAGENT: debug on (log %s)\r\n", LOG_PATH);
        } else if (strcmp(args, "0") == 0 || strcmp(args, "off") == 0 || strcmp(args, "OFF") == 0) {
            if (g_debug) {
                debug_puts("LLMAGENT: debug off\r\n");
                ConsolePrintf("LLMAGENT: debug off\r\n");
            }
            g_debug = 0;
        } else {
            send_cstr("ERR:usage DEBUG [0|1]\n");
            return -1;
        }
    }
    sprintf(reply, "OK debug=%d\n", g_debug);
    return send_cstr(reply);
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
            handle_key(g_line + 4);
        } else if (strncmp(g_line, "TYPE ", 5) == 0) {
            handle_type(g_line + 5);
        } else if (strcmp(g_line, "PSLIST") == 0 || strncmp(g_line, "PSKILL ", 7) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "SYSINFO") == 0) {
            handle_sysinfo();
        } else if (strcmp(g_line, "SCREENS") == 0) {
            handle_screens();
        } else if (strcmp(g_line, "REBOOT") == 0) {
            send_cstr(ERR_NOSUP);
        } else if (strcmp(g_line, "SHUTDOWN") == 0) {
            handle_shutdown();
            break;
        } else if (strcmp(g_line, "UPDATE") == 0) {
            handle_update();
            break;
        } else if (strcmp(g_line, "AUTOEXEC") == 0) {
            handle_autoexec();
        } else if (strcmp(g_line, "DEBUG") == 0) {
            handle_debug("");
        } else if (strncmp(g_line, "DEBUG ", 6) == 0) {
            handle_debug(g_line + 6);
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

    ConsolePrintf("LLMAGENT: opening TCP listen socket...\r\n");
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ConsolePrintf("LLMAGENT: socket() failed (rc=%d) â€” is TCP/IP up?\r\n", ls);
        return 1;
    }
    g_listen = ls;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ConsolePrintf("LLMAGENT: bind(%u) failed\r\n", (unsigned)g_port);
        close(ls);
        g_listen = -1;
        return 1;
    }
    if (listen(ls, 1) < 0) {
        ConsolePrintf("LLMAGENT: listen failed (errno=%d)\r\n", errno);
        close(ls);
        g_listen = -1;
        return 1;
    }

    /* Establish the listening endpoint before switching accept to polling. */
    if (ioctl(ls, FIONBIO, &one) < 0) {
        ConsolePrintf("LLMAGENT: FIONBIO failed\r\n");
        close(ls);
        g_listen = -1;
        return 1;
    }

    if (g_token[0]) nw_publish_ready();

    ConsolePrintf("LLMAGENT: listening on port %u\r\n", (unsigned)g_port);
    if (g_debug) {
        debug_puts(g_token[0]
                   ? "LLMAGENT: token configured: yes\r\n"
                   : "LLMAGENT: token configured: NO - set token= in SYS:SYSTEM\\LLMAGENT.INI\r\n");
    }

    while (g_running) {
        struct sockaddr_in peer;
        int peerlen = sizeof(peer);
        g_client = accept(ls, (struct sockaddr *)&peer, &peerlen);
        if (g_client < 0) {
            cpu_idle();
            continue;
        }
        {
            int nb = 1;
            if (ioctl(g_client, FIONBIO, &nb) < 0) {
                close(g_client);
                g_client = -1;
                continue;
            }
        }
        debug_puts("LLMAGENT: client connected\r\n");
        handle_client();
        close(g_client);
        g_client = -1;
        debug_puts("LLMAGENT: client done\r\n");
    }

    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    /* Only accept a volume-qualified loader path. Never guess SYS:SYSTEM
       when the loader did not identify the running image. */
    if (argc > 0 && argv && argv[0] && strchr(argv[0], ':') &&
        !strpbrk(argv[0], "\r\n"))
        update_identity_init(argv[0], (unsigned long)GetCurrentTicks(), (unsigned long)GetNLMID());
    AtUnload(on_unload);
    load_config();
    return server_main();
}
