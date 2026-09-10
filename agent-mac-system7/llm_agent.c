/* llm_agent for classic Mac OS (System 7.x) over MacTCP.
 *
 * Speaks the same token-authed TCP wire protocol as the other ports in
 * this repo (see ../mcp-server/agent_client.py for the authoritative
 * docs) so it can be driven by the same bridge tooling. Classic Mac OS
 * has no built-in command shell (no COMMAND.COM/CMD.EXE equivalent), so
 * EXEC/EXECDETACH are not implemented in this first pass -- everything
 * else (PING, SYSINFO, GET, PUT, SCREENSHOT, QUIT) is, plus three
 * commands not part of the shared protocol: QUITAGENT, which
 * terminates the agent process itself; PSLIST, which reports the live
 * Process Manager process list (useful for confirming whether a
 * process has actually exited -- Finder's icon rendering for these
 * icon-less apps looks identical whether they're running or not, which
 * isn't a reliable signal either way); and UPDATE, which takes a
 * MacBinary-encoded new build of this agent and self-updates with no
 * user interaction -- stages it, launches the llm_updater companion
 * app (see llm_updater.c) to decode/replace/relaunch, and exits. This
 * app runs with no console, no windows, and (see llm_agent.r) no
 * foreground UI at all -- there is no Finder menu or window to close
 * it from, so QUITAGENT is the only way to stop it short of rebooting
 * the machine, and llm_updater is the only way to replace it short of
 * StuffIt Expander + manual file swap.
 *
 * MacTCP is a driver-style API (PBControlSync/PBControlAsync on a
 * TCPiopb parameter block), not Berkeley sockets. System 7 is
 * cooperatively multitasked: a synchronous call that blocks for an
 * unbounded time (waiting for an incoming connection, waiting for data)
 * would stall the *entire machine*, not just this process. So long-wait
 * operations (TCPPassiveOpen, TCPRcv) are issued async and polled with
 * WaitNextEvent() in between (see Idle()) -- SystemTask() alone yields
 * to drivers/DAs but never actually hands the CPU to another
 * application under MultiFinder, which silently starves everything
 * else (including Finder) of CPU time. Quick operations (TCPCreate,
 * TCPSend, TCPClose, TCPRelease) are issued synchronously for
 * simplicity.
 */

#include <Types.h>

/* Multiversal's MixedMode.h is a stub -- it doesn't define the UPP
 * calling-convention descriptor machinery MacTCP.h's unconditional (not
 * GENERATINGCFM-guarded) enum block references. That enum only matters
 * for CFM/PowerPC builds; for our classic 68k, non-CFM build the values
 * are dead code, so trivial stand-ins just need to be syntactically
 * valid, not semantically correct. */
#ifndef GENERATINGCFM
#define GENERATINGCFM 0
#endif
#define kPascalStackBased 0
#define kCStackBased 0
#define SIZE_CODE(x) (x)
#define STACK_ROUTINE_PARAMETER(x, y) 0

#include <MacTCP.h>
#include <OSUtils.h>
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <Files.h>
#include <Gestalt.h>
#include <Quickdraw.h>
#include <Windows.h>
#include <Processes.h>
#include <ToolUtils.h>
#include <Errors.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define AGENT_PORT      2222
#define RCV_BUFFER_SIZE 16384
#define LINE_MAX_LEN    512
#define READ_CHUNK      8192
#define TOKEN_MAX_LEN   256

static short   gMacTCPRefNum;
static StreamPtr gStream;
static char    gRcvBuffer[RCV_BUFFER_SIZE];
static char    gToken[TOKEN_MAX_LEN];
static char    gLine[LINE_MAX_LEN];
static char    gIOBuf[READ_CHUNK];

/* ------------------------------------------------------------------ */
/* MacTCP driver plumbing                                             */
/* ------------------------------------------------------------------ */

static OSErr OpenMacTCP(void)
{
    return OpenDriver("\p.IPP", &gMacTCPRefNum);
}

/* Issue a control call synchronously. Fine for operations that complete
 * quickly (create/send/close/release) without risking a long stall. */
static OSErr TCPControlSync(TCPiopb *pb, short csCode)
{
    pb->ioCRefNum = gMacTCPRefNum;
    pb->csCode = csCode;
    pb->ioCompletion = NULL;
    return PBControlSync((ParmBlkPtr)pb);
}

/* Cooperative yield used while polling for an async MacTCP call to
 * complete. SystemTask() alone is NOT enough here -- it gives time to
 * drivers/desk accessories, but MultiFinder only actually switches to
 * a *different application* (e.g. Finder) in response to
 * WaitNextEvent/GetNextEvent. A poll loop that only calls SystemTask()
 * keeps our own driver I/O flowing (which is why the agent still
 * answered network requests) while silently starving every other
 * process of CPU time forever -- including Finder's own event loop,
 * which is what made the whole machine look "frozen" (mouse moved via
 * low-level cursor tracking, but no click was ever delivered to
 * anything, because nothing else was ever scheduled to run). We don't
 * care about the event's contents, just about periodically handing
 * control back to the Process Manager. */
static void Idle(void)
{
    EventRecord event;
    WaitNextEvent(everyEvent, &event, 1, NULL);
}

/* Issue a control call asynchronously and cooperatively poll for
 * completion, yielding to the rest of the system via Idle() between
 * checks. Use for anything that can block for an unbounded time
 * (waiting for a connection, waiting for data). */
static OSErr TCPControlWait(TCPiopb *pb, short csCode)
{
    pb->ioCRefNum = gMacTCPRefNum;
    pb->csCode = csCode;
    pb->ioCompletion = NULL;
    pb->ioResult = inProgress;
    PBControlAsync((ParmBlkPtr)pb);
    while (pb->ioResult == inProgress) {
        Idle();
    }
    return pb->ioResult;
}

static OSErr TCPStreamCreate(StreamPtr *stream)
{
    TCPiopb pb;
    OSErr err;

    memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = gRcvBuffer;
    pb.csParam.create.rcvBuffLen = sizeof(gRcvBuffer);
    pb.csParam.create.notifyProc = NULL;
    pb.csParam.create.userDataPtr = NULL;

    err = TCPControlSync(&pb, TCPCreate);
    if (err == noErr) *stream = pb.tcpStream;
    return err;
}

/* Waits (cooperatively) for an incoming connection on AGENT_PORT. */
static OSErr TCPListen(StreamPtr stream)
{
    TCPiopb pb;

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    pb.csParam.open.ulpTimeoutValue = 0;   /* 0 = no timeout, wait forever */
    pb.csParam.open.ulpTimeoutAction = 1;  /* report, don't auto-abort */
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 0;
    pb.csParam.open.remoteHost = 0;
    pb.csParam.open.remotePort = 0;
    pb.csParam.open.localHost = 0;
    pb.csParam.open.localPort = AGENT_PORT;

    return TCPControlWait(&pb, TCPPassiveOpen);
}

static OSErr TCPSendBytes(StreamPtr stream, const void *data, unsigned short len)
{
    TCPiopb pb;
    wdsEntry wds[2];

    if (len == 0) return noErr;

    wds[0].length = len;
    wds[0].ptr = (Ptr)data;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    pb.csParam.send.ulpTimeoutValue = 30;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag = true;
    pb.csParam.send.urgentFlag = false;
    pb.csParam.send.wdsPtr = (Ptr)wds;

    return TCPControlSync(&pb, TCPSend);
}

/* Waits (cooperatively) for at least one byte; returns however many
 * arrived in *got (may be less than bufLen). */
static OSErr TCPRecvSome(StreamPtr stream, void *buf, unsigned short bufLen,
                          unsigned short *got)
{
    TCPiopb pb;
    OSErr err;

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    pb.csParam.receive.commandTimeoutValue = 0; /* wait forever */
    pb.csParam.receive.rcvBuff = buf;
    pb.csParam.receive.rcvBuffLen = bufLen;

    err = TCPControlWait(&pb, TCPRcv);
    *got = pb.csParam.receive.rcvBuffLen;
    return err;
}

static void TCPStreamCloseAndRelease(StreamPtr stream)
{
    TCPiopb pb;

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    pb.csParam.close.ulpTimeoutValue = 10;
    pb.csParam.close.ulpTimeoutAction = 1;
    pb.csParam.close.validityFlags = timeoutValue | timeoutAction;
    TCPControlSync(&pb, TCPClose);

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    TCPControlSync(&pb, TCPRelease);
}

static void TCPStreamAbortAndRelease(StreamPtr stream)
{
    TCPiopb pb;

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    TCPControlSync(&pb, TCPAbort);

    memset(&pb, 0, sizeof(pb));
    pb.tcpStream = stream;
    TCPControlSync(&pb, TCPRelease);
}

/* ------------------------------------------------------------------ */
/* Line-oriented I/O over the current session's stream                */
/* ------------------------------------------------------------------ */

static int RecvByte(char *out)
{
    unsigned short got;
    OSErr err = TCPRecvSome(gStream, out, 1, &got);
    if (err != noErr || got == 0) return -1;
    return 0;
}

/* Reads up to outlen-1 bytes terminated by \n (CR is skipped, matching
 * the other agents' line convention), NUL-terminates into out. */
static int RecvLine(char *out, int outlen)
{
    int i = 0;
    char c;

    while (i < outlen - 1) {
        if (RecvByte(&c) < 0) return -1;
        if (c == '\n') break;
        if (c != '\r') out[i++] = c;
    }
    out[i] = '\0';
    return i;
}

static int SendCStr(const char *s)
{
    unsigned short len = (unsigned short)strlen(s);
    return (TCPSendBytes(gStream, s, len) == noErr) ? 0 : -1;
}

static int SendAll(const char *buf, long len)
{
    while (len > 0) {
        unsigned short chunk = (len > 32000) ? 32000 : (unsigned short)len;
        if (TCPSendBytes(gStream, buf, chunk) != noErr) return -1;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

static int RecvExact(char *buf, long n)
{
    long total = 0;
    while (total < n) {
        unsigned short want = (n - total > READ_CHUNK) ? READ_CHUNK
                                                         : (unsigned short)(n - total);
        unsigned short got = 0;
        if (TCPRecvSome(gStream, buf + total, want, &got) != noErr) return -1;
        if (got == 0) return -1;
        total += got;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Byte-safe little-endian writes (avoid unaligned word/long access,   */
/* which faults with an Address Error on 68k)                          */
/* ------------------------------------------------------------------ */

static void PutLE32(unsigned char *p, long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void PutLE16(unsigned char *p, short v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                   */
/* ------------------------------------------------------------------ */

static void HandlePing(void)
{
    SendCStr("PONG\n");
}

/* Lists every process the Process Manager currently knows about --
 * unlike Finder's icon rendering (which, for these icon-less apps,
 * looks the same whether a process is running or not, and was a
 * source of real confusion diagnosing QUITAGENT), this queries actual
 * OS-level process state directly. Matches the other agents' PSLIST
 * naming, though the fields differ (no PID on classic Mac -- uses the
 * low longword of the ProcessSerialNumber as a stand-in identifier). */
static void HandlePslist(void)
{
    static char buf[2048];
    int len = 0;
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    unsigned char nameBuf[64];
    char hdr[32];

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;

    while (GetNextProcess(&psn) == noErr) {
        info.processInfoLength = sizeof(ProcessInfoRec);
        info.processName = nameBuf;
        info.processAppSpec = NULL;

        if (GetProcessInformation(&psn, &info) == noErr) {
            int nameLen = nameBuf[0];
            if (len + nameLen + 32 < (int)sizeof(buf)) {
                len += sprintf(buf + len, "%lu\t", (unsigned long)psn.lowLongOfPSN);
                memcpy(buf + len, nameBuf + 1, nameLen);
                len += nameLen;
                buf[len++] = '\r';
                buf[len++] = '\n';
            }
        }
    }

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

static void HandleSysinfo(void)
{
    static char buf[512];
    int len = 0;
    char hdr[32];
    long sysVersion = 0;
    long totalMem = 0;
    unsigned long freeMem;
    OSErr err;

    len += sprintf(buf + len, "os_family=mac68k\r\n");

    err = Gestalt(gestaltSystemVersion, &sysVersion);
    if (err == noErr) {
        /* BCD-ish: 0x0753 style for 7.5.3, 0x0705 for 7.0.5 etc. */
        len += sprintf(buf + len, "os_version=%lx\r\n", sysVersion);
    } else {
        len += sprintf(buf + len, "os_version=unknown\r\n");
    }

    err = Gestalt(gestaltMachineType, &totalMem);
    if (err == noErr) {
        len += sprintf(buf + len, "machine_gestalt=%ld\r\n", totalMem);
    }

    /* FreeMem() alone answers "how big is the heap zone right now,"
     * not "how much memory can this app actually still use" -- Retro68
     * apps start with a tiny heap zone (confirmed via MaxMem: an app
     * with the full 1MB SIZE-resource partition granted showed
     * FreeMem() = ~2KB) that grows on demand as allocations need it,
     * up to the partition's real ceiling. MaxMem()'s growp output is
     * exactly that remaining headroom, so freeMem + grow is the
     * number that actually answers "how much room is left." Force the
     * zone back to our own application heap first -- some Toolbox/
     * driver calls switch the current zone without restoring it. */
    SetZone(ApplicationZone());
    {
        Size grow = 0;
        (void)MaxMem(&grow);
        freeMem = FreeMem() + (unsigned long)grow;
    }
    len += sprintf(buf + len, "free_mem_kb=%lu\r\n", freeMem / 1024);

    len += sprintf(buf + len, "agent=llm_agent-mac68k\r\n");

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* GET <path> -- path is taken as a plain (already-mounted-volume-
 * relative or full HFS colon-separated) path usable by fopen(). */
static void HandleGet(char *args)
{
    FILE *f;
    long size;
    char hdr[32];

    while (*args == ' ') args++;

    f = fopen(args, "rb");
    if (!f) {
        SendCStr("ERR:file not found\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    sprintf(hdr, "SIZE:%ld\n", size);
    SendCStr(hdr);

    {
        long remaining = size;
        while (remaining > 0) {
            size_t want = (remaining > (long)sizeof(gIOBuf)) ? sizeof(gIOBuf)
                                                               : (size_t)remaining;
            size_t n = fread(gIOBuf, 1, want, f);
            if (n == 0) break;
            SendAll(gIOBuf, (long)n);
            remaining -= (long)n;
        }
    }
    fclose(f);
}

/* PUT <path> <size> */
static void HandlePut(char *args)
{
    char path[256];
    long size;
    FILE *f;

    while (*args == ' ') args++;
    {
        char *p = path;
        while (*args && *args != ' ' && (p - path) < (long)sizeof(path) - 1) {
            *p++ = *args++;
        }
        *p = '\0';
    }
    while (*args == ' ') args++;
    size = atol(args);

    f = fopen(path, "wb");
    if (!f) {
        SendCStr("ERR:cannot create file\n");
        /* Drain the incoming bytes so the protocol stays in sync. */
        {
            long remaining = size;
            while (remaining > 0) {
                long want = (remaining > (long)sizeof(gIOBuf)) ? sizeof(gIOBuf) : remaining;
                if (RecvExact(gIOBuf, want) < 0) break;
                remaining -= want;
            }
        }
        return;
    }

    {
        long remaining = size;
        int ok = 1;
        while (remaining > 0) {
            long want = (remaining > (long)sizeof(gIOBuf)) ? sizeof(gIOBuf) : remaining;
            if (RecvExact(gIOBuf, want) < 0) { ok = 0; break; }
            fwrite(gIOBuf, 1, (size_t)want, f);
            remaining -= want;
        }
        fclose(f);
        SendCStr(ok ? "OK\n" : "ERR:transfer failed\n");
    }
}

/* UPDATE <size> -- receives a MacBinary-encoded llm_agent build (same
 * encoding `hcopy -m` produces), stages it as STAGED_AGENT.bin,
 * launches llm_updater to decode/replace/relaunch, then exits. This is
 * the one-shot, no-user-interaction version of what was previously a
 * manual PUT-to-STAGED_AGENT.bin + QUITAGENT + "please launch
 * llm_updater" sequence -- now that the whole chain (MacBinary decode,
 * the FSpDelete teardown race, LaunchApplication's parameter block,
 * and QUITAGENT's own stream leak) has been individually verified
 * working, wiring it into one command is just removing the manual
 * steps in between, not new risk surface. */
static void HandleUpdate(char *args)
{
    long size;
    FILE *f;
    unsigned char pname[256];
    int nlen;
    FSSpec spec;
    OSErr err;

    while (*args == ' ') args++;
    size = atol(args);

    f = fopen("STAGED_AGENT.bin", "wb");
    if (!f) {
        SendCStr("ERR:cannot create staging file\n");
        {
            long remaining = size;
            while (remaining > 0) {
                long want = (remaining > (long)sizeof(gIOBuf)) ? sizeof(gIOBuf) : remaining;
                if (RecvExact(gIOBuf, want) < 0) break;
                remaining -= want;
            }
        }
        return;
    }

    {
        long remaining = size;
        int ok = 1;
        while (remaining > 0) {
            long want = (remaining > (long)sizeof(gIOBuf)) ? sizeof(gIOBuf) : remaining;
            if (RecvExact(gIOBuf, want) < 0) { ok = 0; break; }
            fwrite(gIOBuf, 1, (size_t)want, f);
            remaining -= want;
        }
        fclose(f);
        if (!ok) {
            SendCStr("ERR:transfer failed\n");
            return;
        }
    }

    /* Confirm llm_updater actually exists before committing to the
     * handoff -- better to report ERR now (agent keeps running) than
     * to quit and leave nothing to relaunch us. */
    nlen = (int)strlen("llm_updater");
    pname[0] = (unsigned char)nlen;
    memcpy(pname + 1, "llm_updater", nlen);
    err = FSMakeFSSpec(0, 0, pname, &spec);
    if (err != noErr) {
        SendCStr("ERR:llm_updater not found next to this agent\n");
        return;
    }

    SendCStr("OK\n");

    {
        LaunchParamBlockRec pb;
        memset(&pb, 0, sizeof(pb));
        pb.launchBlockID = extendedBlock;
        pb.launchEPBLength = extendedBlockLen;
        pb.launchAppSpec = &spec;
        pb.launchControlFlags = launchContinue;
        LaunchApplication(&pb);
    }

    /* Same teardown QUITAGENT uses -- release gStream before exiting
     * so llm_updater's relaunch of us doesn't inherit an orphaned
     * MacTCP stream still bound to AGENT_PORT. */
    TCPStreamAbortAndRelease(gStream);
    ExitToShell();
}

/* SCREENSHOT -- captures the main screen via CopyBits into an offscreen
 * GWorld-free 1-bit-per-pixel-free... kept simple: we walk the screen
 * PixMap directly and emit an uncompressed 24-bit BMP, matching what
 * the other agents' SCREENSHOT already returns to the bridge tooling. */
/* Reads one pixel at (x, rowBase) as RGB, given the source PixMap's bit
 * depth. Plain qd.screenBits is always a 1-bit monochrome *view* --
 * classic QuickDraw's original model -- and does NOT reflect a color
 * screen's actual pixel data; that lives in the current GDevice's
 * PixMap (GetMainDevice()), which is what this reads instead. Verified
 * live: reading qd.screenBits directly on this (color) display produced
 * pure noise, since 8-bit-per-pixel color byte values were being
 * misread as 8 packed 1-bit pixels. */
static void GetPixelRGB(CTabHandle table, short pixelSize, const unsigned char *rowBase,
                         long x, unsigned char *outR, unsigned char *outG, unsigned char *outB)
{
    if (pixelSize <= 8) {
        /* Indexed color: pixelSize-bit values packed MSB-first within
         * each byte, looked up in the device's color table. */
        int pixelsPerByte = 8 / pixelSize;
        int shiftAmount = (pixelsPerByte - 1 - (int)(x % pixelsPerByte)) * pixelSize;
        int mask = (1 << pixelSize) - 1;
        unsigned char byte = rowBase[x / pixelsPerByte];
        int value = (byte >> shiftAmount) & mask;

        if (table != NULL && value <= (**table).ctSize) {
            RGBColor rgb = (**table).ctTable[value].rgb;
            *outR = (unsigned char)(rgb.red >> 8);
            *outG = (unsigned char)(rgb.green >> 8);
            *outB = (unsigned char)(rgb.blue >> 8);
        } else {
            *outR = *outG = *outB = 0;
        }
    } else if (pixelSize == 16) {
        /* Thousands of colors: 1 unused + 5-5-5 RGB, big-endian (68k
         * native, no byte-swap needed). */
        const unsigned short *p = (const unsigned short *)(rowBase + x * 2);
        unsigned short v = *p;
        int r5 = (v >> 10) & 0x1F;
        int g5 = (v >> 5) & 0x1F;
        int b5 = v & 0x1F;
        *outR = (unsigned char)((r5 * 255) / 31);
        *outG = (unsigned char)((g5 * 255) / 31);
        *outB = (unsigned char)((b5 * 255) / 31);
    } else {
        /* Millions of colors: 1 unused + 8-8-8 RGB. */
        const unsigned char *p = rowBase + x * 4;
        *outR = p[1];
        *outG = p[2];
        *outB = p[3];
    }
}

/* Reading GetMainDevice()->gdPMap->baseAddr directly (the textbook
 * approach, and what an earlier version of this function did) turned
 * out to return garbage under qemu-system-m68k -M q800: recognizable
 * 68k code (UNLK/RTS/LINK opcodes), not pixels, even though the
 * address matches what a real Mac's PixMap uses and qd.screenBits
 * agrees with it. A live test writing a marker there to find the real
 * offset caused a fatal double MMU fault, ruling out "just guess a
 * different offset" as safe. But the ROM's own Shift-Command-3 screen
 * capture works correctly in this same environment (confirmed: opened
 * the resulting PICT and it showed the real desktop) -- proving real
 * pixel data *is* reachable here, just not via a raw baseAddr read.
 * The difference is almost certainly that Shift-Command-3 goes through
 * CopyBits rather than touching VRAM directly, so this does the same:
 * CopyBits from the screen into a normal offscreen GWorld (ordinary
 * allocated memory, nothing mysterious about its address), then reads
 * pixels from that safe copy instead. */
static void HandleScreenshot(void)
{
    GDHandle mainDevice;
    PixMapHandle screenPM;
    GWorldPtr offscreen;
    PixMapHandle pm;
    CTabHandle table;
    CGrafPtr savePort;
    GDHandle saveDevice;
    QDErr gwErr;
    Rect bounds;
    short width, height;
    short pixelSize;
    long rowBytesAbs;
    Ptr baseAddr;
    long imageSize;
    long fileSize;
    char hdr[32];
    unsigned char bmpHeader[54];
    short x, y;

    mainDevice = GetMainDevice();
    screenPM = (**mainDevice).gdPMap;
    bounds = (**screenPM).bounds;
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;

    GetGWorld(&savePort, &saveDevice);

    offscreen = NULL;
    gwErr = NewGWorld(&offscreen, 0, &bounds, NULL, mainDevice, 0);
    if (gwErr != noErr || offscreen == NULL) {
        SendCStr("ERR:NewGWorld failed\n");
        return;
    }

    pm = GetGWorldPixMap(offscreen);
    LockPixels(pm);

    SetGWorld(offscreen, mainDevice);
    {
        BitMap *srcBits = (BitMap *)*screenPM;
        BitMap *dstBits = (BitMap *)*pm;
        CopyBits(srcBits, dstBits, &bounds, &bounds, srcCopy, NULL);
    }
    SetGWorld(savePort, saveDevice);

    rowBytesAbs = (**pm).rowBytes & 0x3fff;
    baseAddr = (**pm).baseAddr;
    pixelSize = (**pm).pixelSize;
    table = (**pm).pmTable;

    imageSize = (long)width * height * 3;
    /* BMP rows are padded to 4 bytes; account for that in fileSize. */
    {
        long rowPad = (4 - ((long)width * 3) % 4) % 4;
        fileSize = 54 + (long)(width * 3 + rowPad) * height;

        /* 68k requires word/long accesses to be even-aligned; a raw
         * "*(long*)(buf+N) = v" cast risks an Address Error if the
         * offset lands odd relative to the array's actual alignment.
         * Write every multi-byte field out byte-by-byte instead. */
        memset(bmpHeader, 0, sizeof(bmpHeader));
        bmpHeader[0] = 'B'; bmpHeader[1] = 'M';
        PutLE32(bmpHeader + 2, fileSize);
        PutLE32(bmpHeader + 10, 54);
        PutLE32(bmpHeader + 14, 40);
        PutLE32(bmpHeader + 18, (long)width);
        PutLE32(bmpHeader + 22, (long)height);
        PutLE16(bmpHeader + 26, 1);
        PutLE16(bmpHeader + 28, 24);
        PutLE32(bmpHeader + 34, imageSize);

        sprintf(hdr, "SIZE:%ld\n", fileSize);
        SendCStr(hdr);
        SendAll((char *)bmpHeader, 54);

        for (y = height - 1; y >= 0; y--) {
            unsigned char rowBuf[2048 * 3];
            long col = 0;
            unsigned char *rowBase = (unsigned char *)baseAddr + (long)y * rowBytesAbs;

            for (x = 0; x < width && x < 2048; x++) {
                unsigned char r, g, b;
                GetPixelRGB(table, pixelSize, rowBase, x, &r, &g, &b);
                rowBuf[col++] = b;
                rowBuf[col++] = g;
                rowBuf[col++] = r;
            }
            SendAll((char *)rowBuf, col);
            if (rowPad) {
                static const char pad[4] = {0, 0, 0, 0};
                SendAll(pad, rowPad);
            }
        }
    }

    UnlockPixels(pm);
    DisposeGWorld(offscreen);
}

/* ------------------------------------------------------------------ */
/* Session handling                                                   */
/* ------------------------------------------------------------------ */

static int LoadToken(void)
{
    FILE *f = fopen("LLMAGENT.INI", "r");
    char line[TOKEN_MAX_LEN];

    gToken[0] = '\0';
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "token=", 6) == 0) {
            char *p = line + 6;
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            nl = strchr(p, '\r');
            if (nl) *nl = '\0';
            strncpy(gToken, p, sizeof(gToken) - 1);
            gToken[sizeof(gToken) - 1] = '\0';
            break;
        }
    }
    fclose(f);
    return gToken[0] != '\0';
}

static void HandleClient(void)
{
    if (RecvLine(gLine, sizeof(gLine)) < 0) return;

    if (gToken[0] == '\0' || strcmp(gLine, gToken) != 0) {
        SendCStr("FAIL\n");
        return;
    }
    SendCStr("OK\n");

    for (;;) {
        if (RecvLine(gLine, sizeof(gLine)) < 0) break;

        if (strcmp(gLine, "PING") == 0) {
            HandlePing();
        } else if (strcmp(gLine, "QUIT") == 0) {
            break;
        } else if (strcmp(gLine, "QUITAGENT") == 0) {
            /* Terminates the whole process, not just this session --
             * see the file header comment. The only way to stop this
             * background-only, UI-less app short of a reboot.
             *
             * Must release gStream first. PSLIST always showed a clean
             * process list after QUITAGENT (Process Manager cleanup is
             * fine), yet the *next* launch reproducibly hung regardless
             * of how long we waited first -- consistent with MacTCP's
             * driver-level control block for this stream (bound to
             * AGENT_PORT) being orphaned rather than released, since
             * main()'s normal per-connection
             * TCPStreamCloseAndRelease() (after HandleClient returns)
             * is never reached when we exit straight from inside the
             * dispatch loop here. A stuck driver-level resource has no
             * timeout, so no amount of waiting before the next launch
             * would ever have fixed it -- it needed to actually be
             * released here. */
            SendCStr("OK\n");
            TCPStreamAbortAndRelease(gStream);
            ExitToShell();
        } else if (strcmp(gLine, "SYSINFO") == 0) {
            HandleSysinfo();
        } else if (strcmp(gLine, "PSLIST") == 0) {
            HandlePslist();
        } else if (strncmp(gLine, "GET ", 4) == 0) {
            HandleGet(gLine + 4);
        } else if (strncmp(gLine, "PUT ", 4) == 0) {
            HandlePut(gLine + 4);
        } else if (strncmp(gLine, "UPDATE ", 7) == 0) {
            HandleUpdate(gLine + 7);
        } else if (strcmp(gLine, "SCREENSHOT") == 0) {
            HandleScreenshot();
        } else {
            SendCStr("ERR:unknown command\n");
        }
    }
}

int main(void)
{
    OSErr err;

    /* No CONSOLE lib is linked (see CMakeLists.txt) -- Retro68's console
     * window crashes with an Illegal Instruction under qemu-system-m68k's
     * q800 machine before any of our code runs, and this agent is a
     * headless TCP service with no use for a console anyway. We still
     * need QuickDraw's globals (qd.screenBits) for SCREENSHOT, which
     * InitConsole() would otherwise have set up for us, so call InitGraf
     * ourselves -- this alone (without InitWindows/InitMenus/NewWindow)
     * is exercised safely by other Retro68 sample apps on this same
     * setup, so it isn't implicated in that crash.
     *
     * NOTE: a menu-bar status icon (InsertMenu/DrawMenuBar +
     * InitFonts/InitWindows/InitMenus/InitCursor) was tried here and
     * caused a fatal double MMU fault that crashed QEMU itself. Classic
     * Mac OS has exactly one system-wide menu bar, owned by whichever
     * process is currently frontmost -- a background-only process (see
     * llm_agent.r's SIZE resource) calling DrawMenuBar() is fighting
     * that model, not using a documented "menu bar extra" API (there
     * isn't one pre-Mac OS 8). Not attempting that again without a much
     * more careful, incremental investigation. */
    InitGraf(&qd.thePort);

    err = OpenMacTCP();
    if (err != noErr) {
        return 1;
    }

    LoadToken();

    for (;;) {
        err = TCPStreamCreate(&gStream);
        if (err != noErr) {
            /* Back off a bit before retrying stream creation. */
            unsigned long until;
            Delay(60, &until);
            continue;
        }

        err = TCPListen(gStream);
        if (err == noErr) {
            HandleClient();
            TCPStreamCloseAndRelease(gStream);
        } else {
            TCPStreamAbortAndRelease(gStream);
        }
    }

    return 0;
}
