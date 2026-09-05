/*
 * _rentest.c - throwaway probe: why does update.exe's rename() fail where
 * CMD.EXE's REN succeeds on the same file?
 *
 * Two suspects, and this separates them:
 *   PLAIN  rename() on its own.
 *   OPEN   fopen()/fclose() on the source first, then rename() - which is
 *          exactly what update.c's replace_file() does via file_exists()
 *          on every loop iteration.
 *
 * Not part of the build; kept because it is the cheapest way to re-check
 * this if the swap ever regresses.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    const char *mode, *from, *to;
    int rc;

    if (argc < 4) {
        printf("usage: _rentest PLAIN|OPEN <from> <to>\n");
        return 2;
    }
    mode = argv[1];
    from = argv[2];
    to = argv[3];

    if (strcmp(mode, "OPEN") == 0) {
        FILE *f = fopen(from, "rb");
        printf("fopen(%s) -> %s\n", from, f ? "ok" : "FAILED");
        if (f) fclose(f);
    }

    errno = 0;
    rc = rename(from, to);
    printf("%s rename(%s -> %s) rc=%d errno=%d\n", mode, from, to, rc, errno);
    return rc == 0 ? 0 : 1;
}
