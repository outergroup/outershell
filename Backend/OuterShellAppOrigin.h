#ifndef OUTERSHELL_APP_ORIGIN_H
#define OUTERSHELL_APP_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Outer Loop opens a Unix-socket app at http://<host>/, where <host> is the
 * socket path in unpadded lowercase base32, split into 50-character labels,
 * followed by .unix.outerssh.localhost. This must match OuterSSH's
 * SSHProxyAddress.h, which is in the Outer Loop repository;
 * Scripts/test_app_origins.sh checks both against the same vectors. */
static inline bool outer_shell_unix_socket_origin(const char *path, bool https, char *origin, size_t capacity) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    static const char suffix[] = ".unix.outerssh.localhost";
    const char *scheme = https ? "https://" : "http://";
    size_t path_length = path ? strlen(path) : 0;
    if (!origin || capacity == 0) return false;
    origin[0] = 0;
    if (path_length == 0 || path[0] != '/' || path_length > 144) return false;
    for (size_t index = 0; index < path_length; index++) {
        if ((unsigned char)path[index] < 32 || path[index] == 127) return false;
    }
    size_t symbols = (path_length * 8 + 4) / 5;
    size_t host_length = symbols + (symbols - 1) / 50 + sizeof(suffix) - 1;
    if (host_length >= 254 || strlen(scheme) + host_length >= capacity) return false;
    size_t out = (size_t)snprintf(origin, capacity, "%s", scheme);
    uint32_t bits = 0;
    unsigned count = 0;
    size_t emitted = 0;
    for (size_t index = 0; index < path_length || count; ) {
        if (count < 5 && index < path_length) {
            bits = (bits << 8) | (unsigned char)path[index++];
            count += 8;
        }
        if (count < 5) {
            bits <<= 5 - count;
            count = 5;
        }
        if (emitted && emitted % 50 == 0) origin[out++] = '.';
        count -= 5;
        origin[out++] = alphabet[(bits >> count) & 31];
        emitted++;
        bits &= (1u << count) - 1;
    }
    memcpy(origin + out, suffix, sizeof(suffix));
    return true;
}

/* A container app's socket is published on the host at
 * <relay directory>/<FNV-1a 64 of the in-container socket path, 16 hex digits>.sock.
 * Both container providers name relays this way. */
static inline bool outer_shell_container_relay_path(const char *directory, const char *inner_path,
                                                    char *path, size_t capacity) {
    if (!directory || !directory[0] || !inner_path || !inner_path[0] || !path || capacity == 0) return false;
    uint64_t digest = 14695981039346656037ULL;
    for (const unsigned char *cursor = (const unsigned char *)inner_path; *cursor; cursor++) {
        digest = (digest ^ *cursor) * 1099511628211ULL;
    }
    int written = snprintf(path, capacity, "%s/%016llx.sock", directory, (unsigned long long)digest);
    return written > 0 && (size_t)written < capacity;
}

#endif
