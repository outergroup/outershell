#define _DARWIN_C_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "OuterShellPlatform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

bool queue_all(int fd, const void *data, size_t len) {
    const char *bytes = (const char *)data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

int64_t monotonic_milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool set_fd_nonblocking(int fd, bool nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    int new_flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (new_flags == flags) return true;
    return fcntl(fd, F_SETFL, new_flags) == 0;
}

bool mkdir_p(const char *path) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path ? path : "");
    size_t len = strlen(copy);
    if (len == 0) return false;
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir(copy, 0755) == 0 || errno == EEXIST;
}

const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

void expand_tilde_path(const char *path, char *out, size_t out_size) {
    if (!path || !path[0]) {
        out[0] = '\0';
    } else if (strcmp(path, "~") == 0) {
        snprintf(out, out_size, "%s", home_directory());
    } else if (path[0] == '~' && path[1] == '/') {
        snprintf(out, out_size, "%s/%s", home_directory(), path + 2);
    } else {
        snprintf(out, out_size, "%s", path);
    }
}

static bool append_url_encoded(StringBuilder *builder, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    while (*p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            char one[2] = {(char)*p, '\0'};
            if (!sb_append(builder, one)) return false;
        } else {
            char escaped[4];
            snprintf(escaped, sizeof(escaped), "%%%02X", *p);
            if (!sb_append(builder, escaped)) return false;
        }
        p++;
    }
    return true;
}

static void normalized_architecture(char *out, size_t out_size) {
    struct utsname names;
    if (uname(&names) != 0) {
        snprintf(out, out_size, "unknown");
        return;
    }
    if (strcmp(names.machine, "x86_64") == 0 || strcmp(names.machine, "amd64") == 0) {
        snprintf(out, out_size, "x86_64");
        return;
    }
    if (strcmp(names.machine, "aarch64") == 0 || strcmp(names.machine, "arm64") == 0) {
        snprintf(out, out_size, "arm64");
        return;
    }
    snprintf(out, out_size, "%s", names.machine);
}

static void coarsen_version(const char *input, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!input || !input[0]) {
        snprintf(out, out_size, "unknown");
        return;
    }

    size_t offset = 0;
    int dot_count = 0;
    for (const char *p = input; *p && offset + 1 < out_size; p++) {
        if (isdigit((unsigned char)*p)) {
            out[offset++] = *p;
            continue;
        }
        if (*p == '.' && dot_count == 0 && offset > 0 && offset + 1 < out_size) {
            out[offset++] = '.';
            dot_count++;
            continue;
        }
        break;
    }
    if (offset == 0 || (offset == 1 && out[0] == '.')) {
        snprintf(out, out_size, "unknown");
    } else {
        out[offset] = '\0';
    }
}

static void operating_system_version(char *out, size_t out_size) {
#ifdef __APPLE__
    char version[64] = "";
    size_t version_size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &version_size, NULL, 0) == 0 && version[0]) {
        coarsen_version(version, out, out_size);
        return;
    }
#endif
    struct utsname names;
    if (uname(&names) == 0) {
        coarsen_version(names.release, out, out_size);
    } else {
        snprintf(out, out_size, "unknown");
    }
}

#ifndef __APPLE__
static bool systemd_major_version(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';

    FILE *pipe = popen("systemctl --version 2>/dev/null", "r");
    if (!pipe) return false;

    char line[256] = "";
    bool found = false;
    if (fgets(line, sizeof(line), pipe)) {
        const char *p = line;
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (isdigit((unsigned char)*p)) {
            size_t offset = 0;
            while (isdigit((unsigned char)*p) && offset + 1 < out_size) {
                out[offset++] = *p++;
            }
            out[offset] = '\0';
            found = offset > 0;
        }
    }

    int status = pclose(pipe);
    if (status != 0 || !found) {
        out[0] = '\0';
        return false;
    }
    return true;
}
#endif

bool outer_shell_append_update_query(StringBuilder *builder, const char *heartbeat, const char *app_version) {
    char arch[64];
    char os_version[64];
    normalized_architecture(arch, sizeof(arch));
    operating_system_version(os_version, sizeof(os_version));
#ifdef __APPLE__
    const char *os = "macos";
#else
    const char *os = "linux";
#endif

    if (!sb_append(builder, "heartbeat=") ||
        !append_url_encoded(builder, heartbeat && heartbeat[0] ? heartbeat : "extra") ||
        !sb_append(builder, "&os=") ||
        !append_url_encoded(builder, os) ||
        !sb_append(builder, "&osVersion=") ||
        !append_url_encoded(builder, os_version) ||
        !sb_append(builder, "&arch=") ||
        !append_url_encoded(builder, arch)) {
        return false;
    }

    if (app_version && app_version[0]) {
        if (!sb_append(builder, "&appVersion=") ||
            !append_url_encoded(builder, app_version)) {
            return false;
        }
    }
#ifndef __APPLE__
    char systemd_version[32];
    if (systemd_major_version(systemd_version, sizeof(systemd_version))) {
        if (!sb_append(builder, "&systemd=") ||
            !append_url_encoded(builder, systemd_version)) {
            return false;
        }
    }
#endif
    return true;
}
