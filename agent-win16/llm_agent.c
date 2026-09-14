/*
 * llm_agent (Win16): Windows for Workgroups 3.11 port of the legacy
 * Windows llm_agent. Speaks the same token-authed TCP wire protocol as
 * agent-win32/llm_agent.c and agent-dos/llm_agent.c so mcp-server/server.py can
 * drive a WFW guest with no protocol fork.
 *
 * Networking uses WFW's own Winsock 1.1 stack (WINSOCK.DLL) -- no packet
 * driver / Watt-32 needed here, unlike the DOS port.  The idle listener
 * uses WSAAsyncSelect()+WaitMessage() instead of a blocking accept():
 * WFW's default blocking hook keeps the GUI paintable, but on this VM it
 * spins hard enough to pin the host CPU while merely waiting for a client.
 *
 * Supported: auth, PING, QUIT, EXEC, EXECDETACH, PUT, GET, SYSINFO,
 *            SCREENSHOT, REBOOT, KEY, TYPE, CLICK, WINLIST, WINMSG,
 *            POSTMSG, LBGETTEXT
 * Unsupported: PSLIST, PSKILL, SHUTDOWN, CLIPSET, REGGET/REGSET
 *
 * EXEC always runs its command inside a virtualized DOS box
 * (COMMAND.COM /c ... > tempfile, for stdout capture) -- correct for a
 * real DOS-style console command, but asking a DOS box to execute a
 * native 16-bit *Windows* EXE (CONTROL.EXE, say) is unsupported and in
 * practice froze the entire VM rather than erroring out. EXECDETACH
 * calls WinExec() directly with no DOS box involved -- use it for any
 * Windows-format program, and especially anything long-lived/GUI.
 *
 * Build: Open Watcom, -bt=windows target -- see build.bat. Needs -zW
 * (Windows-style prologs/epilogs) and the WndProc declared __export;
 * both are required for a moveable-segment window procedure to be
 * callable back from Windows itself. Pattern taken from Watcom's own
 * samples/win/generic sample, which is the closest thing to an
 * authoritative reference for this SDK.
 *
 * KEY/TYPE/CLICK use a WH_JOURNALPLAYBACK hook -- the same system-wide
 * input-injection mechanism Recorder.exe uses -- since that is the only
 * way to reach a window that isn't our own without knowing its handle.
 * This is dangerous in a way nothing else in this file is: while
 * installed, the hook replaces ALL real keyboard/mouse input
 * system-wide, and a hook that never returns control freezes input for
 * the whole VM until a hard reset. Every caller goes through
 * run_journal_events(), which guarantees UnhookWindowsHookEx() runs no
 * matter what. KNOWN ISSUE: in practice this has repeatedly failed to
 * deliver queued events within the wait deadline and has caused an
 * unrelated foreground app to close -- root cause not yet found (no
 * debugger access, header-only documentation). Treat KEY/TYPE/CLICK as
 * unreliable until that's understood; prefer WINLIST+WINMSG (below)
 * wherever the target window's hwnd can be discovered, since SendMessage
 * only ever touches the one window named, with none of the journal
 * hook's system-wide reach or its failure modes.
 *
 * WINLIST/WINMSG are the safer alternative for driving a *known* window:
 * WINLIST (EnumWindows/EnumChildWindows) is pure read-only
 * reconnaissance, and WINMSG is a thin SendMessage() passthrough --
 * intentionally as dumb/general as the other agents' REGGET/REGSET, with
 * whichever messages/params accomplish a given UI action left to whoever
 * is driving this (a script, an LLM controller), not baked in here.
 */

#include <windows.h>
#include <winsock.h>
#include <toolhelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

#define _EXPORT __export

#define IDC_EXIT_BTN 100
#define WM_AGENT_SOCKET (WM_USER + 100)

#define DEFAULT_PORT 2222
#define LINE_MAX_LEN 512
#define READ_CHUNK   4096
#define EXEC_CAP     8192
#define ERR_NOSUP    "ERR:not supported on Windows 3.11\r\n"

static char g_token[128] = "";
static WORD g_port = DEFAULT_PORT;
static char g_exedir[144] = ".";
static char g_ipstr[32] = "?";

static SOCKET g_listen = INVALID_SOCKET;
static SOCKET g_client = INVALID_SOCKET;
static int g_shutdown = 0;
static int g_accept_ready = 0;

/* Large I/O buffers as statics: -bt=windows here uses one near data
   segment (max 64K) shared by every global below plus the C library's
   own state, so keep these modest and never put them on the stack. */
static char g_line[LINE_MAX_LEN];
static char g_cmd[LINE_MAX_LEN + 64];
static char g_iobuf[READ_CHUNK];
static char g_execbuf[EXEC_CAP];

static HWND g_hwnd = NULL;
static HINSTANCE g_hinst = NULL;

long _EXPORT FAR PASCAL WndProc(HWND, unsigned, UINT, LONG);
LRESULT _EXPORT FAR PASCAL JournalPlaybackProc(int, WPARAM, LPARAM);
BOOL _EXPORT CALLBACK EnumWindowsProc(HWND, LPARAM);
BOOL _EXPORT CALLBACK EnumChildProc(HWND, LPARAM);

#define WINLIST_CAP 4096
static char g_winlist_buf[WINLIST_CAP];
static int g_winlist_len;

/* ---- KEY/TYPE/CLICK: WH_JOURNALPLAYBACK event queue -- see the safety
   note at the top of the file before touching any of this. ---- */

#define MAX_EVENTS 256
static EVENTMSG g_events[MAX_EVENTS];
static int g_event_count = 0;
static int g_event_pos = 0;
static HHOOK g_hook = NULL;

/* A custom WSASetBlockingHook()-installed hook (GetMessage()-based,
   meant to replace whatever spin loop 16-bit Winsock's own default
   blocking hook uses -- confirmed by measurement that this agent
   running is what pins the host CPU, unrelated to WQGHLT.386, already
   loaded here for idle detection) was tried and reverted: it GPF'd
   inside the compiler's stack-check runtime (__STK) at the same address
   regardless of stack size, which points at a calling-convention
   mismatch in the hook procedure rather than stack exhaustion -- i.e.
   the blocking hook's actual required signature was never verified
   against real Winsock 1.1 documentation, only recalled from memory,
   and evidently wrong. Left unfixed; do not reinstall without a
   verified signature to build it against. */

/* ---- tiny helpers ---- */

static int send_all(const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(g_client, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int send_cstr(const char *text) {
    return send_all(text, (int)strlen(text));
}

static int recv_byte(char *out) {
    int n = recv(g_client, out, 1, 0);
    return (n == 1) ? 0 : -1;
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
        out[1] = (path[1] == ':') ? ':' : '\0';
        if (out[1] == ':') { out[2] = '\\'; out[3] = '\0'; }
    }
}

static void load_config(HINSTANCE hInst) {
    char exePath[160];
    char path[176];
    char line[256];
    FILE *f;

    GetModuleFileName(hInst, exePath, sizeof(exePath));
    dirname_of(exePath, g_exedir, sizeof(g_exedir));
    sprintf(path, "%s\\LLMAGENT.INI", g_exedir);

    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0) g_port = (WORD)p;
        } else if (strncmp(line, "token=", 6) == 0) {
            strncpy(g_token, line + 6, sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = '\0';
        }
    }
    fclose(f);
}

static void get_local_ip(char *out, int outlen) {
    char hostname[128];
    struct hostent FAR *he;

    strncpy(out, "?", outlen - 1);
    out[outlen - 1] = '\0';
    if (gethostname(hostname, sizeof(hostname)) != 0) return;
    he = gethostbyname(hostname);
    if (!he || !he->h_addr_list[0]) return;
    {
        struct in_addr addr;
        /* he->h_addr_list[0] and inet_ntoa()'s return are both FAR --
           WINSOCK.DLL's buffers are not guaranteed to sit in our DGROUP,
           so a plain near memcpy/strncpy would silently truncate the
           segment and read garbage. */
        _fmemcpy(&addr, he->h_addr_list[0], sizeof(addr));
        _fstrncpy(out, inet_ntoa(addr), outlen - 1);
        out[outlen - 1] = '\0';
    }
}

/* ---- EXEC: COMMAND.COM /c <cmdline> with stdout redirected to a temp
   file, mirroring agent-dos's proven system()-based approach exactly --
   ONE command line, no intermediate .BAT file. An earlier version wrote
   the command into a generated LLMEXEC.BAT plus an
   "echo LLMEXITCODE:%ERRORLEVEL%" marker line to recover a real exit
   code (WinExec() is fire-and-forget with no exit-code API under
   Win16). That version left a COMMAND.COM instance permanently resident
   after every single EXEC call -- confirmed visually, an icon per call,
   never reclaimed short of a reboot -- for reasons not fully understood
   (COMMAND.COM /C running a .BAT file, specifically, is the one thing
   that differed from the direct single-line form below, which does not
   reproduce the leak). Real exit codes were not worth that; this always
   reports EXIT:0 on completion. Polling GetModuleUsage() while pumping
   messages and calling Yield() is the standard Win16 idiom for "block
   until a WinExec'd process exits", bounded to 30 real seconds so a
   GUI program run here by mistake can't wedge this agent forever (use
   EXECDETACH for those instead). Output is capped at EXEC_CAP; put
   long-running or high-output work in a .BAT and EXEC that file. ---- */

static int run_exec(const char *cmdline) {
    FILE *f;
    char outpath[176];
    UINT hi;
    long n;
    char hdr[32];

    if (cmdline[0] == '\0') {
        send_cstr("ERR:empty command\r\n");
        return -1;
    }

    sprintf(outpath, "%s\\LLMOUT.TMP", g_exedir);

    /* Do NOT build "COMSPEC /c <cmd> > outpath" and hand it to WinExec --
       on this box, ANY secondary COMMAND.COM /c invocation whose command
       tail contains "> file" crashes with a Windows 3.1 "violated system
       integrity" UAE. Confirmed independent of the target path and of
       foreground/minimized show state; a non-redirected "/c <cmd>" never
       crashes. Leading theory: WFW's DOS-box network-redirector hook
       (NET START, loaded before WIN) corrupts the INT 21h handle-
       duplication sequence COMMAND.COM's own "> file" parsing relies on
       -- the plain-DOS boot side of this same machine, which loads no
       redirector, has never shown this for equivalent redirected
       commands. REDIR.EXE (redir.c, DOS-target build) sidesteps the bug
       entirely: it does the file-handle redirection itself via direct
       dup2() BEFORE calling system(cmdline) with no ">" in it at all, so
       the shell it spawns is never asked to parse redirection syntax. */
    sprintf(g_cmd, "%s\\REDIR.EXE %s %s", g_exedir, outpath, cmdline);

    /* Must be SW_SHOWNORMAL (foreground), not SW_SHOWMINNOACTIVE --
       confirmed by direct testing that REDIR.EXE's nested spawnl() child
       (see redir.c) never terminates its VDM when launched minimized in
       the background, even though the exact same binary/cmdline
       completes and exits cleanly within a couple seconds when launched
       SW_SHOWNORMAL. Minimized was originally chosen only to hide the
       video corruption an older SVGA driver caused when a DOS box came
       to the foreground -- vbesvga.drv (see README.md) fixed that, so
       foreground is safe now and is what actually works. */
    hi = WinExec(g_cmd, SW_SHOWNORMAL);
    if (hi <= 32) {
        send_cstr("ERR:WinExec failed\r\n");
        return -1;
    }
    {
        DWORD start = GetTickCount();
        const DWORD max_exec_wait_ms = 30000UL;
        while (GetModuleUsage((HINSTANCE)hi) > 0) {
            MSG msg;
            if ((GetTickCount() - start) >= max_exec_wait_ms) {
                send_cstr("ERR:still running after 30s -- use EXECDETACH for long-lived/GUI programs\r\n");
                return -1;
            }
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Yield();
        }
    }

    f = fopen(outpath, "rb");
    if (!f) {
        send_cstr("LEN:0\nEXIT:0\n");
        return 0;
    }
    n = (long)fread(g_execbuf, 1, EXEC_CAP - 1, f);
    fclose(f);
    remove(outpath);
    if (n < 0) n = 0;

    sprintf(hdr, "LEN:%ld\n", n);
    if (send_cstr(hdr) < 0) return -1;
    if (n > 0 && send_all(g_execbuf, (int)n) < 0) return -1;
    send_cstr("EXIT:0\n");
    return 0;
}

/* ---- EXECDETACH: fire-and-forget WinExec(), no COMMAND.COM/DOS box
   involved at all -- the only correct way to launch a native 16-bit
   Windows program from here. run_exec() above always wraps its command
   in a virtualized DOS box for stdout redirection, which is fine for a
   real DOS-style console command but is NOT how you run a Windows-
   format EXE: asking a DOS box to execute one is unsupported and, in
   practice, froze the entire VM rather than just erroring out. Anything
   that isn't a short-lived console command -- CONTROL.EXE included --
   belongs here, not in EXEC. ---- */

static int handle_execdetach(const char *cmdline) {
    UINT hi;
    char hdr[32];

    if (cmdline[0] == '\0') {
        send_cstr("ERR:empty command\r\n");
        return -1;
    }
    hi = WinExec(cmdline, SW_SHOWNORMAL);
    if (hi <= 32) {
        send_cstr("ERR:WinExec failed\r\n");
        return -1;
    }
    sprintf(hdr, "OK pid=%u\r\n", hi);
    send_cstr(hdr);
    return 0;
}

/* ---- PUT / GET ---- */

static int handle_put(char *args) {
    char *lastSpace = strrchr(args, ' ');
    long size;
    FILE *f;
    long remaining;
    int failed = 0;

    if (!lastSpace) {
        send_cstr("ERR:bad PUT syntax\r\n");
        return -1;
    }
    size = atol(lastSpace + 1);
    *lastSpace = '\0';
    if (size < 0) {
        send_cstr("ERR:bad size\r\n");
        return -1;
    }

    f = fopen(args, "wb");
    if (!f) failed = 1;

    remaining = size;
    while (remaining > 0) {
        int want = remaining < (long)sizeof(g_iobuf) ? (int)remaining : (int)sizeof(g_iobuf);
        int got = recv(g_client, g_iobuf, want, 0);
        if (got <= 0) { failed = 1; break; }
        if (f && (int)fwrite(g_iobuf, 1, got, f) != got) failed = 1;
        remaining -= got;
    }
    if (f) fclose(f);
    if (failed) {
        send_cstr("ERR:write failed\r\n");
        return -1;
    }
    send_cstr("OK\r\n");
    return 0;
}

static int handle_get(const char *path) {
    FILE *f;
    long size;
    char hdr[32];
    size_t n;

    f = fopen(path, "rb");
    if (!f) {
        send_cstr("ERR:cannot open file\r\n");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_cstr("ERR:cannot stat file\r\n");
        return -1;
    }
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    sprintf(hdr, "SIZE:%ld\n", size);
    if (send_cstr(hdr) < 0) { fclose(f); return -1; }
    while ((n = fread(g_iobuf, 1, sizeof(g_iobuf), f)) > 0) {
        if (send_all(g_iobuf, (int)n) < 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ---- SYSINFO: same INT 21h disk-free trick as agent-dos -- Win16
   standard/386-enhanced mode still virtualizes DOS calls, and this SDK
   has no Win32-style GetDiskFreeSpace to call instead. ---- */

static int handle_sysinfo(void) {
    static char buf[512];
    int len = 0;
    union REGS r;
    char hdr[32];
    DWORD verInfo;
    unsigned long free_kb, total_kb;

    verInfo = GetVersion();
    len += sprintf(buf + len, "os_family=win16\r\n");
    len += sprintf(buf + len, "windows_version=%u.%u\r\n",
                   (unsigned)LOBYTE(LOWORD(verInfo)), (unsigned)HIBYTE(LOWORD(verInfo)));
    /* Unlike the Windows word (low byte=major/high byte=minor), the DOS
       word is high byte=major/low byte=minor -- mixing these up prints
       "22.6" for MS-DOS 6.22. */
    len += sprintf(buf + len, "dos_version=%u.%u\r\n",
                   (unsigned)HIBYTE(HIWORD(verInfo)), (unsigned)LOBYTE(HIWORD(verInfo)));
    len += sprintf(buf + len, "free_system_resources_pct=%u\r\n",
                   (unsigned)GetFreeSystemResources(GFSR_SYSTEMRESOURCES));

    /* GetFreeSystemResources() above is GDI/USER heap %, not physical
       memory -- ToolHelp's MemManInfo() is the real memory picture:
       total/free RAM in pages, converted here to KB. dwSwapFilePages is
       the 386 enhanced mode swap file's configured CAPACITY (pages
       available for it to grow into), not how much of it is currently
       in use -- confirmed suspicious by testing: it came back as exactly
       49152 KB (48MB), a suspiciously round number for "currently
       swapped", matching this field's documented meaning of capacity
       rather than live usage. Don't rename this to imply "in use". */
    {
        MEMMANINFO mmi;
        mmi.dwSize = sizeof(MEMMANINFO);
        if (MemManInfo(&mmi)) {
            unsigned long page_kb = (unsigned long)mmi.wPageSize / 1024UL;
            if (page_kb == 0) page_kb = 4; /* 4K pages if wPageSize < 1024 for any reason */
            len += sprintf(buf + len, "mem_total_kb=%lu\r\n",
                           (unsigned long)mmi.dwTotalPages * page_kb);
            len += sprintf(buf + len, "mem_free_kb=%lu\r\n",
                           (unsigned long)mmi.dwFreePages * page_kb);
            len += sprintf(buf + len, "mem_swapfile_capacity_kb=%lu\r\n",
                           (unsigned long)mmi.dwSwapFilePages * page_kb);
        } else {
            len += sprintf(buf + len, "mem_total_kb=?\r\n");
            len += sprintf(buf + len, "mem_free_kb=?\r\n");
            len += sprintf(buf + len, "mem_swapfile_capacity_kb=?\r\n");
        }
    }

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

    len += sprintf(buf + len, "agent=llm_agent-win16\r\n");

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0 || send_all(buf, len) < 0) return -1;
    return 0;
}

/* ---- SCREENSHOT: whole-desktop BitBlt -> 24bpp BMP, streamed one
   scanline at a time via GetDIBits so a single call never needs more
   than a few KB of the one shared 64K data segment. GDI numbers DIB
   scanlines bottom-up for a positive biHeight, which is exactly the row
   order a BMP file wants, so no reversal pass is needed. ---- */

static int handle_screenshot(void) {
    HDC hdcScreen, hdcMem;
    HBITMAP hbm, hbmOld;
    int width, height;
    DWORD rowBytes, imageSize, fileSize;
    BITMAPINFOHEADER bi;
    unsigned char fileHdr[14];
    unsigned char infoHdr[40];
    static unsigned char rowbuf[4096]; /* covers up to ~1365px wide @ 24bpp */
    char hdr[32];
    int y;
    int ok = 1;

    hdcScreen = GetDC(NULL);
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
    hdcMem = CreateCompatibleDC(hdcScreen);
    hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    hbmOld = SelectObject(hdcMem, hbm);
    BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
    /* GetDIBits requires the bitmap NOT be selected into a DC -- leaving
       it selected while calling GetDIBits once per scanline (below) is a
       documented GDI hazard and a plausible cause of the display
       corruption seen after a capture on this driver. Deselect now. */
    SelectObject(hdcMem, hbmOld);

    rowBytes = ((DWORD)width * 3UL + 3UL) & ~3UL;
    if (rowBytes > sizeof(rowbuf)) {
        send_cstr("ERR:screen too wide for this build\r\n");
        ok = 0;
        goto cleanup;
    }
    imageSize = rowBytes * (DWORD)height;
    fileSize = 14UL + 40UL + imageSize;

    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = height; /* positive = bottom-up */
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imageSize;

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
    infoHdr[5] = (unsigned char)((unsigned)width >> 8);
    infoHdr[8] = (unsigned char)(height);
    infoHdr[9] = (unsigned char)((unsigned)height >> 8);
    infoHdr[12] = 1;
    infoHdr[14] = 24;
    infoHdr[20] = (unsigned char)(imageSize);
    infoHdr[21] = (unsigned char)(imageSize >> 8);
    infoHdr[22] = (unsigned char)(imageSize >> 16);
    infoHdr[23] = (unsigned char)(imageSize >> 24);

    sprintf(hdr, "SIZE:%lu\n", fileSize);
    if (send_cstr(hdr) < 0) { ok = 0; goto cleanup; }
    if (send_all((const char *)fileHdr, 14) < 0) { ok = 0; goto cleanup; }
    if (send_all((const char *)infoHdr, 40) < 0) { ok = 0; goto cleanup; }

    for (y = 0; y < height; y++) {
        if (GetDIBits(hdcScreen, hbm, y, 1, rowbuf, (BITMAPINFO FAR *)&bi, DIB_RGB_COLORS) == 0) {
            ok = 0;
            goto cleanup;
        }
        if (send_all((const char *)rowbuf, (int)rowBytes) < 0) {
            ok = 0;
            goto cleanup;
        }
    }

cleanup:
    /* hbm was already deselected (back to hbmOld) right after the BitBlt
       above, before any GetDIBits call -- nothing left to re-select. */
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return ok ? 0 : -1;
}

/* ---- WINLIST [hwnd]: no argument enumerates top-level windows; a
   decimal hwnd enumerates that window's immediate children instead.
   Pure read-only reconnaissance (EnumWindows/EnumChildWindows,
   GetClassName, GetWindowText, GetWindowRect) -- no side effects on
   anything, unlike KEY/TYPE/CLICK, so safe to run freely, including on
   a live modal dialog to see what it actually contains before trying to
   drive it. ---- */

BOOL _EXPORT CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    char cls[64];
    char title[128];
    RECT rc;

    lParam = lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;

    GetClassName(hwnd, cls, sizeof(cls));
    GetWindowText(hwnd, title, sizeof(title));
    GetWindowRect(hwnd, &rc);

    if (g_winlist_len < WINLIST_CAP - 220) {
        g_winlist_len += sprintf(g_winlist_buf + g_winlist_len,
                                  "%u\t%d\t%d\t%d\t%d\t%s\t%s\r\n",
                                  (unsigned)hwnd, (int)rc.left, (int)rc.top,
                                  (int)(rc.right - rc.left), (int)(rc.bottom - rc.top),
                                  cls, title);
    }
    return TRUE;
}

BOOL _EXPORT CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    char cls[64];
    char title[128];
    RECT rc;
    int id;

    lParam = lParam;
    GetClassName(hwnd, cls, sizeof(cls));
    GetWindowText(hwnd, title, sizeof(title));
    GetWindowRect(hwnd, &rc);
    id = GetWindowWord(hwnd, GWW_ID);

    if (g_winlist_len < WINLIST_CAP - 220) {
        g_winlist_len += sprintf(g_winlist_buf + g_winlist_len,
                                  "%u\t%d\t%d\t%d\t%d\t%s\t%d:%s\r\n",
                                  (unsigned)hwnd, (int)rc.left, (int)rc.top,
                                  (int)(rc.right - rc.left), (int)(rc.bottom - rc.top),
                                  cls, id, title);
    }
    return TRUE;
}

static int handle_winlist(const char *args) {
    char hdr[32];
    HWND parent = NULL;

    while (*args == ' ') args++;
    if (*args) parent = (HWND)(unsigned)atoi(args);

    g_winlist_len = 0;
    if (parent) {
        if (!IsWindow(parent)) {
            send_cstr("ERR:no such window\r\n");
            return -1;
        }
        EnumChildWindows(parent, (WNDENUMPROC)EnumChildProc, 0);
    } else {
        EnumWindows((WNDENUMPROC)EnumWindowsProc, 0);
    }

    sprintf(hdr, "SIZE:%d\n", g_winlist_len);
    if (send_cstr(hdr) < 0) return -1;
    if (g_winlist_len > 0 && send_all(g_winlist_buf, g_winlist_len) < 0) return -1;
    return 0;
}

/* ---- WINMSG <hwnd> <msg> <wparam> <lparam> -- raw SendMessage()
   passthrough, all four fields plain decimal integers. Deliberately a
   thin, general primitive rather than a "click this specific checkbox"
   command: SendMessage targets exactly the one window named, with none
   of WH_JOURNALPLAYBACK's system-wide reach, so whoever is driving this
   (a script, an LLM controller) carries the knowledge of which
   messages/params accomplish a given UI action -- same division of
   labor as the real Windows agent's REGGET/REGSET plus the bridge-side
   knowledge of which registry keys matter, rather than baking Winlogon
   specifics into the agent itself.

   To press a button: send it WM_LBUTTONDOWN then WM_LBUTTONUP directly
   (wparam=1 i.e. MK_LBUTTON for the down, 0 for the up; lparam=
   (y<<16)|x, client-relative to the button, e.g. near its center) --
   the button handles its own click and notifies its parent, without
   needing to know its control ID. ---- */

static int handle_winmsg(const char *args) {
    long hwndVal, msgVal, wparamVal, lparamVal;
    LRESULT result;
    char hdr[48];

    if (sscanf(args, "%ld %ld %ld %ld", &hwndVal, &msgVal, &wparamVal, &lparamVal) != 4) {
        send_cstr("ERR:bad WINMSG syntax\r\n");
        return -1;
    }
    if (!IsWindow((HWND)hwndVal)) {
        send_cstr("ERR:no such window\r\n");
        return -1;
    }

    result = SendMessage((HWND)hwndVal, (UINT)msgVal, (WPARAM)wparamVal, (LPARAM)lparamVal);
    sprintf(hdr, "OK:%ld\r\n", (long)result);
    send_cstr(hdr);
    return 0;
}

/* ---- POSTMSG <hwnd> <msg> <wparam> <lparam> -- PostMessage()
   passthrough. Unlike WINMSG (SendMessage), this returns the instant the
   message is queued, without waiting for it to be processed at all.
   Needed for anything that might open a modal dialog: SendMessage blocks
   the CALLER until the entire receiving chain finishes processing,
   including any nested DialogBox() call the message triggers along the
   way -- confirmed the hard way driving Control Panel's applet list,
   where double-clicking the Network item via WINMSG wedged this agent
   for as long as that dialog stayed open. Even a button's own internal
   click handling notifies its parent via SendMessage synchronously, so
   PostMessage has to be the entry point, not just the last step: post
   WM_LBUTTONDOWN then WM_LBUTTONUP to press a button, or post a
   WM_COMMAND with an LBN_DBLCLK notification to activate a listbox
   item, and whatever dialog that opens runs on its own time via the
   normal message pump, decoupled from this call entirely. ---- */

static int handle_postmsg(const char *args) {
    long hwndVal, msgVal, wparamVal, lparamVal;

    if (sscanf(args, "%ld %ld %ld %ld", &hwndVal, &msgVal, &wparamVal, &lparamVal) != 4) {
        send_cstr("ERR:bad POSTMSG syntax\r\n");
        return -1;
    }
    if (!IsWindow((HWND)hwndVal)) {
        send_cstr("ERR:no such window\r\n");
        return -1;
    }

    if (!PostMessage((HWND)hwndVal, (UINT)msgVal, (WPARAM)wparamVal, (LPARAM)lparamVal)) {
        send_cstr("ERR:PostMessage failed\r\n");
        return -1;
    }
    send_cstr("OK\r\n");
    return 0;
}

/* ---- LBGETTEXT <hwnd> <index> -- LB_GETTEXT passthrough. WINMSG can't
   carry this (LB_GETTEXT's lParam is a buffer pointer, not a plain
   integer), so it gets its own tiny command: read-only, used to find
   which listbox index a Control-Panel-style owner-drawn icon list's
   item corresponds to (e.g. "Network") before selecting it by index
   via WINMSG's LB_SETCURSEL, with no string-pointer message needed for
   the actual selection. ---- */

static int handle_lbgettext(const char *args) {
    long hwndVal, idx;
    static char buf[160];
    LRESULT len;

    if (sscanf(args, "%ld %ld", &hwndVal, &idx) != 2) {
        send_cstr("ERR:bad LBGETTEXT syntax\r\n");
        return -1;
    }
    if (!IsWindow((HWND)hwndVal)) {
        send_cstr("ERR:no such window\r\n");
        return -1;
    }

    len = SendMessage((HWND)hwndVal, LB_GETTEXT, (WPARAM)idx, (LPARAM)(LPSTR)buf);
    if (len == LB_ERR) {
        send_cstr("ERR:LB_ERR (bad index?)\r\n");
        return -1;
    }
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;

    send_cstr("OK:");
    if (len > 0) send_all(buf, (int)len);
    send_cstr("\r\n");
    return 0;
}

/* ---- WH_JOURNALPLAYBACK hook proc. Windows calls this instead of
   reading real hardware input while the hook is installed.
   HC_GETNEXT: fill in the EVENTMSG at lParam with the next queued event
   and return 0 to deliver it immediately (0 = no delay). If the queue
   is empty, return a short delay instead -- the hook stays installed
   only for the few milliseconds it takes run_journal_events()'s own
   loop to notice and call UnhookWindowsHookEx(), so this is never
   observed for long.
   HC_SKIP: the event previously handed out has been consumed; advance.
   code < 0: not ours to handle, pass it down the chain. ---- */

LRESULT _EXPORT FAR PASCAL JournalPlaybackProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }
    if (code == HC_SKIP) {
        if (g_event_pos < g_event_count) g_event_pos++;
        return 0;
    }
    if (code == HC_GETNEXT) {
        if (g_event_pos < g_event_count) {
            *((LPEVENTMSG)lParam) = g_events[g_event_pos];
            return 0;
        }
        return 50;
    }
    return 0;
}

static void push_event(UINT message, UINT paramL, UINT paramH) {
    if (g_event_count < MAX_EVENTS) {
        g_events[g_event_count].message = message;
        g_events[g_event_count].paramL = paramL;
        g_events[g_event_count].paramH = paramH;
        g_events[g_event_count].time = GetTickCount();
        g_event_count++;
    }
}

static void push_key(unsigned vk, unsigned scan, int extended) {
    unsigned base = scan | (extended ? KF_EXTENDED : 0);
    push_event(WM_KEYDOWN, vk, base);
    push_event(WM_KEYUP, vk, base | KF_UP | KF_REPEAT);
}

/* Physical-key scan codes for ASCII 0..127 -- identical table to
   agent-dos/llm_agent.c's scan_for_ascii (same PC hardware scan codes,
   layout-independent of DOS vs Windows). Journal playback needs this
   PLUS a separate shift flag, unlike DOS's BIOS buffer stuffing which
   takes a pre-resolved ASCII character directly. */
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

static int needs_shift(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return 1;
    switch (ch) {
    case '!': case '@': case '#': case '$': case '%': case '^': case '&':
    case '*': case '(': case ')': case '_': case '+': case '{': case '}':
    case '|': case ':': case '"': case '<': case '>': case '?': case '~':
        return 1;
    default:
        return 0;
    }
}

/* VK_A..VK_Z and VK_0..VK_9 are the same numeric values as their ASCII
   uppercase/digit forms; symbol keys don't have a meaningful named VK
   here; the scan code carries the actual key identity for those. */
static unsigned vk_for_ascii(unsigned char ch) {
    if (ch >= 'a' && ch <= 'z') return (unsigned)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return (unsigned)ch;
    if (ch >= '0' && ch <= '9') return (unsigned)ch;
    return 0;
}

static int push_char(unsigned char ch) {
    unsigned scan = scan_for_ascii(ch);
    unsigned vk = vk_for_ascii(ch);
    int shift = needs_shift(ch);

    if (!scan) return -1;
    if (shift) push_event(WM_KEYDOWN, VK_SHIFT, 0x2A);
    push_key(vk, scan, 0);
    if (shift) push_event(WM_KEYUP, VK_SHIFT, 0x2A | KF_UP | KF_REPEAT);
    return 0;
}

/* Installs the journal hook, pumps messages until the queued events are
   all delivered (or a hard cap is hit), then ALWAYS unhooks before
   returning -- this is the one function every KEY/TYPE/CLICK handler
   must route through, and the only place g_hook is set or cleared. The
   iteration cap exists so that no bug in JournalPlaybackProc above can
   ever hang real keyboard/mouse input for longer than a handful of
   Yield() cycles: worst case we bail out having FAILED to deliver every
   queued event, but we still unhook and hand input back. */
/* Return: 0 = every queued event delivered. -1 = SetWindowsHookEx()
   itself failed (hook never installed). -2 = installed but the wait
   deadline passed before every event was delivered. Distinct codes
   because these point at very different problems.

   Bounded on WALL-CLOCK time via GetTickCount(), not a raw iteration
   count: Win16's input polling is tied to the ~55ms system timer tick,
   and a tight Yield()-only loop can burn through hundreds of iterations
   in far less than one tick if nothing else has real work to do --
   which looked exactly like "the hook never gets serviced" when this
   used to be a plain iteration cap. 3 seconds is generous for a few
   dozen keystrokes' worth of events while still bounding worst case. */
static int run_journal_events(void) {
    DWORD start;
    const DWORD max_wait_ms = 3000UL;

    g_event_pos = 0;
    g_hook = SetWindowsHookEx(WH_JOURNALPLAYBACK, (HOOKPROC)JournalPlaybackProc, g_hinst, 0);
    if (!g_hook) {
        g_event_count = 0;
        return -1;
    }

    start = GetTickCount();
    while (g_event_pos < g_event_count && (GetTickCount() - start) < max_wait_ms) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Yield();
    }

    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    {
        int delivered_all = (g_event_pos >= g_event_count);
        g_event_count = 0;
        g_event_pos = 0;
        return delivered_all ? 0 : -2;
    }
}

static void send_journal_error(int rc) {
    if (rc == -1) {
        send_cstr("ERR:SetWindowsHookEx failed\r\n");
    } else {
        send_cstr("ERR:hook installed but playback did not finish\r\n");
    }
}

/* ---- KEY: named keys ("enter", "f5", ...) or a single character,
   with an optional "shift-" prefix. Unlike the DOS agent's BIOS buffer
   stuffing, journal playback injects real keydown/keyup pairs, so
   holding a modifier down around the base key actually works -- but
   only shift is wired up for now; ctrl/alt are ERR_NOSUP until there is
   a real need for them. ---- */

typedef struct { const char *name; unsigned vk; unsigned scan; int extended; } NamedKey;

static const NamedKey KEY_NAMES[] = {
    {"enter", VK_RETURN, 0x1C, 0}, {"return", VK_RETURN, 0x1C, 0},
    {"esc", VK_ESCAPE, 0x01, 0}, {"escape", VK_ESCAPE, 0x01, 0},
    {"tab", VK_TAB, 0x0F, 0}, {"space", VK_SPACE, 0x39, 0},
    {"backspace", VK_BACK, 0x0E, 0}, {"bksp", VK_BACK, 0x0E, 0},
    {"delete", VK_DELETE, 0x53, 1}, {"del", VK_DELETE, 0x53, 1},
    {"insert", VK_INSERT, 0x52, 1}, {"ins", VK_INSERT, 0x52, 1},
    {"home", VK_HOME, 0x47, 1}, {"end", VK_END, 0x4F, 1},
    {"pageup", VK_PRIOR, 0x49, 1}, {"pgup", VK_PRIOR, 0x49, 1},
    {"pagedown", VK_NEXT, 0x51, 1}, {"pgdn", VK_NEXT, 0x51, 1},
    {"up", VK_UP, 0x48, 1}, {"down", VK_DOWN, 0x50, 1},
    {"left", VK_LEFT, 0x4B, 1}, {"right", VK_RIGHT, 0x4D, 1},
    {"f1", VK_F1, 0x3B, 0}, {"f2", VK_F2, 0x3C, 0}, {"f3", VK_F3, 0x3D, 0}, {"f4", VK_F4, 0x3E, 0},
    {"f5", VK_F5, 0x3F, 0}, {"f6", VK_F6, 0x40, 0}, {"f7", VK_F7, 0x41, 0}, {"f8", VK_F8, 0x42, 0},
    {"f9", VK_F9, 0x43, 0}, {"f10", VK_F10, 0x44, 0},
    {NULL, 0, 0, 0}
};

static int handle_key(const char *keyspec) {
    char buf[128];
    char tokens[8][32];
    int ntok = 0;
    char *tok;
    int i;
    int use_shift = 0;
    unsigned vk = 0, scan = 0;
    int extended = 0;
    int found = 0;
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
        send_cstr("ERR:empty key\r\n");
        return -1;
    }

    for (i = 0; i < ntok - 1; i++) {
        if (stricmp(tokens[i], "shift") == 0) {
            use_shift = 1;
        } else if (stricmp(tokens[i], "ctrl") == 0 || stricmp(tokens[i], "control") == 0 ||
                   stricmp(tokens[i], "alt") == 0) {
            send_cstr(ERR_NOSUP);
            return -1;
        } else {
            send_cstr("ERR:unknown modifier\r\n");
            return -1;
        }
    }

    base = tokens[ntok - 1];
    for (i = 0; KEY_NAMES[i].name; i++) {
        if (stricmp(base, KEY_NAMES[i].name) == 0) {
            vk = KEY_NAMES[i].vk;
            scan = KEY_NAMES[i].scan;
            extended = KEY_NAMES[i].extended;
            found = 1;
            break;
        }
    }
    if (!found) {
        if (strlen(base) == 1) {
            unsigned char ch = (unsigned char)base[0];
            scan = scan_for_ascii(ch);
            vk = vk_for_ascii(ch);
            if (!scan) {
                send_cstr("ERR:unknown key name\r\n");
                return -1;
            }
            if (needs_shift(ch)) use_shift = 1;
        } else {
            send_cstr("ERR:unknown key name\r\n");
            return -1;
        }
    }

    g_event_count = 0;
    if (use_shift) push_event(WM_KEYDOWN, VK_SHIFT, 0x2A);
    push_key(vk, scan, extended);
    if (use_shift) push_event(WM_KEYUP, VK_SHIFT, 0x2A | KF_UP | KF_REPEAT);

    {
        int rc = run_journal_events();
        if (rc < 0) {
            send_journal_error(rc);
            return -1;
        }
    }
    send_cstr("OK\r\n");
    return 0;
}

static int handle_type(const char *text) {
    const char *p;

    g_event_count = 0;
    for (p = text; *p && g_event_count < MAX_EVENTS - 4; p++) {
        if (push_char((unsigned char)*p) < 0) {
            /* Unsupported character: drop it rather than fail the
               whole string, matching the DOS agent's TYPE behavior. */
            continue;
        }
    }
    {
        int rc = run_journal_events();
        if (rc < 0) {
            send_journal_error(rc);
            return -1;
        }
    }
    send_cstr("OK\r\n");
    return 0;
}

/* ---- CLICK <x> <y> [button] -- screen coordinates, button 1=left
   (default) or 2=right. ---- */

static int handle_click(const char *args) {
    int x = 0, y = 0, button = 1;

    if (sscanf(args, "%d %d %d", &x, &y, &button) < 2) {
        send_cstr("ERR:bad CLICK syntax\r\n");
        return -1;
    }

    g_event_count = 0;
    push_event(WM_MOUSEMOVE, (UINT)x, (UINT)y);
    if (button == 2) {
        push_event(WM_RBUTTONDOWN, (UINT)x, (UINT)y);
        push_event(WM_RBUTTONUP, (UINT)x, (UINT)y);
    } else {
        push_event(WM_LBUTTONDOWN, (UINT)x, (UINT)y);
        push_event(WM_LBUTTONUP, (UINT)x, (UINT)y);
    }

    {
        int rc = run_journal_events();
        if (rc < 0) {
            send_journal_error(rc);
            return -1;
        }
    }
    send_cstr("OK\r\n");
    return 0;
}

/* ---- REBOOT via ExitWindows(EW_REBOOTSYSTEM) -- unlike plain
   ExitWindows(EW_RESTARTWINDOWS), this resets the whole machine rather
   than just dropping back to a DOS prompt. Needs WINVER >= 0x030A,
   which is this SDK's default and true for WFW 3.11. ---- */

static int handle_reboot(void) {
    int i;

    send_cstr("OK\r\n");
    /* Give the ACK a moment to actually leave the NIC before Windows
       (and WINSOCK.DLL under it) starts tearing down. */
    for (i = 0; i < 20; i++) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Yield();
    }
    ExitWindows(EW_REBOOTSYSTEM, 0);
    /* Only reached if Windows refused to exit (e.g. an app vetoed
       WM_QUERYENDSESSION). */
    return -1;
}

/* ---- SHUTDOWN via ExitWindows(0) -- Windows 3.1 predates ACPI/APM power
   management in the OS API surface entirely, so there is no Win16
   equivalent of Win32's ExitWindowsEx(EWX_POWEROFF); this VM also has no
   VMware Tools available for a DOS/Win3.1x guest to request a real
   power-off from the hypervisor.

   ExitWindows()'s low word is normally just an MS-DOS errorlevel handed
   back to whatever continues after Windows exits -- EW_RESTARTWINDOWS
   (0x42) and EW_REBOOTSYSTEM (0x43) are the only two values it
   special-cases, both of which mean "reload/reset", not "exit and stay
   out". Confirmed by direct testing: passing EW_RESTARTWINDOWS here
   looked identical to REBOOT from the console (agent came back on its
   own within moments) -- exactly what its name says it does, restart
   Windows, not drop to DOS. Plain 0 is what actually exits to a DOS
   prompt and stays there, the same as Program Manager's File > Exit
   Windows, without powering off or resetting the VM itself. ---- */

static int handle_shutdown(void) {
    int i;

    send_cstr("OK\r\n");
    for (i = 0; i < 20; i++) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Yield();
    }
    ExitWindows(0, 0);
    /* Only reached if Windows refused to exit (e.g. an app vetoed
       WM_QUERYENDSESSION). */
    return -1;
}

/* ---- UPDATE: self-update without a full system REBOOT. Overwriting
   LLMAGENT.EXE on disk (PUT) never took effect for an already-running
   instance before this existed -- Win16 keeps a module's code resident
   in memory by name until every instance of it has exited
   (GetModuleUsage() hits zero), so WinExec()'ing the same path again
   while we're still up would just hand back another instance of the
   OLD code already loaded, not the new bytes on disk. So the update
   client stages the new binary as LLMNEW.EXE, and RESTART.EXE
   (restart.c, a separate tiny helper, built via build_restart.bat) is
   the piece that has to outlive us: it waits for us to exit, renames
   LLMAGENT.EXE to LLMAGENT.OLD for recovery, moves LLMNEW.EXE into
   place, then launches the fresh copy.

   Does NOT call DestroyWindow() to reuse the Exit button's WM_DESTROY
   cleanup path, despite how tempting that looked -- confirmed by direct
   testing that calling DestroyWindow() from this deep, non-WndProc call
   stack (accept() -> handle_client() -> handle_update()) GPFs inside
   the C runtime's own _exit_ cleanup once WinMain eventually returns
   (crash address traced via llm_agent.map to fall inside _exit_'s
   range). The Exit button's identical-looking DestroyWindow() call
   never crashed because it runs from *within* WndProc's own WM_COMMAND
   handling, a context Windows actually expects a window to destroy
   itself from -- ours isn't that.

   Also does NOT manually closesocket() g_client/g_listen the way
   WM_DESTROY does, despite how closely that seems to mirror it -- a
   first version of this did exactly that and froze the ENTIRE desktop
   solid (not just this agent), needing a VM reboot to recover.
   WM_DESTROY's force-close exists to unblock a call that's genuinely
   *blocked* elsewhere (accept()/recv() during an async WM_DESTROY
   delivered mid-block). handle_update() isn't in that situation -- it
   runs synchronously, already past accept() and deep inside
   handle_client()'s own command loop, and server_main()'s existing
   fall-through cleanup closes both sockets exactly once anyway
   (closesocket(g_client) unconditionally right after handle_client()
   returns; closesocket(g_listen) once the outer loop notices
   g_shutdown). Closing them here too double-closed both handles.
   WINSOCK.DLL's state is shared system-wide across every Win16 app,
   not per-process -- corrupting it doesn't stay contained to this
   agent, which is almost certainly why the whole desktop froze rather
   than just this one task. Setting the flag and returning is
   sufficient; let the normal path do the rest exactly once. ---- */

static int handle_update(void) {
    char cmd[288];
    int i;

    send_cstr("OK\r\n");
    for (i = 0; i < 10; i++) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Yield();
    }

    sprintf(cmd, "%s\\RESTART.EXE %s\\LLMNEW.EXE %s\\LLMAGENT.EXE",
            g_exedir, g_exedir, g_exedir);
    WinExec(cmd, SW_SHOWMINNOACTIVE);

    g_shutdown = 1;
    return 0;
}

/* ---- PSLIST / PSKILL via ToolHelp -- Windows 3.1 has no real process
   model (no isolated address spaces, no PIDs), but it does have "tasks",
   and TOOLHELP.DLL is the documented, official API for enumerating and
   terminating them -- it's what Windows 3.1's own Task List (Ctrl+Esc)
   and the Ctrl+Alt+Del handler use internally. HTASK stands in for a
   PID: it's just a 16-bit handle, but it's the closest thing this OS
   has, and it's what TerminateApp()/TaskFindHandle() key off. ---- */

/* Basename of a path, with tabs/CR/LF scrubbed to keep the wire format's
   tab-delimited lines intact regardless of what a module's path contains. */
static void path_basename(char *dst, size_t dstsz, const char *src) {
    const char *base = src;
    const char *p;
    size_t n;
    char *d;

    if (!src) src = "";
    for (p = src; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    n = strlen(base);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, base, n);
    dst[n] = '\0';
    for (d = dst; *d; d++) {
        if (*d == '\t' || *d == '\r' || *d == '\n') *d = ' ';
    }
}

static int handle_pslist(void) {
    static char out[8192];
    int len = 0;
    char hdr[32];
    char name[MAX_PATH + 1];
    TASKENTRY te;
    MODULEENTRY me;
    BOOL ok;

    te.dwSize = sizeof(TASKENTRY);
    for (ok = TaskFirst(&te); ok; ok = TaskNext(&te)) {
        me.dwSize = sizeof(MODULEENTRY);
        if (ModuleFindHandle(&me, te.hModule)) {
            path_basename(name, sizeof(name), me.szExePath);
        } else {
            path_basename(name, sizeof(name), te.szModule);
        }
        if (len > (int)sizeof(out) - 160) break;
        len += sprintf(out + len, "%u\t%s\r\n", (unsigned)te.hTask, name);
    }

    sprintf(hdr, "SIZE:%d\n", len);
    if (send_cstr(hdr) < 0) return -1;
    if (len > 0 && send_all(out, len) < 0) return -1;
    return 0;
}

/* PSKILL <hTask>: TerminateApp(hTask, NO_UAE_BOX) -- the same call
   Windows 3.1's own Task List "End Task" button and Ctrl+Alt+Del use.
   NO_UAE_BOX suppresses the "has caused a General Protection Fault"
   dialog a forced kill would otherwise show, so this doesn't leave a
   blocking dialog behind the way the crashes earlier in this file's
   history did -- confirmed no such dialog appears via direct testing.
   Refuses to kill this agent's own task; TerminateApp on yourself would
   just silently drop the connection with none of server_main()'s normal
   socket cleanup. */
static void handle_pskill(const char *args) {
    HTASK hTask;
    TASKENTRY te;

    while (*args == ' ') args++;
    hTask = (HTASK)atoi(args);
    if (hTask == 0) {
        send_cstr("ERR:bad PID\r\n");
        return;
    }
    if (hTask == GetCurrentTask()) {
        send_cstr("ERR:refusing to kill this agent's own task\r\n");
        return;
    }

    te.dwSize = sizeof(TASKENTRY);
    if (!TaskFindHandle(&te, hTask)) {
        send_cstr("ERR:no such task\r\n");
        return;
    }

    TerminateApp(hTask, NO_UAE_BOX);
    send_cstr("OK\r\n");
}

/* ---- session ---- */

static void handle_client(void) {
    if (recv_line(g_line, sizeof(g_line)) < 0) return;
    if (g_token[0] == '\0' || strcmp(g_line, g_token) != 0) {
        send_cstr("FAIL\r\n");
        return;
    }
    send_cstr("OK\r\n");

    for (;;) {
        if (recv_line(g_line, sizeof(g_line)) < 0) break;

        if (strncmp(g_line, "EXEC ", 5) == 0) {
            run_exec(g_line + 5);
        } else if (strncmp(g_line, "PUT ", 4) == 0) {
            handle_put(g_line + 4);
        } else if (strncmp(g_line, "GET ", 4) == 0) {
            handle_get(g_line + 4);
        } else if (strcmp(g_line, "SCREENSHOT") == 0) {
            handle_screenshot();
        } else if (strcmp(g_line, "SYSINFO") == 0) {
            handle_sysinfo();
        } else if (strcmp(g_line, "PING") == 0) {
            send_cstr("PONG\r\n");
        } else if (strcmp(g_line, "QUIT") == 0) {
            break;
        } else if (strcmp(g_line, "REBOOT") == 0) {
            handle_reboot();
        } else if (strcmp(g_line, "SHUTDOWN") == 0) {
            handle_shutdown();
        } else if (strcmp(g_line, "UPDATE") == 0) {
            handle_update();
        } else if (strncmp(g_line, "KEY ", 4) == 0) {
            handle_key(g_line + 4);
        } else if (strncmp(g_line, "TYPE ", 5) == 0) {
            handle_type(g_line + 5);
        } else if (strncmp(g_line, "CLICK ", 6) == 0) {
            handle_click(g_line + 6);
        } else if (strcmp(g_line, "WINLIST") == 0) {
            handle_winlist("");
        } else if (strncmp(g_line, "WINLIST ", 8) == 0) {
            handle_winlist(g_line + 8);
        } else if (strncmp(g_line, "WINMSG ", 7) == 0) {
            handle_winmsg(g_line + 7);
        } else if (strncmp(g_line, "POSTMSG ", 8) == 0) {
            handle_postmsg(g_line + 8);
        } else if (strncmp(g_line, "LBGETTEXT ", 10) == 0) {
            handle_lbgettext(g_line + 10);
        } else if (strncmp(g_line, "EXECDETACH ", 11) == 0) {
            handle_execdetach(g_line + 11);
        } else if (strcmp(g_line, "PSLIST") == 0) {
            handle_pslist();
        } else if (strncmp(g_line, "PSKILL ", 7) == 0) {
            handle_pskill(g_line + 7);
        } else if (strncmp(g_line, "CLIPSET ", 8) == 0 ||
                   strncmp(g_line, "REGGET\t", 7) == 0 ||
                   strncmp(g_line, "REGSET\t", 7) == 0) {
            send_cstr(ERR_NOSUP);
        } else if (g_line[0] == '\0') {
            /* ignore */
        } else {
            send_cstr("ERR:unknown command\r\n");
        }
    }
}

/* ---- window / winsock bring-up ---- */

long _EXPORT FAR PASCAL WndProc(HWND hwnd, unsigned msg, UINT wParam, LONG lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        char line[128];
        sprintf(line, "llm_agent-win16 -- listening on port %u", (unsigned)g_port);
        TextOut(hdc, 8, 8, line, lstrlen(line));
        sprintf(line, "IP address: %s", g_ipstr);
        TextOut(hdc, 8, 26, line, lstrlen(line));
        sprintf(line, "token configured: %s", g_token[0] ? "yes" : "NO - set token= in LLMAGENT.INI");
        TextOut(hdc, 8, 44, line, lstrlen(line));
        EndPaint(hwnd, &ps);
        return 0L;
    }
    case WM_COMMAND:
        if (wParam == IDC_EXIT_BTN) {
            /* Route through the exact same shutdown path as the system
               menu's Close -- DestroyWindow() triggers WM_DESTROY below,
               which is the one place g_shutdown gets set and both
               sockets get force-closed. No separate logic to keep in
               sync. */
            DestroyWindow(hwnd);
            return 0L;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_AGENT_SOCKET:
        if (WSAGETSELECTERROR(lParam) != 0) {
            return 0L;
        }
        if (WSAGETSELECTEVENT(lParam) == FD_ACCEPT) {
            g_accept_ready = 1;
            return 0L;
        }
        if (WSAGETSELECTEVENT(lParam) == FD_CLOSE) {
            return 0L;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_DESTROY:
        /* Force any blocked recv() to fail and wake server_main()'s
           WaitMessage()-based listener so the process exits cleanly. */
        g_shutdown = 1;
        if (g_client != INVALID_SOCKET) closesocket(g_client);
        if (g_listen != INVALID_SOCKET) closesocket(g_listen);
        PostQuitMessage(0);
        return 0L;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static BOOL init_window(HINSTANCE hInst, HINSTANCE hPrevInst, int cmdShow) {
    WNDCLASS wc;

    /* A window class only needs registering once per module, not once
       per running instance -- registering it again for a second
       instance is at best redundant and is the kind of thing to rule
       out rather than leave in when a fresh launch just GPF'd. */
    if (!hPrevInst) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = (WNDPROC)WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = "LLMAgentWin16";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClass(&wc)) return FALSE;
    }

    g_hwnd = CreateWindow("LLMAgentWin16", "LLM Agent", WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, 340, 155,
                          NULL, NULL, hInst, NULL);
    if (!g_hwnd) return FALSE;

    CreateWindow("BUTTON", "Exit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                 8, 70, 80, 24, g_hwnd, (HMENU)IDC_EXIT_BTN, hInst, NULL);

    ShowWindow(g_hwnd, cmdShow);
    UpdateWindow(g_hwnd);
    return TRUE;
}

static int server_main(void) {
    struct sockaddr_in addr;

    while (!g_shutdown) {
        g_listen = socket(AF_INET, SOCK_STREAM, 0);
        if (g_listen == INVALID_SOCKET) return 1;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(g_port);

        /* Retry bind() for a few seconds instead of failing immediately.
           Right after UPDATE's self-relaunch, the just-exited old
           instance's listening socket on this same port isn't always
           released by WINSOCK.DLL's TCP/IP stack instantly -- the same
           shape of lag the OS/2 agent's self-update hit and documented
           against real hardware (see agent-os2-13/llm_agent.c). Without
           this, an ordinary UPDATE could intermittently land on a
           blocking MessageBox needing a manual dismiss for no reason
           other than timing. */
        {
            int attempt;
            int bound = 0;
            for (attempt = 0; attempt < 10; attempt++) {
                if (bind(g_listen, (struct sockaddr FAR *)&addr, sizeof(addr)) != SOCKET_ERROR) {
                    bound = 1;
                    break;
                }
                {
                    DWORD start = GetTickCount();
                    while (GetTickCount() - start < 500UL) {
                        MSG msg;
                        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&msg);
                            DispatchMessage(&msg);
                        }
                        Yield();
                    }
                }
            }
            if (!bound) {
                MessageBox(g_hwnd, "bind() failed -- is the port already in use?",
                           "llm_agent-win16", MB_OK | MB_ICONHAND);
                return 1;
            }
        }
        if (listen(g_listen, 1) == SOCKET_ERROR) {
            /* Previously unchecked -- a silent failure here left the
               status window painting its normal "listening on port"
               text (that text is static/config-derived, not tied to
               actual bind/listen success) while nothing was really
               bound, so a client saw connection-refused with no visible
               sign anything was wrong on the console. */
            MessageBox(g_hwnd, "listen() failed after a successful bind()",
                       "llm_agent-win16", MB_OK | MB_ICONHAND);
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
            return 1;
        }
        if (WSAAsyncSelect(g_listen, g_hwnd, WM_AGENT_SOCKET, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
            MessageBox(g_hwnd, "WSAAsyncSelect() failed for listening socket",
                       "llm_agent-win16", MB_OK | MB_ICONHAND);
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
            return 1;
        }

        while (!g_shutdown) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_shutdown = 1;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (g_shutdown) break;

            if (g_accept_ready) {
                g_accept_ready = 0;
                for (;;) {
                    unsigned long blocking = 0;

                    g_client = accept(g_listen, NULL, NULL);
                    if (g_client == INVALID_SOCKET) break;

                    WSAAsyncSelect(g_client, g_hwnd, 0, 0);
                    ioctlsocket(g_client, FIONBIO, &blocking);
                    handle_client();
                    closesocket(g_client);
                    g_client = INVALID_SOCKET;
                    if (g_shutdown) break;
                }
            }

            if (!g_shutdown) WaitMessage();
        }
        WSAAsyncSelect(g_listen, g_hwnd, 0, 0);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    WSACleanup();
    return 0;
}

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdLine, int cmdShow) {
    WSADATA wsaData;

    cmdLine = cmdLine;

    g_hinst = hInst;
    load_config(hInst);
    if (!init_window(hInst, hPrevInst, cmdShow)) return 0;

    /* 0x0101 = Winsock version 1.1 (low byte = major, high byte = minor);
       this SDK's headers do not define MAKEWORD. */
    if (WSAStartup((WORD)0x0101, &wsaData) != 0) {
        MessageBox(g_hwnd, "WSAStartup failed -- is WFW networking (NET START) up?",
                   "llm_agent-win16", MB_OK | MB_ICONHAND);
        return 1;
    }
    get_local_ip(g_ipstr, sizeof(g_ipstr));
    InvalidateRect(g_hwnd, NULL, TRUE);
    UpdateWindow(g_hwnd);

    return server_main();
}
