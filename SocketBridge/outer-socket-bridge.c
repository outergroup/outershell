#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define OSB_MAGIC 0x3142524fu
#define OSB_VERSION 1u
#define OSB_MAX_PAYLOAD (1024u * 1024u)
#define OSB_CHUNK 65536u
#define OSB_KEEPALIVE_TIMEOUT_MS 60000u
#define OSB_POLL_INTERVAL_MS 1000u
#define OSB_SOCKET_MISSING_DEBOUNCE_MS 3000u
#define OSB_ALLOWLIST_MAX_LINE 4096u

#ifdef __APPLE__
#define OSB_USER_ALLOWLIST_SUFFIX "/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow"
#define OSB_SYSTEM_ALLOWLIST "/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow"
#define OSB_SYSTEM_RUNTIME_DIR "/var/run"
#else
#define OSB_USER_ALLOWLIST_SUFFIX "/.config/outerloop/http-unix.allow"
#define OSB_SYSTEM_ALLOWLIST "/etc/outerloop/http-unix.allow"
#define OSB_SYSTEM_RUNTIME_DIR "/run"
#endif

static int copy_format(char *out, size_t out_len, const char *format, const char *value) {
    int written = snprintf(out, out_len, format, value);
    return written >= 0 && (size_t)written < out_len ? 0 : -1;
}

enum {
    OSB_HELLO = 1,
    OSB_OPEN = 2,
    OSB_OPEN_OK = 3,
    OSB_OPEN_ERROR = 4,
    OSB_DATA = 5,
    OSB_EOF = 6,
    OSB_CLOSE = 7,
    OSB_PING = 8,
    OSB_PONG = 9,
    OSB_SOCKET_STATE = 10
};

struct frame {
    uint16_t type;
    uint32_t stream_id;
    uint32_t length;
    unsigned char *payload;
};

struct stream {
    uint32_t id;
    int fd;
};

enum socket_scope {
    SOCKET_SCOPE_USER,
    SOCKET_SCOPE_SYSTEM
};

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_frame(uint16_t type, uint32_t stream_id, const void *payload, uint32_t length) {
    unsigned char header[16];
    wr32(header, OSB_MAGIC);
    wr16(header + 4, OSB_VERSION);
    wr16(header + 6, type);
    wr32(header + 8, stream_id);
    wr32(header + 12, length);
    if (write_all(STDOUT_FILENO, header, sizeof(header)) < 0) return -1;
    if (length > 0 && write_all(STDOUT_FILENO, payload, length) < 0) return -1;
    return 0;
}

static int read_frame(struct frame *f) {
    unsigned char header[16];
    int r = read_all(STDIN_FILENO, header, sizeof(header));
    if (r <= 0) return r;
    if (rd32(header) != OSB_MAGIC || rd16(header + 4) != OSB_VERSION) return -1;
    f->type = rd16(header + 6);
    f->stream_id = rd32(header + 8);
    f->length = rd32(header + 12);
    f->payload = NULL;
    if (f->length > OSB_MAX_PAYLOAD) return -1;
    if (f->length > 0) {
        f->payload = (unsigned char *)malloc(f->length);
        if (!f->payload) return -1;
        r = read_all(STDIN_FILENO, f->payload, f->length);
        if (r <= 0) {
            free(f->payload);
            f->payload = NULL;
            return r;
        }
    }
    return 1;
}

static char *trim_line(char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return line;
}

static int path_has_prefix_component(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/';
}

static int current_home_directory(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        return copy_format(out, out_len, "%s", home);
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) {
        return copy_format(out, out_len, "%s", pw->pw_dir);
    }
    return -1;
}

static int user_allowlist_path(char *out, size_t out_len) {
#ifndef __APPLE__
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        return copy_format(out, out_len, "%s/outerloop/http-unix.allow", xdg);
    }
#endif
    char home[PATH_MAX];
    if (current_home_directory(home, sizeof(home)) < 0) return -1;
    return copy_format(out, out_len, "%s" OSB_USER_ALLOWLIST_SUFFIX, home);
}

static int user_runtime_dir(char *out, size_t out_len) {
#ifdef __APPLE__
    size_t required = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (required > 0 && required < out_len && confstr(_CS_DARWIN_USER_TEMP_DIR, out, required) > 0 && out[0]) {
        size_t n = strlen(out);
        if (n > 1 && out[n - 1] == '/') out[n - 1] = '\0';
        return 0;
    }
    snprintf(out, out_len, "/tmp");
    return 0;
#else
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_len, "%s", runtime);
        return 0;
    }
    snprintf(out, out_len, "/run/user/%ld", (long)getuid());
    return 0;
#endif
}

static int expand_allowlist_path(const char *value, char *out, size_t out_len) {
    char runtime[PATH_MAX];
    char uid[32];
    if (user_runtime_dir(runtime, sizeof(runtime)) < 0) return -1;
    snprintf(uid, sizeof(uid), "%ld", (long)getuid());

    size_t used = 0;
    for (const char *p = value; *p; p++) {
        const char *replacement = NULL;
        char literal[2] = {*p, '\0'};
        if (*p == '%' && p[1]) {
            p++;
            switch (*p) {
            case 't':
                replacement = runtime;
                break;
            case 'T':
                replacement = OSB_SYSTEM_RUNTIME_DIR;
                break;
            case 'u':
                replacement = uid;
                break;
            case '%':
                replacement = "%";
                break;
            default:
                return -1;
            }
        } else {
            replacement = literal;
        }
        size_t n = strlen(replacement);
        if (used + n >= out_len) return -1;
        memcpy(out + used, replacement, n);
        used += n;
    }
    out[used] = '\0';
    return 0;
}

static int allowlist_file_is_safe(const char *path, uid_t owner_uid) {
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (st.st_uid != owner_uid) return 0;
    if ((st.st_mode & 0022) != 0) return 0;
    return 1;
}

static int classify_socket(const char *path, const struct stat *st) {
#ifndef __APPLE__
    char user_run_prefix[64];
    snprintf(user_run_prefix, sizeof(user_run_prefix), "/run/user/%ld", (long)getuid());
    if (path_has_prefix_component(path, user_run_prefix) && st->st_uid == getuid()) {
        return SOCKET_SCOPE_USER;
    }
#else
    char runtime[PATH_MAX];
    if (user_runtime_dir(runtime, sizeof(runtime)) == 0 &&
        path_has_prefix_component(path, runtime) &&
        st->st_uid == getuid()) {
        return SOCKET_SCOPE_USER;
    }
#endif
    if (st->st_uid == getuid() && geteuid() != 0 && !path_has_prefix_component(path, OSB_SYSTEM_RUNTIME_DIR)) {
        return SOCKET_SCOPE_USER;
    }
    return SOCKET_SCOPE_SYSTEM;
}

static int allowlist_contains_socket(const char *allowlist_path,
                                     uid_t owner_uid,
                                     const char *socket_path,
                                     char *err,
                                     size_t err_len) {
    if (!allowlist_file_is_safe(allowlist_path, owner_uid)) {
        snprintf(err, err_len, "allowlist is missing or unsafe");
        return 0;
    }
    FILE *file = fopen(allowlist_path, "r");
    if (!file) {
        snprintf(err, err_len, "failed to read allowlist: %s", strerror(errno));
        return 0;
    }
    char line[OSB_ALLOWLIST_MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        char *trimmed = trim_line(line);
        if (!trimmed[0] || trimmed[0] == '#') continue;
        char *comment = strchr(trimmed, '#');
        if (comment) {
            *comment = '\0';
            trimmed = trim_line(trimmed);
            if (!trimmed[0]) continue;
        }
        char expanded[PATH_MAX];
        if (expand_allowlist_path(trimmed, expanded, sizeof(expanded)) < 0) continue;
        if (strcmp(expanded, socket_path) == 0) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    snprintf(err, err_len, "socket is not allowed");
    return 0;
}

static int validate_socket(const char *path, char *err, size_t err_len) {
    struct stat st;
    if (!path || path[0] != '/') {
        snprintf(err, err_len, "socket path must be absolute");
        return -1;
    }
    if (lstat(path, &st) < 0) {
        snprintf(err, err_len, "lstat failed: %s", strerror(errno));
        return -1;
    }
    if (!S_ISSOCK(st.st_mode)) {
        snprintf(err, err_len, "target is not a Unix socket");
        return -1;
    }
    int scope = classify_socket(path, &st);
    if (scope == SOCKET_SCOPE_USER) {
        char allowlist[PATH_MAX];
        if (user_allowlist_path(allowlist, sizeof(allowlist)) < 0) {
            snprintf(err, err_len, "could not resolve user allowlist path");
            return -1;
        }
        if (!allowlist_contains_socket(allowlist, getuid(), path, err, err_len)) return -1;
    } else {
        if (!allowlist_contains_socket(OSB_SYSTEM_ALLOWLIST, 0, path, err, err_len)) return -1;
        if (geteuid() != 0) {
            snprintf(err, err_len, "root/system socket requires root bridge");
            return -1;
        }
    }
    return 0;
}

static int authorize_socket_command(const char *path) {
    char err[512];
    if (validate_socket(path, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 3;
    }
    printf("allowed %s\n", path);
    return 0;
}

static void list_allowlist(const char *allowlist_path, uid_t owner_uid, const char *scope) {
    if (!allowlist_file_is_safe(allowlist_path, owner_uid)) return;
    FILE *file = fopen(allowlist_path, "r");
    if (!file) return;
    char line[OSB_ALLOWLIST_MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        char *trimmed = trim_line(line);
        if (!trimmed[0] || trimmed[0] == '#') continue;
        char *comment = strchr(trimmed, '#');
        if (comment) {
            *comment = '\0';
            trimmed = trim_line(trimmed);
            if (!trimmed[0]) continue;
        }
        char expanded[PATH_MAX];
        if (expand_allowlist_path(trimmed, expanded, sizeof(expanded)) < 0) continue;
        struct stat st;
        int available = lstat(expanded, &st) == 0 && S_ISSOCK(st.st_mode);
        printf("%s\t%s\t%s\n", scope, expanded, available ? "available" : "missing");
    }
    fclose(file);
}

static int list_command(void) {
    char user_path[PATH_MAX];
    if (user_allowlist_path(user_path, sizeof(user_path)) == 0) {
        list_allowlist(user_path, getuid(), "user");
    }
    list_allowlist(OSB_SYSTEM_ALLOWLIST, 0, "system");
    return 0;
}

static int connect_socket(const char *path, char *err, size_t err_len) {
    if (validate_socket(path, err, err_len) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, err_len, "socket failed: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        snprintf(err, err_len, "socket path is too long");
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(err, err_len, "connect failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int find_stream(struct stream *streams, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (streams[i].id == id) return (int)i;
    }
    return -1;
}

static void remove_stream(struct stream *streams, size_t *count, size_t index) {
    close(streams[index].fd);
    if (index + 1 < *count) {
        memmove(&streams[index], &streams[index + 1], (*count - index - 1) * sizeof(streams[0]));
    }
    (*count)--;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

static int socket_path_is_available(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int update_socket_state(const char *path,
                               int *reported_available,
                               uint64_t *missing_since_ms,
                               uint64_t now_ms) {
    if (socket_path_is_available(path)) {
        *missing_since_ms = 0;
        if (!*reported_available) {
            const unsigned char available = 1;
            if (send_frame(OSB_SOCKET_STATE, 0, &available, sizeof(available)) < 0) return -1;
            *reported_available = 1;
        }
        return 0;
    }

    if (!*reported_available) return 0;
    if (*missing_since_ms == 0) {
        *missing_since_ms = now_ms == 0 ? 1 : now_ms;
        return 0;
    }
    if (now_ms < *missing_since_ms ||
        now_ms - *missing_since_ms < OSB_SOCKET_MISSING_DEBOUNCE_MS) {
        return 0;
    }

    const unsigned char available = 0;
    if (send_frame(OSB_SOCKET_STATE, 0, &available, sizeof(available)) < 0) return -1;
    *reported_available = 0;
    *missing_since_ms = 0;
    return 0;
}

int main(int argc, char **argv) {
    const char *socket_path = NULL;
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("outer-socket-bridge 1");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        return list_command();
    }
    if (argc == 4 && strcmp(argv[1], "authorize") == 0 && strcmp(argv[2], "--socket") == 0) {
        return authorize_socket_command(argv[3]);
    }
    if (argc == 2 && strcmp(argv[1], "watch") == 0) {
        return list_command();
    }
    if (argc == 4 && strcmp(argv[1], "bridge") == 0 && strcmp(argv[2], "--socket") == 0) {
        socket_path = argv[3];
    } else if (argc == 3 && strcmp(argv[1], "--socket") == 0) {
        socket_path = argv[2];
    } else {
        fprintf(stderr, "usage: outer-socket-bridge list|watch|authorize --socket PATH|bridge --socket PATH\n");
        fprintf(stderr, "compatibility: outer-socket-bridge --socket PATH\n");
        return 2;
    }
    char err[512];
    if (validate_socket(socket_path, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    if (send_frame(OSB_HELLO, 0, NULL, 0) < 0) return 1;

    /*
     * Each active stream is one logical client connection from the caller,
     * mapped to one Unix-domain socket fd connected to socket_path.
     */
    struct stream streams[1024];
    size_t stream_count = 0;
    unsigned char buf[OSB_CHUNK];
    uint64_t last_frame_ms = monotonic_ms();
    int reported_socket_available = 1;
    uint64_t socket_missing_since_ms = 0;

    for (;;) {
        struct pollfd pfds[1025];
        size_t polled_stream_count = stream_count;
        memset(pfds, 0, sizeof(pfds));

        /*
         * pfds[0] watches stdin for framed commands from the parent process.
         * pfds[1...] watch the Unix-socket fds for bytes flowing back.
         */
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        for (size_t i = 0; i < polled_stream_count; i++) {
            pfds[i + 1].fd = streams[i].fd;
            pfds[i + 1].events = POLLIN;
        }
        uint64_t now_ms = monotonic_ms();
        if (last_frame_ms > 0 && now_ms > last_frame_ms &&
            now_ms - last_frame_ms >= OSB_KEEPALIVE_TIMEOUT_MS) {
            return 0;
        }

        int timeout_ms = OSB_POLL_INTERVAL_MS;
        if (last_frame_ms > 0 && now_ms >= last_frame_ms) {
            uint64_t remaining_ms = OSB_KEEPALIVE_TIMEOUT_MS - (now_ms - last_frame_ms);
            if (remaining_ms < (uint64_t)timeout_ms) timeout_ms = (int)remaining_ms;
            if (timeout_ms < 1) timeout_ms = 1;
        }

        nfds_t poll_count = (nfds_t)(polled_stream_count + 1);
        int pr = poll(pfds, poll_count, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (update_socket_state(socket_path,
                                &reported_socket_available,
                                &socket_missing_since_ms,
                                monotonic_ms()) < 0) {
            return 1;
        }
        if (pr == 0) continue;

        /* Forward data from Unix sockets back to stdout as OSB_DATA frames.
         * Walk backward so removing a stream can only compact entries that have
         * already been processed for this poll result.
         */
        for (size_t i = polled_stream_count; i > 0; i--) {
            size_t idx = i - 1;
            if (!(pfds[idx + 1].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            ssize_t n = read(streams[idx].fd, buf, sizeof(buf));
            if (n > 0) {
                if (send_frame(OSB_DATA, streams[idx].id, buf, (uint32_t)n) < 0) return 1;
            } else {
                send_frame(OSB_EOF, streams[idx].id, NULL, 0);
                send_frame(OSB_CLOSE, streams[idx].id, NULL, 0);
                remove_stream(streams, &stream_count, idx);
            }
        }

        /* Handle framed commands from stdin: open streams, write data, or close. */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            struct frame f;
            int rr = read_frame(&f);
            if (rr == 0) return 0;
            if (rr < 0) return 1;
            last_frame_ms = monotonic_ms();

            if (f.type == OSB_OPEN) {
                if (stream_count >= 1024) {
                    const char *msg = "too many streams";
                    send_frame(OSB_OPEN_ERROR, f.stream_id, msg, (uint32_t)strlen(msg));
                } else {
                    int fd = connect_socket(socket_path, err, sizeof(err));
                    if (fd < 0) {
                        send_frame(OSB_OPEN_ERROR, f.stream_id, err, (uint32_t)strlen(err));
                    } else {
                        streams[stream_count].id = f.stream_id;
                        streams[stream_count].fd = fd;
                        stream_count++;
                        send_frame(OSB_OPEN_OK, f.stream_id, NULL, 0);
                    }
                }
            } else {
                int idx = find_stream(streams, stream_count, f.stream_id);
                if (idx < 0) {
                    if (f.stream_id != 0) {
                        send_frame(OSB_CLOSE, f.stream_id, NULL, 0);
                    }
                } else {
                    switch (f.type) {
                    case OSB_DATA:
                        if (write_all(streams[idx].fd, f.payload, f.length) < 0) {
                            send_frame(OSB_CLOSE, f.stream_id, NULL, 0);
                            remove_stream(streams, &stream_count, (size_t)idx);
                        }
                        break;
                    case OSB_EOF:
                        shutdown(streams[idx].fd, SHUT_WR);
                        break;
                    case OSB_CLOSE:
                        remove_stream(streams, &stream_count, (size_t)idx);
                        break;
                    case OSB_PING:
                        send_frame(OSB_PONG, f.stream_id, f.payload, f.length);
                        break;
                    }
                }
            }
            free(f.payload);
        }
    }
}
