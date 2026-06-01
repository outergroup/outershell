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

