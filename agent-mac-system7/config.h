/* Bounded parser shared by the native loader and host regression tests. */
#ifndef MAC_AGENT_CONFIG_H
#define MAC_AGENT_CONFIG_H
#define MAC_CONFIG_MAX 4096
#define MAC_TOKEN_MAX 256
#define MAC_DEFAULT_PORT 2222

typedef struct {
    char token[MAC_TOKEN_MAX];
    unsigned short port;
} MacAgentConfig;

static const char *MacParseConfig(const char *data, long size, MacAgentConfig *out)
{
    MacAgentConfig parsed;
    long pos = 0;
    int haveToken = 0, havePort = 0;
    memset(&parsed, 0, sizeof(parsed));
    parsed.port = MAC_DEFAULT_PORT;
    if (size <= 0 || size > MAC_CONFIG_MAX) return "INI is empty or exceeds 4096 bytes";
    while (pos < size) {
        long first = pos, last, equal, value, i;
        while (pos < size && data[pos] != '\r' && data[pos] != '\n') {
            unsigned char ch = (unsigned char)data[pos++];
            if ((ch < 32 && ch != '\t') || ch > 126) return "INI must contain ASCII text";
        }
        last = pos;
        if (pos < size && data[pos++] == '\r' && pos < size && data[pos] == '\n') pos++;
        if (last - first > 511) return "INI line exceeds 511 bytes";
        while (first < last && (data[first] == ' ' || data[first] == '\t')) first++;
        while (last > first && (data[last - 1] == ' ' || data[last - 1] == '\t')) last--;
        if (first == last || data[first] == '#' || data[first] == ';') continue;
        for (equal = first; equal < last && data[equal] != '='; equal++) {}
        if (equal == last) return "INI entry needs key=value";
        value = equal + 1;
        while (equal > first && (data[equal - 1] == ' ' || data[equal - 1] == '\t')) equal--;
        while (value < last && (data[value] == ' ' || data[value] == '\t')) value++;
        if (equal - first == 5 && !memcmp(data + first, "token", 5)) {
            if (haveToken++) return "duplicate token entry";
            if (last - value < 1 || last - value >= MAC_TOKEN_MAX) return "token needs 1 to 255 bytes";
            for (i = value; i < last; i++)
                if ((unsigned char)data[i] < 33 || (unsigned char)data[i] > 126)
                    return "token must be printable ASCII without spaces";
            memcpy(parsed.token, data + value, (size_t)(last - value));
            if (!strcmp(parsed.token, "REPLACE_WITH_UNIQUE_TOKEN")) return "replace the example token";
        } else if (equal - first == 4 && !memcmp(data + first, "port", 4)) {
            unsigned long port = 0;
            if (havePort++) return "duplicate port entry";
            if (value == last) return "port needs a decimal integer from 1 to 65535";
            for (i = value; i < last; i++) {
                if (data[i] < '0' || data[i] > '9') return "port needs a decimal integer from 1 to 65535";
                port = port * 10 + (unsigned)(data[i] - '0');
                if (port > 65535) return "port needs a decimal integer from 1 to 65535";
            }
            if (!port) return "port needs a decimal integer from 1 to 65535";
            parsed.port = (unsigned short)port;
        } else return "unknown INI key (expected token or port)";
    }
    if (!haveToken) return "INI needs a token entry";
    *out = parsed;
    return NULL;
}
#endif
