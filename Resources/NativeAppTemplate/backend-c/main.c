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
                                 "Cache-Control: no-store\r\n"
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

static bool request_header_value(const char *request,
                                 const char *header_name,
                                 const char **out_value,
                                 size_t *out_value_length) {
    const char *line = strstr(request, "\r\n");
    size_t header_name_length = strlen(header_name);
    while (line) {
        line += 2;
        if (line[0] == '\r' && line[1] == '\n') break;
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon && (size_t)(colon - line) == header_name_length &&
            strncasecmp(line, header_name, header_name_length) == 0) {
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
            *out_value = value;
            *out_value_length = (size_t)(end - value);
            return true;
        }
        line = end;
    }
    return false;
}

static long stat_mtime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#else
    return st->st_mtim.tv_nsec;
#endif
}

static long stat_ctime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_ctimespec.tv_nsec;
#else
    return st->st_ctim.tv_nsec;
#endif
}

static void file_etag(const struct stat *st, char *out, size_t out_size) {
    snprintf(out, out_size, "W/\"%llx-%lx-%llx-%lx-%llx\"",
             (unsigned long long)st->st_mtime,
             (unsigned long)stat_mtime_nanoseconds(st),
             (unsigned long long)st->st_ctime,
             (unsigned long)stat_ctime_nanoseconds(st),
             (unsigned long long)st->st_size);
}

static void format_http_date(time_t value, char *out, size_t out_size) {
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm date;
    if (!gmtime_r(&value, &date) || date.tm_wday < 0 || date.tm_wday > 6 ||
        date.tm_mon < 0 || date.tm_mon > 11) {
        snprintf(out, out_size, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    snprintf(out, out_size, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             weekdays[date.tm_wday], date.tm_mday, months[date.tm_mon], date.tm_year + 1900,
             date.tm_hour, date.tm_min, date.tm_sec);
}

static bool parse_http_date(const char *value, size_t value_length, time_t *out) {
    if (value_length == 0 || value_length >= 128) return false;
    char date_string[128];
    memcpy(date_string, value, value_length);
    date_string[value_length] = '\0';
    const char *formats[] = {
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%y %H:%M:%S GMT",
        "%a %b %e %H:%M:%S %Y"
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct tm date;
        memset(&date, 0, sizeof(date));
        char *end = strptime(date_string, formats[i], &date);
        if (end && *end == '\0') {
            date.tm_isdst = 0;
            time_t timestamp = timegm(&date);
            struct tm normalized;
            if (gmtime_r(&timestamp, &normalized) &&
                normalized.tm_year == date.tm_year &&
                normalized.tm_mon == date.tm_mon &&
                normalized.tm_mday == date.tm_mday &&
                normalized.tm_hour == date.tm_hour &&
                normalized.tm_min == date.tm_min &&
                normalized.tm_sec == date.tm_sec) {
                *out = timestamp;
                return true;
            }
        }
    }
    return false;
}

static void trim_optional_whitespace(const char **value, size_t *value_length) {
    while (*value_length > 0 && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*value_length)--;
    }
    while (*value_length > 0 &&
           ((*value)[*value_length - 1] == ' ' || (*value)[*value_length - 1] == '\t')) {
        (*value_length)--;
    }
}

static bool weak_etag_equal(const char *candidate,
                            size_t candidate_length,
                            const char *etag) {
    trim_optional_whitespace(&candidate, &candidate_length);
    if (candidate_length >= 2 && candidate[0] == 'W' && candidate[1] == '/') {
        candidate += 2;
        candidate_length -= 2;
    }
    const char *current = etag;
    size_t current_length = strlen(etag);
    if (current_length >= 2 && current[0] == 'W' && current[1] == '/') {
        current += 2;
        current_length -= 2;
    }
    return candidate_length == current_length &&
           memcmp(candidate, current, current_length) == 0;
}

static bool if_none_match_matches(const char *value, size_t value_length, const char *etag) {
    const char *cursor = value;
    const char *end = value + value_length;
    while (cursor < end) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == ',')) cursor++;
        if (cursor >= end) break;
        if (*cursor == '*') {
            const char *after = cursor + 1;
            while (after < end && (*after == ' ' || *after == '\t')) after++;
            if (after == end || *after == ',') return true;
        }
        const char *candidate = cursor;
        bool quoted = false;
        while (cursor < end) {
            if (*cursor == '"') quoted = !quoted;
            if (*cursor == ',' && !quoted) break;
            cursor++;
        }
        if (weak_etag_equal(candidate, (size_t)(cursor - candidate), etag)) return true;
        if (cursor < end) cursor++;
    }
    return false;
}

static bool file_is_not_modified(const char *request, const char *etag, time_t last_modified) {
    const char *condition = NULL;
    size_t condition_length = 0;
    bool has_if_none_match = request_header_value(request, "If-None-Match",
                                                   &condition, &condition_length);
    if (has_if_none_match) {
        return if_none_match_matches(condition, condition_length, etag);
    }
    if (request_header_value(request, "If-Modified-Since",
                             &condition, &condition_length)) {
        time_t modified_since;
        return parse_http_date(condition, condition_length, &modified_since) &&
               last_modified <= modified_since;
    }
    return false;
}

static void send_cached_file_header(int fd,
                                    int status,
                                    const char *content_type,
                                    size_t content_length,
                                    const char *etag,
                                    const char *last_modified,
                                    bool vary_outerframe_accept) {
    char header[1024];
    const char *vary = vary_outerframe_accept ? "Vary: Outerframe-Accept\r\n" : "";
    int header_length;
    if (status == 304) {
        header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 304 Not Modified\r\n"
                                 "Cache-Control: public, max-age=0, must-revalidate\r\n"
                                 "ETag: %s\r\n"
                                 "Last-Modified: %s\r\n"
                                 "%s"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 etag, last_modified, vary);
    } else {
        header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: %s\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Cache-Control: public, max-age=0, must-revalidate\r\n"
                                 "ETag: %s\r\n"
                                 "Last-Modified: %s\r\n"
                                 "%s"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 content_type, content_length, etag, last_modified, vary);
    }
    if (header_length > 0 && (size_t)header_length < sizeof(header)) {
        (void)write_all(fd, header, (size_t)header_length);
    }
}

static void serve_file(int fd,
                       const char *path,
                       const char *content_type,
                       const char *request,
                       bool send_body,
                       bool vary_outerframe_accept) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_text(fd, 404, "Not Found", "not found\n");
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_text(fd, 404, "Not Found", "not found\n");
        return;
    }

    char etag[96];
    char last_modified[64];
    file_etag(&st, etag, sizeof(etag));
    format_http_date(st.st_mtime, last_modified, sizeof(last_modified));
    if (file_is_not_modified(request, etag, st.st_mtime)) {
        close(file_fd);
        send_cached_file_header(fd, 304, content_type, 0, etag, last_modified,
                                vary_outerframe_accept);
        return;
    }

    size_t length = (size_t)st.st_size;
    if (!send_body) {
        close(file_fd);
        send_cached_file_header(fd, 200, content_type, length, etag, last_modified,
                                vary_outerframe_accept);
        return;
    }
    char *body = malloc(length == 0 ? 1 : length);
    if (!body) {
        close(file_fd);
        send_text(fd, 500, "Internal Server Error", "out of memory\n");
        return;
    }
    size_t offset = 0;
    while (offset < length) {
        ssize_t got = read(file_fd, body + offset, length - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(body);
            close(file_fd);
            send_text(fd, 500, "Internal Server Error", "failed to read file\n");
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    send_cached_file_header(fd, 200, content_type, offset, etag, last_modified,
                            vary_outerframe_accept);
    if (offset > 0) (void)write_all(fd, body, offset);
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
    bool send_body = strcmp(method, "HEAD") != 0;
    char *request_path = (char *)origin_form_path(path);
    char *query = strpbrk(request_path, "?#");
    if (query) *query = '\0';

    if (strcmp(request_path, "/") == 0) {
        char outer_path[PATH_MAX];
        char html_path[PATH_MAX];
        snprintf(outer_path, sizeof(outer_path), "%s/app.outer", root);
        snprintf(html_path, sizeof(html_path), "%s/web/index.html", root);
        if (request_accepts_outerframe(request) && regular_file_exists(outer_path)) {
            serve_file(fd, outer_path, "application/vnd.outerframe", request, send_body, true);
        } else if (regular_file_exists(html_path)) {
            serve_file(fd, html_path, "text/html; charset=utf-8", request, send_body, true);
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
        serve_file(fd, frontend_path, "application/octet-stream", request, send_body, false);
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
            serve_file(fd, web_path, content_type_for_path(web_path), request, send_body, false);
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
