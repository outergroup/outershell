// Runs an app's helper in a container, bringing the helper into the container
// when the container does not have it yet. Any app can use this; Outer Shell
// gives its own apps no other way in.
//
// An app ships its static Linux helpers as helpers/linux-x86_64/<name> and
// helpers/linux-aarch64/<name>. A helper is identified by the SHA-256 of its
// file, so different versions never collide. In a container, a helper lives
// at <directory>/<app id>/<sha256>/<name>, where <directory> is one of:
//
//   /usr/local/lib/outershell/helpers         copied in when an image is built
//   $HOME/.cache/outershell/helpers           uploaded on first use
//   /tmp/outershell-helpers-<uid>             uploaded when $HOME is not writable
//
// The app runs container_helper_command() through the runtime's `exec -i`.
// That runs a shell script that writes one line to standard output:
//
//   R             the helper is present and now running
//   U <arch>      write the helper for <arch> (x86_64 or aarch64) to standard
//                 input; it is installed and then runs
//   X <message>   the helper cannot run, for the reason given
//
// After R, or after the upload following U, the helper speaks its own
// protocol on the same streams. The container needs /bin/sh, head, wc, id,
// mkdir, chmod and mv, which images Outer Shell builds always have.
//
// Apps copy this file from Outer Shell's Resources/ContainerHelper.h.
#ifndef OUTER_CONTAINER_HELPER_H
#define OUTER_CONTAINER_HELPER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CONTAINER_HELPER_BAKED_DIRECTORY "/usr/local/lib/outershell/helpers"
#define CONTAINER_HELPER_MAX_SIZE (64u * 1024u * 1024u)

typedef struct {
    char id[128], name[64];
    char hash[2][65], size[2][24];
    unsigned char *data[2];
    size_t length[2];
} ContainerHelper;

static const char *const container_helper_architectures[2] = {"x86_64", "aarch64"};

static const char container_helper_script[] =
    "case \"$(uname -m)\" in\n"
    "x86_64|amd64) h=$3 s=$4 a=x86_64 ;;\n"
    "aarch64|arm64) h=$5 s=$6 a=aarch64 ;;\n"
    "*) echo \"X Unsupported container architecture $(uname -m).\"; exit 1 ;;\n"
    "esac\n"
    "[ -n \"$h\" ] || { echo \"X This app has no helper for $a containers.\"; exit 1; }\n"
    "i=$1 n=$2\n"
    "shift 6\n"
    "u=$(id -u)\n"
    "for d in " CONTAINER_HELPER_BAKED_DIRECTORY " \"${HOME:-/nonexistent}/.cache/outershell/helpers\" \"/tmp/outershell-helpers-$u\"; do\n"
    "    if [ -x \"$d/$i/$h/$n\" ]; then echo R; exec \"$d/$i/$h/$n\" \"$@\"; fi\n"
    "done\n"
    "p=\n"
    "for d in \"${HOME:-/nonexistent}/.cache/outershell/helpers\" \"/tmp/outershell-helpers-$u\"; do\n"
    "    if mkdir -p \"$d/$i/$h\" 2>/dev/null && [ -O \"$d\" ] && [ -w \"$d/$i/$h\" ]; then p=$d/$i/$h/$n; break; fi\n"
    "done\n"
    "[ -n \"$p\" ] || { echo \"X The container has no writable directory for this app's helper.\"; exit 1; }\n"
    "echo \"U $a\"\n"
    "t=$p.$$\n"
    "head -c \"$s\" > \"$t\" && [ \"$(wc -c < \"$t\")\" -eq \"$s\" ] && chmod 755 \"$t\" && mv -f \"$t\" \"$p\" || { rm -f \"$t\"; exit 1; }\n"
    "exec \"$p\" \"$@\"\n";

typedef struct {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} ContainerHelperSHA256;

static inline void container_helper_sha256_block(ContainerHelperSHA256 *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define CONTAINER_HELPER_ROTATE(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = CONTAINER_HELPER_ROTATE(w[i - 15], 7) ^ CONTAINER_HELPER_ROTATE(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = CONTAINER_HELPER_ROTATE(w[i - 2], 17) ^ CONTAINER_HELPER_ROTATE(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, c->state, sizeof(v));
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = CONTAINER_HELPER_ROTATE(v[4], 6) ^ CONTAINER_HELPER_ROTATE(v[4], 11) ^ CONTAINER_HELPER_ROTATE(v[4], 25);
        uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
        uint32_t s0 = CONTAINER_HELPER_ROTATE(v[0], 2) ^ CONTAINER_HELPER_ROTATE(v[0], 13) ^ CONTAINER_HELPER_ROTATE(v[0], 22);
        uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(*v));
        v[4] += t1;
        v[0] = t1 + t2;
    }
#undef CONTAINER_HELPER_ROTATE
    for (int i = 0; i < 8; i++) c->state[i] += v[i];
}

static inline void container_helper_sha256(const unsigned char *data, size_t length, char hex[65]) {
    ContainerHelperSHA256 c = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    size_t offset = 0;
    for (; offset + 64 <= length; offset += 64) container_helper_sha256_block(&c, data + offset);
    unsigned char tail[128] = {0};
    size_t rest = length - offset;
    memcpy(tail, data + offset, rest);
    tail[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++) tail[total - 1 - i] = (unsigned char)(bits >> (i * 8));
    for (size_t i = 0; i < total; i += 64) container_helper_sha256_block(&c, tail + i);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", c.state[i]);
}

static unsigned char *container_helper_read_file(const char *path, size_t *length) {
#ifdef _WIN32
    int count = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wide = count > 0 ? (wchar_t *)malloc((size_t)count * sizeof(wchar_t)) : NULL;
    FILE *file = wide && MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, count) ? _wfopen(wide, L"rb") : NULL;
    free(wide);
#else
    FILE *file = fopen(path, "rb");
#endif
    if (!file) return NULL;
    unsigned char *data = NULL;
    size_t used = 0, capacity = 0;
    for (;;) {
        if (used == capacity) {
            capacity = capacity ? capacity * 2 : 1024 * 1024;
            unsigned char *grown = capacity <= CONTAINER_HELPER_MAX_SIZE ? (unsigned char *)realloc(data, capacity) : NULL;
            if (!grown) { free(data); fclose(file); return NULL; }
            data = grown;
        }
        size_t n = fread(data + used, 1, capacity - used, file);
        used += n;
        if (n == 0) break;
    }
    bool ok = !ferror(file);
    fclose(file);
    if (!ok || !used) { free(data); return NULL; }
    *length = used;
    return data;
}

// Loads the app's helpers from directory (which contains linux-x86_64 and
// linux-aarch64). Returns false when neither is present.
static inline bool container_helper_load(ContainerHelper *h, const char *id, const char *name, const char *directory) {
    memset(h, 0, sizeof(*h));
    if (strlen(id) >= sizeof(h->id) || strlen(name) >= sizeof(h->name)) return false;
    strcpy(h->id, id);
    strcpy(h->name, name);
    for (int i = 0; i < 2; i++) {
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/linux-%s/%s", directory, container_helper_architectures[i], name) >= (int)sizeof(path))
            continue;
        h->data[i] = container_helper_read_file(path, &h->length[i]);
        if (!h->data[i]) continue;
        container_helper_sha256(h->data[i], h->length[i], h->hash[i]);
        snprintf(h->size[i], sizeof(h->size[i]), "%zu", h->length[i]);
    }
    return h->data[0] || h->data[1];
}

// Appends the shell command that runs the helper with helper_args. Returns the
// new argument count, or 0 when capacity is too small.
static inline size_t container_helper_command(const ContainerHelper *h, const char **args, size_t count, size_t capacity,
                                       const char *const *helper_args) {
    const char *fixed[] = {"/bin/sh", "-c", container_helper_script, "outer-container-helper", h->id, h->name,
                           h->hash[0], h->size[0], h->hash[1], h->size[1]};
    size_t extra = 0;
    while (helper_args[extra]) extra++;
    if (count + sizeof(fixed) / sizeof(*fixed) + extra + 1 > capacity) return 0;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(*fixed); i++) args[count++] = fixed[i];
    for (size_t i = 0; i < extra; i++) args[count++] = helper_args[i];
    args[count] = NULL;
    return count;
}

// Interprets the script's first line. Returns 1 when the helper is running,
// 2 when the helper for *architecture must be written to standard input, and
// 0 on failure, with the reason in message.
static inline int container_helper_reply(const char *line, int *architecture, char *message, size_t capacity) {
    if (!strcmp(line, "R")) return 1;
    for (int i = 0; i < 2; i++) {
        if (!strncmp(line, "U ", 2) && !strcmp(line + 2, container_helper_architectures[i])) {
            *architecture = i;
            return 2;
        }
    }
    snprintf(message, capacity, "%s", !strncmp(line, "X ", 2) ? line + 2 : "The container helper could not start.");
    return 0;
}

#endif
