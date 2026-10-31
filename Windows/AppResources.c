#include "AppResources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool app_resources_payload(Connection *c, const HttpRequest *r,
                           const char *directory, const char *bundle,
                           const void *initial, size_t initial_length) {
    bool root = !strcmp(r->path, "/") && r->accepts_outerframe;
    if (!root && strncmp(r->path, "/bundles/", 9))
        return false;
    if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
        http_text(c, r, 405, "Use GET or HEAD.");
        return true;
    }
    char name[256], url[256];
    snprintf(name, sizeof(name), "bundles/%s.macos-arm.aar", bundle);
    char *arm = win_join(directory, name);
    snprintf(name, sizeof(name), "bundles/%s.macos-x86.aar", bundle);
    char *x86 = win_join(directory, name);
    wchar_t *a = win_wide(arm), *x = win_wide(x86);
    WIN32_FILE_ATTRIBUTE_DATA info;
    bool available = GetFileAttributesExW(a, GetFileExInfoStandard, &info) &&
                     GetFileAttributesW(x) != INVALID_FILE_ATTRIBUTES;
    free(a);
    free(x);
    snprintf(url, sizeof(url), "/bundles/%s-%08lx%08lx", bundle,
             available ? info.ftLastWriteTime.dwHighDateTime : 0,
             available ? info.ftLastWriteTime.dwLowDateTime : 0);
    if (!available)
        http_text(c, r, 503, "The macOS app bundles are not installed.");
    else if (root) {
        StringBuilder content = {0};
        if (!strcmp(bundle, "PlaintextContent.bundle")) {
            content = payload(36);
            payload_u32(&content, 0, 1);
            const char *paths[] = {"/api/file", "/api/files"};
            for (size_t i = 0; i < 2; i++) {
                payload_u64(&content, 4 + i * 16, content.length);
                payload_u64(&content, 12 + i * 16, strlen(paths[i]));
                sb_append(&content, paths[i]);
            }
        } else
            sb_append_n(&content, initial, initial_length);
        StringBuilder out = payload(40);
        memcpy(out.data, "OUTR", 4);
        payload_u32(&out, 4, 1);
        payload_u64(&out, 8, 40);
        payload_u64(&out, 16, strlen(url));
        payload_u64(&out, 24, 40 + strlen(url));
        payload_u64(&out, 32, content.length);
        sb_append(&out, url);
        sb_append_n(&out, content.data, content.length);
        http_send(c, r, 200, "application/vnd.outerframe", out.data,
                  out.length);
        free(content.data);
        free(out.data);
    } else if (!strcmp(r->path, url))
        http_text(c, r, 200, "macos-arm\nmacos-x86\n");
    else {
        size_t n = strlen(url);
        if (!strncmp(r->path, url, n) && (!strcmp(r->path + n, "/macos-arm") ||
                                          !strcmp(r->path + n, "/macos-x86")))
            http_file(c, r, !strcmp(r->path + n, "/macos-arm") ? arm : x86,
                      false, false);
        else
            http_text(c, r, 404, "Bundle not found.");
    }
    free(arm);
    free(x86);
    return true;
}

bool app_resources(Connection *c, const HttpRequest *r, const char *directory,
                   const char *bundle, const char *initial) {
    return app_resources_payload(c, r, directory, bundle, initial,
                                 strlen(initial));
}
