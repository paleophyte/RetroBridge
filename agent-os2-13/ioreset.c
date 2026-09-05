/*
 * IORESET.EXE - OS/2 1.3 hardware reset via an I/O privilege segment.
 *
 * Ring-3 half of the pair; IOSEG.DLL is the ring-2 half (see ioseg.c for
 * how the IOPL segment and its call gates are set up). This side asks
 * OS/2 for the ports with DosPortAccess() - the grant is per-process and
 * has to happen before any IN/OUT, even from ring 2 - then calls through.
 *
 * IOSEG.DLL must sit in the same directory as this .EXE (it is loaded by
 * path, not via LIBPATH), and CONFIG.SYS needs IOPL=YES.
 *
 *   IORESET <method> [FLUSH|CACHE]
 *
 * method:
 *   KBD      8042 pulse reset, out 0xFE to port 0x64. The one the agent
 *            uses, and the only one confirmed to work on this VM.
 *   92       system control port A, bit 0.
 *   CF9      chipset reset control, 0x02 then 0x06.
 *   CAD      synthesise Ctrl-Alt-Del via 8042 command 0xD2.
 *   ALL      every method above, in turn.
 *   PROBE    read-only: grant the ports and read them back. Proves the
 *            IOPL path works without resetting anything.
 *   IOCTL    device-driver IOCTL probes, no ports touched.
 *   WATCHDOG internal - see arm_watchdog().
 *
 * mode:
 *   FLUSH    DosShutdown() first, behind a watchdog. What the agent uses:
 *            it is the only thing that clears HPFS's dirty-volume flag,
 *            and so the only way to get a boot without CHKDSK.
 *   CACHE    CACHE.EXE /LAZY:OFF instead. Keeps the filesystem writable,
 *            so a failed reset costs nothing - but the volume still comes
 *            up dirty and CHKDSK still runs. Useful for experimenting.
 *   (none)   reset with buffers flushed but the volume left dirty.
 *
 * Everything is logged to IORESET.LOG next to the .EXE as well as stdout,
 * because a method that works takes the machine down mid-printf - and
 * because after DosShutdown neither destination is available at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <process.h>

#define INCL_DOS
#define INCL_DOSDEVICES
#define INCL_DOSFILEMGR
#define INCL_DOSPROCESS
#include <os2.h>

/*
 * IOSEG.DLL's exports, reached through ring-2 call gates.
 *
 * Resolved at runtime with DosLoadModule/DosGetProcAddr rather than
 * imported at link time, purely so IOSEG.DLL can sit next to this .EXE in
 * C:\LLMAGENT instead of having to be somewhere on LIBPATH. Confirmed
 * live that this really does hand back a call gate and not a plain far
 * address: DosGetProcAddr returns selector:0000, and calling through it
 * from ring 3 executes the IN without faulting.
 */
typedef USHORT (__far __pascal *PFNIO1)(USHORT);
typedef USHORT (__far __pascal *PFNIO2)(USHORT, USHORT);
typedef USHORT (__far __pascal *PFNIO0)(void);

static PFNIO1 IOIN8;
static PFNIO2 IOOUT8;
static PFNIO0 IOKBDRESET;
static PFNIO0 IOPORT92RESET;
static PFNIO0 IOCF9RESET;
static PFNIO0 IOCAD;

#define PORT_ACCESS_REQUEST 1
#define PORT_ACCESS_RELEASE 0

static char g_log[160];

/*
 * Set once DosShutdown() has quiesced the filesystem, after which logf()
 * goes completely silent - both the log file and stdout, since the agent
 * redirects stdout to a temp file of its own.
 *
 * This is not tidiness, it is the whole reason the first DosShutdown
 * attempt failed: writing anything at all after the shutdown blocks
 * forever on a filesystem that has stopped accepting writes, so the
 * process never reached the port write and the machine just sat there
 * quiesced and wedged. Nothing may touch a file between DosShutdown() and
 * the reset.
 */
static int g_quiesced;

static void logf(const char *fmt, ...)
{
    va_list ap;
    FILE *f;

    if (g_quiesced)
        return;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);

    f = fopen(g_log, "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void set_log_path(const char *argv0)
{
    char *slash;

    strcpy(g_log, "IORESET.LOG");
    if (!argv0 || !argv0[0])
        return;
    strncpy(g_log, argv0, sizeof(g_log) - 16);
    g_log[sizeof(g_log) - 16] = '\0';
    slash = strrchr(g_log, '\\');
    if (!slash)
        slash = strrchr(g_log, '/');
    if (slash)
        strcpy(slash + 1, "IORESET.LOG");
    else
        strcpy(g_log, "IORESET.LOG");
}

/*
 * Resolve every IOSEG.DLL entry point. Tries the DLL sitting next to this
 * .EXE first, then bare "IOSEG" so a copy on LIBPATH still works.
 * Returns 0 on success.
 */
static int load_ioseg(const char *argv0)
{
    char path[160];
    char fail[160];
    char *slash;
    HMODULE hmod = 0;
    USHORT rc;
    int i;

    static const struct { const char *name; PFN *slot; } entries[] = {
        { "IOIN8",         (PFN *)&IOIN8         },
        { "IOOUT8",        (PFN *)&IOOUT8        },
        { "IOKBDRESET",    (PFN *)&IOKBDRESET    },
        { "IOPORT92RESET", (PFN *)&IOPORT92RESET },
        { "IOCF9RESET",    (PFN *)&IOCF9RESET    },
        { "IOCAD",         (PFN *)&IOCAD         }
    };

    path[0] = '\0';
    if (argv0 && argv0[0]) {
        strncpy(path, argv0, sizeof(path) - 16);
        path[sizeof(path) - 16] = '\0';
        slash = strrchr(path, '\\');
        if (!slash)
            slash = strrchr(path, '/');
        if (slash)
            strcpy(slash + 1, "IOSEG.DLL");
        else
            path[0] = '\0';
    }

    fail[0] = '\0';
    rc = path[0] ? DosLoadModule((PSZ)fail, sizeof(fail), (PSZ)path, &hmod) : 1;
    if (rc != 0) {
        logf("DosLoadModule(%s) rc=%u fail='%s' - retrying via LIBPATH",
             path[0] ? path : "(no path)", rc, fail);
        fail[0] = '\0';
        rc = DosLoadModule((PSZ)fail, sizeof(fail), (PSZ)"IOSEG", &hmod);
    }
    if (rc != 0) {
        logf("DosLoadModule(IOSEG) rc=%u fail='%s' - cannot reach ring 2",
             rc, fail);
        return -1;
    }

    for (i = 0; i < (int)(sizeof(entries) / sizeof(entries[0])); i++) {
        rc = DosGetProcAddr(hmod, (PSZ)entries[i].name, entries[i].slot);
        if (rc != 0) {
            logf("DosGetProcAddr(%s) rc=%u", entries[i].name, rc);
            return -1;
        }
    }
    logf("IOSEG.DLL loaded, %d ring-2 gates resolved", i);
    return 0;
}

/* Grant this process one port range. Logged individually: OS/2 can refuse
 * ports a device driver already owns, and which ones it refuses is the
 * whole answer to which reset methods are even available. */
static USHORT grant(USHORT first, USHORT last)
{
    USHORT rc = DosPortAccess(0, PORT_ACCESS_REQUEST, first, last);

    logf("  DosPortAccess(0x%03X..0x%03X) rc=%u%s",
         first, last, rc, rc ? "  <-- DENIED" : "  ok");
    return rc;
}

static void grant_all(void)
{
    logf("granting port access:");
    grant(0x0060, 0x0060);      /* 8042 data */
    grant(0x0064, 0x0064);      /* 8042 command/status */
    grant(0x0092, 0x0092);      /* system control port A */
    grant(0x0CF9, 0x0CF9);      /* chipset reset control */
}

static void probe(void)
{
    logf("reading ports back through the ring-2 gate:");
    logf("  in 0x64 (8042 status)      = 0x%02X", IOIN8(0x64));
    logf("  in 0x92 (sys control A)    = 0x%02X", IOIN8(0x92));
    logf("  in 0xCF9 (reset control)   = 0x%02X", IOIN8(0x0CF9));
    logf("  in 0x61 (port B, ungranted)= 0x%02X", IOIN8(0x61));
    logf("PROBE survived - IOPL segment works, no fault.");
}

static void ioctl_probe(void)
{
    static const char *devs[] = { "OEMHLP$", "DOS$", "\\DEV\\DOS$", "KBD$",
                                  "SCREEN$", "POINTER$", NULL };
    USHORT i;

    logf("device open probes:");
    for (i = 0; devs[i]; i++) {
        HFILE hf = 0;
        USHORT action = 0;
        USHORT rc;

        rc = DosOpen((PSZ)devs[i], &hf, &action, 0L, FILE_NORMAL,
                     OPEN_ACTION_OPEN_IF_EXISTS,
                     OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE |
                     OPEN_FLAGS_FAIL_ON_ERROR, 0L);
        if (rc != 0) {
            logf("  DosOpen %-12s rc=%u (absent)", devs[i], rc);
            continue;
        }
        logf("  DosOpen %-12s rc=0 handle=%u", devs[i], hf);
        rc = DosDevIOCtl(NULL, NULL, 0x5D, 0x80, hf);
        logf("      IOCTL cat=0x80 fn=0x5D rc=%u", rc);
        rc = DosDevIOCtl(NULL, NULL, 0xAB, 0xD5, hf);
        logf("      IOCTL cat=0xD5 fn=0xAB rc=%u", rc);
        DosClose(hf);
    }
}

/* Flush every open file buffer to disk. Cheap, reversible, and always
 * worth doing before yanking the hardware out from under HPFS386's
 * lazy-write cache - unlike DosShutdown below, this leaves the system
 * perfectly usable if the reset then doesn't happen. */
static void flush_buffers(void)
{
    USHORT rc = DosBufReset((HFILE)0xFFFF);

    logf("DosBufReset(all) rc=%u", rc);
}

/* Turn HPFS386's lazy-write cache off at runtime and give it a moment to
 * drain. This box boots CACHE.EXE with /LAZY:ON, and that cache is a
 * layer below DosBufReset - which is why flushing file buffers alone
 * still left a freshly written file in pieces across the first successful
 * reset, and why the volume came up dirty enough for AUTOCHECK to run
 * CHKDSK. Unlike DosShutdown this is reversible and leaves the filesystem
 * writable, so logging keeps working and a failed reset costs nothing. */
static void cache_lazy_off(void)
{
    static const char *cache = "C:\\LANMAN\\NETPROG\\CACHE.EXE";
    int rc;

    logf("disabling HPFS386 lazy write: %s /LAZY:OFF", cache);
    rc = spawnl(P_WAIT, cache, cache, "/LAZY:OFF", NULL);
    logf("  spawnl rc=%d", rc);
    flush_buffers();
    DosSleep(4000L);
    flush_buffers();
}

/*
 * Bring the ring-2 gate and its segment into memory *now*, while the
 * filesystem still works.
 *
 * IOSEG is a discardable, load-on-call segment, and MEMMAN=SWAP is on. If
 * OS/2 has to demand-load or swap it back in on the first call, that call
 * needs disk I/O - and after DosShutdown there is no disk I/O to be had,
 * so the process hangs on the very instruction that was supposed to reset
 * the machine. That is exactly what happened on the first two DosShutdown
 * attempts: the box sat quiesced and wedged, agent included, and never
 * reset. Touching every entry point first makes the post-shutdown calls
 * pure memory accesses.
 */
static void warm_gate(void)
{
    logf("warming the ring-2 gate (in 0x64 = 0x%02X)", IOIN8(0x64));
}

/* The real thing: stops all disk writes system-wide so the volume is
 * consistent across the reset. One-way - after this the filesystem is
 * read-only until the machine actually restarts, which is why it is
 * opt-in (the FLUSH argument) rather than automatic. Nothing between here
 * and the port write may touch a file: see g_quiesced and warm_gate. */
static void shutdown_disks(void)
{
    logf("DosShutdown(0) - quiescing the filesystem (one-way);"
         " logging stops here by design");
    fflush(NULL);
    g_quiesced = 1;
    DosShutdown(0L);
    /* No log line for the return code, deliberately - see g_quiesced. If
     * the caller's reset then works, nobody needed it; if it doesn't, the
     * machine is quiesced and wants a Ctrl-Alt-Del either way. No DosSleep
     * here either: every moment between the shutdown and the reset is
     * another chance for OS/2 to swap something out that we then cannot
     * page back in. */
}

/*
 * Watchdog: a second copy of this program, in its own process, that
 * resets the machine unconditionally N seconds from now.
 *
 * DosShutdown is the only thing that clears HPFS's dirty-volume flag, so
 * it is the only way to get a boot without CHKDSK - but if anything after
 * it blocks (DosShutdown not returning, a swapped-out page that can no
 * longer be paged back in), the box is left quiesced, wedged, and needing
 * someone at the console. Arming this first degrades the worst case to
 * "reset a few seconds late with the volume still dirty".
 *
 * A separate process rather than a thread, deliberately: the first
 * version of this used DosCreateThread and the process died on the spot,
 * before DosShutdown was even reached - Watcom's 16-bit OS/2 C runtime
 * here is the single-threaded one. A child process has its own address
 * space and its own copy of the runtime, and survives whatever happens to
 * the parent.
 *
 * The child grants its own ports and warms its own gate before it starts
 * sleeping, so that once it wakes it needs nothing from the filesystem.
 */
#define WATCHDOG_SECS 25

static void watchdog_child(const char *secs)
{
    USHORT n = (USHORT)atoi(secs);

    grant_all();
    warm_gate();
    logf("watchdog child ready: unconditional reset in %us", n);
    fflush(NULL);

    /* Past this point the parent may have quiesced the filesystem, so
     * this thread of control must not touch a file again. */
    g_quiesced = 1;
    DosSleep((ULONG)n * 1000L);
    IOKBDRESET();
    IOPORT92RESET();
    IOCF9RESET();
}

static void arm_watchdog(const char *self)
{
    char secs[8];
    int rc;

    sprintf(secs, "%d", WATCHDOG_SECS);
    rc = spawnl(P_NOWAIT, self, self, "WATCHDOG", secs, NULL);
    logf("watchdog armed: %s WATCHDOG %s -> spawnl rc=%d", self, secs, rc);
    /* Give the child time to grant its ports, warm its gate and get its
     * own logging out to disk before the filesystem goes away. */
    DosSleep(3000L);
}

int main(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "PROBE";
    const char *mode = (argc > 2) ? argv[2] : "";
    int all;

    set_log_path(argc > 0 ? argv[0] : NULL);
    logf("=== IORESET.EXE %s ===", what);

    if (strcmp(what, "IOCTL") == 0) {
        ioctl_probe();
        return 0;
    }

    if (load_ioseg(argc > 0 ? argv[0] : NULL) != 0)
        return 1;

    if (strcmp(what, "WATCHDOG") == 0) {
        watchdog_child(argc > 2 ? argv[2] : "25");
        return 1;
    }

    grant_all();

    if (strcmp(what, "PROBE") == 0) {
        probe();
        return 0;
    }

    flush_buffers();
    if (strcmp(mode, "CACHE") == 0)
        cache_lazy_off();

    /* Order matters and is the whole lesson of this file: load the gate,
     * arm the watchdog, and only then take the filesystem away. */
    warm_gate();
    if (strcmp(mode, "FLUSH") == 0) {
        arm_watchdog(argv[0]);
        shutdown_disks();
    }

    all = (strcmp(what, "ALL") == 0);

    /* Order: least to most disruptive if it half-works. Nothing below
     * gates on the previous call's return value - reaching the next line
     * at all is the only evidence that the previous method failed. */
    if (all || strcmp(what, "KBD") == 0) {
        logf("trying 8042 pulse reset (out 0x64,0xFE)");
        IOKBDRESET();
        logf("  still running - no reset");
    }
    if (all || strcmp(what, "92") == 0) {
        logf("trying system control port A (out 0x92, bit0)");
        IOPORT92RESET();
        logf("  still running - no reset");
    }
    if (all || strcmp(what, "CF9") == 0) {
        logf("trying chipset reset control (out 0xCF9, 0x02/0x06)");
        IOCF9RESET();
        logf("  still running - no reset");
    }
    if (all || strcmp(what, "CAD") == 0) {
        logf("trying Ctrl-Alt-Del injection (8042 cmd 0xD2)");
        IOCAD();
        logf("  still running - no reset");
    }

    logf("=== IORESET.EXE finished: machine still up (FAILED) ===");
    return 1;
}
