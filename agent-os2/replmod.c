/* replmod.exe <old-file> <new-file> <backup-file>
 *
 * Thin CLI wrapper around DosReplaceModule -- the API IBM's own CSD/fixpak
 * installers use to swap a DLL/EXE that's currently locked by the running
 * system (e.g. DOSCALL1.DLL, loaded by nearly every process from very
 * early boot onward, so a plain COPY/REN fails with no error at all).
 * If old-file is in use, OS/2 defers the actual rename-in/rename-out until
 * the next time it becomes free -- in practice, next reboot.
 */
#define INCL_DOSMODULEMGR
#include <os2.h>
#include <stdio.h>

int main(int argc, char **argv) {
    APIRET rc;

    if (argc != 4) {
        printf("usage: replmod <old-file> <new-file> <backup-file>\n");
        return 2;
    }

    rc = DosReplaceModule(argv[1], argv[2], argv[3]);
    if (rc != 0) {
        printf("DosReplaceModule failed, rc=%lu\n", (unsigned long)rc);
        return 1;
    }

    printf("DosReplaceModule OK (rc=0) -- replacement applied immediately "
           "if %s was free, else deferred to next boot\n", argv[1]);
    return 0;
}
