/* llm_updater for classic Mac OS (System 7.x).
 *
 * Companion to llm_agent -- see llm_agent.c's UPDATE command. llm_agent
 * launches this app and then exits, right before this app's own main()
 * actually gets any CPU time: System 7 is cooperatively scheduled, so
 * nothing else runs until llm_agent yields, and it yields precisely by
 * exiting. By the time we're executing, llm_agent's file and TCP port
 * are already free.
 *
 * Classic Mac files are two forks (data + resource), and the transfer
 * pipeline for a new llm_agent build is a flat MacBinary-encoded blob
 * (the same encoding `hcopy -m` produces, and what llm_agent's UPDATE
 * handler stages to STAGED_AGENT.bin next to itself before launching
 * us) -- so getting from "new build, staged as a flat file" to "a
 * proper dual-fork app ready to launch" means decoding MacBinary
 * ourselves. That decoding used to be done by hand, by dragging the
 * .bin onto StuffIt Expander; this replaces that manual step so a full
 * update -- new code running, old code gone -- needs no GUI
 * interaction at all.
 *
 * MacBinary header layout below is taken directly from hfsutils'
 * copyin.c/copyout.c (the exact encoder/decoder pair `hcopy -m` uses),
 * not from memory of the spec -- offsets confirmed against that source
 * rather than guessed.
 *
 * Logs every step to UPDATER.LOG next to itself (same convention as
 * agent-os2/update.c) -- this has no console and no visible UI, so
 * without a log file a failure is completely silent.
 */

#include <Types.h>

#ifndef GENERATINGCFM
#define GENERATINGCFM 0
#endif
#define kPascalStackBased 0
#define kCStackBased 0
#define SIZE_CODE(x) (x)
#define STACK_ROUTINE_PARAMETER(x, y) 0

#include <Files.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <Events.h>
#include <OSUtils.h>
#include <Errors.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STAGED_PATH   "STAGED_AGENT.bin"
#define TARGET_NAME   "llm_agent"
#define MACBIN_HDR_SZ 128
#define COPY_CHUNK    8192
#define LOG_PATH      "UPDATER.LOG"

typedef struct {
    OSType type;
    OSType creator;
    unsigned long dataSize;
    unsigned long rsrcSize;
} MacBinInfo;

static void Log(const char *msg)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

static void LogErr(const char *prefix, OSErr err)
{
    char buf[128];
    sprintf(buf, "%s (err=%d)", prefix, (int)err);
    Log(buf);
}

static void CToPascal(const char *src, unsigned char *dst)
{
    int len = (int)strlen(src);
    if (len > 255) len = 255;
    dst[0] = (unsigned char)len;
    memcpy(dst + 1, src, len);
}

/* Offsets per hfsutils copyin.c/copyout.c: name length at [1], name
 * bytes from [2], type at [65], creator at [69], data-fork size (big-
 * endian long) at [83], resource-fork size (big-endian long) at [87].
 * We don't check the CRC at [124] -- this pipeline controls both the
 * encoder (hcopy -m on the host) and this decoder, delivered over a
 * TCP connection we've already round-trip-verified byte-for-byte
 * (PUT/GET readback), so the extra check isn't buying us protection
 * against a real failure mode here. */
static int ParseMacBinaryHeader(const unsigned char *hdr, MacBinInfo *info)
{
    int nameLen;
    char buf[96];

    if (hdr[0] != 0) {
        sprintf(buf, "parse: bad version byte hdr[0]=%d", hdr[0]);
        Log(buf);
        return 0;
    }
    nameLen = hdr[1];
    if (nameLen < 1 || nameLen > 63) {
        sprintf(buf, "parse: bad name length %d", nameLen);
        Log(buf);
        return 0;
    }

    memcpy(&info->type, &hdr[65], 4);
    memcpy(&info->creator, &hdr[69], 4);

    info->dataSize = ((unsigned long)hdr[83] << 24) | ((unsigned long)hdr[84] << 16) |
                      ((unsigned long)hdr[85] << 8)  |  (unsigned long)hdr[86];
    info->rsrcSize = ((unsigned long)hdr[87] << 24) | ((unsigned long)hdr[88] << 16) |
                      ((unsigned long)hdr[89] << 8)  |  (unsigned long)hdr[90];

    sprintf(buf, "parse: type=%.4s creator=%.4s dsize=%lu rsize=%lu",
            (char *)&info->type, (char *)&info->creator, info->dataSize, info->rsrcSize);
    Log(buf);
    return 1;
}

/* Copies `size` bytes from src into a data-fork FILE* (dstFile != NULL)
 * or a resource-fork ref num (dstFile == NULL), then advances src past
 * the MacBinary padding up to the next 128-byte boundary. */
static int CopyForkFromMacBinary(FILE *src, unsigned long size,
                                  FILE *dstFile, short dstRefNum)
{
    static unsigned char buf[COPY_CHUNK];
    unsigned long remaining = size;
    unsigned long padded = (size + 127UL) & ~127UL;
    unsigned long padRemaining;

    while (remaining > 0) {
        unsigned long want = (remaining > COPY_CHUNK) ? COPY_CHUNK : remaining;
        if (fread(buf, 1, want, src) != want) {
            Log("copyfork: fread (data) short");
            return 0;
        }
        if (dstFile != NULL) {
            if (fwrite(buf, 1, want, dstFile) != want) {
                Log("copyfork: fwrite short");
                return 0;
            }
        } else {
            long count = (long)want;
            OSErr err = FSWrite(dstRefNum, &count, buf);
            if (err != noErr || (unsigned long)count != want) {
                LogErr("copyfork: FSWrite failed", err);
                return 0;
            }
        }
        remaining -= want;
    }

    padRemaining = padded - size;
    while (padRemaining > 0) {
        unsigned long want = (padRemaining > COPY_CHUNK) ? COPY_CHUNK : padRemaining;
        if (fread(buf, 1, want, src) != want) {
            Log("copyfork: fread (pad) short");
            return 0;
        }
        padRemaining -= want;
    }

    return 1;
}

static int DecodeStagedUpdate(void)
{
    FILE *src;
    unsigned char hdr[MACBIN_HDR_SZ];
    MacBinInfo info;
    FSSpec spec;
    unsigned char pname[256];
    OSErr err;
    short rfRefNum;
    FILE *dataFile;
    int ok = 1;

    Log("decode: opening staged file");
    src = fopen(STAGED_PATH, "rb");
    if (!src) {
        Log("decode: fopen(STAGED_PATH) failed -- file missing or wrong directory");
        return 0;
    }

    if (fread(hdr, 1, MACBIN_HDR_SZ, src) != MACBIN_HDR_SZ) {
        Log("decode: short read on 128-byte header");
        fclose(src);
        return 0;
    }
    if (!ParseMacBinaryHeader(hdr, &info)) {
        fclose(src);
        return 0;
    }

    /* Replace any existing file at the target name via the File
     * Manager directly, not POSIX remove() -- a file decoded by
     * StuffIt Expander can come out with the Finder "locked" flag set,
     * which remove() can't delete through (confirmed via UPDATER.LOG:
     * remove() returned -1, then FSpCreate failed with dupFNErr since
     * the old file was still there). Clear any lock first, then
     * FSpDelete. Ignore fnfErr -- the target may not exist yet on a
     * first-ever update.
     *
     * The old llm_agent's own QUITAGENT handler stops accepting new
     * network connections essentially immediately (confirmed
     * separately), but that's apparently not the same moment its file
     * is fully released -- confirmed via UPDATER.LOG: FSpDelete came
     * back fBsyErr (-47, "file busy") on a run where QUITAGENT had
     * already been sent and acknowledged moments earlier.
     *
     * agent-win16's RESTART.EXE hit the exact same shape of problem
     * (old instance mid-teardown when the helper tries to touch its
     * file) and found that *actively polling* the old process's state
     * in a retry loop was itself the cause of intermittent crashes --
     * a different fault each time, always right around when the poll
     * loop queried old-task state while that task's own exit path was
     * mid-teardown. Its fix was a single flat, hands-off wait with zero
     * interaction with the old task's state, then one clean attempt.
     * Same fix here: don't retry FSpDelete in a loop (that's still
     * repeatedly touching the file the old process may still be
     * tearing down); wait once, quietly, then try exactly once.
     *
     * That wait can't be a raw Delay() though (confirmed live: froze
     * mouse clicks system-wide for the whole 10 seconds) -- Delay()
     * alone doesn't hand the CPU to other applications any more than
     * llm_agent's old SystemTask()-only loop did. Loop on
     * WaitNextEvent instead, discarding whatever it returns, exactly
     * like llm_agent's own Idle(). */
    {
        unsigned long startTicks = TickCount();
        Log("decode: waiting for old process to finish tearing down");
        while (TickCount() - startTicks < 600) { /* 10s at 60 ticks/sec */
            EventRecord event;
            WaitNextEvent(everyEvent, &event, 6, NULL);
        }
    }

    CToPascal(TARGET_NAME, pname);
    err = FSMakeFSSpec(0, 0, pname, &spec);
    LogErr("decode: FSMakeFSSpec (pre-delete)", err);
    if (err == noErr) {
        FSpRstFLock(&spec);
        err = FSpDelete(&spec);
        LogErr("decode: FSpDelete", err);
        if (err != noErr) {
            fclose(src);
            return 0;
        }
    } else if (err != fnfErr) {
        fclose(src);
        return 0;
    } else {
        Log("decode: target does not exist yet (first update)");
    }

    err = FSpCreate(&spec, info.creator, info.type, smSystemScript);
    LogErr("decode: FSpCreate", err);
    if (err != noErr) {
        fclose(src);
        return 0;
    }

    dataFile = fopen(TARGET_NAME, "wb");
    if (!dataFile) {
        Log("decode: fopen(TARGET_NAME, wb) failed");
        fclose(src);
        return 0;
    }
    ok = CopyForkFromMacBinary(src, info.dataSize, dataFile, 0);
    fclose(dataFile);
    if (!ok) {
        Log("decode: data fork copy failed");
        fclose(src);
        return 0;
    }
    Log("decode: data fork written");

    err = FSpOpenRF(&spec, fsWrPerm, &rfRefNum);
    LogErr("decode: FSpOpenRF", err);
    if (err != noErr) {
        fclose(src);
        return 0;
    }
    ok = CopyForkFromMacBinary(src, info.rsrcSize, NULL, rfRefNum);
    FSClose(rfRefNum);
    fclose(src);

    if (!ok) {
        Log("decode: resource fork copy failed");
        return 0;
    }
    Log("decode: resource fork written -- success");
    return 1;
}

static void LaunchTargetAndQuit(void)
{
    FSSpec spec;
    unsigned char pname[256];
    LaunchParamBlockRec pb;
    OSErr err;

    CToPascal(TARGET_NAME, pname);
    err = FSMakeFSSpec(0, 0, pname, &spec);
    LogErr("launch: FSMakeFSSpec", err);
    if (err != noErr) return;

    memset(&pb, 0, sizeof(pb));
    /* launchBlockID/launchEPBLength MUST be set to these exact values
     * (Inside Macintosh: Processes) so the Process Manager recognizes
     * this as a valid extended launch parameter block. Leaving them
     * zeroed (the earlier bug here) hard-locked the whole machine --
     * confirmed via UPDATER.LOG, which stopped mid-sequence right
     * before this call with no error ever logged. */
    pb.launchBlockID = extendedBlock;
    pb.launchEPBLength = extendedBlockLen;
    pb.launchAppSpec = &spec;
    pb.launchControlFlags = launchContinue;
    Log("launch: about to call LaunchApplication");
    err = LaunchApplication(&pb);
    LogErr("launch: LaunchApplication", err);
}

int main(void)
{
    InitGraf(&qd.thePort);

    Log("=== llm_updater starting ===");

    if (DecodeStagedUpdate()) {
        int r = remove(STAGED_PATH);
        char buf[64];
        sprintf(buf, "cleanup: remove(%s) returned %d", STAGED_PATH, r);
        Log(buf);
        LaunchTargetAndQuit();
        Log("=== llm_updater finished: success ===");
    } else {
        Log("=== llm_updater finished: DECODE FAILED, target not touched ===");
    }

    return 0;
}
