#ifndef OUTERSHELLD_ENDPOINT_ICONS_H
#define OUTERSHELLD_ENDPOINT_ICONS_H

/* Reads OuterSSH's per-user endpoint icon cache, which Outer Loop fills after it
 * loads an endpoint's page. See endpoint-icons.md. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENDPOINT_ICON_MAXIMUM_BYTES 65536

typedef struct {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} EndpointIconSHA256;

static const uint32_t endpoint_icon_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t endpoint_icon_rotate(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

static void endpoint_icon_sha256_block(EndpointIconSHA256 *hash) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)hash->block[i * 4] << 24 | (uint32_t)hash->block[i * 4 + 1] << 16 |
            (uint32_t)hash->block[i * 4 + 2] << 8 | hash->block[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (endpoint_icon_rotate(w[i - 15], 7) ^ endpoint_icon_rotate(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
            w[i - 7] + (endpoint_icon_rotate(w[i - 2], 17) ^ endpoint_icon_rotate(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(s, hash->state, sizeof(s));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (endpoint_icon_rotate(s[4], 6) ^ endpoint_icon_rotate(s[4], 11) ^ endpoint_icon_rotate(s[4], 25)) +
            ((s[4] & s[5]) ^ (~s[4] & s[6])) + endpoint_icon_sha256_k[i] + w[i];
        uint32_t t2 = (endpoint_icon_rotate(s[0], 2) ^ endpoint_icon_rotate(s[0], 13) ^ endpoint_icon_rotate(s[0], 22)) +
            ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof(*s));
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) hash->state[i] += s[i];
}

static void endpoint_icon_sha256_update(EndpointIconSHA256 *hash, const unsigned char *bytes, size_t length) {
    hash->length += length;
    while (length--) {
        hash->block[hash->used++] = *bytes++;
        if (hash->used == 64) {
            endpoint_icon_sha256_block(hash);
            hash->used = 0;
        }
    }
}

static void endpoint_icon_sha256_hex(const char *text, char hex[65]) {
    EndpointIconSHA256 hash = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    endpoint_icon_sha256_update(&hash, (const unsigned char *)text, strlen(text));
    uint64_t bits = hash.length * 8;
    unsigned char padding[72] = {0x80};
    size_t pad = (hash.used < 56 ? 56 : 120) - hash.used;
    for (int i = 0; i < 8; i++) padding[pad + i] = (unsigned char)(bits >> (56 - 8 * i));
    endpoint_icon_sha256_update(&hash, padding, pad + 8);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        unsigned char byte = (unsigned char)(hash.state[i / 4] >> (24 - 8 * (i % 4)));
        hex[i * 2] = digits[byte >> 4];
        hex[i * 2 + 1] = digits[byte & 15];
    }
    hex[64] = '\0';
}

static bool endpoint_icon_loopback_host(const char *host) {
    return !host || !host[0] || !strcasecmp(host, "localhost") || !strcmp(host, "127.0.0.1") ||
           !strcmp(host, "::1") || !strcmp(host, "[::1]");
}

/* Writes the endpoints.d record line OuterSSH hashes for an endpoint: a Unix
 * socket path, or else a TCP port on host (loopback hosts are "localhost"). */
static bool endpoint_icon_record_line(const char *socket_path, const char *host, int port,
                                      char *line, size_t size) {
    int length;
    if (socket_path && socket_path[0]) {
        if (socket_path[0] != '/' || strpbrk(socket_path, "\r\n")) return false;
        length = snprintf(line, size, "unix %s", socket_path);
    } else if (port > 0 && port <= 65535) {
        const char *name = endpoint_icon_loopback_host(host) ? "localhost" : host;
        if (strpbrk(name, " /\\?#@[]\r\n")) return false;
        length = snprintf(line, size, "tcp %s %d", name, port);
        if (length > 4) for (char *c = line + 4; *c != ' '; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    } else {
        return false;
    }
    return length > 0 && (size_t)length < size;
}

static bool endpoint_icon_directory(char *path, size_t size) {
    const char *home = getenv("HOME");
#ifdef __APPLE__
    if (!home || home[0] != '/') return false;
    return snprintf(path, size, "%s/Library/Application Support/org.outerssh/endpoint-icons", home) < (int)size;
#else
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config && config[0] == '/') return snprintf(path, size, "%s/outerssh/endpoint-icons", config) < (int)size;
    if (!home || home[0] != '/') return false;
    return snprintf(path, size, "%s/.config/outerssh/endpoint-icons", home) < (int)size;
#endif
}

/* Finds the cached icon for an endpoint and writes its path. The cache
 * directory and file must belong to this user, must not be group- or
 * world-writable, and the file must be a regular PNG file within the cache's
 * size limit. */
static bool endpoint_icon_cached_path(const char *socket_path, const char *host, int port,
                                      char *path, size_t size) {
    char line[PATH_MAX + 32], name[65], directory[PATH_MAX];
    if (!endpoint_icon_record_line(socket_path, host, port, line, sizeof(line)) ||
        !endpoint_icon_directory(directory, sizeof(directory))) return false;
    endpoint_icon_sha256_hex(line, name);
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0) return false;
    struct stat st;
    bool ok = fstat(directory_fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 022);
    char file_name[70];
    snprintf(file_name, sizeof(file_name), "%s.png", name);
    int fd = ok ? openat(directory_fd, file_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
    close(directory_fd);
    if (fd < 0) return false;
    unsigned char signature[8];
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 022) &&
         st.st_size > 8 && st.st_size <= ENDPOINT_ICON_MAXIMUM_BYTES &&
         read(fd, signature, sizeof(signature)) == (ssize_t)sizeof(signature) &&
         !memcmp(signature, "\x89PNG\r\n\x1a\n", 8);
    close(fd);
    return ok && snprintf(path, size, "%s/%s", directory, file_name) < (int)size;
}

#endif
