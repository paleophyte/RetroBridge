/* Recoverable NetWare replacement. Never UNLOAD: NetWare 3.12 can abend.
 * Backups survive success and rollback. An uncertain live module is untouched.
 */
#include <stdio.h>
#include <string.h>
#undef ferror
#include "nwsock.h"
#include "../common/update_identity.h"
#include "update_state.h"

static char old_hash[65], new_hash[65], archive_dir[80];

static void pause_ticks(int n) {
    while (n-- > 0) { delay(100); ThreadSwitchWithDelay(); }
}

static int update_log(const char *message) {
    FILE *f;
    unsigned n = (unsigned)strlen(message);
    int failed;
    ConsolePrintf("UPDATE: %s\r\n", message);
    f = fopen(NW_LOG, "ab");
    if (!f) return -1;
    failed = fwrite(message, 1, n, f) != n || fwrite("\n", 1, 1, f) != 1;
    if (fflush(f) != 0) failed = 1;
    if (fclose(f) != 0) failed = 1;
    return failed ? -1 : 0;
}

static int finish_update(const char *state, int result) {
    char destination[96], last[192];
    if (nw_write_text(NW_RESULT, state) != 0) {
        ConsolePrintf("UPDATE: result write failed; inspect " NW_WORK "\r\n");
        return 3;
    }
    sprintf(destination, "%s\\RESULT", archive_dir);
    sprintf(last, "%s %s %s", state, new_hash, destination);
    if (nw_write_text(NW_LAST, last) != 0) {
        ConsolePrintf("UPDATE: last-result record failed; inspect " NW_WORK "\r\n");
        return 3;
    }
    if (rename(NW_WORK, destination) != 0) {
        ConsolePrintf("UPDATE: archive rename failed; inspect " NW_WORK "\r\n");
        return 3;
    }
    ConsolePrintf("UPDATE: %s; recovery records: %s\r\n", state, destination);
    return result;
}

static int reserve_archive(void) {
    unsigned i;
    unsigned long value = (unsigned long)GetCurrentTicks() & 0xffffffUL;
    /* mkdir reserves ownership; no existing directory is reused or removed. */
    for (i = 0; i < 256; i++) {
        sprintf(archive_dir, NW_ARCHIVE "%06lx", (value + i) & 0xffffffUL);
        if (mkdir(archive_dir) == 0) return 0;
        if (nw_path_state(archive_dir) != 1) return -1;
    }
    return -1;
}

static int ready_for(const char *hash) {
    char receipt[128];
    int n = nw_read_text(NW_READY, receipt, sizeof(receipt));
    return n == 83 && !memcmp(receipt, hash, 64) && receipt[64] == '\n' &&
           receipt[82] == '\n' && FindNLMHandle("LLMAGENT.NLM") != 0;
}

static int wait_ready(const char *hash) {
    int i;
    unsigned long started = (unsigned long)GetCurrentTicks();
    for (i = 0; i < 60 && (((unsigned long)GetCurrentTicks() - started) & 0xffffffffUL) < 540UL; i++) {
        if (ready_for(hash)) return 1;
        pause_ticks(5);
    }
    return 0;
}

/* Verify a fresh restoration copy. OLD.NLM remains after rollback. */
static int prepare_restore(void) {
    FILE *in, *out;
    unsigned char bytes[1024];
    size_t n;
    char hash[65];
    int failed = 0;
    if (nw_path_state(NW_RESTORE) != 0 || nw_hash(NW_OLD, hash) || strcmp(hash, old_hash)) return -1;
    in = fopen(NW_OLD, "rb");
    if (!in) return -1;
    out = fopen(NW_RESTORE, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(bytes, 1, sizeof(bytes), in)) != 0) {
        if (fwrite(bytes, 1, n, out) != n) { failed = 1; break; }
    }
    if (ferror(in)) failed = 1;
    if (fclose(in) != 0) failed = 1;
    if (fflush(out) != 0) failed = 1;
    if (fclose(out) != 0) failed = 1;
    if (failed || nw_hash(NW_RESTORE, hash) || strcmp(hash, old_hash)) return -1;
    return 0;
}

static int rollback_update(int installed) {
    char hash[65];
    update_log("replacement failed; attempting rollback without UNLOAD");
    if (FindNLMHandle("LLMAGENT.NLM")) goto manual;
    if (prepare_restore() != 0) goto manual;
    if (installed) {
        if (nw_path_state(NW_BAD) != 0 || rename(NW_AGENT, NW_BAD) != 0) goto manual;
    }
    if (nw_path_state(NW_AGENT) != 0 || rename(NW_RESTORE, NW_AGENT) != 0) goto manual;
    if (nw_hash(NW_AGENT, hash) || strcmp(hash, old_hash)) goto manual;
    if (nw_path_state(NW_READY) != 0 && remove(NW_READY) != 0) goto manual;
    if (nw_write_text(NW_EXPECT, old_hash) != 0) goto manual;
    update_log("previous executable restored; requesting LOAD");
    system("LOAD SYS:SYSTEM\\LLMAGENT.NLM");
    if (wait_ready(old_hash)) return finish_update("rolled-back", 1);
manual:
    update_log("RECOVERY REQUIRED: keep " NW_WORK " and inspect PLAN.TXT/LOG.TXT; no automatic retry");
    nw_write_text(NW_RESULT, "recovery-required");
    return 3;
}

int main(void) {
    char plan[512], prepared[132], hash[65];
    int i;
    unsigned long started;
    ConsolePrintf("UPDATE: " NW_HELPER_TAG " preparing recoverable replacement\r\n");
    if (mkdir(NW_WORK) != 0) {
        ConsolePrintf("UPDATE: work directory unavailable or unresolved recovery; no files changed\r\n");
        return 3;
    }
    if (reserve_archive() != 0 || nw_hash(NW_AGENT, old_hash) || nw_hash(NW_STAGE, new_hash)) {
        update_log("preparation failed; installed file untouched; inspect work directory");
        return 3;
    }
    sprintf(plan, "target=" NW_AGENT "\nbackup=" NW_OLD "\narchive=%s\\RESULT\nold_sha256=%s\nnew_sha256=%s\n",
            archive_dir, old_hash, new_hash);
    sprintf(prepared, "%s\n%s\n", old_hash, new_hash);
    if (nw_write_text(NW_PLAN, plan) || nw_write_text(NW_EXPECT, new_hash) ||
        update_log("prepared; waiting for outgoing module to exit") || nw_write_text(NW_PREPARED, prepared)) return 3;

    started = (unsigned long)GetCurrentTicks();
    for (i = 0; i < 60 && FindNLMHandle("LLMAGENT.NLM") &&
         (((unsigned long)GetCurrentTicks() - started) & 0xffffffffUL) < 540UL; i++) pause_ticks(5);
    if (FindNLMHandle("LLMAGENT.NLM")) return finish_update("aborted-old-agent-still-loaded", 2);
    if (nw_hash(NW_AGENT, hash) || strcmp(hash, old_hash)) {
        update_log("installed input changed or unreadable; recovery required, no swap");
        nw_write_text(NW_RESULT, "recovery-required");
        return 3;
    }
    if (nw_hash(NW_STAGE, hash) || strcmp(hash, new_hash)) {
        if (nw_write_text(NW_EXPECT, old_hash) != 0) return 3;
        system("LOAD SYS:SYSTEM\\LLMAGENT.NLM");
        return finish_update("aborted-stage-changed-or-unreadable", 2);
    }
    if (update_log("moving installed executable to retained backup") || rename(NW_AGENT, NW_OLD) != 0) {
        system("LOAD SYS:SYSTEM\\LLMAGENT.NLM");
        return finish_update("aborted-before-swap", 2);
    }
    if (nw_hash(NW_OLD, hash) || strcmp(hash, old_hash)) return rollback_update(0);
    if (rename(NW_STAGE, NW_AGENT) != 0) return rollback_update(0);
    if (nw_hash(NW_AGENT, hash) || strcmp(hash, new_hash)) return rollback_update(1);
    if (update_log("replacement installed; requesting LOAD") != 0) return rollback_update(1);
    system("LOAD SYS:SYSTEM\\LLMAGENT.NLM");
    if (wait_ready(new_hash)) return finish_update("installed", 0);
    return rollback_update(1);
}
