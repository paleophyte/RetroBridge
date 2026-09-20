/* C89: reserve an invocation directory atomically, including across restart.
 * Never reuse a timed-out child's output/script/completion files. The caller
 * removes only its known files and this directory after observed completion.
 */
#ifndef RETRO_EXEC_SPOOL_H
#define RETRO_EXEC_SPOOL_H
#include <errno.h>
#include <string.h>
#if defined(_WIN32) || defined(__WATCOMC__)
#include <direct.h>
#define EXEC_MKDIR(path) mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define EXEC_MKDIR(path) mkdir(path,0700)
#endif

static int exec_spool_create(const char *parent, char *out, unsigned capacity) {
    static unsigned long sequence;
    static const char hex[] = "0123456789ABCDEF";
    unsigned length = (unsigned)strlen(parent), attempt;
    int digit;
    unsigned long value;
    if (length + 10 >= capacity) return -1;
    for (attempt = 0; attempt < 4096; ++attempt) {
        strcpy(out, parent);
        out[length] = '\\'; out[length+1] = 'L'; out[length+2] = 'X';
        value = sequence++ & 0xffffffUL;
        for (digit = 8; digit >= 3; --digit) {
            out[length+digit] = hex[value & 15]; value >>= 4;
        }
        out[length+9] = '\0';
        if (EXEC_MKDIR(out) == 0) return 0;
        if (errno != EEXIST) break;
    }
    out[0] = '\0';
    return -1;
}
#endif
