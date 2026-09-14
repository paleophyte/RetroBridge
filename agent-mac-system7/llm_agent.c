/* llm_agent for classic Mac OS (System 7.x) over MacTCP.
 *
 * Speaks the same token-authed TCP wire protocol as the other ports in
 * this repo (see ../mcp-server/agent_client.py for the authoritative
 * docs) so it can be driven by the same bridge tooling. Classic Mac OS
 * has no built-in command shell (no COMMAND.COM/CMD.EXE equivalent), so
 * EXEC/EXECDETACH are not implemented -- everything else in the shared
 * protocol (PING, SYSINFO, GET, PUT, SCREENSHOT, QUIT, REBOOT,
 * SHUTDOWN) is, plus commands
 * not part of the shared protocol: QUITAGENT, which terminates the
 * agent process itself; PSLIST, which reports the live Process Manager
 * process list (useful for confirming whether a process has actually
 * exited -- Finder's icon rendering for these icon-less apps looks
 * identical whether they're running or not, which isn't a reliable
 * signal either way); UPDATE, which takes a MacBinary-encoded new build
 * of this agent and self-updates with no user interaction -- stages it,
 * launches the llm_updater companion app (see llm_updater.c) to
 * decode/replace/relaunch, and exits; and MOUSEPOS, which reports the
 * current cursor position and button state.
 *
 * REBOOT and SHUTDOWN go through the Shutdown Manager (trap 0xA895):
 * ShutDwnStart() and ShutDwnPower() respectively. Both reply "OK" first,
 * then release the MacTCP stream and go down. Using the Shutdown Manager
 * rather than anything harder is what keeps the HFS volume clean -- after
 * either one the guest boots straight back to the Finder with no "you have
 * restarted improperly" dialog, which matters because that dialog blocks
 * Startup Items and would stop the agent relaunching on its own. Measured:
 * the agent is answering again ~24s after REBOOT, against ~48s and a
 * required keystroke for a cold start after an unclean stop.
 *
 * CLICK and DBLCLICK are implemented, and take the shared protocol's
 * optional third "button" argument. Coordinates are global screen
 * coordinates, the same space MOUSEPOS reports. Only button 1 exists on this
 * hardware; 2 and 3 are refused rather than quietly treated as a left click.
 *
 * An earlier round of this investigation concluded that synthetic mouse
 * clicks were impossible on classic Mac OS, on the theory that Toolbox
 * click tracking polls live ADB hardware state. That was WRONG.
 * Watchpointing the low-level event queue during a real QuicKeys 3.5.3
 * click showed it simply writes the target point into
 * MTemp/RawMouse/Mouse and then posts ordinary mouseDown/mouseUp events.
 * MBState is never written and journaling is never used. See
 * QUICKEYS_CLICK_INVESTIGATION.md for the evidence; HandleClick() below
 * carries the implementation notes.
 *
 * KEY/TYPE are still not implemented. Keyboard event injection was proven
 * to work in the same investigation, but no command was built on it --
 * see README.md for the working technique and exact evidence if picking
 * this back up.
 *
 * This app runs with no console, no windows, and (see llm_agent.r) no
 * foreground UI at all -- there is no Finder menu or window to close it
 * from, so QUITAGENT is the only way to stop it short of rebooting the
 * machine, and llm_updater is the only way to replace it short of
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

/* MOUSEPOS -- not part of the shared protocol; reports current cursor
 * position and button state. GetMouse() returns local (port-relative)
 * coordinates, but this app never creates a window or moves the port
 * origin from (0,0), so local == global here. */
static void HandleMousePos(void)
{
    Point pt;
    char buf[64];
    char hdr[32];
    int len;

    /* GetMouse() returns local (port-relative) coordinates, converted
     * using whatever the *current* GrafPort's origin happens to be --
     * and this app never creates a window or otherwise establishes a
     * full-screen port after InitGraf(), so that conversion isn't
     * trustworthy (confirmed live: returned x=15397 on a 640px-wide
     * screen). Read MTemp directly instead -- already-global screen
     * coordinates, no port involved. */
    pt = LMGetMTemp();
    len = sprintf(buf, "x=%d\r\ny=%d\r\nbutton=%d\r\n", pt.h, pt.v, Button() ? 1 : 0);

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* ---- Synthetic mouse clicks -------------------------------------------
 *
 * How QuicKeys 3.5.3 actually does it, established by watchpointing the
 * low-level event queue during a real click (full evidence in
 * QUICKEYS_CLICK_INVESTIGATION.md):
 *
 *   1. write the target point into MTemp/RawMouse/Mouse and set CrsrNew,
 *      so the Toolbox believes the mouse is physically there;
 *   2. PPostEvent() a mouseDown, and fill in evtQWhere/evtQModifiers in
 *      the queue element it hands back;
 *   3. the same again for mouseUp, about two ticks later.
 *
 * Step 1 is the part every earlier attempt missed. The Toolbox resolves a
 * click against where it believes the mouse *is*, so a mouseDown whose
 * evtQWhere disagrees with Mouse/RawMouse lands nowhere -- which is why
 * the plain Enqueue() attempt recorded in README.md produced no visible
 * effect and led to the wrong conclusion that this was impossible.
 *
 * PPostEvent is trap 0xA12F. Multiverse.h declares it but also lists it in
 * needs-glue.txt, i.e. there is no inline trap encoding available to call,
 * so it is invoked as a raw .word here -- the same idiom the DEVPROBE
 * diagnostic already uses successfully. Plain PostEvent() is not enough:
 * it offers no way to set evtQWhere, and PPostEvent exists precisely
 * because it returns the queue element so the caller can fill that in.
 */

/* Multiversal has no LM accessors for these three. */
#define LM_RAWMOUSE   (*(Point *)0x082CL)
#define LM_CRSRNEW    (*(volatile unsigned char *)0x08CEL)
#define LM_CRSRCOUPLE (*(volatile unsigned char *)0x08CFL)

/* qType for an event queue element. Multiversal does not define evType,
 * and 4 is what QuicKeys' own elements carry (observed live). */
#define EVQEL_TYPE 4

static void SetMouseTo(Point p)
{
    unsigned char savedCouple;
    long deadline;

    /* Spin while the cursor is mid-update, exactly as QuicKeys' routine at
     * CDRV_0+0x17dc does, so we never write underneath the VBL cursor
     * tracking code. Microseconds in practice. */
    while (LMGetCrsrBusy()) {
        /* busy wait */
    }

    savedCouple = LM_CRSRCOUPLE;
    LM_CRSRCOUPLE = 0;
    LMSetMTemp(p);
    LM_RAWMOUSE = p;
    LMSetMouseLocation(p);
    LM_CRSRNEW = 0xFF;   /* "hardware reported a move" */

    /* Wait for the VBL cursor-tracking code to consume CrsrNew, then put
     * CrsrCouple back. Leaving it at 0 decouples the cursor from the
     * physical mouse *permanently*, so the console mouse stops working and
     * the machine looks hung to anyone sitting in front of it -- even
     * though everything is actually running fine. Learned the hard way.
     * The wait is bounded so a missed VBL cannot wedge the agent. */
    deadline = (long)LMGetTicks() + 30;
    while (LM_CRSRNEW != 0 && (long)LMGetTicks() < deadline) {
        /* busy wait */
    }
    LM_CRSRCOUPLE = savedCouple;
}

static EvQEl *PPostEventTrap(short code, long msg, short *errOut)
{
    /* Explicit register locals rather than a clobber list: A0 and D0 are
     * both inputs and outputs here, and naming them directly avoids the
     * output/clobber conflict that shape would otherwise have. */
    register long  d0 asm("%d0") = msg;
    register void *a0 asm("%a0") = (void *)(long)code;

    asm volatile (
        ".word 0xA12F"
        : "+d"(d0), "+a"(a0)
        :
        : "d1", "a1", "cc", "memory"
    );

    *errOut = (short)d0;
    return (EvQEl *)a0;
}

static int PostMouseEvent(short what, Point where)
{
    short err = 0;
    EvQEl *qel = PPostEventTrap(what, 0, &err);

    /* Only touch the element when the trap actually succeeded. On failure
     * (a full event queue, say) A0 is not a valid pointer, and writing 22
     * bytes through it would corrupt whatever it happens to point at. */
    if (err != noErr || qel == NULL) {
        return 0;
    }

    /* Set the fields explicitly rather than trusting the register
     * convention. The one observed live on this ROM (A0 = event code,
     * D0 = message) is the reverse of what is normally documented, so
     * write what we actually want regardless of which register carried
     * it. evtQWhere should already equal `where` because SetMouseTo()
     * ran first, but QuicKeys writes it too and it costs nothing. */
    qel->qType = EVQEL_TYPE;
    qel->evtQWhat = what;
    qel->evtQMessage = 0;
    qel->evtQWhere = where;
    qel->evtQModifiers = 0x0080;   /* btnState, matching QuicKeys */
    return 1;
}




/* PSKILL <pid> -- ask a process to quit.
 *
 * Classic Mac OS has no kill. Nothing can terminate another process from
 * outside: there is no protected memory and no supervisor able to reclaim a
 * task, so the only thing available is to *ask*, by sending the application a
 * standard 'quit' AppleEvent (kCoreEventClass / kAEQuitApplication). That is
 * cooperative in the fullest sense -- a well-behaved application quits, one
 * with unsaved changes may put up a save dialog and sit there, and one that is
 * wedged ignores it completely.
 *
 * So OK here means "the quit request was delivered", not "the process is
 * gone", and the two are genuinely different. Callers that need certainty
 * should follow up with PSLIST rather than assume.
 *
 * The <pid> is the value PSLIST reports, which is the low long of the
 * process serial number.
 */

/* Spelled out rather than relying on multi-character constants. */
#define AE_TYPE_PSN        0x70736E20L   /* 'psn ' */
#define AE_CLASS_CORE      0x61657674L   /* 'aevt' */
#define AE_ID_QUIT         0x71756974L   /* 'quit' */
#define AE_AUTO_RETURN_ID  (-1)
#define AE_ANY_TRANSACTION 0L
#define AE_NO_REPLY        1
#define AE_NORMAL_PRIORITY 0
#define AE_DEFAULT_TIMEOUT (-1L)

static void HandlePsKill(unsigned long pidLow)
{
    ProcessSerialNumber psn;
    ProcessSerialNumber self;
    AEAddressDesc target;
    AppleEvent theEvent;
    AppleEvent reply;
    char buf[128];
    OSErr err;
    int found = 0;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        if ((unsigned long)psn.lowLongOfPSN == pidLow) {
            found = 1;
            break;
        }
    }
    if (!found) {
        sprintf(buf, "ERR:no such process %lu\n", pidLow);
        SendCStr(buf);
        return;
    }

    /* Refuse to target ourselves. This is a correctness point rather than
     * caution: quitting during command handling means the reply never gets
     * sent and the caller is left hanging. QUITAGENT exists for this and
     * tears the MacTCP stream down properly first. */
    if (GetCurrentProcess(&self) == noErr &&
        self.lowLongOfPSN == psn.lowLongOfPSN &&
        self.highLongOfPSN == psn.highLongOfPSN) {
        SendCStr("ERR:refusing to quit the agent itself, use QUITAGENT\n");
        return;
    }

    err = AECreateDesc((DescType)AE_TYPE_PSN, (const void *)&psn,
                       (Size)sizeof(psn), &target);
    if (err != noErr) {
        sprintf(buf, "ERR:AECreateDesc failed (OSErr %d)\n", (int)err);
        SendCStr(buf);
        return;
    }

    err = AECreateAppleEvent((AEEventClass)AE_CLASS_CORE, (AEEventID)AE_ID_QUIT,
                             &target, AE_AUTO_RETURN_ID, AE_ANY_TRANSACTION,
                             &theEvent);
    if (err != noErr) {
        AEDisposeDesc(&target);
        sprintf(buf, "ERR:AECreateAppleEvent failed (OSErr %d)\n", (int)err);
        SendCStr(buf);
        return;
    }

    /* kAENoReply: do not block waiting for the application to answer. A
     * process showing a save dialog would otherwise hold this command open
     * for the full AppleEvent timeout. */
    err = AESend(&theEvent, &reply, AE_NO_REPLY, AE_NORMAL_PRIORITY,
                 AE_DEFAULT_TIMEOUT, NULL, NULL);

    AEDisposeDesc(&theEvent);
    AEDisposeDesc(&target);

    if (err != noErr) {
        sprintf(buf, "ERR:AESend failed (OSErr %d)\n", (int)err);
        SendCStr(buf);
        return;
    }

    SendCStr("OK\n");
}

/* WINLIST -- NOT SUPPORTED, and the reason is worth keeping.
 *
 * The obvious implementation is to walk the Window Manager's list from the
 * WindowList low-memory global (0x09D6) via nextWindow. That was written and
 * it works -- it just cannot see anything useful, because under MultiFinder
 * WindowList is part of the per-process low-memory state the Process Manager
 * swaps on every context switch. A background application therefore sees only
 * its own windows.
 *
 * Measured rather than assumed. With the visible/title filters removed, the
 * walk returned exactly one entry:
 *
 *     hwnd 0x0FD425B0  (12,34) 621x441  windowKind 8  visible 1  titleLen 0
 *
 * -- a single untitled full-screen document window, which is this agent's own
 * Retro68 console, while four windows from other applications were plainly on
 * screen at the time. nextWindow was NULL, so that was the whole list.
 *
 * Returning an empty list would be worse than refusing: the client documents
 * WINLIST as "visible top-level windows", and an empty result reads as "there
 * are no windows" when in fact there are several that simply cannot be reached
 * from here.
 *
 * The route that could actually work is AppleEvents -- asking each running
 * application for its windows, which is how a scripting client would do it.
 * That needs real AppleEvent plumbing and would only cover applications that
 * are scriptable, so it is a separate piece of work rather than a fix.
 */
static void HandleWinList(void)
{
    SendCStr("ERR:WINLIST unavailable, WindowList is per-process under MultiFinder\n");
}

/* CLIPSET <text> -- set the clipboard to plain text.
 *
 * Scrap Manager: ZeroScrap() to empty the desk scrap, then PutScrap() with
 * type 'TEXT'. Both are plain inline traps (0xA9FC, 0xA9FE) and both return an
 * OSErr as a long, which is checked here -- a silently failing clipboard is
 * exactly the sort of thing that wastes an afternoon later.
 *
 * The scrap is global rather than per-process, so it does not matter that this
 * agent is a background application with no window.
 *
 * Text is stored exactly as received. No line-ending translation happens
 * because none is needed: agent_client.py rejects newlines in clipboard_set(),
 * and were that ever relaxed, classic Mac text uses CR rather than LF and the
 * conversion would have to be deliberate.
 *
 * An empty payload is treated as "clear the clipboard" rather than an error --
 * ZeroScrap() alone does exactly that.
 */

/* 'TEXT' written out, to avoid relying on multi-character constants. */
#define SCRAP_TYPE_TEXT 0x54455854L

static void HandleClipSet(char *text)
{
    char buf[96];
    long err;
    long len;

    len = (long)strlen(text);

    err = ZeroScrap();
    if (err != noErr) {
        sprintf(buf, "ERR:ZeroScrap failed (OSErr %ld)\n", err);
        SendCStr(buf);
        return;
    }

    if (len == 0) {
        SendCStr("OK\n");      /* cleared */
        return;
    }

    err = PutScrap(len, (ResType)SCRAP_TYPE_TEXT, (Ptr)text);
    if (err != noErr) {
        sprintf(buf, "ERR:PutScrap failed (OSErr %ld)\n", err);
        SendCStr(buf);
        return;
    }

    /* Flush the in-memory scrap out to the scrap file. The Process Manager
     * normally does this when applications switch; doing it here means the
     * scrap is durable for whoever reads it next rather than depending on a
     * switch happening. Failure is not fatal -- the in-memory scrap is still
     * set -- so it is not reported.  */
    (void)UnloadScrap();

    /* Mirror it into the TextEdit scrap as well.
     *
     * Setting the desk scrap alone is not enough to make Paste work in most
     * places. TextEdit keeps its own private scrap in low memory
     * (TEScrpHandle/TEScrpLength), and that is what dialog text fields paste
     * from -- an application is supposed to call TEFromScrap() itself, and
     * plenty do not. Observed directly: after a desk-scrap-only CLIPSET,
     * CLIPGET returned the text correctly but Cmd-V into Find File's search
     * field produced nothing.
     *
     * Those low-memory globals are system-wide, so doing it here fixes paste
     * for applications that never bother.
     *
     * TEFromScrap() itself cannot be called: Multiversal declares it but lists
     * it in needs-glue.txt, so there is no trap encoding and no glue to link
     * against -- it fails at link time with an undefined reference. What it
     * does is small enough to do directly instead: read the desk scrap into
     * the existing TE scrap handle and set the length. Both globals have
     * accessors (TEScrpHandle at 0x0AB4, TEScrpLength at 0x0AB0).
     *
     * Skipped rather than forced if TextEdit has not set up a scrap handle
     * yet; allocating one here would mean taking over ownership of a system
     * global, which is not worth the risk for a convenience. Non-fatal either
     * way, since the desk scrap is already set. */
    {
        Handle teScrap = LMGetTEScrpHandle();

        if (teScrap != NULL) {
            long teOffset = 0;
            long teLen = GetScrap(teScrap, (ResType)SCRAP_TYPE_TEXT, &teOffset);

            /* TEScrpLength is a 16-bit field, so anything larger simply
             * cannot be represented there; the desk scrap still holds it. */
            if (teLen >= 0 && teLen <= 32767L) {
                LMSetTEScrpLength((short)teLen);
            }
        }
    }

    SendCStr("OK\n");
}

/* CLIPGET -- read the clipboard back as text. Not part of the shared protocol;
 * a Mac-side extension, added because without it CLIPSET cannot be verified at
 * all. Whether a given application's Paste picks the text up is a separate
 * question from whether the scrap holds it -- dialog text fields go through
 * TextEdit's private scrap and only see the desk scrap if the application
 * calls TEFromScrap() -- so this reports what the scrap actually contains. */
static void HandleClipGet(void)
{
    Handle h;
    long len;
    long offset = 0;
    char hdr[32];

    h = NewHandle(0);
    if (h == NULL) {
        SendCStr("ERR:out of memory\n");
        return;
    }

    len = GetScrap(h, (ResType)SCRAP_TYPE_TEXT, &offset);
    if (len < 0) {
        DisposeHandle(h);
        sprintf(hdr, "ERR:GetScrap failed (OSErr %ld)\n", len);
        SendCStr(hdr);
        return;
    }

    sprintf(hdr, "SIZE:%ld\n", len);
    SendCStr(hdr);
    if (len > 0) {
        HLock(h);
        SendAll(*h, (int)len);
        HUnlock(h);
    }
    DisposeHandle(h);
}

/* CLICK x y [button] / DBLCLICK x y [button]
 *
 * Coordinates are global screen coordinates, the same space MOUSEPOS reports.
 *
 * The shared protocol spells this "CLICK <x> <y> <button>" and agent_client.py
 * always sends three arguments, defaulting button to 1, so the button is
 * accepted here for compatibility. It is optional: omitted means 1.
 *
 * This hardware has one mouse button. Rather than silently left-click when
 * asked for button 2 or 3 -- which would let a caller believe it had
 * right-clicked when nothing of the sort happened -- those are refused. System
 * 7.5.3 has no contextual menus for a second button to open anyway.
 *
 * Replies "OK" / "ERR:..." as the protocol requires. It used to reply with a
 * SIZE: body, which agent_client.py's _simple_command rejects outright since
 * it compares the first line against "OK". */
static void HandleClick(short x, short y, int dbl, short button)
{
    Point p;
    long finalTicks;
    int pairs;
    int i;

    if (button != 1) {
        SendCStr("ERR:single-button hardware, only button 1 is supported\n");
        return;
    }

    p.h = x;
    p.v = y;

    SetMouseTo(p);

    pairs = dbl ? 2 : 1;
    for (i = 0; i < pairs; i++) {
        if (!PostMouseEvent(mouseDown, p)) {
            SendCStr("ERR:PPostEvent gave no queue element (mouseDown)\n");
            return;
        }
        Delay(2, &finalTicks);          /* QuicKeys' own down->up gap */
        if (!PostMouseEvent(mouseUp, p)) {
            SendCStr("ERR:PPostEvent gave no queue element (mouseUp)\n");
            return;
        }
        if (dbl && i == 0) {
            Delay(4, &finalTicks);      /* comfortably inside GetDblTime() */
        }
    }

    SendCStr("OK\n");
}

/* ---- Synthetic keyboard: KEY and TYPE ---------------------------------
 *
 * Same route as CLICK -- PPostEvent (trap 0xA12F) hands back the queue
 * element and we fill it in. The message layout was taken from a real
 * keypress captured off the event queue rather than from a manual:
 *
 *     evtQWhat    0x0003            keyDown
 *     evtQMessage 0x00023260        (adbAddr<<16) | (keyCode<<8) | charCode
 *
 * 0x32 is the ADB code for the grave key and 0x60 is '`', which is exactly
 * what had been pressed, so both the layout and the key-code table below are
 * anchored to observed reality. adbAddr is 2, the usual keyboard address.
 *
 * No cursor positioning is involved, so unlike CLICK this touches no
 * low-memory globals and has no CrsrCouple hazard.
 */

#define ADB_KEYBOARD_ADDR 0x02L

/* US layout, ADB virtual key codes. The two strings are index-aligned: the
 * same key produces kUnshiftedChars[i] alone and kShiftedChars[i] with shift,
 * and its key code is kKeyCodes[i]. */
static const char kUnshiftedChars[] =
    "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./` ";
static const char kShiftedChars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ)!@#$%^&*(_+{}|:\"<>?~ ";
static const unsigned char kKeyCodes[] = {
    0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25,
    0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07,
    0x10, 0x06,                                             /* a-z */
    0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19,   /* 0-9 */
    0x1B, 0x18, 0x21, 0x1E, 0x2A, 0x29, 0x27, 0x2B, 0x2F, 0x2C, 0x32,
    0x31                                                    /* space */
};

/* Named keys, for KEY. charCode then ADB key code. */
typedef struct {
    const char    *name;
    unsigned char  charCode;
    unsigned char  keyCode;
} NamedKey;

static const NamedKey kNamedKeys[] = {
    { "enter",     0x0D, 0x24 }, { "return",   0x0D, 0x24 },
    { "tab",       0x09, 0x30 }, { "space",    0x20, 0x31 },
    { "esc",       0x1B, 0x35 }, { "escape",   0x1B, 0x35 },
    { "bksp",      0x08, 0x33 }, { "backspace",0x08, 0x33 },
    { "del",       0x08, 0x33 },              /* Mac Delete = backspace */
    { "fwddel",    0x7F, 0x75 }, { "delete",   0x7F, 0x75 },
    { "left",      0x1C, 0x7B }, { "right",    0x1D, 0x7C },
    { "up",        0x1E, 0x7E }, { "down",     0x1F, 0x7D },
    { "home",      0x01, 0x73 }, { "end",      0x04, 0x77 },
    { "pgup",      0x0B, 0x74 }, { "pgdn",     0x0C, 0x79 },
    { "f1",  0x10, 0x7A }, { "f2",  0x10, 0x78 }, { "f3",  0x10, 0x63 },
    { "f4",  0x10, 0x76 }, { "f5",  0x10, 0x60 }, { "f6",  0x10, 0x61 },
    { "f7",  0x10, 0x62 }, { "f8",  0x10, 0x64 }, { "f9",  0x10, 0x65 },
    { "f10", 0x10, 0x6D }, { "f11", 0x10, 0x67 }, { "f12", 0x10, 0x6F },
    { NULL, 0, 0 }
};

/* Event modifier bits. btnState is set because the button is up. */
#define MOD_BTNSTATE 0x0080
#define MOD_CMD      0x0100
#define MOD_SHIFT    0x0200
#define MOD_OPTION   0x0800
#define MOD_CONTROL  0x1000

static short gLastKeyErr = 0;

static int PostKeyEvent(short what, unsigned char charCode,
                        unsigned char keyCode, short modifiers)
{
    short err = 0;
    EvQEl *qel = PPostEventTrap(what, 0, &err);

    gLastKeyErr = err;
    if (err != noErr || qel == NULL) {
        return 0;
    }
    qel->qType = EVQEL_TYPE;
    qel->evtQWhat = what;
    qel->evtQMessage = (ADB_KEYBOARD_ADDR << 16)
                     | ((long)keyCode << 8)
                     | (long)charCode;
    qel->evtQWhere = LMGetMouseLocation();
    qel->evtQModifiers = modifiers;
    return 1;
}

/* One keypress.
 *
 * keyDown is the one that has to land. keyUp is posted best-effort and its
 * failure is deliberately ignored: classic Mac OS leaves keyUpMask out of
 * SysEvtMask by default, so PostEvent refuses keyUp with evtNotEnb (-1) and
 * real keypresses do not enqueue a keyUp either. Treating that as fatal is
 * what made the first cut of this fail with every character rejected. */
static int TapKey(unsigned char charCode, unsigned char keyCode, short modifiers)
{
    long finalTicks;

    if (!PostKeyEvent(3, charCode, keyCode, (short)(modifiers | MOD_BTNSTATE))) {
        return 0;
    }
    Delay(1, &finalTicks);
    (void)PostKeyEvent(4, charCode, keyCode, (short)(modifiers | MOD_BTNSTATE));
    return 1;
}

/* Find a printable character's key code, and whether it needs shift. */
static int LookupChar(char ch, unsigned char *keyCode, int *needShift)
{
    const char *q;

    q = strchr(kUnshiftedChars, ch);
    if (q != NULL && ch != '\0') {
        *keyCode = kKeyCodes[q - kUnshiftedChars];
        *needShift = 0;
        return 1;
    }
    q = strchr(kShiftedChars, ch);
    if (q != NULL && ch != '\0') {
        *keyCode = kKeyCodes[q - kShiftedChars];
        *needShift = 1;
        return 1;
    }
    return 0;
}

/* TYPE <text> -- literal text, no newlines (the client rejects those and
 * tells callers to use KEY enter instead).
 *
 * Every character is posted as a keyDown/keyUp pair. The event queue is a
 * fixed pool and only drains when the receiving application runs, which it
 * cannot do until this command returns, so a long enough string will exhaust
 * it. That shows up as a short count rather than silent truncation. */
static void HandleType(char *text)
{
    char buf[80];
    int sent = 0;
    int i;

    if (*text == '\0') {
        SendCStr("ERR:TYPE needs text\n");
        return;
    }

    for (i = 0; text[i] != '\0'; i++) {
        unsigned char keyCode = 0;
        int needShift = 0;

        if (!LookupChar(text[i], &keyCode, &needShift)) {
            sprintf(buf, "ERR:no key for character 0x%02x at offset %d\n",
                    (unsigned char)text[i], i);
            SendCStr(buf);
            return;
        }
        if (!TapKey((unsigned char)text[i], keyCode,
                    (short)(needShift ? MOD_SHIFT : 0))) {
            sprintf(buf, "ERR:keyDown rejected (OSErr %d) after %d of %d characters\n",
                    (int)gLastKeyErr, sent, (int)strlen(text));
            SendCStr(buf);
            return;
        }
        sent++;
    }

    SendCStr("OK\n");
}

/* KEY <keyspec> [<keyspec> ...] -- keyspecs are space separated, and each may
 * carry '-'-joined modifiers, e.g. "cmd-q", "shift-tab", "enter". Modifier
 * names follow the other ports (ctrl, alt, shift) with Mac spellings added:
 * cmd/command, and opt/option as an alias for alt. */
static void HandleKey(char *args)
{
    char spec[64];
    char buf[96];
    int nkeys = 0;

    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        SendCStr("ERR:KEY needs at least one keyspec\n");
        return;
    }

    while (*args != '\0') {
        char *tok;
        char *save;
        short modifiers = 0;
        unsigned char charCode = 0;
        unsigned char keyCode = 0;
        int haveBase = 0;
        int n = 0;

        while (*args == ' ') {
            args++;
        }
        if (*args == '\0') {
            break;
        }
        while (args[n] != '\0' && args[n] != ' ' && n < (int)sizeof(spec) - 1) {
            spec[n] = args[n];
            n++;
        }
        spec[n] = '\0';
        args += n;

        /* Split the keyspec on '-'. Everything but the last piece is a
         * modifier. A literal '-' key still works because it only ever
         * appears as the final piece. */
        for (tok = spec, save = spec; ; save++) {
            if (*save != '-' && *save != '\0') {
                continue;
            }
            {
                char had = *save;
                const NamedKey *nk;
                int isMod = 0;

                *save = '\0';
                if (had == '-' && *tok != '\0') {
                    if (strcmp(tok, "cmd") == 0 || strcmp(tok, "command") == 0) {
                        modifiers |= MOD_CMD; isMod = 1;
                    } else if (strcmp(tok, "shift") == 0) {
                        modifiers |= MOD_SHIFT; isMod = 1;
                    } else if (strcmp(tok, "alt") == 0 ||
                               strcmp(tok, "opt") == 0 ||
                               strcmp(tok, "option") == 0) {
                        modifiers |= MOD_OPTION; isMod = 1;
                    } else if (strcmp(tok, "ctrl") == 0 ||
                               strcmp(tok, "control") == 0) {
                        modifiers |= MOD_CONTROL; isMod = 1;
                    }
                }
                if (!isMod && *tok != '\0') {
                    /* Base key: a name, or a single character. */
                    for (nk = kNamedKeys; nk->name != NULL; nk++) {
                        if (strcmp(tok, nk->name) == 0) {
                            charCode = nk->charCode;
                            keyCode = nk->keyCode;
                            haveBase = 1;
                            break;
                        }
                    }
                    if (!haveBase && tok[1] == '\0') {
                        int needShift = 0;
                        if (LookupChar(tok[0], &keyCode, &needShift)) {
                            charCode = (unsigned char)tok[0];
                            if (needShift) {
                                modifiers |= MOD_SHIFT;
                            }
                            haveBase = 1;
                        }
                    }
                }
                if (had == '\0') {
                    break;
                }
                tok = save + 1;
            }
        }

        if (!haveBase) {
            sprintf(buf, "ERR:unknown keyspec\n");
            SendCStr(buf);
            return;
        }
        if (!TapKey(charCode, keyCode, modifiers)) {
            sprintf(buf, "ERR:keyDown rejected (OSErr %d) after %d key(s)\n",
                    (int)gLastKeyErr, nkeys);
            SendCStr(buf);
            return;
        }
        nkeys++;
    }

    if (nkeys == 0) {
        SendCStr("ERR:KEY needs at least one keyspec\n");
        return;
    }
    SendCStr("OK\n");
}

/* ---- Synthetic click-and-drag ----------------------------------------
 *
 * A drag cannot be done the way CLICK is. Dragging puts the Finder into a
 * *blocking* tracking loop (StillDown()/GetMouse()), and classic Mac OS is
 * cooperatively scheduled, so while that loop spins llm_agent gets no CPU
 * at all. Confirmed live: PING goes unanswered for as long as a real mouse
 * button is held down over an icon, and only recovers on release. Driving
 * a drag straight from the command handler would therefore deadlock -- we
 * would be waiting to move the very mouse the Finder is waiting on.
 *
 * So the drag runs from a VBL task, at interrupt time, which keeps ticking
 * while the Finder blocks. QuicKeys evidently does the same: the
 * investigation notes a step counter driving smooth multi-step movement in
 * its own code rather than an instant jump.
 *
 *   main code   position at source, MBState = down, post mouseDown,
 *               install the VBL task, reply, return to the event loop
 *   Finder      picks up the mouseDown, enters its tracking loop, blocks
 *   VBL @60Hz   steps the cursor along the path, holding MBState down
 *   VBL last    lands on the target and sets MBState = up
 *   Finder      loop exits, the drop happens
 *
 * MBState matters here in a way it does not for a plain click: StillDown()
 * reads it, and it is what keeps the Finder's loop alive mid-drag. That is
 * also why a stuck MBState is dangerous -- the whole machine would believe
 * the button is held -- hence the hard tick budget below.
 *
 * STATUS: INCOMPLETE -- DO NOT USE UNATTENDED.
 *
 * The movement half works: the VBL interpolation drives the cursor along
 * the path and the dragged object visibly follows it (confirmed on screen).
 * What does not work is *ending* the drag. The ROM tracking loop on this
 * machine is
 *     while (WaitMouseUp()) { GetMouse(...); ... }
 * with WaitMouseUp (trap 0xA977) at 0x408193f0 and GetMouse (0xA972) at
 * 0x4081937c, and it will not exit for anything this code can do:
 *   - releasing MBState is not enough (mb_at_end=80 confirmed);
 *   - posting a mouseUp is not enough, and the post itself demonstrably
 *     succeeds (up_posted=1, up_err=0 straight out of the VBL);
 *   - re-posting a mouseUp every tick for 12 ticks is not enough either.
 * Only a genuine hardware click ends it. WaitMouseUp appears to consult
 * live ADB state rather than the software event queue -- which is the one
 * place the old "clicks are impossible" claim in README.md was right, even
 * though it is wrong for ordinary click delivery (CLICK works fine).
 *
 * Until that is solved, a DRAG leaves the Finder spinning in its tracking
 * loop, which starves this agent of CPU (cooperative scheduling) until a
 * human clicks. DRAGRESET and DRAGSTAT exist for exactly that situation.
 * The untried next idea is patching trap 0xA977 for the duration of the
 * drag so it returns false on demand -- note README.md technique 3, where
 * an unrestored trap patch caused a hard fault, so any attempt must restore
 * the vector on every exit path including QUITAGENT/UPDATE.
 *
 * The VBL procedure runs with no valid A5, so it must not touch globals or
 * call helpers that do. It works only through the record pointer it gets in
 * A0 and through absolute low-memory addresses, and it must never spin
 * (no CrsrBusy wait) because it is interrupt context.
 */

typedef struct {
    VBLTask vbl;            /* MUST be first: A0 points here at VBL time */
    short   step;
    short   nSteps;
    short   ticksPerStep;
    short   x0, y0;
    short   x1, y1;
    short   ticksUsed;
    short   maxTicks;
    short   finished;
    short   installed;
    short   savedCouple;
    long    forceUp;       /* non-zero => the 0xA977 patch returns false */
    short   releaseTicks;  /* mouseUp reposts remaining after the path ends */
    short   upPosted;      /* 1 if the VBL's mouseUp PPostEvent gave an element */
    short   upErr;         /* the OSErr it returned */
    short   mbAtEnd;       /* MBState as the VBL left it */
} DragRec;

static DragRec *gDragRec = NULL;

/* Trap patch on WaitMouseUp (0xA977) -- the only thing that ends the Finder's
 * drag loop. See the STATUS note above: releasing MBState and posting mouseUp
 * events both provably fail to stop it, so the loop's own exit test gets
 * answered directly instead.
 *
 * gForceUpPtr points at gDragRec->forceUp. The stub reaches it through this
 * global rather than through gDragRec so there is no struct-offset constant
 * baked into the assembly. Globals are reachable from trap context in this
 * build -- the HLEWATCH stub above relies on exactly the same "m" operands.
 *
 * DANGER: if this patch is ever left installed while the agent's code goes
 * away, the Finder calls a dangling pointer many times a second and the
 * machine dies. README.md technique 3 is the cautionary tale. Every exit path
 * must call RestoreWaitMouseUp() -- the command loop, DRAGRESET, QUITAGENT
 * and UPDATE all do. */
static void *gRealA977 = NULL;
static volatile long *gForceUpPtr = NULL;
static volatile long gA977Hits = 0;      /* every call that reached the stub */
static volatile long gA977Forced = 0;    /* calls answered false by us */

/* CONTROL for the a977_hits=0 result. The ROM drag loop calls GetMouse
 * (0xA972) every iteration, right next to the WaitMouseUp that never reached
 * our patch. Counting GetMouse the same way separates two very different
 * explanations:
 *
 *   a972_hits > 0  the trap table IS live for this loop, so the WaitMouseUp
 *                  miss is something specific to that trap or that patch;
 *   a972_hits == 0 ROM-internal A-traps here bypass the dispatch table
 *                  altogether, and no trap patch will ever reach this loop.
 *
 * This stub only counts and jumps through -- it never changes behaviour --
 * which matters because GetMouse is called constantly by everything running. */
static void *gRealA972 = NULL;
static volatile long gA972Hits = 0;
/* Control-of-the-control: a hit count of zero only means "ROM bypasses the
 * table" if the patch was in the table to begin with. Read the vector straight
 * back after installing and compare it with the stub's own address. */
static unsigned long gA972Before = 0;
static unsigned long gA972After = 0;
static unsigned long gA972StubAddr = 0;

static void GetMouseStub(void)
{
    asm volatile (
        "addql  #1,%1\n\t"
        "unlk   %%fp\n\t"
        "movel  %0,%%a0\n\t"
        "jmp    %%a0@"
        :
        : "m"(gRealA972), "m"(gA972Hits)
        : "a0", "cc", "memory"
    );
}

static void WaitMouseUpStub(void)
{
    /* Stack here, after GCC's own linkw prologue (verified by disassembly):
     *   sp@(0) saved A6, sp@(4) return address, sp@(8) the Pascal Boolean
     *   result slot the caller reserved with subq.w #2,%sp.
     * Returning false means clearing that byte and doing a plain rts; the
     * caller then does tst.b (sp)+ and falls out of its loop. */
    asm volatile (
        "addql  #1,%2\n\t"
        "movel  %0,%%a0\n\t"
        "movel  %%a0,%%d0\n\t"
        "beq    1f\n\t"
        "tstl   %%a0@\n\t"
        "beq    1f\n\t"

        "addql  #1,%3\n\t"
        "clrb   %%sp@(8)\n\t"
        "unlk   %%fp\n\t"
        "rts\n"

        "1:\n\t"
        "unlk   %%fp\n\t"
        "movel  %1,%%a0\n\t"
        "jmp    %%a0@"
        :
        : "m"(gForceUpPtr), "m"(gRealA977), "m"(gA977Hits), "m"(gA977Forced)
        : "d0", "a0", "cc", "memory"
    );
}

static void RestoreWaitMouseUp(void)
{
    if (gRealA977 != NULL) {
        SetTrapAddress((ProcPtr)gRealA977, 0xA977);
        gRealA977 = NULL;
    }
    if (gRealA972 != NULL) {
        SetTrapAddress((ProcPtr)gRealA972, 0xA972);
        gRealA972 = NULL;
    }
    gForceUpPtr = NULL;
    if (gDragRec != NULL) {
        gDragRec->forceUp = 0;
    }
}

static void DragVBLProc(void)
{
    DragRec *r;
    Point p;
    long packed;

    /* A0 holds the VBLTask pointer, which is also the DragRec pointer. */
    asm volatile ("movea.l %%a0,%0" : "=a"(r));

    if (r->finished) {
        r->vbl.vblCount = 0;
        return;
    }

    r->ticksUsed += r->ticksPerStep;
    r->step++;

    if (r->step >= r->nSteps || r->ticksUsed >= r->maxTicks) {
        p.h = r->x1;
        p.v = r->y1;
        packed = *(long *)&p;
        *(long *)0x0828L = packed;               /* MTemp    */
        *(long *)0x082CL = packed;               /* RawMouse */
        *(long *)0x0830L = packed;               /* Mouse    */
        *(unsigned char *)0x08CEL = 0xFF;        /* CrsrNew  */
        *(unsigned char *)0x0172L = 0x80;        /* MBState: button UP */
        *(unsigned char *)0x08CFL =
            (unsigned char)r->savedCouple;       /* recouple the cursor */

        /* Post a real mouseUp. This is what actually ends the drag:
         * DragWindow/DragGrayRgn exit on the mouseUp *event*, not on
         * MBState, so releasing MBState alone leaves the Finder tracking
         * forever -- observed live, the window kept following the physical
         * mouse until a human clicked. PPostEvent is inlined here rather
         * than calling the shared helper because this is interrupt context. */
        {
            register long  d0 asm("%d0") = 0;
            register void *a0 asm("%a0") = (void *)2L;   /* mouseUp */

            asm volatile (
                ".word 0xA12F"
                : "+d"(d0), "+a"(a0)
                :
                : "d1", "a1", "cc", "memory"
            );

            r->upErr = (short)d0;
            r->upPosted = (a0 != NULL && (short)d0 == 0) ? 1 : 0;
            if (a0 != NULL && (short)d0 == 0) {
                EvQEl *q = (EvQEl *)a0;
                q->qType = 4;
                q->evtQWhat = 2;                 /* mouseUp */
                q->evtQMessage = 0;
                q->evtQWhere = p;
                q->evtQModifiers = 0x0080;
            }
        }

        r->mbAtEnd = (short)(*(unsigned char *)0x0172L);
        r->forceUp = 1;      /* make the patched WaitMouseUp answer false */

        /* Re-post rather than fire once. The ROM tracking loop is
         *     while (WaitMouseUp()) { GetMouse(...); }
         * (trap 0xA977 at 0x408193f0 on this ROM), and WaitMouseUp *removes*
         * the mouseUp it finds. A single posted event can therefore be
         * consumed by something else before the loop next looks, leaving it
         * spinning forever -- which is exactly what was observed, with
         * up_posted=1 and up_err=0 proving the post itself succeeded. So keep
         * re-posting for a few ticks until the loop actually takes one. */
        if (r->releaseTicks > 0) {
            r->releaseTicks--;
            r->step = r->nSteps;        /* stay in the release branch */
            r->vbl.vblCount = 1;        /* every tick */
            return;
        }

        r->finished = 1;
        r->vbl.vblCount = 0;
        return;
    }

    p.h = (short)(r->x0 + ((long)(r->x1 - r->x0) * (long)r->step) / (long)r->nSteps);
    p.v = (short)(r->y0 + ((long)(r->y1 - r->y0) * (long)r->step) / (long)r->nSteps);
    packed = *(long *)&p;
    *(long *)0x0828L = packed;
    *(long *)0x082CL = packed;
    *(long *)0x0830L = packed;
    *(unsigned char *)0x08CEL = 0xFF;
    *(unsigned char *)0x0172L = 0x00;            /* MBState: hold DOWN */

    r->vbl.vblCount = r->ticksPerStep;
}

/* DRAG x0 y0 x1 y1 [steps] [ticksPerStep] -- global screen coordinates.
 * Returns as soon as the drag is armed; the drop completes asynchronously
 * a few ticks later, so poll MOUSEPOS or take a screenshot to confirm. */
static void HandleDrag(short x0, short y0, short x1, short y1,
                       short steps, short ticksPerStep)
{
    Point p;
    char buf[160];
    char hdr[32];
    int len;

    if (steps < 2) { steps = 2; }
    if (steps > 200) { steps = 200; }
    if (ticksPerStep < 1) { ticksPerStep = 1; }
    if (ticksPerStep > 10) { ticksPerStep = 10; }

    if (gDragRec == NULL) {
        gDragRec = (DragRec *)NewPtrSys((Size)sizeof(DragRec));
        if (gDragRec == NULL) {
            SendCStr("ERR:NewPtrSys failed for drag record\n");
            return;
        }
        memset(gDragRec, 0, sizeof(DragRec));
    }

    /* A previous drag could still be installed if it was cut short. */
    if (gDragRec->installed) {
        VRemove((VBLTaskPtr)&gDragRec->vbl);
        gDragRec->installed = 0;
    }
    LMSetMBState(0x80);      /* never start from a stuck button */

    gDragRec->step = 0;
    gDragRec->nSteps = steps;
    gDragRec->ticksPerStep = ticksPerStep;
    gDragRec->x0 = x0;
    gDragRec->y0 = y0;
    gDragRec->x1 = x1;
    gDragRec->y1 = y1;
    gDragRec->ticksUsed = 0;
    gDragRec->finished = 0;
    gDragRec->releaseTicks = 12;
    gDragRec->upPosted = 0;
    gDragRec->upErr = 0;
    gDragRec->mbAtEnd = 0;

    /* Hard budget. However the interpolation goes, the button is released
     * within this many ticks -- a stuck-down MBState would leave the entire
     * machine believing the mouse is held, so this is not optional. */
    gDragRec->maxTicks = (short)(steps * ticksPerStep + 90);
    if (gDragRec->maxTicks > 600) {
        gDragRec->maxTicks = 600;
    }

    gDragRec->vbl.qType = 1;              /* vType */
    gDragRec->vbl.vblAddr = (ProcPtr)DragVBLProc;
    gDragRec->vbl.vblCount = ticksPerStep;
    gDragRec->vbl.vblPhase = 0;

    p.h = x0;
    p.v = y0;
    SetMouseTo(p);

    /* Decouple the cursor from the physical mouse for the duration. With it
     * coupled, the ADB interrupt keeps rewriting RawMouse/Mouse from the real
     * hardware and simply overwrites every position this drag writes -- seen
     * live, the dragged window followed the human's mouse instead of the
     * programmed path. QuicKeys' own routine clears this for the same reason.
     * The VBL restores it on the final step, and DRAGRESET is the backstop. */
    gDragRec->savedCouple = (short)LM_CRSRCOUPLE;
    if (gDragRec->savedCouple == 0) {
        gDragRec->savedCouple = 0xFF;     /* never persist a stuck value */
    }
    LM_CRSRCOUPLE = 0;

    gDragRec->forceUp = 0;
    gA977Hits = 0;
    gA977Forced = 0;
    gA972Hits = 0;
    gForceUpPtr = &gDragRec->forceUp;

    /* Arm the GetMouse control (counts only, never alters behaviour). */
    if (gRealA972 == NULL) {
        gRealA972 = (void *)GetTrapAddress(0xA972);
        gA972Before = (unsigned long)gRealA972;
        gA972StubAddr = (unsigned long)GetMouseStub;
        SetTrapAddress((ProcPtr)GetMouseStub, 0xA972);
        gA972After = (unsigned long)GetTrapAddress(0xA972);
    }

    /* DISARMED -- do not re-enable without reading this.
     *
     * Patching WaitMouseUp (0xA977) was the obvious way to answer the Finder's
     * tracking loop directly. It does not work, and the reason is measured
     * rather than guessed: with the patch installed across a full drag,
     * a977_hits came back 0. The stub was never entered even once, so the ROM
     * loop at 0x408193f0 does not dispatch WaitMouseUp through the Toolbox
     * trap table that SetTrapAddress writes -- despite HLEWATCH patching
     * 0xA88F by exactly the same route and counting thousands of calls.
     *
     * Leaving a trap patch armed costs real safety (README.md technique 3:
     * an unrestored patch caused a hard fault needing a full VM restart) and
     * buys nothing while it never fires, so the install is commented out.
     * The stub, the counters and RestoreWaitMouseUp() are kept because they
     * are the instrumentation that produced the result -- re-enable the two
     * lines below to reproduce it.
     *
     *   gRealA977 = (void *)GetTrapAddress(0xA977);
     *   SetTrapAddress((ProcPtr)WaitMouseUpStub, 0xA977);
     *
     * Worth trying next: patch GetMouse (0xA972), which the same loop calls,
     * purely as a control. If that also never fires, ROM-internal A-traps on
     * this machine bypass the dispatch table generally, and no trap-patch
     * approach will ever reach this loop. */

    LMSetMBState(0x00);                   /* button down */
    if (!PostMouseEvent(mouseDown, p)) {
        LMSetMBState(0x80);
        LM_CRSRCOUPLE = (unsigned char)gDragRec->savedCouple;
        RestoreWaitMouseUp();
        SendCStr("ERR:PPostEvent gave no queue element (drag mouseDown)\n");
        return;
    }

    if (VInstall((VBLTaskPtr)&gDragRec->vbl) != noErr) {
        LMSetMBState(0x80);
        LM_CRSRCOUPLE = (unsigned char)gDragRec->savedCouple;
        RestoreWaitMouseUp();
        SendCStr("ERR:VInstall failed\n");
        return;
    }
    gDragRec->installed = 1;

    len = sprintf(buf, "drag=%d,%d->%d,%d\r\nsteps=%d\r\nticks_per_step=%d\r\n",
                  x0, y0, x1, y1, steps, ticksPerStep);
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}


/* DRAGSTAT -- reports the drag record after the fact. The agent gets no CPU
 * while a drag is in flight (the Finder's tracking loop blocks it), so this
 * cannot be polled live -- but read afterwards it answers the question that
 * matters: did the VBL task actually run? step/ticks_used stay 0 if it never
 * fired, which is otherwise indistinguishable from a drag that ran but had
 * no effect. */
static void HandleDragStat(void)
{
    char buf[640];   /* grew past 256 once and smashed the stack */
    char hdr[32];
    int len;

    if (gDragRec == NULL) {
        SendCStr("SIZE:16\nno_drag_record\r\n");
        return;
    }

    len = sprintf(buf,
                  "installed=%d\r\nfinished=%d\r\nstep=%d\r\nnsteps=%d\r\n"
                  "ticks_used=%d\r\nmax_ticks=%d\r\nticks_per_step=%d\r\n"
                  "from=%d,%d\r\nto=%d,%d\r\nsaved_couple=%02x\r\n"
                  "vbl_count=%d\r\nvbl_qtype=%d\r\n"
                  "a977_hits=%ld\r\na977_forced=%ld\r\na972_hits=%ld\r\na972_before=%08lx\r\na972_after=%08lx\r\na972_stub=%08lx\r\n"
                  "release_ticks=%d\r\nup_posted=%d\r\nup_err=%d\r\nmb_at_end=%02x\r\n",
                  gDragRec->installed, gDragRec->finished,
                  gDragRec->step, gDragRec->nSteps,
                  gDragRec->ticksUsed, gDragRec->maxTicks,
                  gDragRec->ticksPerStep,
                  gDragRec->x0, gDragRec->y0,
                  gDragRec->x1, gDragRec->y1,
                  (unsigned char)gDragRec->savedCouple,
                  gDragRec->vbl.vblCount, gDragRec->vbl.qType,
                  gA977Hits, gA977Forced, gA972Hits,
                  gA972Before, gA972After, gA972StubAddr,
                  gDragRec->releaseTicks, gDragRec->upPosted, gDragRec->upErr,
                  (unsigned char)gDragRec->mbAtEnd);

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* REBOOT / SHUTDOWN -- part of the shared protocol (see
 * ../mcp-server/agent_client.py), reply "OK\n" then go down.
 *
 * Both go through the Shutdown Manager (trap 0xA895) rather than anything
 * harder: ShutDwnStart() restarts, ShutDwnPower() powers off. That matters
 * because the Shutdown Manager runs registered shutdown procedures and
 * flushes/unmounts volumes on the way out. Yanking the machine instead would
 * leave the HFS volume dirty, which on this guest means the "you have
 * restarted improperly" dialog on the next boot -- and that dialog blocks
 * Startup Items processing, so the agent would not come back on its own.
 *
 * Neither call returns, so everything that has to happen must happen first:
 * send the reply, take out any trap patch, and release the MacTCP stream the
 * same way QUITAGENT does so the port is not left bound. The short Delay is
 * there to give MacTCP a chance to actually put the reply on the wire before
 * the machine stops executing. */
static void HandlePower(int restart)
{
    long finalTicks;

    SendCStr("OK\n");

    RestoreWaitMouseUp();
    Delay(30, &finalTicks);          /* ~0.5s for the reply to go out */
    TCPStreamAbortAndRelease(gStream);
    Delay(15, &finalTicks);

    if (restart) {
        ShutDwnStart();
    } else {
        ShutDwnPower();
    }

    /* Not reached. If the Shutdown Manager ever did return, exiting is the
     * least surprising thing left to do -- the caller has already been told
     * the machine is going away. */
    ExitToShell();
}

/* DRAGRESET -- panic button. Releases the mouse button and tears down any
 * in-flight drag, for when a drag leaves the machine thinking the button is
 * still held. */
static void HandleDragReset(void)
{
    if (gDragRec != NULL && gDragRec->installed) {
        VRemove((VBLTaskPtr)&gDragRec->vbl);
        gDragRec->installed = 0;
        gDragRec->finished = 1;
    }
    RestoreWaitMouseUp();
    LMSetMBState(0x80);
    LM_CRSRCOUPLE = 0xFF;
    /* A tracking loop waits on the mouseUp event, so releasing MBState is
     * not enough on its own to free a Finder stuck mid-drag. */
    PostMouseEvent(mouseUp, LMGetMouseLocation());
    SendCStr("OK\n");
}

/* TEMP DIAGNOSTIC -- broadened version of the earlier single-site
 * 0xA88F (_OSDispatch) observer: instead of filtering for one exact
 * (selector, return-addr) pair, logs the (selector, return-addr) of
 * EVERY call whose return address falls inside CDRV_0's current
 * memory range, into a small 32-entry rolling buffer -- so we can see
 * every OSDispatch selector QuicKeys' driver actually uses during a
 * real click, not just guess call sites one at a time. Same stack-
 * layout facts as before apply (GCC's "linkw %fp,#0" prologue shifts
 * everything by +4, hence sp@(8)/sp@(4) and the explicit unlk before
 * jumping through to the real handler). CDRV_0's base is re-derived
 * fresh via GetResource('CDRV',0) each HLEWATCH START rather than
 * trusting a stale address from a prior boot, since heap addresses
 * are only reproducible boot-to-boot, not guaranteed. Remove once
 * identified. */
#define LOGBUF_SIZE 32
static short gLogSel[LOGBUF_SIZE];
static unsigned long gLogAddr[LOGBUF_SIZE];
static volatile unsigned long gLogWriteIdx = 0;
static volatile unsigned long gLogCount = 0;
static void *gRealA88F = NULL;
static volatile unsigned long gA88FTotalCount = 0;
static unsigned long gCDRV0Base = 0;
static unsigned long gCDRV0End = 0;

static void LogWatchStub(void)
{
    asm volatile (
        "movew  %%sp@(8),%%d0\n\t"
        "movel  %%sp@(4),%%d1\n\t"
        "addql  #1,%0\n\t"

        "movel  %4,%%d2\n\t"
        "cmpl   %%d2,%%d1\n\t"
        "bcs    1f\n\t"
        "cmpl   %5,%%d1\n\t"
        "bcc    1f\n\t"

        "movel  %1,%%d2\n\t"
        "andl   #31,%%d2\n\t"
        "asll   #1,%%d2\n\t"
        "lea    %2,%%a0\n\t"
        "addal  %%d2,%%a0\n\t"
        "movew  %%d0,%%a0@\n\t"

        "movel  %1,%%d2\n\t"
        "andl   #31,%%d2\n\t"
        "asll   #2,%%d2\n\t"
        "lea    %3,%%a0\n\t"
        "addal  %%d2,%%a0\n\t"
        "movel  %%d1,%%a0@\n\t"

        "addql  #1,%1\n\t"
        "addql  #1,%6\n"
        "1:\n\t"
        "unlk   %%fp\n\t"
        "movel  %7,%%a0\n\t"
        "jmp    %%a0@"
        :
        : "m"(gA88FTotalCount), "m"(gLogWriteIdx), "m"(gLogSel), "m"(gLogAddr),
          "m"(gCDRV0Base), "m"(gCDRV0End), "m"(gLogCount), "m"(gRealA88F)
        : "d0", "d1", "d2", "a0", "cc", "memory"
    );
}

static void HandleHLEWatchStart(unsigned long base, unsigned long size)
{
    char buf[96];
    char hdr[32];
    int len;

    /* base/size name whichever loaded driver block to watch (CDRV_0
     * or CDRV_1) -- re-verify the current address with SCANCDRV/
     * SCANSIG/PEEK after any reboot rather than trusting a prior
     * boot's value; heap layout shifts whenever what auto-launches at
     * startup changes. A stale range here is self-limiting (the check
     * just fails to match anything real), not a crash risk. */
    gCDRV0Base = base;
    gCDRV0End = base + size;

    gLogWriteIdx = 0;
    gLogCount = 0;
    gA88FTotalCount = 0;

    if (!gRealA88F) {
        gRealA88F = (void *)GetTrapAddress(0xA88F);
        SetTrapAddress((ProcPtr)LogWatchStub, 0xA88F);
    }
    len = sprintf(buf, "watching cdrv0=%08lx-%08lx real_a88f=%08lx\r\n",
                  gCDRV0Base, gCDRV0End, (unsigned long)gRealA88F);
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

static void HandleHLEWatchStatus(void)
{
    char buf[512];
    char hdr[32];
    int len;
    unsigned long i, n, idx;

    len = sprintf(buf, "total=%lu logged=%lu\r\n", gA88FTotalCount, gLogCount);
    n = gLogCount < LOGBUF_SIZE ? gLogCount : LOGBUF_SIZE;
    for (i = 0; i < n; i++) {
        idx = (gLogCount < LOGBUF_SIZE) ? i : ((gLogWriteIdx + i) & (LOGBUF_SIZE - 1));
        len += sprintf(buf + len, "sel=%04x addr=%08lx\r\n",
                        (unsigned)(unsigned short)gLogSel[idx], gLogAddr[idx]);
    }
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

static void HandleHLEWatchStop(void)
{
    char buf[64];
    char hdr[32];
    int len;

    if (gRealA88F) {
        SetTrapAddress((ProcPtr)gRealA88F, 0xA88F);
        gRealA88F = NULL;
    }
    len = sprintf(buf, "stopped total=%lu logged=%lu\r\n", gA88FTotalCount, gLogCount);
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- generic memory peek, hex dump of N bytes (capped)
 * at an arbitrary address. Plain reads only, no writes. Remove once
 * the live QuicKeys debugging session is done. */
static void HandlePeek(unsigned long addr, int n)
{
    char buf[600];
    char hdr[32];
    int len, i;
    const unsigned char *p = (const unsigned char *)addr;

    if (n > 256) n = 256;
    if (n < 1) n = 1;
    len = sprintf(buf, "addr=%08lx n=%d\r\n", addr, n);
    for (i = 0; i < n; i++) {
        len += sprintf(buf + len, "%02x", p[i]);
    }
    len += sprintf(buf + len, "\r\n");
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- bounded, 4-byte-aligned memory scan for CDRV_0's
 * known 16-byte signature (its raw resource header through the start
 * of the embedded ".QuicKeys" length-prefixed name). Needed because
 * the address found live via MacsBug earlier this session no longer
 * matches post-reboot -- almost certainly because llm_agent now
 * auto-launches from Startup Items, shifting boot-time heap
 * allocation order enough to move CDRV_0. Reports up to 4 matches so
 * we can re-anchor without guessing. Only ever reads. Remove once the
 * live QuicKeys debugging session is done. */
static const unsigned char kCDRV0Sig[16] = {
    0x64, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x22, 0x0e, 0x12, 0x00, 0x4e, 0x0e, 0x12
};

static void HandleScanCDRV0(unsigned long start, unsigned long end)
{
    char buf[128];
    char hdr[32];
    int len, matches;
    unsigned long addr;
    const unsigned char *p;

    len = sprintf(buf, "scanning %08lx-%08lx\r\n", start, end);
    matches = 0;
    for (addr = start; addr + 16 <= end && matches < 4; addr += 4) {
        p = (const unsigned char *)addr;
        if (p[0] == kCDRV0Sig[0] && p[1] == kCDRV0Sig[1] &&
            p[2] == kCDRV0Sig[2] && p[3] == kCDRV0Sig[3]) {
            if (memcmp((const void *)p, kCDRV0Sig, 16) == 0) {
                len += sprintf(buf + len, "match=%08lx\r\n", addr);
                matches++;
            }
        }
    }
    if (matches == 0) {
        len += sprintf(buf + len, "no match\r\n");
    }
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- same as SCANCDRV but the 16-byte signature is
 * passed in (as 32 hex chars) instead of hardcoded, so it can be
 * reused for CDRV_1 or anything else without a rebuild. Only reads. */
static void HandleScanSig(unsigned long start, unsigned long end, const unsigned char *sig)
{
    char buf[128];
    char hdr[32];
    int len, matches;
    unsigned long addr;
    const unsigned char *p;

    len = sprintf(buf, "scanning %08lx-%08lx\r\n", start, end);
    matches = 0;
    for (addr = start; addr + 16 <= end && matches < 4; addr += 4) {
        p = (const unsigned char *)addr;
        if (p[0] == sig[0] && p[1] == sig[1] && p[2] == sig[2] && p[3] == sig[3]) {
            if (memcmp((const void *)p, sig, 16) == 0) {
                len += sprintf(buf + len, "match=%08lx\r\n", addr);
                matches++;
            }
        }
    }
    if (matches == 0) {
        len += sprintf(buf + len, "no match\r\n");
    }
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- looks up a resource by type+id via the normal,
 * safe Resource Manager search chain (GetResource), rather than
 * blind-probing memory. Unlike CDRV_0/1 (an INIT's resource file,
 * closed once boot finishes even though the loaded code stays
 * resident), a live *application*'s own resource fork stays open for
 * its entire run -- so this should find QuicKeys Toolbox's CODE
 * segments directly, with no bus-error risk at all. Reports the
 * resource's master pointer (its actual runtime address) and size.
 * Remove once the live QuicKeys debugging session is done. */
static void HandleResLookup(unsigned long type, int id)
{
    Handle h;
    char buf[64];
    char hdr[32];
    int len;

    h = GetResource(type, id);
    if (!h) {
        len = sprintf(buf, "not found\r\n");
    } else if (!*h) {
        len = sprintf(buf, "found handle=%08lx but purged/null\r\n", (unsigned long)h);
    } else {
        len = sprintf(buf, "addr=%08lx size=%ld\r\n",
                       (unsigned long)*h, (long)GetHandleSize(h));
    }
    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- reports the real dispatch address of the _Control
 * (0xA004) trap via GetTrapAddress(), the same properly-encoded call
 * used earlier this session for trap-patching. Needed because MacsBug's
 * bare-symbol-name lookup for OS traps outside the #A800-#ABFF range
 * kept resolving to the wrong (non-trap) symbol -- this sidesteps that
 * entirely by asking the OS directly. Remove once the live QuicKeys
 * debugging session is done. */
static void HandleTrapAddr(void)
{
    void *addr;
    char buf[256];
    char hdr[32];
    int len;

    addr = (void *)GetTrapAddress(0xA004); /* _Control */
    len = sprintf(buf, "control_addr=%08lx\r\n", (unsigned long)addr);
    addr = (void *)GetTrapAddress(0xA000); /* _Open */
    len += sprintf(buf + len, "open_addr=%08lx\r\n", (unsigned long)addr);
    addr = (void *)GetTrapAddress(0xA816); /* _Pack8 -- AppleEvent Manager dispatch */
    len += sprintf(buf + len, "pack8_addr=%08lx\r\n", (unsigned long)addr);
    addr = (void *)GetTrapAddress(0xA12F); /* unknown -- QuicKeys CDRV_0 calls this with a selector in A0 */
    len += sprintf(buf + len, "a12f_addr=%08lx\r\n", (unsigned long)addr);
    addr = (void *)GetTrapAddress(0xA0FF); /* known-unassigned OS trap, for comparison */
    len += sprintf(buf + len, "a0ff_addr=%08lx\r\n", (unsigned long)addr);

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- calls the mystery trap 0xA12F ourselves with the
 * same A0=<small index>, D0=0 convention QuicKeys' CDRV_0 uses, and
 * reports back the pointer it returns in A0 plus a short hex dump of
 * what that pointer points to. QuicKeys writes into offset+16 of this
 * returned structure without ever null-checking it first, so a valid
 * pointer for index 1/2 is expected. Remove once identified. */
static void HandleDeviceProbe(long index)
{
    void *devPtr = NULL;
    char buf[160];
    char hdr[32];
    int len;
    unsigned char *p;
    int i;

    asm volatile (
        "movea.l %1,%%a0\n\t"
        "clr.w   %%d0\n\t"
        ".word   0xA12F\n\t"
        "movea.l %%a0,%0"
        : "=a"(devPtr)
        : "r"(index)
        : "d0", "d1", "a0", "a1", "cc"
    );

    len = sprintf(buf, "index=%ld\r\n", index);
    len += sprintf(buf + len, "dev_ptr=%08lx\r\n", (unsigned long)devPtr);
    if (devPtr) {
        p = (unsigned char *)devPtr;
        len += sprintf(buf + len, "dev_bytes=");
        for (i = 0; i < 32; i++) {
            len += sprintf(buf + len, "%02x", p[i]);
        }
        len += sprintf(buf + len, "\r\n");
    }

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
}

/* TEMP DIAGNOSTIC -- same as HandleDeviceProbe but also lets D0 be set
 * to an arbitrary value, since QuicKeys itself calls trap 0xA12F with
 * A0=1, D0='QK2 ' (0x514B3220, its own signature) at CDRV_1+0x52e8,
 * distinct from every other call site which clears D0. Testing whether
 * that variant surfaces QuicKeys' own registered device record. Remove
 * once identified. */
static void HandleDeviceProbe2(long index, unsigned long d0val)
{
    void *devPtr = NULL;
    char buf[160];
    char hdr[32];
    int len;
    unsigned char *p;
    int i;

    asm volatile (
        "movea.l %1,%%a0\n\t"
        "move.l  %2,%%d0\n\t"
        ".word   0xA12F\n\t"
        "movea.l %%a0,%0"
        : "=a"(devPtr)
        : "r"(index), "r"(d0val)
        : "d0", "d1", "a0", "a1", "cc"
    );

    len = sprintf(buf, "index=%ld d0=%08lx\r\n", index, d0val);
    len += sprintf(buf + len, "dev_ptr=%08lx\r\n", (unsigned long)devPtr);
    if (devPtr) {
        p = (unsigned char *)devPtr;
        len += sprintf(buf + len, "dev_bytes=");
        for (i = 0; i < 32; i++) {
            len += sprintf(buf + len, "%02x", p[i]);
        }
        len += sprintf(buf + len, "\r\n");
    }

    sprintf(hdr, "SIZE:%d\n", len);
    SendCStr(hdr);
    SendAll(buf, len);
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

    /* Build stamp, so a live UPDATE can be verified as having actually taken
     * effect. Without it a successful-looking update is indistinguishable from
     * the old binary still running. */
    len += sprintf(buf + len, "agent_build=%s %s\r\n", __DATE__, __TIME__);

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
        /* The wire format is "PUT <path> <size>", so the path and the size are
         * space-separated -- but classic Mac paths are full of spaces
         * ("Desktop Folder", "System Folder", "Startup Items"). Splitting on
         * the FIRST space truncated every such path, which meant PUT happily
         * reported OK while writing to the wrong place, and a following GET of
         * the same path then said "file not found". Split on the LAST space
         * instead, the way agent-win32 already does. */
        char *lastSpace = strrchr(args, ' ');
        long n;

        if (lastSpace == NULL) {
            SendCStr("ERR:PUT needs <path> <size>\n");
            return;
        }
        n = (long)(lastSpace - args);
        if (n > (long)sizeof(path) - 1) {
            n = (long)sizeof(path) - 1;
        }
        memcpy(path, args, (size_t)n);
        path[n] = '\0';
        size = atol(lastSpace + 1);
    }

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
    RestoreWaitMouseUp();   /* the replacement binary lands at a different
                             * address; a patch left pointing into this one
                             * would be called by the Finder within
                             * milliseconds and take the machine down */
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
    /* Relative lookup works when launched from its own folder; fall back
     * to the absolute path when launched via a Startup Items alias/copy,
     * whose default directory is Startup Items itself, not LLMAGENT. */
    FILE *f = fopen("LLMAGENT.INI", "r");
    char line[TOKEN_MAX_LEN];

    if (!f) f = fopen("MacOS:Desktop Folder:LLMAGENT:LLMAGENT.INI", "r");

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

        /* Take the WaitMouseUp patch back out as soon as we are running
         * again. A drag installs it and is then starved until the Finder's
         * tracking loop ends, so this is the first safe opportunity. Leaving
         * it installed any longer than necessary is what makes a trap patch
         * dangerous -- see the DANGER note on WaitMouseUpStub. */
        RestoreWaitMouseUp();

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
            RestoreWaitMouseUp();   /* never exit with the trap still patched */
            TCPStreamAbortAndRelease(gStream);
            ExitToShell();
        } else if (strcmp(gLine, "SYSINFO") == 0) {
            HandleSysinfo();
        } else if (strcmp(gLine, "REBOOT") == 0) {
            HandlePower(1);
        } else if (strcmp(gLine, "SHUTDOWN") == 0) {
            HandlePower(0);
        } else if (strcmp(gLine, "PSLIST") == 0) {
            HandlePslist();
        } else if (strcmp(gLine, "MOUSEPOS") == 0) {
            HandleMousePos();
        } else if (strncmp(gLine, "PSKILL ", 7) == 0) {
            HandlePsKill(strtoul(gLine + 7, NULL, 10));
        } else if (strcmp(gLine, "WINLIST") == 0) {
            HandleWinList();
        } else if (strncmp(gLine, "WINLIST ", 8) == 0) {
            /* The parent form lists a dialog's child controls and is
             * Win16-specific; classic Mac controls are not windows. */
            SendCStr("ERR:WINLIST <parent> not supported on this platform\n");
        } else if (strcmp(gLine, "CLIPGET") == 0) {
            HandleClipGet();
        } else if (strncmp(gLine, "CLIPSET ", 8) == 0) {
            HandleClipSet(gLine + 8);
        } else if (strncmp(gLine, "KEY ", 4) == 0) {
            HandleKey(gLine + 4);
        } else if (strncmp(gLine, "TYPE ", 5) == 0) {
            HandleType(gLine + 5);
        } else if (strncmp(gLine, "CLICK ", 6) == 0) {
            char *csp;
            long cx = strtol(gLine + 6, &csp, 10);
            long cy = strtol(csp, &csp, 10);
            /* Optional third argument. strtol leaves 0 when there are no
             * digits left, which is the "omitted" case -- treat it as 1. */
            long cb = strtol(csp, NULL, 10);
            HandleClick((short)cx, (short)cy, 0, (short)(cb == 0 ? 1 : cb));
        } else if (strncmp(gLine, "DBLCLICK ", 9) == 0) {
            char *csp;
            long cx = strtol(gLine + 9, &csp, 10);
            long cy = strtol(csp, &csp, 10);
            long cb = strtol(csp, NULL, 10);
            HandleClick((short)cx, (short)cy, 1, (short)(cb == 0 ? 1 : cb));
        } else if (strcmp(gLine, "DRAGRESET") == 0) {
            HandleDragReset();
        } else if (strcmp(gLine, "DRAGSTAT") == 0) {
            HandleDragStat();
        } else if (strncmp(gLine, "DRAG ", 5) == 0) {
            char *dsp = gLine + 5;
            long dx0 = strtol(dsp, &dsp, 10);
            long dy0 = strtol(dsp, &dsp, 10);
            long dx1 = strtol(dsp, &dsp, 10);
            long dy1 = strtol(dsp, &dsp, 10);
            long dst = strtol(dsp, &dsp, 10);
            long dtp = strtol(dsp, NULL, 10);
            if (dst <= 0) { dst = 20; }
            if (dtp <= 0) { dtp = 2; }
            HandleDrag((short)dx0, (short)dy0, (short)dx1, (short)dy1,
                       (short)dst, (short)dtp);
        } else if (strcmp(gLine, "TRAPADDR") == 0) {
            HandleTrapAddr(); /* temp diagnostic */
        } else if (strncmp(gLine, "DEVPROBE ", 9) == 0) {
            HandleDeviceProbe(atol(gLine + 9)); /* temp diagnostic */
        } else if (strncmp(gLine, "DEVPROBE2 ", 10) == 0) {
            long idx;
            unsigned long d0v;
            char *sp;
            idx = strtol(gLine + 10, &sp, 10);
            d0v = strtoul(sp, NULL, 16);
            HandleDeviceProbe2(idx, d0v); /* temp diagnostic */
        } else if (strncmp(gLine, "HLEWATCH START ", 15) == 0) {
            unsigned long wbase, wsize;
            char *wsp;
            wbase = strtoul(gLine + 15, &wsp, 16);
            wsize = strtoul(wsp, NULL, 16);
            HandleHLEWatchStart(wbase, wsize); /* temp diagnostic */
        } else if (strcmp(gLine, "HLEWATCH STATUS") == 0) {
            HandleHLEWatchStatus(); /* temp diagnostic */
        } else if (strcmp(gLine, "HLEWATCH STOP") == 0) {
            HandleHLEWatchStop(); /* temp diagnostic */
        } else if (strncmp(gLine, "PEEK ", 5) == 0) {
            unsigned long paddr;
            int pn;
            char *psp;
            paddr = strtoul(gLine + 5, &psp, 16);
            pn = (int)strtol(psp, NULL, 10);
            HandlePeek(paddr, pn); /* temp diagnostic */
        } else if (strncmp(gLine, "SCANCDRV ", 9) == 0) {
            unsigned long sstart, send_;
            char *ssp;
            sstart = strtoul(gLine + 9, &ssp, 16);
            send_ = strtoul(ssp, NULL, 16);
            HandleScanCDRV0(sstart, send_); /* temp diagnostic */
        } else if (strncmp(gLine, "SCANSIG ", 8) == 0) {
            unsigned long xstart, xend;
            unsigned char sig[16];
            char *xsp, *sigStr;
            int si;
            xstart = strtoul(gLine + 8, &xsp, 16);
            xend = strtoul(xsp, &xsp, 16);
            while (*xsp == ' ') xsp++;
            sigStr = xsp;
            for (si = 0; si < 16; si++) {
                char hx[3];
                hx[0] = sigStr[si * 2];
                hx[1] = sigStr[si * 2 + 1];
                hx[2] = '\0';
                sig[si] = (unsigned char)strtoul(hx, NULL, 16);
            }
            HandleScanSig(xstart, xend, sig); /* temp diagnostic */
        } else if (strncmp(gLine, "RESLOOKUP ", 10) == 0) {
            unsigned long rtype;
            int rid;
            char *rsp;
            rtype = strtoul(gLine + 10, &rsp, 16);
            rid = (int)strtol(rsp, NULL, 10);
            HandleResLookup(rtype, rid); /* temp diagnostic */
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
