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
static inline bool endpoint_name_append_json_string(StringBuilder *out, const char *text) {
    bool ok = sb_append(out, "\"");
    for (const unsigned char *p = (const unsigned char *)text; ok && *p; p++) {
        const char *escape = *p == '"' ? "\\\"" : *p == '\\' ? "\\\\" : *p == '\b' ? "\\b" :
            *p == '\f' ? "\\f" : *p == '\n' ? "\\n" : *p == '\r' ? "\\r" : *p == '\t' ? "\\t" : NULL;
        if (escape) {
            ok = sb_append(out, escape);
        } else if (*p < 32) {
            char code[8];
            snprintf(code, sizeof(code), "\\u%04x", *p);
            ok = sb_append(out, code);
        } else {
            ok = sb_append_n(out, (const char *)p, 1);
        }
    }
    return ok && sb_append(out, "\"");
}
static inline bool endpoint_name_host_layout_key(StringBuilder *out, const char *scope, const char *service_id,
                                                 const char *frontend_id) {
    return sb_append(out, "[\"host\",") && endpoint_name_append_json_string(out, scope) &&
        sb_append(out, ",") && endpoint_name_append_json_string(out, service_id) &&
        sb_append(out, ",") && endpoint_name_append_json_string(out, frontend_id) && sb_append(out, "]");
}
#endif
