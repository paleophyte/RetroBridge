/* llm_updater for classic Mac OS (System 7.x).
 *
 * Companion to llm_agent. After a cooperative teardown grace period,
 * decode and read back a separate two-fork application, then exchange it
 * with the installed file. Retain the previous application and exchange
 * it back if launch fails. Never delete the installed application.
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

#include "app_files.h"

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
    MacAppendLog(LOG_PATH, msg);
}

static void LogErr(const char *prefix, OSErr err)
{
    char buf[128];
    sprintf(buf, "%s (err=%d)", prefix, (int)err);
    Log(buf);
}

/* This decoder accepts the classic 128-byte MacBinary layout used by
 * Retro68/hcopy, with no secondary header. Check all lengths before creating
 * a file; the header's filename is never used as a destination. */
static int ParseMacBinaryHeader(const unsigned char *hdr, MacBinInfo *info)
{
    unsigned long dataPadded, rsrcPadded;
    if (hdr[0] != 0 || hdr[1] < 1 || hdr[1] > 63 ||
        hdr[74] != 0 || hdr[82] != 0 || hdr[120] != 0 || hdr[121] != 0 ||
        memcmp(hdr + 65, "APPL", 4) != 0) {
        Log("parse: unsupported MacBinary application header");
        return 0;
    }
    memcpy(&info->type, hdr + 65, 4);
    memcpy(&info->creator, hdr + 69, 4);
    info->dataSize = ((unsigned long)hdr[83] << 24) | ((unsigned long)hdr[84] << 16) |
                    ((unsigned long)hdr[85] << 8) | hdr[86];
    info->rsrcSize = ((unsigned long)hdr[87] << 24) | ((unsigned long)hdr[88] << 16) |
                    ((unsigned long)hdr[89] << 8) | hdr[90];
    if (info->dataSize > 0x7FFFFF00UL || info->rsrcSize > 0x7FFFFF00UL ||
        info->rsrcSize < 256) return 0;
    dataPadded = (info->dataSize + 127UL) & ~127UL;
    rsrcPadded = (info->rsrcSize + 127UL) & ~127UL;
    return dataPadded <= 0x7FFFFFFFUL - MACBIN_HDR_SZ - rsrcPadded;
}

static OSErr NamedSpec(const char *name, FSSpec *spec)
{
    return MacApplicationFile(name, spec);
}

/* Never reuse or delete a recovery copy from an earlier attempt. Before
 * exchange this is a candidate; afterward it contains the previous agent. */
static int ReserveCandidate(FSSpec *spec, MacBinInfo *info)
{
    int i;
    char name[32], message[80];
    OSErr err;
    for (i = 1; i <= 99; i++) {
        sprintf(name, "llm_agent.saved.%03d", i);
        err = NamedSpec(name, spec);
        if (err == noErr) continue;
        if (err != fnfErr) { LogErr("reserve: lookup failed", err); return 0; }
        sprintf(message, "reserve: candidate/recovery file %s", name);
        Log(message);
        err = FSpCreate(spec, info->creator, info->type, smSystemScript);
        LogErr("reserve: FSpCreate", err);
        return err == noErr;
    }
    Log("reserve: all 99 recovery slots exist; remove obsolete copies manually");
    return 0;
}

static int ReadExact(short ref, void *buf, long want)
{
    long count = want;
    OSErr err = FSRead(ref, &count, buf);
    if (err != noErr || count != want) { LogErr("read: short/failed FSRead", err); return 0; }
    return 1;
}

/* Use File Manager calls for both forks, including checked close and a
 * volume flush. The stdio remove shim does not work on the tested runtime. */
static int CopyFork(short src, unsigned long size, FSSpec *spec, int resource)
{
    static unsigned char buf[COPY_CHUNK];
    unsigned long remaining = size;
    short ref;
    int ok = 1;
    OSErr err = resource ? FSpOpenRF(spec, fsWrPerm, &ref) : FSpOpenDF(spec, fsWrPerm, &ref);
    if (err != noErr) { LogErr("copy: open fork", err); return 0; }
    while (remaining && ok) {
        long count = remaining > COPY_CHUNK ? COPY_CHUNK : (long)remaining;
        long want = count;
        if (!ReadExact(src, buf, want)) { ok = 0; break; }
        err = FSWrite(ref, &count, buf);
        if (err != noErr || count != want) { LogErr("copy: short/failed FSWrite", err); ok = 0; }
        remaining -= (unsigned long)want;
    }
    err = FSClose(ref);
    if (err != noErr) { LogErr("copy: FSClose", err); ok = 0; }
    if (ok && SetFPos(src, fsFromMark, (long)((128UL - (size & 127UL)) & 127UL)) != noErr) ok = 0;
    return ok;
}

static unsigned long ReadBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static int ResourceHeaderValid(const unsigned char *p, unsigned long size)
{
    unsigned long data = ReadBE32(p), map = ReadBE32(p + 4);
    unsigned long dataLen = ReadBE32(p + 8), mapLen = ReadBE32(p + 12);
    if (data < 256 || map < 256 || data > size || map > size ||
        dataLen > size - data || mapLen > size - map || mapLen < 28) return 0;
    return data + dataLen <= map || map + mapLen <= data;
}

/* HFS writes catalog recovery information into the resource header's
 * system-reserved bytes [16,128) when flushing/closing the file. Compare
 * all other bytes, including the application-reserved area and resource map.
 * Only allow this exception after validating a standard resource header. */
static long ForkDifference(const unsigned char *a, const unsigned char *b,
                           long n, unsigned long offset, int resource)
{
    long i;
    for (i = 0; i < n; i++) {
        unsigned long pos = offset + (unsigned long)i;
        if (resource && pos >= 16 && pos < 128) continue;
        if (a[i] != b[i]) return i;
    }
    return -1;
}

/* Read both forks back before touching the installed application. */
static int VerifyFork(short src, unsigned long size, FSSpec *spec, int resource)
{
    static unsigned char expected[COPY_CHUNK], actual[COPY_CHUNK];
    unsigned long remaining = size;
    short ref;
    long length = 0;
    int ok;
    OSErr err = resource ? FSpOpenRF(spec, fsRdPerm, &ref) : FSpOpenDF(spec, fsRdPerm, &ref);
    if (err != noErr) { LogErr("verify: open fork", err); return 0; }
    err = GetEOF(ref, &length);
    ok = err == noErr && length == (long)size;
    if (!ok) {
        char msg[128];
        sprintf(msg, "verify: fork=%d length=%ld expected=%lu err=%d", resource, length, size, (int)err);
        Log(msg);
    }
    while (remaining && ok) {
        long count = remaining > COPY_CHUNK ? COPY_CHUNK : (long)remaining;
        long want = count;
        if (!ReadExact(src, expected, want)) { ok = 0; break; }
        if (resource && remaining == size && !ResourceHeaderValid(expected, size)) {
            Log("verify: invalid resource header"); ok = 0; break;
        }
        err = FSRead(ref, &count, actual);
        if (err != noErr || count != want || ForkDifference(expected, actual, want, size - remaining, resource) >= 0) {
            char msg[128];
            sprintf(msg, "verify: fork=%d offset=%lu count=%ld expected=%ld err=%d mismatch=%d",
                    resource, size - remaining, count, want, (int)err,
                    err == noErr && count == want ? ForkDifference(expected, actual, want, size - remaining, resource) >= 0 : -1);
            Log(msg);
            if (err == noErr && count == want) {
                long i;
                for (i = 0; i < want; i++) if (ForkDifference(expected + i, actual + i, 1, size - remaining + (unsigned long)i, resource) >= 0) {
                    sprintf(msg, "verify: first difference at fork byte %lu", size - remaining + (unsigned long)i);
                    Log(msg);
                    break;
                }
            }
            ok = 0;
        }
        remaining -= (unsigned long)want;
    }
    err = FSClose(ref);
    if (err != noErr) { LogErr("verify: FSClose", err); ok = 0; }
    if (ok && SetFPos(src, fsFromMark, (long)((128UL - (size & 127UL)) & 127UL)) != noErr) ok = 0;
    return ok;
}

static int PrepareCandidate(FSSpec *spec)
{
    short src;
    FSSpec staged;
    unsigned char hdr[MACBIN_HDR_SZ];
    MacBinInfo info;
    long length;
    unsigned long expected;
    int ok = 0, created = 0;
    OSErr err;
    if (NamedSpec(STAGED_PATH, &staged) != noErr || FSpOpenDF(&staged, fsRdPerm, &src) != noErr) {
        Log("prepare: cannot open staging file"); return 0;
    }
    if (!ReadExact(src, hdr, sizeof(hdr)) || !ParseMacBinaryHeader(hdr, &info)) goto done;
    expected = MACBIN_HDR_SZ + ((info.dataSize + 127UL) & ~127UL) + ((info.rsrcSize + 127UL) & ~127UL);
    if (GetEOF(src, &length) != noErr) goto done;
    if (length < 0 || (unsigned long)length != expected) { Log("prepare: staged length mismatch"); goto done; }
    if (SetFPos(src, fsFromStart, MACBIN_HDR_SZ) != noErr) goto done;
    if (!ReserveCandidate(spec, &info)) goto done;
    created = 1;
    if (!CopyFork(src, info.dataSize, spec, 0) || !CopyFork(src, info.rsrcSize, spec, 1)) { Log("prepare: fork copy failed"); goto done; }
    err = FlushVol(NULL, spec->vRefNum);
    if (err != noErr) { LogErr("prepare: FlushVol", err); goto done; }
    if (SetFPos(src, fsFromStart, MACBIN_HDR_SZ) != noErr ||
        !VerifyFork(src, info.dataSize, spec, 0) || !VerifyFork(src, info.rsrcSize, spec, 1)) { Log("prepare: fork readback failed"); goto done; }
    ok = 1;
done:
    err = FSClose(src);
    if (err != noErr) { LogErr("prepare: close staging file", err); ok = 0; }
    if (!ok && created) LogErr("prepare: remove incomplete candidate", FSpDelete(spec));
    if (ok) Log("prepare: both forks closed, flushed and verified");
    return ok;
}

static OSErr Launch(FSSpec *spec)
{
    LaunchParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof(pb));
    pb.launchBlockID = extendedBlock;
    pb.launchEPBLength = extendedBlockLen;
    pb.launchAppSpec = spec;
    pb.launchControlFlags = launchContinue;
    err = LaunchApplication(&pb);
    LogErr("launch: LaunchApplication", err);
    return err;
}

/* Exchange keeps the installed file's ID (and Startup Items aliases) valid.
 * It swaps both forks in the catalog; there is no delete-before-create gap.
 * If exchange is unsupported, retain both files and leave the target alone.
 * See Inside Macintosh: Files, File Manager, FSpExchangeFiles. */
static int InstallUpdate(void)
{
    FSSpec target, saved, stage;
    OSErr err;
    if (NamedSpec(TARGET_NAME, &target) != noErr) {
        Log("update: installed target missing; refusing replacement");
        return 0;
    }
    if (!PrepareCandidate(&saved)) {
        Log("update: preparation failed; installed application preserved");
        Launch(&target);
        return 0;
    }
    err = FSpExchangeFiles(&target, &saved);
    LogErr("install: exchange", err);
    if (err != noErr) {
        Log("update: exchange failed; retaining both files for recovery");
        Launch(&target);
        return 0;
    }
    err = FlushVol(NULL, target.vRefNum);
    if (err != noErr) LogErr("install: FlushVol", err);
    if (err == noErr) err = Launch(&target);
    if (err != noErr) {
        OSErr restored = FSpExchangeFiles(&target, &saved);
        LogErr("rollback: exchange", restored);
        LogErr("rollback: FlushVol", FlushVol(NULL, target.vRefNum));
        if (restored == noErr) {
            Log("update: previous application restored; retrying its launch");
            Launch(&target);
        } else {
            Log("update: ROLLBACK FAILED; previous application retained in saved file; trying it directly");
            Launch(&saved);
        }
        return 0;
    }
    /* Only remove the staging blob after a successful launch. Keep the
     * previous application in its numbered saved file for manual recovery. */
    err = NamedSpec(STAGED_PATH, &stage);
    if (err == noErr) err = FSpDelete(&stage);
    LogErr("cleanup: staged file", err);
    Log("update: replacement launched; previous application retained");
    return 1;
}

int main(void)
{
    unsigned long startTicks;
    InitGraf(&qd.thePort);
    if (MacLocateApplication() != noErr) return 1;
    if (MacSelectApplicationDirectory() != noErr) {
        Log("startup: cannot select updater folder");
        return 1;
    }
    Log("=== llm_updater starting ===");
    /* Retain the proven cooperative teardown grace period. Do not poll or
     * manipulate the old agent's process or files during its cleanup. */
    startTicks = TickCount();
    while (TickCount() - startTicks < 600) {
        EventRecord event;
        WaitNextEvent(everyEvent, &event, 6, NULL);
    }
    if (!InstallUpdate()) {
        Log("=== llm_updater finished: UPDATE FAILED (see recovery results above) ===");
        return 1;
    }
    Log("=== llm_updater finished: launch accepted ===");
    return 0;
}
