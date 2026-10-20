#ifndef OUTERSHELL_ENDPOINT_NAMES_H
#define OUTERSHELL_ENDPOINT_NAMES_H
#include <string.h>
#include <stdio.h>
static bool endpoint_name_field(const char *text, size_t maximum) {
    if (!text || strlen(text) > maximum) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p == 127 || (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f)) return false;
    return true;
}
static bool endpoint_name_append(StringBuilder *out, const char *transport, const char *address,
                                 int port, const char *container, const char *app) {
    if (!endpoint_name_field(address, 4096) || !address[0] || !endpoint_name_field(container, 256) ||
        !endpoint_name_field(app, 256) || !app[0]) return true;
    char port_text[24] = "";
    if (!strcmp(transport, "tcp")) {
        if (port < 1 || port > 65535 || strpbrk(address, " /\\?#@")) return true;
        snprintf(port_text, sizeof(port_text), " %d", port);
    }
    return sb_append(out, transport) && sb_append(out, " ") && sb_append(out, address) &&
        sb_append(out, port_text) && sb_append(out, "\n") && sb_append(out, container) &&
        sb_append(out, "\n") &&
        sb_append(out, app) && sb_append(out, "\n");
}
#endif
