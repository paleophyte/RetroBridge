/*
 * UPDATE.EXE (OS/2 1.3, 16-bit): replace a running llm_agent with a new
 * binary. Same role as ../agent-win32/update.c (Windows) and ../agent-os2/update.c
 * (2.x), simplified for the 16-bit toolchain.
 *
 * Launched via EXECDETACH so it outlives the agent it is about to stop -
 * which is exactly why it CANNOT use DosKillProcess on it: EXECDETACH makes
 * this process a *child* of the agent (spawnv), and OS/2 only allows
 * DosKillProcess to kill descendants. Killing your own parent fails with
 * ERROR_NOT_DESCENDANT (rc=305) - confirmed live against the real os2-13
 * box, not just reasoned about. So instead, this connects to the running
 * agent as an ordinary client (same token-authed wire protocol, host/port/
 * token read from LLMAGENT.INI next to the target - host= must be the
 * machine's real LAN address; an earlier version tried 127.0.0.1 and hung
 * indefinitely, also confirmed live - loopback doesn't appear to work on
 * this TCP/IP stack) and sends SELFEXIT, which the agent handles by
 * exiting itself. DosKillProcess is still tried as a harmless fallback,
 * but SELFEXIT is what actually works - confirmed live, twice.
 *
 * THE SWAP BUG, and why it looked like a locking problem for so long: the
 * backup name used to be "<target>.OLD", i.e. LLMAGENT.EXE.OLD. Watcom's
 * 16-bit rename() rejects a second dot - errno=1, every time, on any file
 * - even though the volume is HPFS and CMD.EXE's own REN accepts exactly
 * that name. So the rename could never have succeeded, on a locked file
 * or an idle one.
 *
 * Everything that made this look like a lock came from testing it with
 * CMD's REN (which takes a different path through the system and does not
 * mind the name) while update.exe used rename(). Isolated eventually with
 * _rentest.c on an ordinary throwaway file, no agent involved:
 *
 *   rename(DUMMY.EXE -> DUMMY.EXE.OLD)  rc=-1 errno=1
 *   rename(DUMMY.EXE -> DUMMY.OLD)      rc=0
 *
 * Two earlier theories died on that data, both worth recording so nobody
 * re-derives them: it is not a timing race (widening the retry window
 * from 8s to 30s changed nothing, and the file is renameable ~2.4s after
 * the agent exits - measured from an independent observer agent), and it
 * is not caused by this process being the agent's child (an fopen/fclose
 * on the target immediately before the rename, which is what
 * file_exists() does every iteration, makes no difference either).
 *
 * The backup is therefore an 8.3-safe name built by replacing the
 * target's extension, not by appending to it - see backup_path_for().
 *
 * Usage: UPDATE.EXE <new-exe-path> <target-exe-path>
 * Both paths must be absolute. Swaps files with rollback, then relaunches
 * the target.
 *
 * Logs to UPDATE.LOG next to the target.
 */

#define INCL_DOS
#define INCL_DOSFILEMGR
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#include <os2.h>

/* DosStartSession's SSF_* constants aren't in Watcom's os21x header set
 * (they're in the 32-bit h\os2 tree, which won't compile 16-bit). Values
 * are the documented OS/2 ones, identical in both trees. */
#define SSF_RELATED_INDEPENDENT 0
#define SSF_FGBG_BACK           1
#define SSF_TRACEOPT_NONE       0
#define SSF_INHERTOPT_SHELL     0
#define SSF_TYPE_DEFAULT        0
#define SSF_CONTROL_MINIMIZE    0x0004

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#include "os2sock.h"

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

static void log_rc(const char *prefix, USHORT rc) {
    char buf[128];
    sprintf(buf, "%s (rc=%u)", prefix, (unsigned)rc);
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
    unsigned pid = 0;

    dirname_of(targetPath, dir, sizeof(dir));
    sprintf(path, "%s\\AGENT.PID", dir);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%u", &pid) != 1) pid = 0;
    fclose(f);
    return (PID)pid;
}

/* port=/token=/host= from LLMAGENT.INI next to the target - same file the
 * agent itself reads on startup, plus a host= line SELFEXIT needs that the
 * agent itself doesn't. host= must be the machine's real LAN address, not
 * loopback: an early version of this connected to 127.0.0.1 and hung
 * indefinitely, confirmed live - this TCP/IP stack's loopback support is
 * evidently not there (or not configured), unlike the real interface,
 * which every external client (this project's MCP server included) reaches
 * without issue. */
static int read_ini(const char *targetPath, unsigned short *port, char *token, size_t tokenLen,
                     char *host, size_t hostLen) {
    char dir[260];
    char path[280];
    char line[256];
    FILE *f;

    *port = 2222;
    token[0] = '\0';
    host[0] = '\0';

    dirname_of(targetPath, dir, sizeof(dir));
    sprintf(path, "%s\\LLMAGENT.INI", dir);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0) *port = (unsigned short)p;
        } else if (strncmp(line, "token=", 6) == 0) {
            strncpy(token, line + 6, tokenLen - 1);
            token[tokenLen - 1] = '\0';
        } else if (strncmp(line, "host=", 5) == 0) {
            strncpy(host, line + 5, hostLen - 1);
            host[hostLen - 1] = '\0';
        }
    }
    fclose(f);
    return token[0] != '\0' && host[0] != '\0';
}

/* Minimal dotted-quad parser - no inet_addr in os2sock.h, and no DNS
 * resolver needed for a single lab-network IP. */
static int parse_ipv4(const char *s, unsigned long *out) {
    unsigned long a, b, c, d;
    char extra;
    if (sscanf(s, "%lu.%lu.%lu.%lu%c", &a, &b, &c, &d, &extra) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

/* Connect to the running agent (on its real LAN address - see read_ini's
 * comment on why not loopback) and ask it to exit. Returns 1 if the
 * request was sent and acknowledged, 0 otherwise (caller falls back to
 * the file-unlock retry loop in replace_file() either way). */
static int send_selfexit(const char *host, unsigned short port, const char *token) {
    int s;
    struct sockaddr_in addr;
    char buf[64];
    unsigned long ip;
    int n;
    int ok = 0;

    if (!parse_ipv4(host, &ip)) {
        log_line("selfexit: bad host= in LLMAGENT.INI");
        return 0;
    }

    if (sock_init() != 0) {
        log_line("selfexit: sock_init failed");
        return 0;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        log_line("selfexit: socket() failed");
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_line("selfexit: connect() failed");
        soclose(s);
        return 0;
    }

    sprintf(buf, "%s\n", token);
    send(s, buf, (int)strlen(buf), 0);
    n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        log_line("selfexit: no reply to auth");
        soclose(s);
        return 0;
    }
    buf[n] = '\0';
    if (strncmp(buf, "OK", 2) != 0) {
        log_line("selfexit: auth FAIL");
        soclose(s);
        return 0;
    }

    send(s, "SELFEXIT\n", 9, 0);
    n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        ok = (strncmp(buf, "OK", 2) == 0);
    }
    soclose(s);
    log_line(ok ? "selfexit: agent acknowledged" : "selfexit: no OK reply (may still have worked)");
    return ok;
}

/* Can anything be reached on the agent's port right now? */
static int agent_listening(const char *host, unsigned short port) {
    int s;
    struct sockaddr_in addr;
    unsigned long ip;
    int ok;

    if (!parse_ipv4(host, &ip)) return 0;
    if (sock_init() != 0) return 0;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);

    ok = (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    soclose(s);
    return ok;
}

/*
 * Wait for the agent's port to stop answering, i.e. for the process we
 * just SELFEXITed to be genuinely gone rather than merely on its way out.
 *
 * A fixed sleep isn't good enough. The outgoing agent holds LLMAGENT.EXE
 * open until it really exits, and renaming a file that is still some
 * process's executable image fails - observed live as a solid
 * `errno=6` on every one of the 150 retry attempts. Returns 1 if the port
 * went quiet.
 */
static int wait_agent_gone(const char *host, unsigned short port) {
    int i;
    char buf[80];

    for (i = 0; i < 60; i++) {          /* up to ~30s */
        if (!agent_listening(host, port)) {
            sprintf(buf, "stop: port %u quiet after %d check(s)",
                    (unsigned)port, i + 1);
            log_line(buf);
            return 1;
        }
        DosSleep(500);
    }
    log_line("stop: port still answering - another agent is running");
    return 0;
}

static int stop_agent(const char *targetPath) {
    unsigned short port;
    char token[128];
    char host[64];
    PID pid;
    USHORT rc;
    char buf[96];

    if (read_ini(targetPath, &port, token, sizeof(token), host, sizeof(host))) {
        sprintf(buf, "stop: SELFEXIT to %s:%u", host, (unsigned)port);
        log_line(buf);
        if (send_selfexit(host, port, token))
            return 1;
    } else {
        log_line("stop: LLMAGENT.INI missing host=/token= - skipping SELFEXIT");
    }

    /* Best-effort fallback - normally fails with ERROR_NOT_DESCENDANT
     * (rc=305) since this process is the agent's child, not its ancestor,
     * but harmless to try. */
    pid = read_agent_pid(targetPath);
    if (!pid) {
        log_line("stop: no AGENT.PID either - will rely on file-unlock retries");
        return 0;
    }
    sprintf(buf, "stop: DosKillProcess pid=%u (fallback)", (unsigned)pid);
    log_line(buf);
    rc = DosKillProcess(0 /* DKP_PROCESS */, pid);
    if (rc != 0) {
        log_rc("stop: DosKillProcess failed", rc);
        return 0;
    }
    log_line("stop: kill requested");
    return 1;
}

/*
 * Build the backup name by *replacing* the target's extension rather than
 * appending to it: LLMAGENT.EXE -> LLMAGENT.BAK, never LLMAGENT.EXE.OLD.
 * Watcom's 16-bit rename() refuses a name with a second dot (errno=1),
 * regardless of the volume being HPFS - see the file header. Only the
 * final path component is considered, so directories containing dots
 * don't confuse it.
 */
static void backup_path_for(const char *targetPath, char *out, int outlen) {
    char *dot;
    char *slash;

    strncpy(out, targetPath, outlen - 5);
    out[outlen - 5] = '\0';

    slash = strrchr(out, '\\');
    if (!slash) slash = strrchr(out, '/');
    dot = strrchr(slash ? slash : out, '.');
    if (dot) *dot = '\0';
    strcat(out, ".BAK");
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
    backup_path_for(targetPath, backupPath, sizeof(backupPath));
    sprintf(buf, "replace: backup name %s", backupPath);
    log_line(buf);
    remove(backupPath);

    /* Run independently (see the file header) this succeeds on the first
     * attempt, within a couple of seconds of the agent exiting. The retry
     * window is kept as slack for a slow exit, not because a long wait is
     * ever expected - if these attempts start running out again, the
     * logged errno is the thing to look at, and the question to ask is
     * whether this process somehow ended up related to the agent after
     * all. */
    for (i = 0; i < 150; i++) {
        if (!file_exists(targetPath)) {
            log_line("replace: target missing - nothing to back up");
            break;
        }
        if (rename(targetPath, backupPath) == 0) {
            renamedOld = 1;
            break;
        }
        if (i == 0 || i == 25) {
            sprintf(buf, "replace: rename attempt %d failed, errno=%d", i, errno);
            log_line(buf);
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

/*
 * Start the new agent in a session of its own, inheriting from the shell
 * rather than from us.
 *
 * This is not cosmetic. spawnl() hands the child our open handles, and we
 * in turn were handed the outgoing agent's, so every self-update used to
 * pass a slightly larger pile of inherited handles down the chain:
 *
 *     agent -> (EXECDETACH/spawnv) update.exe -> (spawnl) new agent -> ...
 *
 * OS/2 1.x gives a process 20 file handles by default, so the accumulation
 * runs out fast. Measured live, updating repeatedly from a freshly booted
 * machine: generations 1-3 healthy, generation 4 unable to open a file at
 * all - it could not read back the AGENT.PID it had just written - and
 * every generation after that dead on arrival. That is the real reason
 * self-update was never dependable even once the swap itself worked, and
 * it degrades silently: the agent answers PING perfectly well while being
 * unable to open a single file.
 *
 * SSF_INHERTOPT_SHELL is the part that matters - the new session inherits
 * the shell's environment and handles, not this process's - so every
 * generation starts as clean as one launched from STARTUP.CMD.
 * SSF_RELATED_INDEPENDENT keeps it alive independently of us.
 *
 * spawnl stays as a fallback: an agent started with inherited baggage is
 * still far better than no agent at all.
 */
static int start_agent(const char *targetPath) {
    STARTDATA sd;
    USHORT sessID = 0;
    USHORT sessPid = 0;
    USHORT rc;
    int spawned;
    char buf[128];

    memset(&sd, 0, sizeof(sd));
    sd.Length      = sizeof(sd);
    sd.Related     = SSF_RELATED_INDEPENDENT;
    sd.FgBg        = SSF_FGBG_BACK;
    sd.TraceOpt    = SSF_TRACEOPT_NONE;
    sd.PgmTitle    = (PSZ)"LLMAGENT.EXE";
    sd.PgmName     = (PSZ)targetPath;
    sd.PgmInputs   = NULL;
    sd.TermQ       = NULL;
    sd.Environment = NULL;
    sd.InheritOpt  = SSF_INHERTOPT_SHELL;
    sd.SessionType = SSF_TYPE_DEFAULT;
    sd.IconFile    = NULL;
    sd.PgmHandle   = 0;
    /* Minimized, not invisible: it stays in the Window List exactly as the
     * STARTUP.CMD-launched agent does, which WINLIST and the smoke test
     * both expect to see. */
    sd.PgmControl  = SSF_CONTROL_MINIMIZE;

    rc = DosStartSession(&sd, &sessID, &sessPid);
    if (rc == 0) {
        sprintf(buf, "start: DosStartSession ok (sess=%u pid=%u)",
                (unsigned)sessID, (unsigned)sessPid);
        log_line(buf);
        return 1;
    }

    sprintf(buf, "start: DosStartSession rc=%u - falling back to spawnl",
            (unsigned)rc);
    log_line(buf);
    spawned = spawnl(P_NOWAIT, targetPath, targetPath, NULL);
    if (spawned < 0) {
        sprintf(buf, "start: spawnl failed (errno=%d)", errno);
        log_line(buf);
        return 0;
    }
    sprintf(buf, "start: pid=%d (inherited handles - see above)", spawned);
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

    {
        unsigned short port;
        char token[128];
        char host[64];
        int haveIni = read_ini(targetPath, &port, token, sizeof(token),
                               host, sizeof(host));

        stop_agent(targetPath);

        /* Don't touch the binary until the port has actually gone quiet -
         * see wait_agent_gone(). If it never does, some *other* agent is
         * still running and holding LLMAGENT.EXE, and the swap cannot
         * work; say so plainly instead of grinding through 150 doomed
         * renames and leaving the file busy for a minute. */
        if (haveIni && !wait_agent_gone(host, port)) {
            log_line("=== UPDATE.EXE finished: another agent still running,"
                     " swap NOT attempted ===");
            return 1;
        }
        if (!haveIni) DosSleep(500);

        replaced = replace_file(newPath, targetPath);

        /*
         * Only start an agent if nothing is listening. start_agent() used
         * to run unconditionally, which is how duplicates accumulated:
         * a failed round would SELFEXIT one agent and start another, and
         * once two existed, SELFEXIT could only ever stop whichever one
         * held the port while the other kept the .EXE locked - wedging
         * every future update. Observed live with two LLM_AGEN processes
         * in PSLIST and a permanent errno=6 on the rename.
         */
        if (haveIni && agent_listening(host, port)) {
            log_line("start: an agent is already listening - not starting"
                     " a second one");
            started = 1;
        } else {
            started = start_agent(targetPath);
        }
    }

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
