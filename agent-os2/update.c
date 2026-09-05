/*
 * UPDATE.EXE (OS/2): replace a running llm_agent with a new binary.
 *
 * Same role as agent/update.c on Windows. Launched via EXECDETACH so it
 * outlives the agent it is about to stop.
 *
 * Usage: UPDATE.EXE <new-exe-path> <target-exe-path>
 * Both paths must be absolute. Reads AGENT.PID next to the target (written
 * by llm_agent on listen), DosKillProcess that PID, swaps files with
 * rollback, then DosStartSession the new target.
 *
 * Logs to UPDATE.LOG next to the target.
 */

#define INCL_DOS
#define INCL_DOSFILEMGR
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_logPath[260];

static void set_log_path(const char *targetPath) {
    char *slash;
    strncpy(g_logPath, targetPath, sizeof(g_logPath) - 12);
    g_logPath[sizeof(g_logPath) - 12] = '\0';
    slash = strrchr(g_logPath, '\\');
    if (slash) *(slash + 1) = '\0';
    else g_logPath[0] = '\0';
    strcat(g_logPath, "UPDATE.LOG");
}

static void log_line(const char *text) {
    FILE *f;
    if (!g_logPath[0]) return;
    f = fopen(g_logPath, "a");
    if (!f) return;
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
}

static void log_rc(const char *prefix, APIRET rc) {
    char buf[128];
    sprintf(buf, "%s (rc=%lu)", prefix, (unsigned long)rc);
    log_line(buf);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void dirname_of(const char *path, char *out, int outlen) {
    int last = -1;
    int i;
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\' || path[i] == '/') last = i;
    }
    if (last < 0) {
        strncpy(out, ".", outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    if (last >= outlen - 1) last = outlen - 2;
    memcpy(out, path, last);
    out[last] = '\0';
}

static PID read_agent_pid(const char *targetPath) {
    char dir[260];
    char path[280];
    FILE *f;
    unsigned long pid = 0;

    dirname_of(targetPath, dir, sizeof(dir));
    sprintf(path, "%s\\AGENT.PID", dir);
    f = fopen(path, "r");
    if (!f) {
        log_line("stop: AGENT.PID not found");
        return 0;
    }
    if (fscanf(f, "%lu", &pid) != 1) pid = 0;
    fclose(f);
    return (PID)pid;
}

static int stop_agent(const char *targetPath) {
    PID pid = read_agent_pid(targetPath);
    APIRET rc;
    char buf[80];

    if (!pid) {
        log_line("stop: no PID — will rely on file-unlock retries");
        return 0;
    }
    sprintf(buf, "stop: DosKillProcess pid=%lu", (unsigned long)pid);
    log_line(buf);
    rc = DosKillProcess(DKP_PROCESS, pid);
    if (rc != 0) {
        log_rc("stop: DosKillProcess failed", rc);
        return 0;
    }
    log_line("stop: kill requested");
    return 1;
}

static int replace_file(const char *newPath, const char *targetPath) {
    char backupPath[280];
    int i;
    int renamedOld = 0;
    char buf[80];

    if (strlen(targetPath) + 5 >= sizeof(backupPath)) {
        log_line("replace: target path too long");
        return 0;
    }
    sprintf(backupPath, "%s.OLD", targetPath);
    remove(backupPath);

    for (i = 0; i < 40; i++) { /* up to ~8s */
        if (!file_exists(targetPath)) {
            log_line("replace: target missing — nothing to back up");
            break;
        }
        if (rename(targetPath, backupPath) == 0) {
            renamedOld = 1;
            break;
        }
        DosSleep(200);
    }
    sprintf(buf, "replace: renamedOld=%d after %d attempt(s)", renamedOld, i);
    log_line(buf);

    if (rename(newPath, targetPath) != 0) {
        log_line("replace: move-new-into-place failed");
        if (renamedOld) {
            if (rename(backupPath, targetPath) == 0)
                log_line("replace: rollback restored old binary");
            else
                log_line("replace: rollback FAILED");
        }
        return 0;
    }
    log_line("replace: new binary is in place");
    if (renamedOld) remove(backupPath);
    return 1;
}

static int start_agent(const char *targetPath) {
    STARTDATA sd;
    ULONG sess_id = 0;
    PID pid = 0;
    APIRET rc;
    char inputs[2];
    char obj[260];
    char buf[80];

    inputs[0] = ' ';
    inputs[1] = '\0';

    memset(&sd, 0, sizeof(sd));
    sd.Length = sizeof(sd);
    sd.Related = SSF_RELATED_INDEPENDENT;
    sd.FgBg = SSF_FGBG_BACK;
    sd.PgmTitle = (PSZ)"llm_agent";
    sd.PgmName = (PSZ)targetPath;
    sd.PgmInputs = (PBYTE)inputs;
    sd.InheritOpt = SSF_INHERTOPT_SHELL;
    sd.SessionType = SSF_TYPE_WINDOWABLEVIO;
    sd.PgmControl = SSF_CONTROL_MINIMIZE;
    sd.ObjectBuffer = obj;
    sd.ObjectBuffLen = sizeof(obj);

    rc = DosStartSession(&sd, &sess_id, &pid);
    if (rc != 0) {
        log_rc("start: DosStartSession failed", rc);
        return 0;
    }
    sprintf(buf, "start: session pid=%lu", (unsigned long)pid);
    log_line(buf);
    return 1;
}

int main(int argc, char **argv) {
    const char *newPath;
    const char *targetPath;
    int replaced;
    int started;

    if (argc < 3) {
        printf("usage: UPDATE.EXE <new-exe> <target-exe>\n");
        return 1;
    }
    newPath = argv[1];
    targetPath = argv[2];

    set_log_path(targetPath);
    log_line("=== UPDATE.EXE starting ===");

    /* Let EXECDETACH's TCP connection finish cleanly first. */
    DosSleep(750);

    stop_agent(targetPath);
    DosSleep(500);

    replaced = replace_file(newPath, targetPath);
    started = start_agent(targetPath);

    if (!started) {
        log_line("=== UPDATE.EXE finished: FAILED TO START ===");
        return 1;
    }
    if (!replaced) {
        log_line("=== UPDATE.EXE finished: restarted old binary, swap failed ===");
        return 1;
    }
    log_line("=== UPDATE.EXE finished: success ===");
    return 0;
}
