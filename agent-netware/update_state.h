/* Shared recovery protocol. CLIB stdio, never Watcom FILE-layout macros. */
#ifndef NW_UPDATE_STATE_H
#define NW_UPDATE_STATE_H
#include <errno.h>

#ifndef NW_AGENT
#define NW_AGENT "SYS:SYSTEM\\LLMAGENT.NLM"
#define NW_STAGE "SYS:SYSTEM\\LLMAGENT.NEW"
#define NW_WORK "SYS:SYSTEM\\LLMUPD"
#define NW_ARCHIVE "SYS:SYSTEM\\LU"
#endif
#define NW_OLD NW_WORK "\\OLD.NLM"
#define NW_BAD NW_WORK "\\BAD.NLM"
#define NW_RESTORE NW_WORK "\\RESTORE.NLM"
#define NW_PLAN NW_WORK "\\PLAN.TXT"
#define NW_PREPARED NW_WORK "\\PREPARED.TXT"
#define NW_EXPECT NW_WORK "\\EXPECT.TXT"
#define NW_READY NW_WORK "\\READY.TXT"
#define NW_RESULT NW_WORK "\\RESULT.TXT"
#define NW_LOG NW_WORK "\\LOG.TXT"
#define NW_LAST "SYS:SYSTEM\\LLMUPD.RES"
#define NW_HELPER_TAG "RETRO_NW_UPDATE_PROTOCOL_2"

/* Compatibility marker, not a signature. Old helpers must not be launched
   while the new agent waits for preparation: they have no such handshake. */
static int nw_helper_supported(void) {
    FILE *f = fopen("SYS:SYSTEM\\UPDATE.NLM", "rb");
    unsigned char block[512];
    unsigned matched = 0, i, n;
    unsigned long total = 0;
    int found = 0, failed;
    if (!f) return 0;
    while (!found && (n = (unsigned)fread(block, 1, sizeof(block), f)) != 0) {
        total += n;
        if (total > 64UL * 1024UL * 1024UL) break;
        for (i = 0; i < n; i++) {
            if (block[i] == NW_HELPER_TAG[matched]) ++matched;
            else matched = block[i] == NW_HELPER_TAG[0] ? 1 : 0;
            if (matched == sizeof(NW_HELPER_TAG) - 1) { found = 1; break; }
        }
    }
    failed = ferror(f);
    if (fclose(f) != 0 || failed) return 0;
    return found;
}

static int nw_path_state(const char *path) {
    if (access(path, 0) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static int nw_read_text(const char *path, char *out, unsigned capacity) {
    FILE *f = fopen(path, "rb");
    unsigned n;
    int failed;
    if (!f) return -1;
    n = (unsigned)fread(out, 1, capacity, f);
    failed = ferror(f);
    if (fclose(f) != 0 || failed || n >= capacity) return -1;
    out[n] = 0;
    if (strlen(out) != n) return -1;
    return (int)n;
}

static int nw_write_text(const char *path, const char *text) {
    char check[512];
    unsigned n = (unsigned)strlen(text);
    FILE *f;
    int failed;
    if (n >= sizeof(check)) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    failed = fwrite(text, 1, n, f) != n;
    if (fflush(f) != 0) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed || nw_read_text(path, check, sizeof(check)) != (int)n || strcmp(text, check)) return -1;
    return 0;
}

static int nw_hash(const char *path, char *hex) {
    FILE *f = fopen(path, "rb");
    long size;
    int failed;
    hex[0] = 0;
    if (!f) return -1;
    failed = fseek(f, 0, SEEK_END) != 0;
    size = ftell(f);
    if (fclose(f) != 0 || failed || size <= 0 || size > 64L * 1024L * 1024L) return -1;
    return update_file_sha256(path, hex);
}

static int nw_is_target(const char *path) {
    const char *expected = NW_AGENT;
    unsigned char a, b;
    while (*path && *expected) {
        a = (unsigned char)*path++; b = (unsigned char)*expected++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (a == '/') a = '\\';
        if (a != b) return 0;
        if (a == ':' && (*path == '\\' || *path == '/')) ++path;
    }
    return !*path && !*expected;
}

/* A fresh exclusive work directory and checked EXPECT prevent stale success. */
static void nw_publish_ready(void) {
    char expected[80], receipt[128];
    if (!nw_is_target(g_update_exe) || strlen(g_update_sha256) != 64 ||
        !g_update_started[0] || nw_read_text(NW_EXPECT, expected, sizeof(expected)) != 64 ||
        strcmp(expected, g_update_sha256)) return;
    sprintf(receipt, "%s\n%s\n", g_update_sha256, g_update_started);
    if (nw_write_text(NW_READY, receipt) != 0)
        ConsolePrintf("LLMAGENT: update readiness receipt failed; recovery files retained\r\n");
}
#endif
