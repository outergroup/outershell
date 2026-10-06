#ifndef OUTER_LOOP_LAUNCHD_SOCKET_H
#define OUTER_LOOP_LAUNCHD_SOCKET_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define OUTER_LAUNCHD_PREFIX "/launchd/"
#define OUTER_LAUNCHD_KEY_PREFIX "OUTERSHELL_SOCKET_"

/* A service address is an identity, not a filesystem path. */
static inline bool outer_launchd_address(const char *address, bool *system, const char **label) {
    if (!address || strncmp(address, OUTER_LAUNCHD_PREFIX, 9) != 0) return false;
    const char *value = address + 9;
    if (strncmp(value, "user/", 5) == 0) { *system = false; value += 5; }
    else if (strncmp(value, "system/", 7) == 0) { *system = true; value += 7; }
    else return false;
    size_t length = strlen(value);
    if (!length || length > 128 || strcmp(value, ".") == 0 || strcmp(value, "..") == 0) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    *label = value;
    return true;
}

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <crt_externs.h>

static inline int outer_launchd_run(char *const arguments[]) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int result = posix_spawn(&pid, "/bin/launchctl", &actions, NULL, arguments, *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return -1;
    int status;
    for (unsigned attempt = 0; attempt < 500; attempt++) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (waited < 0 && errno != EINTR) return -1;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return -1;
}

static inline bool outer_launchd_resolve_with_key_prefix(const char *address, const char *key_prefix, char *out, size_t capacity,
                                        char *error, size_t error_capacity) {
    bool system;
    const char *label;
    if (!outer_launchd_address(address, &system, &label)) {
        snprintf(error, error_capacity, "Invalid launchd service address.");
        return false;
    }
    if (system && geteuid() != 0) {
        snprintf(error, error_capacity, "Resolving a system service requires the root socket bridge.");
        return false;
    }
    char domain[64], target[256];
    if (system) snprintf(domain, sizeof(domain), "system");
    else snprintf(domain, sizeof(domain), "gui/%u", getuid());
    snprintf(target, sizeof(target), "%s/%s", domain, label);
    char command[] = "launchctl", print_command[] = "print";
    char *check[] = {command, print_command, target, NULL};
    if (outer_launchd_run(check) != 0) {
        snprintf(error, error_capacity, "The launchd service %s is not loaded in %s.", label, domain);
        return false;
    }
    char directory[] = "/private/tmp/outershell-resolve.XXXXXX";
    if (!mkdtemp(directory)) {
        snprintf(error, error_capacity, "Could not create socket resolver directory: %s", strerror(errno));
        return false;
    }
    char plist[PATH_MAX], output[PATH_MAX], resolver[256], resolver_target[320];
    snprintf(plist, sizeof(plist), "%s/resolver.plist", directory);
    snprintf(output, sizeof(output), "%s/socket", directory);
    snprintf(resolver, sizeof(resolver), "org.outershell.resolve.%s", strrchr(directory, '.') + 1);
    snprintf(resolver_target, sizeof(resolver_target), "%s/%s", domain, resolver);
    FILE *file = fopen(plist, "w");
    bool loaded = false, success = false;
    if (file) {
        fprintf(file, "<plist version=\"1.0\"><dict>"
                "<key>Label</key><string>%s</string>"
                "<key>ProgramArguments</key><array><string>/usr/bin/printenv</string><string>%s%s</string></array>"
                "<key>RunAtLoad</key><true/>"
                "<key>StandardOutPath</key><string>%s</string>"
                "</dict></plist>", resolver, key_prefix, label, output);
        bool written = !ferror(file);
        if (fclose(file) != 0) written = false;
        char bootstrap_command[] = "bootstrap";
        char *bootstrap[] = {command, bootstrap_command, domain, plist, NULL};
        loaded = written && outer_launchd_run(bootstrap) == 0;
    }
    for (unsigned attempt = 0; loaded && attempt < 100; attempt++) {
        file = fopen(output, "r");
        if (file) {
            if (fgets(out, (int)capacity, file)) {
                size_t length = strlen(out);
                if (length && out[length - 1] == '\n') {
                    out[length - 1] = '\0';
                    struct stat socket_info;
                    success = out[0] == '/' && lstat(out, &socket_info) == 0 &&
                        S_ISSOCK(socket_info.st_mode) && socket_info.st_uid == (system ? 0 : getuid());
                    fclose(file);
                    break;
                }
            }
            fclose(file);
        }
        usleep(10000);
    }
    if (loaded) {
        char bootout_command[] = "bootout";
        char *bootout[] = {command, bootout_command, resolver_target, NULL};
        if (outer_launchd_run(bootout) != 0) success = false;
    }
    unlink(output);
    unlink(plist);
    rmdir(directory);
    if (!success) snprintf(error, error_capacity, "The launchd service %s has no available socket. Reinstall the app if its socket configuration is outdated.", label);
    return success;
}
static inline bool outer_launchd_resolve(const char *address, char *out, size_t capacity,
                                        char *error, size_t error_capacity) {
    return outer_launchd_resolve_with_key_prefix(address, OUTER_LAUNCHD_KEY_PREFIX,
                                                out, capacity, error, error_capacity);
}
#endif
#endif
