#ifndef OUTERSHELL_PATHS_H
#define OUTERSHELL_PATHS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline const char *outer_shell_home_directory(void) {
    const char *home = getenv("HOME");
    return home && home[0] ? home : "";
}

static inline void outer_shell_expand_tilde_path(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!path) {
        out[0] = '\0';
        return;
    }
    if (strcmp(path, "~") == 0) {
        snprintf(out, out_size, "%s", outer_shell_home_directory());
    } else if (path[0] == '~' && path[1] == '/') {
        snprintf(out, out_size, "%s/%s", outer_shell_home_directory(), path + 2);
    } else {
        snprintf(out, out_size, "%s", path);
    }
}

static inline void outer_shell_default_system_api_socket_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    snprintf(out, out_size, "/var/run/outershelld-api");
#else
    snprintf(out, out_size, "/run/outershelld-api");
#endif
}

static inline void outer_shell_default_api_socket_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    const char *env_path = getenv("OUTERSHELLD_API_SOCKET");
    if (env_path && env_path[0]) {
        outer_shell_expand_tilde_path(env_path, out, out_size);
        return;
    }
    if (geteuid() == 0) {
        outer_shell_default_system_api_socket_path(out, out_size);
        return;
    }
#ifdef __APPLE__
    const char *tmp = getenv("DARWIN_USER_TEMP_DIR");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) {
        snprintf(out, out_size, "%s%soutershelld-api", tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/");
        return;
    }
    snprintf(out, out_size, "/tmp/outershelld-api-%d", (int)getuid());
#else
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_size, "%s/outershelld-api", runtime);
        return;
    }
    snprintf(out, out_size, "/run/user/%d/outershelld-api", (int)getuid());
#endif
}

#endif
