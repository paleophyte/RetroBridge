/* Fingerprint the Process Manager's application file, including both forks.
 * Startup values never change. Disk values are recomputed for verification;
 * neither is a memory-image signature or protection against a hostile guest.
 */
#ifndef MAC_UPDATE_IDENTITY_H
#define MAC_UPDATE_IDENTITY_H
#include "../common/update_sha256.h"

static char gMacStarted[48], gMacLocation[160];
static char gMacDataSHA[65], gMacResourceSHA[65];

static unsigned long MacIdentityBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static int MacForkSHA(FSSpec *spec, int resource, char *hex)
{
    UpdateSHA256 state;
    unsigned char block[1024];
    short ref;
    long size, remaining, count, after;
    OSErr err;
    int failed = 0;
    hex[0] = 0;
    err = resource ? FSpOpenRF(spec, fsRdPerm, &ref) : FSpOpenDF(spec, fsRdPerm, &ref);
    if (err != noErr) return -1;
    err = GetEOF(ref, &size);
    if (err != noErr || size < 0 || size > 64L * 1024L * 1024L ||
        (resource && size < 256)) failed = 1;
    update_sha_init(&state);
    remaining = failed ? 0 : size;
    while (remaining > 0) {
        long want = remaining > (long)sizeof(block) ? (long)sizeof(block) : remaining;
        count = want;
        err = FSRead(ref, &count, block);
        if (err != noErr || count != want) {
            failed = 1;
            break;
        }
        if (resource && remaining == size) {
            unsigned long data = MacIdentityBE32(block), map = MacIdentityBE32(block + 4);
            unsigned long dataSize = MacIdentityBE32(block + 8), mapSize = MacIdentityBE32(block + 12);
            if (data < 256 || map < 256 || data > (unsigned long)size || map > (unsigned long)size ||
                dataSize > (unsigned long)size - data || mapSize > (unsigned long)size - map) {
                failed = 1;
                break;
            }
            /* Inside Macintosh I, Resource File Format: bytes 16..127 are
               system-owned directory metadata, changed by System 7's swap.
               Preserve the header, application area, map, and all resources. */
            memset(block + 16, 0, 112);
        }
        if (update_sha_add(&state, block, (unsigned)count)) { failed = 1; break; }
        remaining -= count;
    }
    if (GetEOF(ref, &after) != noErr || (!failed && after != size)) failed = 1;
    if (FSClose(ref) != noErr) failed = 1;
    if (failed) return -1;
    update_sha_finish(&state, hex);
    return 0;
}

static void MacUpdateIdentityInit(void)
{
    ProcessSerialNumber psn;
    unsigned i, offset;
    static const char digits[] = "0123456789abcdef";
    if (!gApplicationLocated || GetCurrentProcess(&psn) != noErr) return;
    update_hex32(gMacStarted, TickCount());
    gMacStarted[8] = '-';
    update_hex32(gMacStarted + 9, psn.highLongOfPSN);
    gMacStarted[17] = '-';
    update_hex32(gMacStarted + 18, psn.lowLongOfPSN);
    /* Opaque location, not a GET path: volume ref, parent ID, hex HFS name.
       Encoding the name avoids injecting line delimiters into SYSINFO. */
    update_hex32(gMacLocation, (unsigned long)(unsigned short)gApplicationSpec.vRefNum);
    gMacLocation[8] = ':';
    update_hex32(gMacLocation + 9, (unsigned long)gApplicationSpec.parID);
    gMacLocation[17] = ':';
    offset = 18;
    for (i = 1; i <= gApplicationSpec.name[0]; i++) {
        unsigned char c = gApplicationSpec.name[i];
        gMacLocation[offset++] = digits[c >> 4];
        gMacLocation[offset++] = digits[c & 15];
    }
    gMacLocation[offset] = 0;
    MacForkSHA(&gApplicationSpec, 0, gMacDataSHA);
    MacForkSHA(&gApplicationSpec, 1, gMacResourceSHA);
}
#endif
