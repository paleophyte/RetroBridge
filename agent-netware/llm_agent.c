/*
 * llm_agent (NetWare NLM): NetWare 3.12+ port of the legacy llm_agent.
 *
 * Same wire protocol as agent/llm_agent.c so bridge/server.py can drive a
 * NetWare server without a protocol fork. Uses CLIB BSD sockets (TCP/IP
 * Transport must already be loaded).
 *
 * Supported: auth, PING, QUIT, EXEC, PUT, GET, SYSINFO, SCREENSHOT,
 *            SHUTDOWN, UPDATE, DEBUG, KEY, TYPE, SCREENS
 * Unsupported: EXECDETACH, CLICK, WINLIST, CLIPSET, REG*, PSLIST,
 *              PSKILL, REBOOT
 *
 * EXEC: CLIB system() — typically a console command; no stdout capture
 * (LEN:0 + EXIT:rc). Prefer PUT of an .NCF/.NLM and EXEC that by name.
 * SCREENSHOT: Install via StuffKey DUMP only (no CopyFromScreenMemory —
 *             that hangs/abends on NWSNUT). DUMP has no color attrs so
 *             menu highlight is not visible. Else System Console copy.
 * KEY/TYPE: ungetch via ScanScreens handle + SetCurrentScreen (no
 *           CreateScreen bind of System Console/Install — that GPFs on UNLOAD).
 * SCREENS: list ScanScreens names (SIZE: text) for menu discovery.
 * SHUTDOWN: OK then DownFileServer(1) (force down — lab use).
 * UPDATE: OK then LOAD UPDATE (expects SYS:SYSTEM\LLMAGENT.NEW + UPDATE.NLM).
 * AUTOEXEC: ensure SYS:SYSTEM\AUTOEXEC.NCF has LOAD CLIBAUX + LOAD LLMAGENT.
 * DEBUG: DEBUG / DEBUG 0 / DEBUG 1 — runtime verbose flag; when on, also
 *        appends to SYS:SYSTEM\LLMAGENT.LOG (truncated on DEBUG 1).
 * Config: prefer SYS:SYSTEM\LLMAGENT.INI (A: is fallback only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nwsock.h"
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
/* Script + tiny NCF wrapper — system() on 3.12 truncates ~32 chars. */
#define STUFFKEY_SCRIPT  "SYS:SYSTEM\\L.SK"
#define STUFFKEY_NCF     "SYS:SYSTEM\\SK.NCF"
#define STUFFKEY_RUN     "SK"
#define SKDUMP_LOG       "SYS:SYSTEM\\MD.TXT"
#define AUTOEXEC_NCF     "SYS:SYSTEM\\AUTOEXEC.NCF"
#define AUTOEXEC_MAX     8192
/* CreateScreen flags (nwconio.h) — must not steal the operator console. */
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
     * Do not DestroyScreen here — NetWare 3.12 has abended in the console
     * command process when UNLOAD runs AtUnload that tears down screens.
     * The loader frees NLM-owned screens when the module exits.
     */
    g_our_screen = -1;
}

/* Never block UNLOAD with a prompt — automated UPDATE / lab ops need silence.
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

static int send_all(const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(g_client, (char *)(buf + sent), len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
        ThreadSwitch();
    }
    return 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_byte(char *out) {
    for (;;) {
        int n = recv(g_client, out, 1, 0);
        if (n == 1) return 0;
        if (n == 0) return -1; /* peer closed */
        /* nonblocking: no data yet */
        if (!g_running) return -1;
        ThreadSwitchWithDelay();
    }
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
        /* g_debug still 0 — quiet unless already on somehow */
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
        ThreadSwitch();
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
        ThreadSwitch();
    }
    fclose(f);
    return 0;
}

static int handle_sysinfo(void) {
    static char buf[768];
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
    len += sprintf(buf + len, "agent_build=keytype22\r\n");
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

/* Score printable chars; NetWare cells are char then attribute. */
static int count_printable(const unsigned char *cells, int ncells) {
    int i, n = 0;
    for (i = 0; i < ncells; i++) {
        unsigned char ch = cells[i * 2];
        if (ch >= 32 && ch < 127) n++;
    }
    return n;
}

/*
 * Private CLIB screen for ScanScreens/CopyFromScreenMemory context.
 * Shows up in Ctrl+Esc as "LLMAGENT" — do not use it interactively; the
 * agent never reads keys there. On 3.12, SetCurrentScreen(LLMAGENT) can
 * still make it the operator-visible screen (despite DONT_SWITCH flags).
 */
static int ensure_screen_context(void) {
    if (g_our_screen >= 0) return 0;
    g_our_screen = CreateScreen(
        "LLMAGENT",
        (BYTE)(NW_DONT_AUTO_ACTIVATE | NW_DONT_SWITCH_SCREEN |
               NW_AUTO_DESTROY_SCREEN));
    if (g_our_screen < 0) return -1;
    return 0;
}

/* Never leave the operator parked on our private LLMAGENT screen. */
static void restore_operator_screen(int prefer_id) {
    int id = 0;
    char name[64];
    LONG attr = 0;
    int install_id = -1;
    int console_id = -1;
    int displayed_id = -1;

    if (prefer_id >= 0 && prefer_id != g_our_screen) {
        SetCurrentScreen(prefer_id);
        return;
    }

    while ((id = ScanScreens(id, name, &attr)) != 0) {
        if (screen_name_ignored(name)) continue;
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL)
            install_id = id;
        if (is_system_console_name(name))
            console_id = id;
        if (CheckIfScreenDisplayed(id, 0) && displayed_id < 0)
            displayed_id = id;
    }
    if (install_id >= 0)
        SetCurrentScreen(install_id);
    else if (displayed_id >= 0 && displayed_id != g_our_screen)
        SetCurrentScreen(displayed_id);
    else if (console_id >= 0)
        SetCurrentScreen(console_id);
}

static int try_copy_screen(int id, unsigned char *cells, WORD *rowsP, WORD *colsP) {
    WORD rows = 25, cols = 80;

    SetCurrentScreen(id);
    if (GetSizeOfScreen(&rows, &cols) != 0 || rows == 0 || cols == 0) {
        rows = 25;
        cols = 80;
    }
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
 * StuffKey <DUMP> already works — parse that into a text cell buffer.
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

/* Write SK.NCF and run it — avoids system() 32-char truncation of LOAD lines. */
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

static int capture_install_via_stuffkey(unsigned char *cells, WORD *rowsP, WORD *colsP) {
    FILE *f;

    if (!stuffkey_present()) return -1;
    if (ensure_clibaux() != 0) return -1;

    f = fopen(STUFFKEY_SCRIPT, "wb");
    if (!f) return -1;
    fprintf(f, "<SCREEN=Install Screen>\n");
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
    int saved;
    int id = 0;
    int best_id = -1;
    int best_score = -1;
    WORD best_rows = 25, best_cols = 80;
    char name[64];
    LONG attr = 0;
    int install_id = -1;
    char install_name[64];
    static unsigned char tmp[SCR_MAX_ROWS * SCR_MAX_COLS * 2];
    FILE *dbgf = NULL;

    install_name[0] = '\0';
    /*
     * Prefer not to CreateScreen("LLMAGENT") — it shows in Ctrl+Esc and
     * console UNLOAD has abended tearing it down. ScanScreens usually works
     * without our own screen; fall back to ensure_screen_context only if needed.
     */
    saved = GetCurrentScreen();
    if (g_debug) dbgf = fopen("SYS:SYSTEM\\LLMSCR.DBG", "w");

    id = 0;
    while ((id = ScanScreens(id, name, &attr)) != 0) {
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL) {
            install_id = id;
            strncpy(install_name, name, sizeof(install_name) - 1);
            install_name[sizeof(install_name) - 1] = '\0';
            break;
        }
    }

    if (install_id >= 0) {
        WORD r, c;
        int score;
        /*
         * Never CopyFromScreenMemory on Install/NWSNUT — hangs or abends on
         * this 3.12 box (full rect and row-by-row both unsafe).
         * StuffKey DUMP is chars-only (no reverse-video highlight).
         */
        score = capture_install_via_stuffkey(cells, &r, &c);
        if (dbgf) fprintf(dbgf, "install stuffkey dump score=%d r=%u c=%u\n",
                          score, (unsigned)r, (unsigned)c);
        if (dbgf) {
            fprintf(dbgf, "best_score=%d (install/stuffkey)\n", score);
            fclose(dbgf);
            dbgf = NULL;
        }
        if (score > 0) {
            *rowsP = r;
            *colsP = c;
            return score;
        }
    }

    if (ensure_screen_context() != 0) {
        if (dbgf) fclose(dbgf);
        return -1;
    }
    if (dbgf) fprintf(dbgf, "saved=%d our=%d\n", saved, g_our_screen);

    id = 0;
    while ((id = ScanScreens(id, name, &attr)) != 0) {
        WORD r, c;
        int score;
        if (strstr(name, "Debugger") != NULL || strstr(name, "DEBUGGER") != NULL)
            continue;
        if (strstr(name, "LLMAGENT") != NULL)
            continue;
/* Install: StuffKey DUMP only — never CopyFromScreenMemory. */
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL)
            continue;

        score = try_copy_screen(id, tmp, &r, &c);
        if (score < 0) continue;
        if (strstr(name, "System Console") != NULL ||
            strstr(name, "SYSTEM CONSOLE") != NULL ||
            name[0] == '\0') {
            score += 1000;
        }
        if (strstr(name, "Monitor") != NULL || strstr(name, "MONITOR") != NULL)
            score += 400;
        if (dbgf) fprintf(dbgf, "scan %d '%s' score=%d r=%u c=%u\n",
                          id, name, score, (unsigned)r, (unsigned)c);
        if (score > best_score) {
            best_score = score;
            best_id = id;
            best_rows = r;
            best_cols = c;
            memcpy(cells, tmp, (unsigned)r * (unsigned)c * 2);
        }
        ThreadSwitchWithDelay();
    }

    if (best_score <= 0) {
        for (id = 0; id < 64; id++) {
            WORD r, c;
            int score;
            name[0] = '\0';
            attr = 0;
            if (GetScreenInfo(id, name, &attr) != 0) continue;
            score = try_copy_screen(id, tmp, &r, &c);
            if (score < 0) continue;
            if (dbgf) fprintf(dbgf, "info %d '%s' score=%d\n", id, name, score);
            if (score > best_score) {
                best_score = score;
                best_id = id;
                best_rows = r;
                best_cols = c;
                memcpy(cells, tmp, (unsigned)r * (unsigned)c * 2);
            }
        }
    }

    (void)best_id;
    if (saved >= 0 && saved != g_our_screen)
        restore_operator_screen(saved);
    else
        restore_operator_screen(best_id >= 0 ? best_id : -1);

    if (dbgf) {
        fprintf(dbgf, "best_score=%d best_id=%d %ux%u\n",
                best_score, best_id, (unsigned)best_rows, (unsigned)best_cols);
        if (best_score > 0) {
            int i;
            fprintf(dbgf, "first32:");
            for (i = 0; i < 32 && i < (int)best_rows * (int)best_cols * 2; i++)
                fprintf(dbgf, " %02X", cells[i]);
            fprintf(dbgf, "\n");
        }
        fclose(dbgf);
    }

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
 * Else bind System Console. Only DisplayScreen for non-console screens —
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
    int install_id = -1;
    int displayed = -1;
    int any_other = -1;
    char install_name[64];
    char displayed_name[64];
    char any_name[64];

    install_name[0] = '\0';
    displayed_name[0] = '\0';
    any_name[0] = '\0';
    if (nameOut && nameOutLen > 0) nameOut[0] = '\0';
    /* StuffKey only needs the name — avoid CreateScreen("LLMAGENT"). */

    while ((id = ScanScreens(id, name, &attr)) != 0) {
        if (screen_name_ignored(name)) continue;
        if (strstr(name, "Install") != NULL || strstr(name, "INSTALL") != NULL) {
            install_id = id;
            strncpy(install_name, name, sizeof(install_name) - 1);
            install_name[sizeof(install_name) - 1] = '\0';
        }
        if (any_other < 0) {
            any_other = id;
            strncpy(any_name, name, sizeof(any_name) - 1);
            any_name[sizeof(any_name) - 1] = '\0';
        }
        if (CheckIfScreenDisplayed(id, 0)) {
            displayed = id;
            strncpy(displayed_name, name, sizeof(displayed_name) - 1);
            displayed_name[sizeof(displayed_name) - 1] = '\0';
            if (!is_system_console_name(name))
                break;
        }
    }

    /* Prefer Install Screen when present (menu driving). */
    if (install_id >= 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, install_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return install_id;
    }
    if (displayed >= 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, displayed_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return displayed;
    }
    if (any_other >= 0) {
        if (nameOut && nameOutLen > 0) {
            strncpy(nameOut, any_name, (unsigned)nameOutLen - 1);
            nameOut[nameOutLen - 1] = '\0';
        }
        return any_other;
    }
    return -1;
}

static int begin_input_stuff(int *savedP) {
    char name[64];
    int target = resolve_input_screen(name, (int)sizeof(name));

    if (target < 0) return -1;
    *savedP = GetCurrentScreen();
    /*
     * CLIB ungetch only feeds getch/getche screens. INSTALL uses NWSNUT
     * (C-Worthy) and ignores it — DisplayScreen does not fix that and has
     * correlated with UNLOAD GPFs. Keep SetCurrentScreen + ungetch for
     * System Console stuffing; INSTALL needs StuffKey (separate helper).
     */
    SetCurrentScreen(target);
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
    /* Binary write — text mode turns \n into \r\r\n and breaks the parser. */
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
 * KEY up up down enter     — one StuffKey run (comma or whitespace separators)
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

    /* Prefer StuffKey whenever present — required for INSTALL/NWSNUT. */
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

    /* No CreateScreen — listing must not invent an LLMAGENT Ctrl+Esc entry. */
    while ((id = ScanScreens(id, name, &attr)) != 0) {
        int n;
        int disp = CheckIfScreenDisplayed(id, 0) ? 1 : 0;
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
    ConsolePrintf("LLMAGENT: SHUTDOWN — DownFileServer(1)\r\n");
    if (send_cstr("OK\n") < 0) return -1;
    g_running = 0;
    DownFileServer(1);
    return 0;
}

/* Remote update: LOAD UPDATE.NLM which unloads us, swaps .NEW -> .NLM, reloads. */
static int handle_update(void) {
    FILE *f = fopen("SYS:SYSTEM\\LLMAGENT.NEW", "rb");
    if (!f) {
        send_cstr("ERR:missing SYS:SYSTEM\\LLMAGENT.NEW\n");
        return -1;
    }
    fclose(f);
    f = fopen("SYS:SYSTEM\\UPDATE.NLM", "rb");
    if (!f) {
        send_cstr("ERR:missing SYS:SYSTEM\\UPDATE.NLM\n");
        return -1;
    }
    fclose(f);
    debug_puts("LLMAGENT: UPDATE — LOAD UPDATE then self-exit\r\n");
    if (send_cstr("OK\n") < 0) return -1;
    /*
     * Console "UNLOAD LLMAGENT" has repeatedly GPF'd on 3.12 (Console
     * Command Process). Instead: allow unload check, drop sockets, start
     * UPDATE, then exit() so the module tears down without a console UNLOAD.
     */
    g_allow_unload = 1;
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
     * Do NOT DestroyScreen here — GPF/abend on 3.12 during UPDATE.
     * Screen was created with AUTO_DESTROY_SCREEN so exit() should not
     * block on "Press any key to close screen".
     */
    g_our_screen = -1;
    system("LOAD UPDATE");
    exit(0);
    return 0;
}

/*
 * AUTOEXEC — ensure SYS:SYSTEM\AUTOEXEC.NCF loads LLMAGENT after TCP is up.
 * Idempotent. Also loads CLIBAUX (StuffKey dependency on 3.12).
 */
static int buf_has_token_ci(const char *buf, int n, const char *token) {
    int tlen = (int)strlen(token);
    int i, j;
    for (i = 0; i + tlen <= n; i++) {
        for (j = 0; j < tlen; j++) {
            char a = buf[i + j];
            char b = token[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == tlen) return 1;
    }
    return 0;
}

static int handle_autoexec(void) {
    static char buf[AUTOEXEC_MAX];
    FILE *f;
    long n;
    int has_agent;
    int has_clibaux;
    char reply[64];

    f = fopen(AUTOEXEC_NCF, "rb");
    if (!f) {
        send_cstr("ERR:cannot open SYS:SYSTEM\\AUTOEXEC.NCF\n");
        return -1;
    }
    n = (long)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';

    has_agent = buf_has_token_ci(buf, (int)n, "llmagent");
    has_clibaux = buf_has_token_ci(buf, (int)n, "clibaux");

    if (has_agent && has_clibaux) {
        send_cstr("OK autoexec=present\n");
        return 0;
    }

    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' ||
                     buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';

    if (n + 96 >= (long)sizeof(buf)) {
        send_cstr("ERR:AUTOEXEC.NCF too large\n");
        return -1;
    }

    n += sprintf(buf + n, "\r\nREM llm_agent (AUTOEXEC command)\r\n");
    if (!has_clibaux)
        n += sprintf(buf + n, "LOAD CLIBAUX\r\n");
    if (!has_agent)
        n += sprintf(buf + n, "LOAD LLMAGENT\r\n");

    f = fopen(AUTOEXEC_NCF, "wb");
    if (!f) {
        send_cstr("ERR:cannot write SYS:SYSTEM\\AUTOEXEC.NCF\n");
        return -1;
    }
    if ((long)fwrite(buf, 1, (size_t)n, f) != n) {
        fclose(f);
        send_cstr("ERR:short write AUTOEXEC.NCF\n");
        return -1;
    }
    fclose(f);

    if (g_debug)
        debug_puts("LLMAGENT: AUTOEXEC updated\r\n");
    if (!has_agent && !has_clibaux)
        sprintf(reply, "OK autoexec=added\n");
    else if (!has_agent)
        sprintf(reply, "OK autoexec=added-llmagent\n");
    else
        sprintf(reply, "OK autoexec=added-clibaux\n");
    return send_cstr(reply);
}

/*
 * DEBUG / DEBUG 0 / DEBUG 1 — runtime verbose toggle.
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
        ConsolePrintf("LLMAGENT: socket() failed (rc=%d) — is TCP/IP up?\r\n", ls);
        return 1;
    }
    g_listen = ls;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
    if (ioctl(ls, FIONBIO, &one) < 0) {
        ConsolePrintf("LLMAGENT: FIONBIO failed\r\n");
        close(ls);
        g_listen = -1;
        return 1;
    }

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
        ConsolePrintf("LLMAGENT: listen failed\r\n");
        close(ls);
        g_listen = -1;
        return 1;
    }

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
            ioctl(g_client, FIONBIO, &nb);
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
    (void)argc;
    (void)argv;
    AtUnload(on_unload);
    load_config();
    return server_main();
}
