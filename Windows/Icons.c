#include "Icons.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ENDPOINT_ICON_MAXIMUM_BYTES = 65536 };

static bool loopback(const char *host) {
    return !host || !host[0] || !_stricmp(host, "localhost") || !strcmp(host, "127.0.0.1") ||
        !strcmp(host, "::1") || !strcmp(host, "[::1]");
}

static bool record_line(const char *socket_path, const char *host, int port, char *line, size_t size) {
    char address[900];
    const char *transport;
    if (socket_path && socket_path[0]) {
        if (_strnicmp(socket_path, "\\\\.\\pipe\\", 9) || !socket_path[9] || strpbrk(socket_path, "\r\n")) return false;
        transport = "pipe";
        if (snprintf(address, sizeof(address), "%s", socket_path) >= (int)sizeof(address)) return false;
    } else if (port > 0 && port <= 65535) {
        transport = "tcp";
        const char *name = loopback(host) ? "localhost" : host;
        if (strpbrk(name, " /\\?#@[]\r\n") || snprintf(address, sizeof(address), "%s", name) >= (int)sizeof(address))
            return false;
    } else {
        return false;
    }
    for (char *c = address; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    int length = strcmp(transport, "tcp") ? snprintf(line, size, "pipe %s", address)
                                          : snprintf(line, size, "tcp %s %d", address, port);
    return length > 0 && (size_t)length < size;
}

bool icons_endpoint_file_name(const char *socket_path, const char *host, int port, char name[69]) {
    char line[1024];
    if (!record_line(socket_path, host, port, line, sizeof(line))) return false;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0 &&
        BCryptHashData(hash, (PUCHAR)line, (ULONG)strlen(line), 0) >= 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        name[i * 2] = hex[digest[i] >> 4];
        name[i * 2 + 1] = hex[digest[i] & 15];
    }
    memcpy(name + 64, ".png", 5);
    return true;
}

bool icons_endpoint(const char *socket_path, const char *host, int port, StringBuilder *image) {
    char name[69];
    if (!icons_endpoint_file_name(socket_path, host, port, name)) return false;
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *directory = local ? win_join(local, "OuterSSH\\endpoint-icons") : NULL;
    char *path = directory ? win_join(directory, name) : NULL;
    wchar_t *wide = path ? win_wide(path) : NULL;
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    StringBuilder data = {0};
    bool ok = attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
        win_read_file(path, ENDPOINT_ICON_MAXIMUM_BYTES, &data) &&
        data.length > 8 && !memcmp(data.data, "\x89PNG\r\n\x1a\n", 8);
    free(local); free(directory); free(path); free(wide);
    if (!ok) {
        free(data.data);
        return false;
    }
    free(image->data);
    *image = data;
    return true;
}
