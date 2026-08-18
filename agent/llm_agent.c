/*
 * llm_agent: minimal command-exec + file-transfer agent for legacy Windows
 * (95/98/ME/NT4/2000/XP).
 *
 * Design constraints (see docs/ARCHITECTURE.md for the why):
 *  - ANSI-only (no "W" APIs) - Windows 9x's wide-char entry points are mostly stubs.
 *  - No threads, no IPv6, no CryptoAPI - keeps it running on the whole OS range
 *    with only kernel32/advapi32/user32/ws2_32, all present since Win95/NT4.
 *  - Single-connection-at-a-time accept loop - this is a lab automation tool
 *    driven by one LLM/bridge process, not a multi-user server.
 *  - Plain pre-shared-token auth, no transport encryption - only safe because
 *    this is meant to run on an isolated host-only/lab network. Do not expose
 *    this port to anything else.
 *
 * Wire protocol (line-oriented, one connection = one session):
 *   client -> server: first line is the token.
 *   server -> client: "OK\n" or "FAIL\n" (then closes on FAIL).
 *   client -> server: "EXEC <cmdline>\n" | "PUT <path> <size>\n" | "GET <path>\n"
 *                      | "SCREENSHOT\n" | "CLICK <x> <y> <button>\n"
 *                      | "KEY <keyspec>\n" | "TYPE <text>\n" | "PING\n" | "QUIT\n"
 *   server -> client (EXEC): repeated "LEN:<n>\n" + <n> raw bytes of combined
 *                            stdout/stderr, then a final "EXIT:<code>\n".
 *   client -> server (PUT):  <size> raw bytes immediately following the PUT line.
 *   server -> client (PUT):  "OK\n" | "ERR:<msg>\n"
 *   server -> client (GET):  "SIZE:<n>\n" + <n> raw bytes, or "ERR:<msg>\n"
 *   server -> client (SCREENSHOT): "SIZE:<n>\n" + <n> raw bytes of a BMP
 *                                  file (BITMAPFILEHEADER+INFOHEADER+pixels),
 *                                  or "ERR:<msg>\n"
 *   server -> client (CLICK/KEY/TYPE): "OK\n" | "ERR:<msg>\n"
 *   server -> client (PING): "PONG\n"
 *
 * File sizes are 32-bit (~4GB ceiling, practically ~2GB via the signed APIs
 * used here) - plenty for installer media and driver packages from this
 * OS era; not meant for anything larger.
 *
 * SCREENSHOT/CLICK/KEY/TYPE use mouse_event/keybd_event, not the newer
 * SendInput - SendInput doesn't exist on Windows 9x, and these do, so this
 * stays consistent with the rest of the agent working across the whole
 * 9x-XP range. Also: pre-Vista Windows has no Session 0 isolation, so an
 * NT-service instance of this agent CAN see/drive the real interactive
 * desktop, but only because install_nt_service() registers it with
 * SERVICE_INTERACTIVE_PROCESS - without that flag these commands would
 * silently operate on an invisible, disconnected window station instead
 * (see docs/ARCHITECTURE.md).
 */

#include <windows.h>
#include <winsvc.h>
#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SERVICE_NAME_A   "LLMAgent"
#define SERVICE_DISPLAY  "LLM Agent (exec + file-transfer bridge service)"
#define DEFAULT_PORT     2222
#define LINE_MAX_LEN     4096
#define READ_CHUNK       4096

static char g_token[256] = "";
static int  g_port = DEFAULT_PORT;
static SERVICE_STATUS_HANDLE g_svcStatusHandle = 0;
static SERVICE_STATUS g_svcStatus;
static volatile int g_running = 1;

/* ---- config: llm_agent.ini next to the exe: port=NNNN / token=... ---- */
static void load_config(void) {
    char path[MAX_PATH];
    char line[512];
    FILE *f;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    if (n == 0 || n >= MAX_PATH) return;
    slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat(path, "llm_agent.ini");

    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "port=", 5) == 0) {
            g_port = atoi(line + 5);
            if (g_port <= 0) g_port = DEFAULT_PORT;
        } else if (strncmp(line, "token=", 6) == 0) {
            strncpy(g_token, line + 6, sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = '\0';
        }
    }
    fclose(f);
}

/* ---- OS family detection: high bit of GetVersion() set => Windows 9x ---- */
static int is_windows_9x(void) {
    return (GetVersion() & 0x80000000) != 0;
}

/* ---- run a command line via cmd.exe /C, stream combined stdout+stderr ---- */
static int run_exec(SOCKET s, const char *cmdline) {
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char full[LINE_MAX_LEN + 16];
    char buf[READ_CHUNK];
    DWORD exitCode = 1;
    BOOL ok;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return -1;
    }
    /* Read end must NOT be inherited, or the child holds it open and
       ReadFile below never sees EOF. Classic MSDN "redirected pipes" gotcha. */
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    ZeroMemory(&pi, sizeof(pi));

    wsprintfA(full, "cmd.exe /C %s", cmdline);

    ok = CreateProcessA(NULL, full, NULL, NULL, TRUE,
                         CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe); /* parent's copy - child holds its own */

    if (!ok) {
        CloseHandle(hReadPipe);
        return -1;
    }

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(hReadPipe, buf, sizeof(buf), &got, NULL) || got == 0) {
            break;
        }
        {
            char hdr[32];
            wsprintfA(hdr, "LEN:%lu\n", (unsigned long)got);
            send(s, hdr, (int)strlen(hdr), 0);
            send(s, buf, (int)got, 0);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    {
        char tail[64];
        wsprintfA(tail, "EXIT:%lu\n", (unsigned long)exitCode);
        send(s, tail, (int)strlen(tail), 0);
    }
    return 0;
}

/* ---- PUT <path> <size>: read <size> raw bytes off the socket, write to path.
   Parses from the right (last space) so paths with spaces work. Drains the
   declared byte count even on a local error, to keep the connection framed
   correctly for whatever command comes next. ---- */
static int handle_put(SOCKET s, char *args) {
    char *lastSpace = strrchr(args, ' ');
    long size;
    HANDLE hFile;
    BOOL openFailed;

    if (!lastSpace) {
        send(s, "ERR:bad PUT syntax\n", 20, 0);
        return -1;
    }
    size = atol(lastSpace + 1);
    *lastSpace = '\0';

    if (size < 0) {
        send(s, "ERR:bad size\n", 13, 0);
        return -1;
    }

    hFile = CreateFileA(args, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    openFailed = (hFile == INVALID_HANDLE_VALUE);

    {
        long remaining = size;
        char buf[READ_CHUNK];
        while (remaining > 0) {
            int want = remaining < (long)sizeof(buf) ? (int)remaining : (int)sizeof(buf);
            int got = recv(s, buf, want, 0);
            if (got <= 0) { openFailed = TRUE; break; }
            if (!openFailed) {
                DWORD written;
                WriteFile(hFile, buf, (DWORD)got, &written, NULL);
            }
            remaining -= got;
        }
    }

    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (openFailed) {
        send(s, "ERR:write failed\n", 18, 0);
        return -1;
    }
    send(s, "OK\n", 3, 0);
    return 0;
}

/* ---- GET <path>: send "SIZE:<n>\n" then <n> raw bytes of the file. ---- */
static int handle_get(SOCKET s, const char *path) {
    HANDLE hFile;
    DWORD size;
    char hdr[32];
    char buf[READ_CHUNK];
    DWORD got;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        send(s, "ERR:cannot open file\n", 22, 0);
        return -1;
    }

    size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        send(s, "ERR:cannot stat file\n", 22, 0);
        return -1;
    }

    wsprintfA(hdr, "SIZE:%lu\n", (unsigned long)size);
    send(s, hdr, (int)strlen(hdr), 0);

    while (ReadFile(hFile, buf, sizeof(buf), &got, NULL) && got > 0) {
        send(s, buf, (int)got, 0);
    }

    CloseHandle(hFile);
    return 0;
}

/* ---- SCREENSHOT: capture the interactive desktop as a BMP. GetDIBits does
   the color-depth conversion to 24-bit for us regardless of the actual
   screen depth (8-bit palette displays included), so no palette handling
   needed here. ---- */
static int handle_screenshot(SOCKET s) {
    HDC hScreenDC, hMemDC;
    HBITMAP hBitmap, hOldBitmap;
    int width, height;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    DWORD imageSize, fileSize;
    BYTE *pixels;
    char hdr[32];

    hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        send(s, "ERR:GetDC failed\n", 18, 0);
        return -1;
    }

    width = GetDeviceCaps(hScreenDC, HORZRES);
    height = GetDeviceCaps(hScreenDC, VERTRES);

    hMemDC = CreateCompatibleDC(hScreenDC);
    hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);

    BitBlt(hMemDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY);

    ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = height; /* positive = standard bottom-up DIB */
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    imageSize = ((width * 3 + 3) & ~3UL) * (DWORD)height; /* rows padded to 4 bytes */
    pixels = (BYTE *)malloc(imageSize);
    if (!pixels || !GetDIBits(hMemDC, hBitmap, 0, height, pixels, (BITMAPINFO *)&bi, DIB_RGB_COLORS)) {
        if (pixels) free(pixels);
        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        send(s, "ERR:capture failed\n", 20, 0);
        return -1;
    }

    ZeroMemory(&bf, sizeof(bf));
    bf.bfType = 0x4D42; /* 'BM' */
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileSize = bf.bfOffBits + imageSize;
    bf.bfSize = fileSize;

    wsprintfA(hdr, "SIZE:%lu\n", (unsigned long)fileSize);
    send(s, hdr, (int)strlen(hdr), 0);
    send(s, (char *)&bf, sizeof(bf), 0);
    send(s, (char *)&bi, sizeof(bi), 0);
    send(s, (char *)pixels, (int)imageSize, 0);

    free(pixels);
    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    return 0;
}

/* ---- CLICK <x> <y> <button>: move the cursor and click. button 1/2/3 =
   left/middle/right, matching the numbering the bridge already uses. ---- */
static int handle_click(SOCKET s, const char *args) {
    int x, y, button;

    if (sscanf(args, "%d %d %d", &x, &y, &button) != 3) {
        send(s, "ERR:bad CLICK syntax\n", 22, 0);
        return -1;
    }

    SetCursorPos(x, y);
    switch (button) {
        case 1:
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case 2:
            mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0);
            break;
        case 3:
            mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            break;
        default:
            send(s, "ERR:bad button (use 1/2/3)\n", 28, 0);
            return -1;
    }
    send(s, "OK\n", 3, 0);
    return 0;
}

typedef struct { const char *name; WORD vk; } KeyName;

/* Named keys for KEY <keyspec>. Single ASCII characters (letters, digits,
   punctuation) don't need an entry here - VkKeyScanA handles those,
   including which ones need Shift. */
static const KeyName KEY_NAMES[] = {
    {"enter", VK_RETURN}, {"return", VK_RETURN},
    {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
    {"tab", VK_TAB}, {"space", VK_SPACE},
    {"backspace", VK_BACK}, {"bksp", VK_BACK},
    {"delete", VK_DELETE}, {"del", VK_DELETE},
    {"insert", VK_INSERT}, {"ins", VK_INSERT},
    {"home", VK_HOME}, {"end", VK_END},
    {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
    {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
    {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
    {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
    {"win", VK_LWIN}, {"lwin", VK_LWIN}, {"rwin", VK_RWIN},
    {"capslock", VK_CAPITAL}, {"numlock", VK_NUMLOCK}, {"scrolllock", VK_SCROLL},
    {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT}, {"pause", VK_PAUSE},
    {NULL, 0}
};

static int lookup_named_key(const char *tok, WORD *vkOut) {
    int i;
    for (i = 0; KEY_NAMES[i].name; i++) {
        if (_stricmp(tok, KEY_NAMES[i].name) == 0) { *vkOut = KEY_NAMES[i].vk; return 1; }
    }
    return 0;
}

static void press_vk(WORD vk, int down) {
    keybd_event((BYTE)vk, 0, down ? 0 : KEYEVENTF_KEYUP, 0);
}

/* ---- KEY <keyspec>: press a single key or "mod-mod-key" combo, e.g.
   "enter", "a", "shift-a", "ctrl-c", "alt-tab", "ctrl-alt-del".
   Note: a synthetic Ctrl+Alt+Del does NOT trigger the secure Winlogon SAS
   on NT-family Windows - that's an intentional OS security measure real
   VNC/RDP hit too, not a bug here. Sending it is harmless but won't
   unlock a locked/secure-desktop screen. ---- */
static int handle_key(SOCKET s, const char *keyspec) {
    char buf[128];
    char tokens[8][32];
    int ntok = 0;
    int i, ctrl = 0, alt = 0, shift = 0, needShift = 0;
    char *tok;
    WORD vk = 0;

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
        send(s, "ERR:empty key\n", 15, 0);
        return -1;
    }

    for (i = 0; i < ntok - 1; i++) {
        if (_stricmp(tokens[i], "ctrl") == 0 || _stricmp(tokens[i], "control") == 0) ctrl = 1;
        else if (_stricmp(tokens[i], "alt") == 0) alt = 1;
        else if (_stricmp(tokens[i], "shift") == 0) shift = 1;
        else {
            send(s, "ERR:unknown modifier\n", 22, 0);
            return -1;
        }
    }

    {
        const char *base = tokens[ntok - 1];
        if (!lookup_named_key(base, &vk)) {
            if (strlen(base) == 1) {
                SHORT r = VkKeyScanA(base[0]);
                if (r == -1) {
                    send(s, "ERR:unmappable character\n", 26, 0);
                    return -1;
                }
                vk = (BYTE)(r & 0xFF);
                if (r & 0x0100) needShift = 1;
            } else {
                send(s, "ERR:unknown key name\n", 22, 0);
                return -1;
            }
        }
    }

    if (ctrl) press_vk(VK_CONTROL, 1);
    if (alt) press_vk(VK_MENU, 1);
    if (shift || needShift) press_vk(VK_SHIFT, 1);

    press_vk(vk, 1);
    press_vk(vk, 0);

    if (shift || needShift) press_vk(VK_SHIFT, 0);
    if (alt) press_vk(VK_MENU, 0);
    if (ctrl) press_vk(VK_CONTROL, 0);

    send(s, "OK\n", 3, 0);
    return 0;
}

/* ---- TYPE <text>: type literal text one keystroke at a time via
   VkKeyScanA (handles Shift for uppercase/punctuation). Characters that
   don't map to a key on the current keyboard layout are skipped. ---- */
static int handle_type(SOCKET s, const char *text) {
    const char *p;
    for (p = text; *p; p++) {
        SHORT r = VkKeyScanA(*p);
        WORD vk;
        int needShift;
        if (r == -1) continue;
        vk = (BYTE)(r & 0xFF);
        needShift = (r & 0x0100) != 0;
        if (needShift) press_vk(VK_SHIFT, 1);
        press_vk(vk, 1);
        press_vk(vk, 0);
        if (needShift) press_vk(VK_SHIFT, 0);
    }
    send(s, "OK\n", 3, 0);
    return 0;
}

static int recv_line(SOCKET s, char *out, int outlen) {
    int i = 0;
    char c;
    while (i < outlen - 1) {
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') out[i++] = c;
    }
    out[i] = '\0';
    return i;
}

static void handle_client(SOCKET s) {
    char line[LINE_MAX_LEN];

    if (recv_line(s, line, sizeof(line)) < 0) return;
    if (g_token[0] == '\0' || strcmp(line, g_token) != 0) {
        send(s, "FAIL\n", 5, 0);
        return;
    }
    send(s, "OK\n", 3, 0);

    for (;;) {
        if (recv_line(s, line, sizeof(line)) < 0) break;
        if (strncmp(line, "EXEC ", 5) == 0) {
            run_exec(s, line + 5);
        } else if (strncmp(line, "PUT ", 4) == 0) {
            handle_put(s, line + 4);
        } else if (strncmp(line, "GET ", 4) == 0) {
            handle_get(s, line + 4);
        } else if (strcmp(line, "SCREENSHOT") == 0) {
            handle_screenshot(s);
        } else if (strncmp(line, "CLICK ", 6) == 0) {
            handle_click(s, line + 6);
        } else if (strncmp(line, "KEY ", 4) == 0) {
            handle_key(s, line + 4);
        } else if (strncmp(line, "TYPE ", 5) == 0) {
            handle_type(s, line + 5);
        } else if (strcmp(line, "PING") == 0) {
            send(s, "PONG\n", 5, 0);
        } else if (strcmp(line, "QUIT") == 0) {
            break;
        } else if (line[0] == '\0') {
            /* ignore blank lines */
        } else {
            send(s, "ERR:unknown command\n", 21, 0);
        }
    }
}

static int server_main(void) {
    WSADATA wsa;
    SOCKET listenSock;
    struct sockaddr_in addr;

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        return 1;
    }

    listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)g_port);

    if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }
    listen(listenSock, 4);

    while (g_running) {
        struct sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(listenSock, (struct sockaddr *)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET) {
            if (!g_running) break;
            continue;
        }
        handle_client(client);
        closesocket(client);
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}

/* ---- NT service plumbing ---- */
static void WINAPI svc_ctrl_handler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_running = 0;
        g_svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
    }
}

static void WINAPI svc_main(DWORD argc, LPSTR *argv) {
    (void)argc; (void)argv;
    g_svcStatusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME_A, svc_ctrl_handler);
    if (!g_svcStatusHandle) return;

    ZeroMemory(&g_svcStatus, sizeof(g_svcStatus));
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
    g_svcStatus.dwCurrentState = SERVICE_RUNNING;
    g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);

    server_main();

    g_svcStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

static int install_nt_service(void) {
    char path[MAX_PATH];
    char cmd[MAX_PATH + 16];
    SC_HANDLE scm, svc;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    /* Must pass "--run" so the SCM-launched process knows to call
       StartServiceCtrlDispatcher instead of falling through to
       print_usage() and exiting immediately - and must quote the path,
       since it commonly contains spaces (e.g. "...\My Documents\..."),
       which would otherwise make Windows try to split it as if the exe
       were literally named "C:\Documents". */
    wsprintfA(cmd, "\"%s\" --run", path);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { printf("OpenSCManager failed: %lu\n", GetLastError()); return 1; }

    /* SERVICE_INTERACTIVE_PROCESS: pre-Vista Windows has no Session 0
       isolation, so a LocalSystem service CAN see/drive the logged-on
       user's real desktop for SCREENSHOT/CLICK/KEY/TYPE - but only if
       registered with this flag. Without it, those commands would
       silently run against an invisible, disconnected window station
       instead of what's actually on screen. Only valid combined with
       LocalSystem (the NULL account below), which is what we already use. */
    svc = CreateServiceA(scm, SERVICE_NAME_A, SERVICE_DISPLAY,
                          SERVICE_ALL_ACCESS,
                          SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                          SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                          cmd, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        printf("CreateService failed: %lu\n", e);
        return 1;
    }
    printf("Installed service '%s'. Start it with: net start %s\n", SERVICE_NAME_A, SERVICE_NAME_A);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int uninstall_nt_service(void) {
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    svc = OpenServiceA(scm, SERVICE_NAME_A, SERVICE_STOP | DELETE);
    if (!svc) { CloseServiceHandle(scm); return 1; }
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    printf("Uninstalled service '%s'.\n", SERVICE_NAME_A);
    return 0;
}

/* ---- Windows 9x autostart: HKLM Run key + RegisterServiceProcess ---- */
static int install_9x_autostart(void) {
    char path[MAX_PATH];
    char cmd[MAX_PATH + 16];
    HKEY key;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    wsprintfA(cmd, "\"%s\" --run", path);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        printf("RegOpenKeyEx failed: %lu\n", GetLastError());
        return 1;
    }
    RegSetValueExA(key, SERVICE_NAME_A, 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
    RegCloseKey(key);
    printf("Added Run-key autostart. It will start on next logon, or run now with:\n  %s\n", cmd);
    return 0;
}

static int uninstall_9x_autostart(void) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return 1;
    }
    RegDeleteValueA(key, SERVICE_NAME_A);
    RegCloseKey(key);
    printf("Removed Run-key autostart.\n");
    return 0;
}

typedef DWORD (WINAPI *RegisterServiceProcessFn)(DWORD, DWORD);

static void become_9x_background_process(void) {
    /* RegisterServiceProcess is a Win9x-only kernel32 export used by
       legitimate background tools of that era to survive logoff and
       stay off the taskbar. Not present on NT-family - look it up
       dynamically so this binary still links/runs fine there too. */
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    RegisterServiceProcessFn fn;
    if (!k32) return;
    fn = (RegisterServiceProcessFn)GetProcAddress(k32, "RegisterServiceProcess");
    if (fn) fn(0, 1);
}

static void print_usage(void) {
    printf(
        "llm_agent - minimal exec + file-transfer agent for legacy Windows\n\n"
        "Usage:\n"
        "  llm_agent.exe --install     Install as autostart (NT service, or Run-key on 9x)\n"
        "  llm_agent.exe --uninstall   Remove autostart\n"
        "  llm_agent.exe --run         Run in foreground (also used internally by autostart)\n\n"
        "Reads port/token from llm_agent.ini next to the exe:\n"
        "  port=2222\n"
        "  token=some-shared-secret\n"
    );
}

int main(int argc, char **argv) {
    load_config();

    if (argc >= 2 && strcmp(argv[1], "--install") == 0) {
        return is_windows_9x() ? install_9x_autostart() : install_nt_service();
    }
    if (argc >= 2 && strcmp(argv[1], "--uninstall") == 0) {
        return is_windows_9x() ? uninstall_9x_autostart() : uninstall_nt_service();
    }
    if (argc >= 2 && strcmp(argv[1], "--run") == 0) {
        if (is_windows_9x()) {
            become_9x_background_process();
            return server_main();
        }
        /* Fall through: NT-family "--run" is what the SCM launches; try the
           service dispatcher first, and if we're not actually being started
           by the SCM (e.g. run manually with --run for testing), just serve
           in the foreground instead. */
        {
            SERVICE_TABLE_ENTRYA table[] = {
                { SERVICE_NAME_A, svc_main },
                { NULL, NULL }
            };
            if (!StartServiceCtrlDispatcherA(table)) {
                if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                    return server_main();
                }
                return 1;
            }
            return 0;
        }
    }

    print_usage();
    return 0;
}
