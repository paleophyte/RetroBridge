/* Authentication/command framing only; never use for binary payloads.
 * Keep the advertised limits: 510 bytes for a 512-byte buffer, 4094 for
 * 4096. LF or CRLF ends a line; neither terminator counts toward the limit.
 * Return 1 only for a complete line, 0 for more bytes, -1 for malformed input.
 * The caller must fail the session on -1, not dispatch or drain its suffix.
 */
#ifndef AGENT_COMMAND_LINE_H
#define AGENT_COMMAND_LINE_H

static int command_line_byte(char *out, int outlen, int *length,
                             int *saw_cr, unsigned char c) {
    if (c == '\n') {
        out[*length] = '\0';
        return 1;
    }
    if (*saw_cr || c == '\0') return -1;
    if (c == '\r') {
        *saw_cr = 1;
        return 0;
    }
    if (*length >= outlen - 2) return -1;
    out[(*length)++] = (char)c;
    return 0;
}

#endif
