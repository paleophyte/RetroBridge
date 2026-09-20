/* Conservative, bounded NCF editor. Only explicit unconditional LOADs count.
 * Other startup commands are preserved, not evaluated or executed here. */
#ifndef LLM_AUTOEXEC_H
#define LLM_AUTOEXEC_H

static int ncf_equal(const char *p, int n, const char *word) {
    int i;
    if (n != (int)strlen(word)) return 0;
    for (i = 0; i < n; i++) {
        char c = p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != word[i]) return 0;
    }
    return 1;
}

/* Read one word, allowing a quoted module path. Bad quotes fail closed. */
static int ncf_word(const char *s, int end, int *pos, int *start, int *size) {
    int quoted;
    while (*pos < end && (s[*pos] == ' ' || s[*pos] == '\t')) ++*pos;
    if (*pos == end || s[*pos] == '#' || s[*pos] == ';') return 0;
    quoted = s[*pos] == '"';
    if (quoted) ++*pos;
    *start = *pos;
    while (*pos < end && (quoted ? s[*pos] != '"' :
           (s[*pos] != ' ' && s[*pos] != '\t')))
        ++*pos;
    *size = *pos - *start;
    if (quoted) {
        if (*pos == end) return -1;
        ++*pos;
        if (*pos < end && s[*pos] != ' ' && s[*pos] != '\t') return -1;
    }
    return *size ? 1 : -1;
}

static int ncf_module(const char *p, int n) {
    int i, base = 0;
    for (i = 0; i < n; i++)
        if (p[i] == ':' || p[i] == '\\' || p[i] == '/') base = i + 1;
    p += base; n -= base;
    if (n >= 4 && ncf_equal(p + n - 4, 4, ".NLM")) n -= 4;
    if (ncf_equal(p, n, "CLIBAUX")) return 1;
    if (ncf_equal(p, n, "LLMAGENT")) return 2;
    if (ncf_equal(p, n, "TCPIP")) return 3;
    return 0;
}

/* Returns 0=present, 1=added both, 2=added agent, 3=added CLIBAUX.
 * Negative: -1 malformed/unsupported text, -2 ambiguous/duplicate/unload,
 * -3 reversed dependency/network order, -4 output too large.
 * A single terminal DOS EOF is preserved, with insertions before it. */
static int autoexec_plan(const char *src, int n, char *dst, int cap, int *written) {
    int endtext = n, line, end, pos, start, size, word, optional, module;
    int clib = -1, agent = -1, network = -1, i, insert, extra, result;
    const char *addition;
    *written = 0;
    if (n && (unsigned char)src[n - 1] == 26) --endtext;
    for (i = 0; i < endtext; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) return -1;
    }
    for (line = 0; line < endtext; line = end) {
        end = line;
        while (end < endtext && src[end] != '\r' && src[end] != '\n') ++end;
        pos = line;
        while (pos < end && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
        optional = pos < end && src[pos] == '?';
        if (optional) {
            ++pos;
            if (pos < end && (src[pos] == 'Y' || src[pos] == 'y' ||
                             src[pos] == 'N' || src[pos] == 'n')) ++pos;
        }
        word = ncf_word(src, end, &pos, &start, &size);
        if (word < 0) return -1;
        if (word && !ncf_equal(src + start, size, "REM")) {
            int load = ncf_equal(src + start, size, "LOAD");
            int unload = ncf_equal(src + start, size, "UNLOAD");
            int bind = ncf_equal(src + start, size, "BIND");
            module = ncf_module(src + start, size);
            if (module == 1 || module == 2) return -2; /* implicit LOAD / NCF call */
            if (load || unload || bind) {
                word = ncf_word(src, end, &pos, &start, &size);
                if (word < 0 || (load && !word)) return -1;
                if (word) {
                    module = ncf_module(src + start, size);
                    if ((module == 1 || module == 2) && (optional || unload)) return -2;
                    if (load && module == 1) { if (clib >= 0) return -2; clib = line; }
                    if (load && module == 2) { if (agent >= 0) return -2; agent = line; }
                    if ((load && module == 3) || (bind && ncf_equal(src + start, size, "IP")))
                        network = line;
                    if (load && !module) {
                        /* Wrapped LOAD forms are not unconditional plain
                           module loads; do not silently append duplicates. */
                        while ((word = ncf_word(src, end, &pos, &start, &size)) > 0) {
                            module = ncf_module(src + start, size);
                            if (module == 1 || module == 2) return -2;
                        }
                        if (word < 0) return -1;
                    }
                }
            }
        }
        if (end < endtext && src[end] == '\r') ++end;
        if (end < endtext && src[end] == '\n') ++end;
    }
    if (agent >= 0 && ((clib >= 0 && clib > agent) || network > agent)) return -3;
    if (clib >= 0 && agent >= 0) { *written = n; return 0; }
    if (agent >= 0) {
        insert = agent; addition = "LOAD CLIBAUX\r\n"; result = 3;
    } else {
        insert = endtext;
        addition = clib >= 0 ? "LOAD LLMAGENT\r\n" : "LOAD CLIBAUX\r\nLOAD LLMAGENT\r\n";
        result = clib >= 0 ? 2 : 1;
    }
    extra = (int)strlen(addition);
    i = insert && src[insert - 1] != '\r' && src[insert - 1] != '\n' ? 2 : 0;
    if (n > cap - extra - i) return -4;
    memcpy(dst, src, (size_t)insert);
    if (i) memcpy(dst + insert, "\r\n", 2);
    memcpy(dst + insert + i, addition, (size_t)extra);
    memcpy(dst + insert + i + extra, src + insert, (size_t)(n - insert));
    *written = n + extra + i;
    return result;
}
#endif
