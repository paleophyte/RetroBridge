/*
 * REBOOT.EXE (OS/2): hardware reboot helper (detached from llm_agent).
 *
 * Usage:
 *   REBOOT.EXE            — reboot (OEMHLP/DOS$ IOCTL, then kbd-controller .COM)
 *
 * Shutdown is not supported via this helper on OS/2 2.11 (see agent README).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INCL_DOS
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#include <os2.h>

static char g_dir[140];
static char g_logPath[160];

static void log_line(const char *line) {
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void set_paths(const char *argv0) {
    char *slash;
    strcpy(g_dir, "C:\\llmagent");
    strcpy(g_logPath, "C:\\llmagent\\REBOOT.LOG");
    if (!argv0 || !argv0[0]) return;
    strncpy(g_dir, argv0, sizeof(g_dir) - 1);
    g_dir[sizeof(g_dir) - 1] = '\0';
    slash = strrchr(g_dir, '\\');
    if (!slash) slash = strrchr(g_dir, '/');
    if (slash) {
        *slash = '\0';
        sprintf(g_logPath, "%s\\REBOOT.LOG", g_dir);
    }
}

static APIRET ioctl_dev(HFILE hf, ULONG cat, ULONG fn, const char *tag) {
    ULONG plen = 0, dlen = 0;
    APIRET rc;
    char msg[96];

    log_line(tag);
    rc = DosDevIOCtl(hf, cat, fn, NULL, 0, &plen, NULL, 0, &dlen);
    sprintf(msg, "  DosDevIOCtl rc=%lu", (unsigned long)rc);
    log_line(msg);
    return rc;
}

static APIRET try_oemhlp_reboot(void) {
    HFILE hf = NULLHANDLE;
    ULONG action = 0;
    APIRET rc;
    char msg[80];

    rc = DosOpen("OEMHLP$", &hf, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READONLY | OPEN_SHARE_DENYNONE | OPEN_FLAGS_FAIL_ON_ERROR,
                 NULL);
    sprintf(msg, "DosOpen OEMHLP$ rc=%lu", (unsigned long)rc);
    log_line(msg);
    if (rc != 0) return rc;
    rc = ioctl_dev(hf, 0x80, 0x5D, "OEMHLP$ IOCTL 80/5D");
    DosClose(hf);
    return rc;
}

static APIRET try_dos_reboot(void) {
    HFILE hf = NULLHANDLE;
    ULONG action = 0;
    APIRET rc;
    char msg[80];

    rc = DosOpen("DOS$", &hf, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE | OPEN_FLAGS_FAIL_ON_ERROR,
                 NULL);
    if (rc != 0) {
        rc = DosOpen("\\DEV\\DOS$", &hf, &action, 0, FILE_NORMAL,
                     OPEN_ACTION_OPEN_IF_EXISTS,
                     OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE | OPEN_FLAGS_FAIL_ON_ERROR,
                     NULL);
    }
    sprintf(msg, "DosOpen DOS$ rc=%lu", (unsigned long)rc);
    log_line(msg);
    if (rc != 0) return rc;
    rc = ioctl_dev(hf, 0xD5, 0xAB, "DOS$ IOCTL D5/AB");
    DosClose(hf);
    return rc;
}

static const unsigned char g_reboot_com[] = {
    0xB0, 0xFE,
    0xE6, 0x64,
    0xF4,
    0xCD, 0x20
};

static void write_com(const char *name, const unsigned char *bytes, size_t n) {
    char path[160];
    FILE *f;
    sprintf(path, "%s\\%s", g_dir, name);
    f = fopen(path, "wb");
    if (!f) {
        log_line("failed to write COM");
        return;
    }
    fwrite(bytes, 1, n, f);
    fclose(f);
    log_line(name);
}

static APIRET start_vdm_com(const char *name) {
    char com_path[160];
    STARTDATA sd;
    ULONG sess_id = 0;
    PID pid = 0;
    APIRET rc;
    char obj[128];
    char msg[120];

    sprintf(com_path, "%s\\%s", g_dir, name);
    memset(&sd, 0, sizeof(sd));
    sd.Length = sizeof(sd);
    sd.Related = SSF_RELATED_INDEPENDENT;
    sd.FgBg = SSF_FGBG_BACK;
    sd.TraceOpt = SSF_TRACEOPT_NONE;
    sd.PgmTitle = (PSZ)name;
    sd.PgmName = com_path;
    sd.PgmInputs = NULL;
    sd.TermQ = NULL;
    sd.Environment = NULL;
    sd.InheritOpt = SSF_INHERTOPT_PARENT;
    sd.SessionType = SSF_TYPE_VDM;
    sd.PgmControl = SSF_CONTROL_MINIMIZE | SSF_CONTROL_INVISIBLE;
    sd.ObjectBuffer = obj;
    sd.ObjectBuffLen = sizeof(obj);

    sprintf(msg, "DosStartSession %s (VDM)", name);
    log_line(msg);
    rc = DosStartSession(&sd, &sess_id, &pid);
    sprintf(msg, "  DosStartSession rc=%lu pid=%lu", (unsigned long)rc, (unsigned long)pid);
    log_line(msg);
    return rc;
}

int main(int argc, char **argv) {
    set_paths(argc > 0 ? argv[0] : NULL);
    log_line("=== REBOOT.EXE starting ===");
    log_line("=== mode=reboot ===");
    DosSleep(1500);
    try_oemhlp_reboot();
    try_dos_reboot();
    write_com("REBOOT.COM", g_reboot_com, sizeof(g_reboot_com));
    start_vdm_com("REBOOT.COM");
    DosSleep(5000);
    log_line("=== REBOOT.EXE finished: still alive (FAILED) ===");
    return 1;
}
