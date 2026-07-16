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
                                 "Vary: Outerframe-Accept\r\n"
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

static bool regular_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool value_contains_case_insensitive(const char *value,
                                            size_t value_length,
                                            const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0 || value_length < needle_length) return false;
    for (size_t i = 0; i + needle_length <= value_length; i++) {
        if (strncasecmp(value + i, needle, needle_length) == 0) return true;
    }
    return false;
}

static bool request_accepts_outerframe(const char *request) {
    const char *line = strstr(request, "\r\n");
    while (line) {
        line += 2;
        if (line[0] == '\r' && line[1] == '\n') break;
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon && (size_t)(colon - line) == strlen("Outerframe-Accept") &&
            strncasecmp(line, "Outerframe-Accept", strlen("Outerframe-Accept")) == 0) {
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            return value_contains_case_insensitive(value,
                                                   (size_t)(end - value),
                                                   "application/vnd.outerframe");
        }
        line = end;
    }
    return false;
}

static const char *content_type_for_path(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return "application/octet-stream";
    if (strcasecmp(extension, ".html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(extension, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(extension, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcasecmp(extension, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(extension, ".png") == 0) return "image/png";
    if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(extension, ".webp") == 0) return "image/webp";
    if (strcasecmp(extension, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(extension, ".json") == 0) return "application/json";
    return "application/octet-stream";
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

static void write_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xff);
    out[1] = (unsigned char)((value >> 8) & 0xff);
    out[2] = (unsigned char)((value >> 16) & 0xff);
    out[3] = (unsigned char)((value >> 24) & 0xff);
}

static void serve_hello(int fd) {
    const char *message = "Hello from your C backend!";
    char hostname[256] = "";
    char os[64] = "";
    char now[64] = "";
    time_t t = time(NULL);
    struct tm tm_value;

    (void)gethostname(hostname, sizeof(hostname) - 1);
    snprintf(os, sizeof(os), "%s/%s", platform_os(), platform_arch());
    if (gmtime_r(&t, &tm_value)) {
        strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
    }

    const char *strings[] = { message, hostname, os, now };
    uint32_t offsets[4];
    uint32_t lengths[4];
    size_t body_length = 32;
    for (size_t i = 0; i < 4; i++) {
        size_t length = strlen(strings[i]);
        if (length > UINT32_MAX || body_length > SIZE_MAX - length) {
            send_text(fd, 500, "Internal Server Error", "response too large\n");
            return;
        }
        if (body_length > UINT32_MAX) {
            send_text(fd, 500, "Internal Server Error", "response too large\n");
            return;
        }
        offsets[i] = (uint32_t)body_length;
        lengths[i] = (uint32_t)length;
        body_length += length;
    }

    unsigned char *body = malloc(body_length);
    if (!body) {
        send_text(fd, 500, "Internal Server Error", "response too large\n");
        return;
    }
    for (size_t i = 0; i < 4; i++) {
        write_u32_le(body + (i * 8), offsets[i]);
        write_u32_le(body + (i * 8) + 4, lengths[i]);
    }
    for (size_t i = 0; i < 4; i++) {
        memcpy(body + offsets[i], strings[i], lengths[i]);
    }

    send_response(fd,
                  200,
                  "OK",
                  "application/octet-stream",
                  body,
                  body_length);
    free(body);
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
    char *request_path = (char *)origin_form_path(path);
    char *query = strpbrk(request_path, "?#");
    if (query) *query = '\0';

    if (strcmp(request_path, "/") == 0) {
        char outer_path[PATH_MAX];
        char html_path[PATH_MAX];
        snprintf(outer_path, sizeof(outer_path), "%s/app.outer", root);
        snprintf(html_path, sizeof(html_path), "%s/web/index.html", root);
        if (request_accepts_outerframe(request) && regular_file_exists(outer_path)) {
            serve_file(fd, outer_path, "application/vnd.outerframe");
        } else if (regular_file_exists(html_path)) {
            serve_file(fd, html_path, "text/html; charset=utf-8");
        } else if (regular_file_exists(outer_path)) {
            send_text(fd, 406, "Not Acceptable", "This app requires an outerframe-aware browser.\n");
        } else {
            send_text(fd, 404, "Not Found", "not found\n");
        }
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

    if (request_path[0] == '/' && request_path[1] != '\0' &&
        !strstr(request_path, "..") && !strchr(request_path, '\\')) {
        char web_path[PATH_MAX];
        int length = snprintf(web_path, sizeof(web_path), "%s/web%s", root, request_path);
        if (length > 0 && (size_t)length < sizeof(web_path) && regular_file_exists(web_path)) {
            serve_file(fd, web_path, content_type_for_path(web_path));
            return;
        }
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
    const char *host = argument_value(argc, argv, "--host");
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
    if (!host || !host[0]) host = "127.0.0.1";

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0) {
        fatal("sigaction: %s", strerror(errno));
    }

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
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            fatal("invalid IPv4 listen address: %s", host);
        }
        addr.sin_port = htons((uint16_t)port);
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            fatal("bind port %d: %s", port, strerror(errno));
        }
        fprintf(stderr, "listening on http://%s:%d/, serving %s\n", host, port, root);
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
