/*
 * llm_agent (DOS): FreeDOS / MS-DOS port of the legacy Windows llm_agent.
 *
 * Same wire protocol as agent-win32/llm_agent.c so mcp-server/server.py can drive a
 * FreeDOS VM without a protocol fork. Unsupported Windows-only commands
 * return "ERR:not supported on DOS\n".
 *
 * Requires Watt-32 + a packet-driver TSR. See agent-dos/README.md.
 *
 * Supported: auth, PING, QUIT, EXEC, PUT, GET, SYSINFO, REBOOT,
 *            SCREENSHOT (text-mode -> BMP), KEY, TYPE
 * Unsupported: EXECDETACH, CLICK, WINLIST, CLIPSET, REG*, PSLIST,
 *              PSKILL, SHUTDOWN
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <process.h>

#include <tcp.h>

#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     512
#define READ_CHUNK       1024
#define OUT_TMP          "LLMOUT.TMP"
#define ERR_NOSUP        "ERR:not supported on DOS\n"

static char g_token[128] = "";
static word g_port = DEFAULT_PORT;
static char g_exedir[128] = ".";

/* Watt-32 sockets must not live on a tiny stack frame. */
static tcp_Socket g_sock;

/* Large I/O buffers in BSS — default DOS stack is ~2KB and Watt-32 +
   system() will overflow if these sit on the call stack. */
static char g_line[LINE_MAX_LEN];
static char g_cmd[LINE_MAX_LEN + 64];
static char g_iobuf[READ_CHUNK];
static char g_tmppath[160];

/* ---- tiny helpers ---- */

static void tcp_pump(void) {
    tcp_tick((sock_type *)&g_sock);
}

/* Busy tcp_tick loops pin the host CPU in a VM. STI+HLT yields until the
   next IRQ (timer/NIC). DOSIDLE alone will not help — we never call DOS idle.
   Also poll the keyboard: DOS BREAK is only checked on INT 21h, so ^C is
   otherwise ignored while we sit in this loop. */
static void cpu_idle(void) {
    if (kbhit()) {
        int ch = getch();
        if (ch == 0 || ch == 0xE0)
            getch(); /* discard scan code for extended keys */
        else if (ch == 3) { /* Ctrl-C */
            printf("\nllm_agent-dos: Ctrl-C, exiting\n");
            sock_exit();
            exit(0);
        }
    }
    __asm {
        sti
        hlt
    }
}

static int sock_alive(void) {
    return tcp_tick((sock_type *)&g_sock) != 0;
}

static int send_all(const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n;
        if (!sock_alive()) return -1;
        n = sock_write((sock_type *)&g_sock, (const BYTE *)(buf + sent), len - sent);
        if (n <= 0) {
            if (!sock_dataready((sock_type *)&g_sock) && !sock_established((sock_type *)&g_sock))
                return -1;
            cpu_idle();
            continue;
        }
        sent += n;
    }
    return 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_byte(char *out) {
    for (;;) {
        if (!sock_alive()) return -1;
        if (sock_dataready((sock_type *)&g_sock)) {
            if (sock_read((sock_type *)&g_sock, (BYTE *)out, 1) == 1)
                return 0;
        }
        cpu_idle();
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
    /* Strict 8.3 name for FreeDOS without LFN. */
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
            /* DOS 16-bit int max is 32767; do not compare to 65536. */
            if (p > 0) g_port = (word)p;
        } else if (strncmp(line, "token=", 6) == 0) {
            strncpy(g_token, line + 6, sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = '\0';
        }
    }
    fclose(f);
}

/* ---- EXEC: command.com /c with stdout redirected to a temp file ---- */

static int run_exec(const char *cmdline) {
    FILE *f;
    int rc;
    size_t n;
    char hdr[32];

    sprintf(g_tmppath, "%s\\%s", g_exedir, OUT_TMP);
    /* system() already invokes COMSPEC; do not wrap another command.com
       (wastes the tiny DOS command-tail budget). Redirection is parsed
       by the shell. */
    if (strlen(cmdline) + strlen(g_tmppath) + 8 >= sizeof(g_cmd)) {
        send_cstr("ERR:command too long for DOS (use a .BAT)\n");
        return -1;
    }
    sprintf(g_cmd, "%s > %s", cmdline, g_tmppath);
    rc = system(g_cmd);

    f = fopen(g_tmppath, "rb");
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
        tcp_pump();
    }
    fclose(f);
    remove(g_tmppath);
    sprintf(hdr, "EXIT:%d\n", rc);
    send_cstr(hdr);
    return 0;
}

/* ---- PUT / GET ---- */

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
        tcp_pump();
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
        tcp_pump();
    }
    fclose(f);
    return 0;
}

/* ---- SYSINFO ---- */

static int handle_sysinfo(void) {
    static char buf[512];
    int len = 0;
    union REGS r;
    char hdr[32];
    unsigned long free_kb;
    unsigned long total_kb;

    r.h.ah = 0x30;
    int86(0x21, &r, &r);
    len += sprintf(buf + len, "os_family=dos\r\n");
    len += sprintf(buf + len, "dos_major=%u\r\n", (unsigned)r.h.al);
    len += sprintf(buf + len, "dos_minor=%u\r\n", (unsigned)r.h.ah);

    r.h.ah = 0x48;
    r.x.bx = 0xFFFF;
    int86(0x21, &r, &r);
    /* After failed alloc, BX = largest available paragraph block */
    len += sprintf(buf + len, "free_conv_kb=%u\r\n", (unsigned)((unsigned long)r.x.bx * 16UL / 1024UL));

    r.h.ah = 0x36;
    r.h.dl = 3; /* C: */
    int86(0x21, &r, &r);
    if (r.x.ax != 0xFFFF) {
        free_kb = (unsigned long)r.x.bx * r.x.ax * r.x.cx / 1024UL;
        total_kb = (unsigned long)r.x.dx * r.x.ax * r.x.cx / 1024UL;
        len += sprintf(buf + len, "disk_c_total_mb=%lu\r\n", total_kb / 1024UL);
        len += sprintf(buf + len, "disk_c_free_mb=%lu\r\n", free_kb / 1024UL);
    } else {
        len += sprintf(buf + len, "disk_c_total_mb=?\r\n");
        len += sprintf(buf + len, "disk_c_free_mb=?\r\n");
    }

    len += sprintf(buf + len, "agent=llm_agent-dos\r\n");

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

/* ---- REBOOT via keyboard controller pulse ---- */

static int handle_reboot(void) {
    send_cstr("OK\n");
    /* Give the ACK a moment to leave the NIC before we reset. */
    {
        int i;
        for (i = 0; i < 30; i++) {
            tcp_pump();
            delay(50);
        }
    }
    _disable();
    outp(0x64, 0xFE);
    for (;;) {
        /* hang until reset */
    }
    return 0;
}

/* ---- KEY / TYPE via BIOS keyboard buffer stuff (INT 16h AH=05h) ---- */

typedef struct { const char *name; unsigned char scan; unsigned char ascii; } KeyName;

static const KeyName KEY_NAMES[] = {
    {"enter", 0x1C, 0x0D}, {"return", 0x1C, 0x0D},
    {"esc", 0x01, 0x1B}, {"escape", 0x01, 0x1B},
    {"tab", 0x0F, 0x09}, {"space", 0x39, 0x20},
    {"backspace", 0x0E, 0x08}, {"bksp", 0x0E, 0x08},
    {"delete", 0x53, 0x00}, {"del", 0x53, 0x00},
    {"insert", 0x52, 0x00}, {"ins", 0x52, 0x00},
    {"home", 0x47, 0x00}, {"end", 0x4F, 0x00},
    {"pageup", 0x49, 0x00}, {"pgup", 0x49, 0x00},
    {"pagedown", 0x51, 0x00}, {"pgdn", 0x51, 0x00},
    {"up", 0x48, 0x00}, {"down", 0x50, 0x00},
    {"left", 0x4B, 0x00}, {"right", 0x4D, 0x00},
    {"f1", 0x3B, 0x00}, {"f2", 0x3C, 0x00}, {"f3", 0x3D, 0x00}, {"f4", 0x3E, 0x00},
    {"f5", 0x3F, 0x00}, {"f6", 0x40, 0x00}, {"f7", 0x41, 0x00}, {"f8", 0x42, 0x00},
    {"f9", 0x43, 0x00}, {"f10", 0x44, 0x00},
    {NULL, 0, 0}
};

static int stuff_key(unsigned char scan, unsigned char ascii) {
    union REGS r;
    r.h.ah = 0x05;
    r.h.cl = ascii;
    r.h.ch = scan;
    int86(0x16, &r, &r);
    /* AL=1 means buffer full on some BIOSes */
    return (r.h.al == 0) ? 0 : -1;
}

/* Rough ASCII -> scan code for TYPE of printable chars (US layout-ish). */
static unsigned char scan_for_ascii(unsigned char ch) {
    static const unsigned char table[128] = {
        0,0,0,0,0,0,0,0, 0x0E,0x0F,0,0,0,0x1C,0,0,
        0,0,0,0,0,0,0,0, 0,0,0,0x01,0,0,0,0,
        0x39,0x02,0x28,0x04,0x05,0x06,0x08,0x28, 0x0A,0x0B,0x09,0x0D,0x33,0x0C,0x34,0x35,
        0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08, 0x09,0x0A,0x27,0x27,0x33,0x0D,0x34,0x35,
        0x03,0x1E,0x30,0x2E,0x20,0x12,0x21,0x22, 0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,
        0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11, 0x2D,0x15,0x2C,0x1A,0x2B,0x1B,0x07,0x0C,
        0x29,0x1E,0x30,0x2E,0x20,0x12,0x21,0x22, 0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,
        0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11, 0x2D,0x15,0x2C,0x1A,0x2B,0x1B,0x29,0
    };
    if (ch > 127) return 0;
    return table[ch];
}

static int handle_key(const char *keyspec) {
    char buf[128];
    char tokens[8][32];
    int ntok = 0;
    int i;
    char *tok;
    unsigned char scan = 0, ascii = 0;
    const char *base;

    strncpy(buf, keyspec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tok = strtok(buf, "-");
    while (tok && ntok < 8) {
        strncpy(tokens[ntok], tok, sizeof(tokens[0]) - 1);
        tokens[ntok][sizeof(tokens[0]) - 1] = '\0';
        ntok++;
        tok = strtok(NULL, "-");
    }
    if (ntok == 0) {
        send_cstr("ERR:empty key\n");
        return -1;
    }

    /* Modifiers are accepted for protocol compatibility but BIOS stuffing
       cannot hold ctrl/alt down; only the base key is injected. */
    for (i = 0; i < ntok - 1; i++) {
        if (stricmp(tokens[i], "ctrl") && stricmp(tokens[i], "control") &&
            stricmp(tokens[i], "alt") && stricmp(tokens[i], "shift")) {
            send_cstr("ERR:unknown modifier\n");
            return -1;
        }
    }

    base = tokens[ntok - 1];
    for (i = 0; KEY_NAMES[i].name; i++) {
        if (stricmp(base, KEY_NAMES[i].name) == 0) {
            scan = KEY_NAMES[i].scan;
            ascii = KEY_NAMES[i].ascii;
            break;
        }
    }
    if (!KEY_NAMES[i].name) {
        if (strlen(base) == 1) {
            ascii = (unsigned char)base[0];
            scan = scan_for_ascii(ascii);
            if (!scan && ascii) scan = 0x39;
        } else {
            send_cstr("ERR:unknown key name\n");
            return -1;
        }
    }

    if (stuff_key(scan, ascii) < 0) {
        send_cstr("ERR:keyboard buffer full\n");
        return -1;
    }
    send_cstr("OK\n");
    return 0;
}

static int handle_type(const char *text) {
    const char *p;
    for (p = text; *p; p++) {
        unsigned char ascii = (unsigned char)*p;
        unsigned char scan = scan_for_ascii(ascii);
        if (!scan && ascii) scan = 0x39;
        if (stuff_key(scan, ascii) < 0) {
            /* Drop remaining rather than fail mid-string hard. */
            break;
        }
        tcp_pump();
    }
    send_cstr("OK\n");
    return 0;
}

/* ---- SCREENSHOT: text mode B800 -> streamed 24-bit BMP ---- */

/* 8x8 ASCII bitmap data: public domain, Daniel Hepper, based on Marcel
 * Sondaar / IBM public-domain VGA fonts. See THIRD_PARTY.md.
 * Source: https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h
 * Adaptation: U+0020..U+007F only, each byte bit-reversed for MSB-left rendering.
 */
static const unsigned char FONT8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0020 */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* U+0021 */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+0022 */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* U+0023 */
    {0x30,0x7C,0xC0,0x78,0x0C,0xF8,0x30,0x00}, /* U+0024 */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* U+0025 */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* U+0026 */
    {0x60,0x60,0xC0,0x00,0x00,0x00,0x00,0x00}, /* U+0027 */
    {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, /* U+0028 */
    {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00}, /* U+0029 */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* U+002A */
    {0x00,0x30,0x30,0xFC,0x30,0x30,0x00,0x00}, /* U+002B */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x60}, /* U+002C */
    {0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00}, /* U+002D */
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, /* U+002E */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* U+002F */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* U+0030 */
    {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00}, /* U+0031 */
    {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00}, /* U+0032 */
    {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00}, /* U+0033 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* U+0034 */
    {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00}, /* U+0035 */
    {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00}, /* U+0036 */
    {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00}, /* U+0037 */
    {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00}, /* U+0038 */
    {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00}, /* U+0039 */
    {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x00}, /* U+003A */
    {0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x60}, /* U+003B */
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00}, /* U+003C */
    {0x00,0x00,0xFC,0x00,0x00,0xFC,0x00,0x00}, /* U+003D */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* U+003E */
    {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00}, /* U+003F */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* U+0040 */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* U+0041 */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* U+0042 */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* U+0043 */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* U+0044 */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* U+0045 */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* U+0046 */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* U+0047 */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* U+0048 */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* U+0049 */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* U+004A */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* U+004B */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* U+004C */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* U+004D */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* U+004E */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* U+004F */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* U+0050 */
    {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* U+0051 */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* U+0052 */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* U+0053 */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* U+0054 */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* U+0055 */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* U+0056 */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* U+0057 */
    {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00}, /* U+0058 */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* U+0059 */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* U+005A */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* U+005B */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* U+005C */
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* U+005D */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* U+005E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* U+005F */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* U+0060 */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* U+0061 */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* U+0062 */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* U+0063 */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* U+0064 */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* U+0065 */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* U+0066 */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* U+0067 */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* U+0068 */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* U+0069 */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* U+006A */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* U+006B */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* U+006C */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* U+006D */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00}, /* U+006E */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* U+006F */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* U+0070 */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* U+0071 */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* U+0072 */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* U+0073 */
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* U+0074 */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* U+0075 */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* U+0076 */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* U+0077 */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* U+0078 */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* U+0079 */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* U+007A */
    {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, /* U+007B */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* U+007C */
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, /* U+007D */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+007E */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* U+007F */
};

static void cga_rgb(unsigned char attr, unsigned char *r, unsigned char *g, unsigned char *b) {
    /* Approximate CGA 16-color palette from low nibble (fg). */
    static const unsigned char pal[16][3] = {
        {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
        {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
        {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
        {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255}
    };
    unsigned char fg = attr & 0x0F;
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
    unsigned char bg = (attr >> 4) & 0x07;
    *r = pal[bg][0];
    *g = pal[bg][1];
    *b = pal[bg][2];
}

static int handle_screenshot(void) {
    const int cols = 80;
    const int rows = 25;
    const int cw = 8;
    const int ch = 8;
    const int width = cols * cw;
    const int height = rows * ch;
    const unsigned long rowBytes = ((unsigned long)width * 3UL + 3UL) & ~3UL;
    const unsigned long imageSize = rowBytes * (unsigned long)height;
    const unsigned long fileSize = 14UL + 40UL + imageSize;
    unsigned char far *vid;
    char hdr[32];
    unsigned char fileHdr[14];
    unsigned char infoHdr[40];
    /* Static: ~2KB is too much for a DOS stack frame next to Watt-32. */
    static unsigned char linebuf[640 * 3 + 4];
    int py;

    vid = (unsigned char far *)MK_FP(0xB800, 0);

    sprintf(hdr, "SIZE:%lu\n", (unsigned long)fileSize);
    if (send_cstr(hdr) < 0) return -1;

    /* Packed BMP headers built by hand (avoid struct padding quirks). */
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
    infoHdr[12] = 1;  /* planes */
    infoHdr[14] = 24; /* bit count */
    infoHdr[20] = (unsigned char)(imageSize);
    infoHdr[21] = (unsigned char)(imageSize >> 8);
    infoHdr[22] = (unsigned char)(imageSize >> 16);
    infoHdr[23] = (unsigned char)(imageSize >> 24);

    if (send_all((const char *)fileHdr, 14) < 0) return -1;
    if (send_all((const char *)infoHdr, 40) < 0) return -1;

    /* BMP is bottom-up: start at last pixel row. */
    for (py = height - 1; py >= 0; py--) {
        int cellRow = py / ch;
        int glyphRow = py % ch;
        int px;

        for (px = 0; px < width; px++) {
            int cellCol = px / cw;
            int glyphCol = px % cw;
            unsigned offset = (unsigned)(cellRow * cols + cellCol) * 2;
            unsigned char chv = vid[offset];
            unsigned char attr = vid[offset + 1];
            unsigned char fr, fg, fb, br, bg, bb;
            unsigned char bits;
            int on;
            unsigned char ascii = chv;
            unsigned long i;

            cga_rgb(attr, &fr, &fg, &fb);
            bg_rgb(attr, &br, &bg, &bb);

            if (ascii < 32 || ascii > 127) ascii = 32;
            bits = FONT8[ascii - 32][glyphRow];
            on = (bits >> (7 - glyphCol)) & 1;

            i = (unsigned long)px * 3UL;
            if (on) {
                linebuf[i] = fb;
                linebuf[i + 1] = fg;
                linebuf[i + 2] = fr;
            } else {
                linebuf[i] = bb;
                linebuf[i + 1] = bg;
                linebuf[i + 2] = br;
            }
        }
        {
            unsigned long used = (unsigned long)width * 3UL;
            while (used < rowBytes) linebuf[used++] = 0;
            if (send_all((const char *)linebuf, (int)rowBytes) < 0) return -1;
        }
        if ((py & 7) == 0) tcp_pump();
    }
    return 0;
}

/* ---- session ---- */

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
        } else if (strcmp(g_line, "REBOOT") == 0) {
            handle_reboot();
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

static int wait_established(void) {
    while (!sock_established((sock_type *)&g_sock)) {
        if (!tcp_tick((sock_type *)&g_sock)) {
            return -1;
        }
        cpu_idle();
    }
    return 0;
}

static int server_main(void) {
    char ipbuf[16];

    printf("llm_agent-dos: Watt-32 init...\n");
    sock_init();
    /* Classic WatTCP sock_init() is void and exits on fatal failure.
       Watt-32's sock_init macro returns int; a zero return means ready. */
    printf("llm_agent-dos: IP address %s\n", _inet_ntoa(ipbuf, my_ip_addr));
    printf("llm_agent-dos: listening on port %u\n", (unsigned)g_port);
    printf("token configured: %s\n", g_token[0] ? "yes" : "NO - set token= in LLMAGENT.INI");

    for (;;) {
        memset(&g_sock, 0, sizeof(g_sock));
        if (!tcp_listen(&g_sock, g_port, 0L, 0, NULL, 0)) {
            printf("tcp_listen failed\n");
            return 1;
        }
        if (wait_established() < 0) {
            sock_abort((sock_type *)&g_sock);
            continue;
        }
        handle_client();
        sock_close((sock_type *)&g_sock);
        /* drain close */
        {
            int i;
            for (i = 0; i < 50; i++) {
                if (!tcp_tick((sock_type *)&g_sock)) break;
                delay(20);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    load_config(argc > 0 ? argv[0] : NULL);
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "/?") == 0)) {
        printf("llm_agent-dos - FreeDOS exec/file/screenshot agent\n");
        printf("Usage: LLMAGENT.EXE\n");
        printf("Reads port/token from LLMAGENT.INI next to the exe.\n");
        printf("Requires packet driver + WATTCP.CFG (see wattcp.cfg.example).\n");
        return 0;
    }
    return server_main();
}
