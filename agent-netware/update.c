/*
 * UPDATE.NLM — remote swap helper for LLMAGENT.
 *
 * Expected files on SYS:SYSTEM:
 *   LLMAGENT.NEW  — replacement agent NLM
 *   UPDATE.NLM    — this module
 *
 * Flow (agent command UPDATE):
 *   Agent closes sockets and exit()s itself, then we rename + LOAD.
 *
 * NEVER call UNLOAD LLMAGENT — NetWare 3.12 has repeatedly GPF'd in the
 * Console Command Process when unloading this module. Wait until the
 * .NLM file is writable instead; abort if it stays locked.
 */
#include <stdio.h>
#include <string.h>
#include "nwsock.h"

#define AGENT_NLM  "SYS:SYSTEM\\LLMAGENT.NLM"
#define AGENT_NEW  "SYS:SYSTEM\\LLMAGENT.NEW"
#define AGENT_OLD  "SYS:SYSTEM\\LLMAGENT.OLD"

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int file_writable(const char *path) {
    FILE *f = fopen(path, "rb+");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void pause_ticks(int n) {
    int i;
    for (i = 0; i < n; i++) {
        delay(100);
        ThreadSwitchWithDelay();
    }
}

int main(void) {
    int tries;

    ConsolePrintf("UPDATE: starting LLMAGENT swap\r\n");

    if (!file_exists(AGENT_NEW)) {
        ConsolePrintf("UPDATE: missing LLMAGENT.NEW — abort\r\n");
        return 1;
    }

    /* Let the agent finish exit() / TCP teardown. */
    pause_ticks(30);

    /* Wait for old NLM to unlock — do NOT UNLOAD (3.12 GPF). */
    for (tries = 0; tries < 100; tries++) {
        if (!file_exists(AGENT_NLM) || file_writable(AGENT_NLM))
            break;
        if ((tries % 10) == 0)
            ConsolePrintf("UPDATE: waiting for LLMAGENT.NLM unlock...\r\n");
        pause_ticks(5);
    }
    if (file_exists(AGENT_NLM) && !file_writable(AGENT_NLM)) {
        ConsolePrintf("UPDATE: LLMAGENT.NLM still locked — abort (no UNLOAD)\r\n");
        return 3;
    }

    remove(AGENT_OLD);
    if (file_exists(AGENT_NLM)) {
        if (rename(AGENT_NLM, AGENT_OLD) != 0)
            ConsolePrintf("UPDATE: warn — could not rename to .OLD\r\n");
        else
            ConsolePrintf("UPDATE: renamed .NLM -> .OLD\r\n");
    }

    if (rename(AGENT_NEW, AGENT_NLM) != 0) {
        ConsolePrintf("UPDATE: rename .NEW -> .NLM failed — try restore\r\n");
        rename(AGENT_OLD, AGENT_NLM);
        system("LOAD LLMAGENT");
        return 2;
    }
    ConsolePrintf("UPDATE: renamed .NEW -> .NLM\r\n");

    ConsolePrintf("UPDATE: LOAD LLMAGENT\r\n");
    system("LOAD LLMAGENT");

    ConsolePrintf("UPDATE: done\r\n");
    return 0;
}
