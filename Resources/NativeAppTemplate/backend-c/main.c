#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BUFFER_SIZE 65536

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int signum) {
    (void)signum;
    g_stop = 1;
}

static void fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

static bool write_all(int fd, const void *data, size_t length) {
    const char *bytes = data;
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

static void send_response(int fd,
                          int status,
                          const char *reason,
                          const char *content_type,
                          const void *body,
                          size_t body_length) {
    char header[512];
    int header_length = snprintf(header,
                                 sizeof(header),
                                 "HTTP/1.1 %d %s\r\n"
                                 "Content-Type: %s\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 status,
                                 reason,
                                 content_type,
                                 body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) return;
    (void)write_all(fd, header, (size_t)header_length);
    if (body_length > 0) {
        (void)write_all(fd, body, body_length);
    }
}

static void send_text(int fd, int status, const char *reason, const char *body) {
    send_response(fd,
                  status,
                  reason,
                  "text/plain; charset=utf-8",
                  body,
                  strlen(body));
}

static bool safe_join(char *out, size_t out_size, const char *root, const char *relative) {
    if (!relative || relative[0] == '\0' || strstr(relative, "..") || strchr(relative, '/')) {
        return false;
    }
    int n = snprintf(out, out_size, "%s/%s", root, relative);
    return n >= 0 && (size_t)n < out_size;
}

static bool append_relative_path(char *out, size_t out_size, const char *root, const char *relative) {
    if (!root || !relative || relative[0] == '\0' || relative[0] == '/' || strstr(relative, "..")) {
        return false;
    }
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    if (root_length + 1 + relative_length + 1 > out_size) {
        return false;
    }
    memcpy(out, root, root_length);
    out[root_length] = '/';
    memcpy(out + root_length + 1, relative, relative_length + 1);
    return true;
}

static bool read_file(const char *path, char **out, size_t *out_length) {
    *out = NULL;
    *out_length = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return false;
    }
    size_t length = (size_t)st.st_size;
    char *buffer = malloc(length == 0 ? 1 : length);
    if (!buffer) {
        close(fd);
        return false;
    }

    size_t offset = 0;
    while (offset < length) {
        ssize_t got = read(fd, buffer + offset, length - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(fd);
            return false;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(fd);
    *out = buffer;
    *out_length = offset;
    return true;
}

static void serve_file(int fd, const char *path, const char *content_type) {
    char *body = NULL;
    size_t body_length = 0;
    if (!read_file(path, &body, &body_length)) {
        send_text(fd, 404, "Not Found", "not found\n");
        return;
    }
    send_response(fd, 200, "OK", content_type, body, body_length);
    free(body);
}

static void json_escape(char *out, size_t out_size, const char *input) {
    size_t offset = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && offset + 2 < out_size; p++) {
        if (*p == '"' || *p == '\\') {
            if (offset + 2 >= out_size) break;
            out[offset++] = '\\';
            out[offset++] = (char)*p;
        } else if (*p >= 0x20) {
            out[offset++] = (char)*p;
        }
    }
    if (out_size > 0) out[offset < out_size ? offset : out_size - 1] = '\0';
}

static const char *platform_os(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char *platform_arch(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__arm__)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "386";
#else
    return "unknown";
#endif
}

static void serve_hello(int fd) {
    char hostname[256] = "";
    char escaped_hostname[512] = "";
    char now[64] = "";
    time_t t = time(NULL);
    struct tm tm_value;

    (void)gethostname(hostname, sizeof(hostname) - 1);
    json_escape(escaped_hostname, sizeof(escaped_hostname), hostname);
    if (gmtime_r(&t, &tm_value)) {
        strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
    }

    char body[1024];
    int body_length = snprintf(body,
                               sizeof(body),
                               "{\"message\":\"Hello from your C backend!\","
                               "\"hostname\":\"%s\","
                               "\"os\":\"%s/%s\","
                               "\"time\":\"%s\"}\n",
                               escaped_hostname,
                               platform_os(),
                               platform_arch(),
                               now);
    if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
        send_text(fd, 500, "Internal Server Error", "response too large\n");
        return;
    }
    send_response(fd,
                  200,
                  "OK",
                  "application/json",
                  body,
                  (size_t)body_length);
}

static const char *origin_form_path(const char *raw_path) {
    const char *scheme = strstr(raw_path, "://");
    if (!scheme) return raw_path;

    const char *after_authority = strchr(scheme + 3, '/');
    return after_authority ? after_authority : "/";
}

static void handle_client(int fd, const char *root) {
    char request[BUFFER_SIZE];
    ssize_t got = read(fd, request, sizeof(request) - 1);
    if (got <= 0) return;
    request[got] = '\0';

    char method[16] = "";
    char path[1024] = "";
    if (sscanf(request, "%15s %1023s", method, path) != 2) {
        send_text(fd, 400, "Bad Request", "bad request\n");
        return;
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_text(fd, 405, "Method Not Allowed", "method not allowed\n");
        return;
    }
    const char *request_path = origin_form_path(path);

    if (strcmp(request_path, "/") == 0) {
        char outer_path[PATH_MAX];
        snprintf(outer_path, sizeof(outer_path), "%s/app.outer", root);
        serve_file(fd, outer_path, "application/vnd.outerframe");
        return;
    }

    if (strncmp(request_path, "/frontend/", 10) == 0) {
        const char *platform = request_path + 10;
        char frontend_path[PATH_MAX];
        char relative[PATH_MAX];
        if (!safe_join(relative, sizeof(relative), "frontends", platform)) {
            send_text(fd, 400, "Bad Request", "bad path\n");
            return;
        }
        if (!append_relative_path(frontend_path, sizeof(frontend_path), root, relative)) {
            send_text(fd, 400, "Bad Request", "bad path\n");
            return;
        }
        serve_file(fd, frontend_path, "application/octet-stream");
        return;
    }

    if (strcmp(request_path, "/api/hello") == 0) {
        serve_hello(fd);
        return;
    }

    send_text(fd, 404, "Not Found", "not found\n");
}

static const char *argument_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int argument_int(int argc, char **argv, const char *name) {
    const char *value = argument_value(argc, argv, name);
    if (!value) return 0;
    return atoi(value);
}

static void executable_directory(char *out, size_t out_size, char **argv) {
    char resolved[PATH_MAX];
    if (realpath(argv[0], resolved)) {
        char *slash = strrchr(resolved, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, out_size, "%s", resolved);
            return;
        }
    }
    snprintf(out, out_size, ".");
}

int main(int argc, char **argv) {
    const char *socket_path = argument_value(argc, argv, "--socket");
    int port = argument_int(argc, argv, "--port");
    const char *root_arg = argument_value(argc, argv, "--root");
    char root[PATH_MAX];

    if ((socket_path == NULL) == (port == 0)) {
        fatal("specify exactly one of --socket or --port");
    }
    if (root_arg && root_arg[0]) {
        snprintf(root, sizeof(root), "%s", root_arg);
    } else {
        executable_directory(root, sizeof(root), argv);
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    int listener = -1;
    if (socket_path) {
        if (unlink(socket_path) != 0 && errno != ENOENT) {
            fatal("cannot remove stale socket %s: %s", socket_path, strerror(errno));
        }
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) fatal("socket: %s", strerror(errno));

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(socket_path) >= sizeof(addr.sun_path)) {
            fatal("socket path is too long: %s", socket_path);
        }
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            fatal("bind %s: %s", socket_path, strerror(errno));
        }
        fprintf(stderr, "listening on unix socket %s, serving %s\n", socket_path, root);
    } else {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) fatal("socket: %s", strerror(errno));
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            fatal("bind port %d: %s", port, strerror(errno));
        }
        fprintf(stderr, "listening on http://127.0.0.1:%d/, serving %s\n", port, root);
    }

    if (listen(listener, 32) != 0) {
        fatal("listen: %s", strerror(errno));
    }

    while (!g_stop) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            fatal("accept: %s", strerror(errno));
        }
        handle_client(client, root);
        close(client);
    }

    close(listener);
    if (socket_path) {
        unlink(socket_path);
    }
    return 0;
}
