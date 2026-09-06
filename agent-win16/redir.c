/* REDIR.EXE -- tiny native DOS helper, built for the DOS target (not
   Windows) with Open Watcom.

   Why this exists: llm_agent.c's EXEC command used to build a command
   line like "COMMAND.COM /c <cmd> > <outfile>" and hand it straight to
   WinExec(). On this box, ANY secondary COMMAND.COM /c invocation that
   includes ">" redirection crashes with a Windows 3.1 "This application
   has violated system integrity" UAE -- confirmed independent of the
   target path, independent of foreground/minimized show state, and
   independent of the calling code (EXEC vs EXECDETACH). A non-redirected
   "/c <cmd>" never crashes. The DOS-only boot side of this same machine
   (no WFW/NDIS loaded) has never shown this for equivalent redirected
   commands, so the leading theory is that WFW's DOS-box network-
   redirector hook (loaded via NET START before WIN) corrupts the INT 21h
   handle-duplication sequence COMMAND.COM's own "> file" parsing relies
   on.

   Workaround: do the redirection ourselves, in a plain DOS program, via
   direct handle duplication -- BEFORE any shell is invoked -- so the
   child shell we spawn is never asked to parse ">" at all; it just
   inherits an already-redirected stdout/stderr.

   Usage: REDIR.EXE <outfile> <command...>
   The command tail is whatever llm_agent.c's EXEC command line was,
   split on spaces by DOS the same way it always was; we just glue it
   back together and hand it to the target command processor unmodified
   except with no ">" appended.

   Getting the redirection itself past the dup2() step is NOT enough,
   though -- an earlier version of this file used system(cmdline) to run
   the actual command afterward, and THAT hung forever (confirmed via
   direct testing: the command's own output was written to the file
   correctly, so the child had already finished, but the VDM never
   exited). A no-op sibling test proved dup2() alone exits cleanly; only
   adding a nested system() call reproduced the hang, so it's specifically
   something about system()'s own spawn+wait implementation misbehaving
   once this process's own stdout no longer points at the console.
   spawnl(P_WAIT, ...) does the equivalent spawn-and-wait directly and
   does not hang -- confirmed via repeated testing with multiple
   commands. Use that, not system(). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <process.h>

int main(int argc, char *argv[]) {
    int fd, i, rc;
    char cmdline[512];
    const char *comspec;

    if (argc < 3) {
        fprintf(stderr, "usage: REDIR <outfile> <command...>\n");
        return 1;
    }

    fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IWRITE);
    if (fd < 0) {
        fprintf(stderr, "REDIR: cannot open %s\n", argv[1]);
        return 1;
    }

    fflush(stdout);
    dup2(fd, 1);
    fflush(stderr);
    dup2(fd, 2);
    close(fd);

    cmdline[0] = '\0';
    for (i = 2; i < argc; i++) {
        if (i > 2) strcat(cmdline, " ");
        strcat(cmdline, argv[i]);
    }

    comspec = getenv("COMSPEC");
    if (!comspec || !comspec[0]) comspec = "C:\\COMMAND.COM";

    rc = spawnl(P_WAIT, comspec, comspec, "/C", cmdline, NULL);

    fflush(stdout);
    return rc;
}
