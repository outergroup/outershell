#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>
#include <pwd.h>

#define REQUEST_LIMIT 8192

static const char *k_bundle_url_path = "/bundles/NetworkInspectorContent";
static const char *k_bundle_url_path_macos_arm = "/bundles/NetworkInspectorContent/macos-arm";
static const char *k_bundle_url_path_macos_x86 = "/bundles/NetworkInspectorContent/macos-x86";

static char g_bundle_file_path_macos_arm[4096] = "bundles/NetworkInspectorContent.bundle.macos-arm.aar";
static char g_bundle_file_path_macos_x86[4096] = "bundles/NetworkInspectorContent.bundle.macos-x86.aar";
static char g_listen_socket_path[4096] = "";

static void write_u32_le(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
}

static void write_u64_le(unsigned char *bytes, uint64_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
    bytes[4] = (unsigned char)((value >> 32) & 0xff);
    bytes[5] = (unsigned char)((value >> 40) & 0xff);
    bytes[6] = (unsigned char)((value >> 48) & 0xff);
    bytes[7] = (unsigned char)((value >> 56) & 0xff);
}

static const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Error";
    }
}

static void send_response(int fd, int status, const char *content_type, const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Cache-Control: no-store\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status, status_text(status), content_type, body_len);
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        (void)write(fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        (void)write(fd, body, body_len);
    }
}

static void send_text(int fd, int status, const char *text) {
    send_response(fd, status, "text/plain; charset=utf-8", text, strlen(text));
}

static void send_head(int fd, int status, const char *content_type) {
    send_response(fd, status, content_type, NULL, 0);
}

static bool read_file(const char *path, unsigned char **out_data, size_t *out_len) {
    *out_data = NULL;
    *out_len = 0;

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0) {
        close(file_fd);
        return false;
    }

    size_t len = (size_t)st.st_size;
    unsigned char *data = malloc(len == 0 ? 1 : len);
    if (!data) {
        close(file_fd);
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        ssize_t n = read(file_fd, data + offset, len - offset);
        if (n <= 0) {
            free(data);
            close(file_fd);
            return false;
        }
        offset += (size_t)n;
    }

    close(file_fd);
    *out_data = data;
    *out_len = len;
    return true;
}

static void send_outer_descriptor(int fd) {
    size_t path_len = strlen(k_bundle_url_path);
    size_t header_len = 40;
    size_t total_len = header_len + path_len;
    unsigned char *payload = calloc(1, total_len);
    if (!payload) {
        send_text(fd, 500, "out of memory\n");
        return;
    }

    payload[0] = 'O';
    payload[1] = 'U';
    payload[2] = 'T';
    payload[3] = 'R';
    write_u32_le(payload + 4, 1);
    write_u64_le(payload + 8, (uint64_t)header_len);
    write_u64_le(payload + 16, (uint64_t)path_len);
    write_u64_le(payload + 24, (uint64_t)total_len);
    write_u64_le(payload + 32, 0);
    memcpy(payload + header_len, k_bundle_url_path, path_len);

    send_response(fd, 200, "application/vnd.outerframe", payload, total_len);
    free(payload);
}

static void send_bundle_file(int fd, const char *path) {
    unsigned char *data = NULL;
    size_t len = 0;
    if (!read_file(path, &data, &len)) {
        char message[4096 + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        send_text(fd, 404, message);
        return;
    }
    send_response(fd, 200, "application/octet-stream", data, len);
    free(data);
}

static void append_bytes(char **buffer, size_t *used, size_t *capacity, const char *bytes, size_t length) {
    if (*used + length + 1 > *capacity) {
        size_t next_capacity = *capacity;
        while (*used + length + 1 > next_capacity) {
            next_capacity *= 2;
        }
        char *next = realloc(*buffer, next_capacity);
        if (!next) {
            return;
        }
        *buffer = next;
        *capacity = next_capacity;
    }
    memcpy(*buffer + *used, bytes, length);
    *used += length;
    (*buffer)[*used] = '\0';
}

static void append_string(char **buffer, size_t *used, size_t *capacity, const char *string) {
    append_bytes(buffer, used, capacity, string, strlen(string));
}

static void append_escaped_field(char **buffer, size_t *used, size_t *capacity, const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        char out[4];
        if (*p == '\\') {
            append_string(buffer, used, capacity, "\\\\");
        } else if (*p == '\t') {
            append_string(buffer, used, capacity, "\\t");
        } else if (*p == '\n' || *p == '\r') {
            append_string(buffer, used, capacity, " ");
        } else if (*p < 32) {
            snprintf(out, sizeof(out), " ");
            append_string(buffer, used, capacity, out);
        } else {
            append_bytes(buffer, used, capacity, (const char *)p, 1);
        }
    }
}

static bool read_process_user(int pid, char *out, size_t out_size) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        unsigned int uid = 0;
        if (sscanf(line, "Uid:\t%u", &uid) == 1) {
            struct passwd *pw = getpwuid((uid_t)uid);
            if (pw && pw->pw_name) {
                snprintf(out, out_size, "%s", pw->pw_name);
            } else {
                snprintf(out, out_size, "%u", uid);
            }
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

static bool read_process_cmdline(int pid, char *out, size_t out_size) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        return false;
    }

    ssize_t n = read(file_fd, out, out_size - 1);
    close(file_fd);
    if (n <= 0) {
        return false;
    }

    out[n] = '\0';
    for (ssize_t i = 0; i < n; i++) {
        if (out[i] == '\0') {
            out[i] = ' ';
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        out[n - 1] = '\0';
        n--;
    }
    return out[0] != '\0';
}

static bool pid_already_seen(int pid, const int *pids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pids[i] == pid) {
            return true;
        }
    }
    return false;
}

static void append_process_metadata(char **buffer, size_t *used, size_t *capacity) {
    int pids[512];
    size_t pid_count = 0;
    const char *cursor = *buffer;

    while ((cursor = strstr(cursor, "pid=")) != NULL) {
        cursor += 4;
        char *end = NULL;
        long value = strtol(cursor, &end, 10);
        if (end != cursor && value > 0 && value <= INT32_MAX &&
            !pid_already_seen((int)value, pids, pid_count) &&
            pid_count < sizeof(pids) / sizeof(pids[0])) {
            pids[pid_count++] = (int)value;
        }
        cursor = (end && end > cursor) ? end : cursor + 1;
    }

    if (pid_count == 0) {
        return;
    }

    append_string(buffer, used, capacity, "# outerframe-network-inspector-meta-v1\n");
    for (size_t i = 0; i < pid_count; i++) {
        char line[64];
        char user[128] = "";
        char cmdline[4096] = "";
        read_process_user(pids[i], user, sizeof(user));
        read_process_cmdline(pids[i], cmdline, sizeof(cmdline));

        int n = snprintf(line, sizeof(line), "# meta\t%d\t", pids[i]);
        if (n > 0) {
            append_bytes(buffer, used, capacity, line, (size_t)n);
        }
        append_escaped_field(buffer, used, capacity, user);
        append_string(buffer, used, capacity, "\t");
        append_escaped_field(buffer, used, capacity, cmdline);
        append_string(buffer, used, capacity, "\n");
    }
}

static void send_socket_snapshot(int fd) {
    FILE *pipe = popen("ss -H -tuinap 2>&1", "r");
    if (!pipe) {
        send_text(fd, 500, "failed to run ss\n");
        return;
    }

    size_t capacity = 65536;
    size_t used = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        pclose(pipe);
        send_text(fd, 500, "out of memory\n");
        return;
    }

    while (!feof(pipe)) {
        if (used + 4096 + 1 > capacity) {
            size_t next_capacity = capacity * 2;
            char *next = realloc(buffer, next_capacity);
            if (!next) {
                free(buffer);
                pclose(pipe);
                send_text(fd, 500, "out of memory\n");
                return;
            }
            buffer = next;
            capacity = next_capacity;
        }
        size_t n = fread(buffer + used, 1, 4096, pipe);
        used += n;
        if (ferror(pipe)) {
            free(buffer);
            pclose(pipe);
            send_text(fd, 500, "failed to read ss output\n");
            return;
        }
    }

    int status = pclose(pipe);
    int http_status = 200;
    if (status == -1 || status != 0) {
        http_status = 500;
    }
    if (http_status == 200) {
        append_process_metadata(&buffer, &used, &capacity);
    }
    send_response(fd, http_status, "text/plain; charset=utf-8", buffer, used);
    free(buffer);
}

static bool url_decode(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    for (const char *p = input; *p; p++) {
        if (used + 1 >= output_size) {
            return false;
        }
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], '\0'};
            char *end = NULL;
            long value = strtol(hex, &end, 16);
            if (end != hex + 2 || value < 0 || value > 255) {
                return false;
            }
            output[used++] = (char)value;
            p += 2;
        } else if (*p == '+') {
            output[used++] = ' ';
        } else {
            output[used++] = *p;
        }
    }
    output[used] = '\0';
    return true;
}

static bool query_value(const char *query, const char *name, char *out, size_t out_size) {
    if (!query) {
        return false;
    }

    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *next = strchr(cursor, '&');
        size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', segment_len);
        if (equals && (size_t)(equals - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            char encoded[512];
            size_t value_len = segment_len - name_len - 1;
            if (value_len >= sizeof(encoded)) {
                return false;
            }
            memcpy(encoded, equals + 1, value_len);
            encoded[value_len] = '\0';
            return url_decode(encoded, out, out_size);
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    return false;
}

static bool is_safe_address(const char *value) {
    if (!value || !*value || strlen(value) > 128) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == ':' || *p == '-' || *p == '_' || *p == '*')) {
            return false;
        }
    }
    return true;
}

static bool is_port_token(const char *value) {
    if (!value || !*value) {
        return false;
    }
    if (strcmp(value, "*") == 0) {
        return true;
    }
    if (strlen(value) > 5) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isdigit(*p)) {
            return false;
        }
    }
    long port = strtol(value, NULL, 10);
    return port >= 0 && port <= 65535;
}

static bool is_wildcard_address(const char *value) {
    return strcmp(value, "*") == 0 ||
           strcmp(value, "0.0.0.0") == 0 ||
           strcmp(value, "::") == 0 ||
           strcmp(value, "[::]") == 0;
}

static void build_capture_filter(char *out,
                                 size_t out_size,
                                 const char *protocol,
                                 const char *local_address,
                                 const char *local_port,
                                 const char *remote_address,
                                 const char *remote_port) {
    bool has_local_host = !is_wildcard_address(local_address);
    bool has_remote_host = !is_wildcard_address(remote_address);
    bool has_local_port = strcmp(local_port, "*") != 0;
    bool has_remote_port = strcmp(remote_port, "*") != 0;

    if (has_local_host && has_remote_host && has_local_port && has_remote_port) {
        snprintf(out, out_size,
                 "%s and ((src host %s and src port %s and dst host %s and dst port %s) or "
                 "(src host %s and src port %s and dst host %s and dst port %s))",
                 protocol,
                 local_address, local_port, remote_address, remote_port,
                 remote_address, remote_port, local_address, local_port);
    } else if (has_local_host && has_local_port) {
        snprintf(out, out_size, "%s and host %s and port %s", protocol, local_address, local_port);
    } else if (has_local_port) {
        snprintf(out, out_size, "%s and port %s", protocol, local_port);
    } else if (has_remote_host && has_remote_port) {
        snprintf(out, out_size, "%s and host %s and port %s", protocol, remote_address, remote_port);
    } else if (has_remote_port) {
        snprintf(out, out_size, "%s and port %s", protocol, remote_port);
    } else {
        snprintf(out, out_size, "%s", protocol);
    }
}

static void send_packet_capture(int fd, const char *query) {
    char protocol[16] = "";
    char local_address[160] = "";
    char local_port[16] = "";
    char remote_address[160] = "";
    char remote_port[16] = "";

    if (!query_value(query, "protocol", protocol, sizeof(protocol)) ||
        !query_value(query, "localAddress", local_address, sizeof(local_address)) ||
        !query_value(query, "localPort", local_port, sizeof(local_port)) ||
        !query_value(query, "remoteAddress", remote_address, sizeof(remote_address)) ||
        !query_value(query, "remotePort", remote_port, sizeof(remote_port))) {
        send_text(fd, 400, "missing capture query parameters\n");
        return;
    }

    for (char *p = protocol; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    if ((strcmp(protocol, "tcp") != 0 && strcmp(protocol, "udp") != 0) ||
        !is_safe_address(local_address) ||
        !is_safe_address(remote_address) ||
        !is_port_token(local_port) ||
        !is_port_token(remote_port)) {
        send_text(fd, 400, "invalid capture query parameters\n");
        return;
    }

    char filter[1024];
    build_capture_filter(filter, sizeof(filter), protocol, local_address, local_port, remote_address, remote_port);

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        send_text(fd, 500, "failed to create capture pipe\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        send_text(fd, 500, "failed to start capture\n");
        return;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        execlp("timeout",
               "timeout",
               "8",
               "tcpdump",
               "-i", "any",
               "-nn",
               "-tttt",
               "-vv",
               "-s", "192",
               "-c", "120",
               filter,
               (char *)NULL);
        perror("exec timeout tcpdump");
        _exit(127);
    }

    close(pipe_fds[1]);

    size_t capacity = 65536;
    size_t used = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        close(pipe_fds[0]);
        waitpid(pid, NULL, 0);
        send_text(fd, 500, "out of memory\n");
        return;
    }
    buffer[0] = '\0';

    append_string(&buffer, &used, &capacity, "# tcpdump filter: ");
    append_string(&buffer, &used, &capacity, filter);
    append_string(&buffer, &used, &capacity, "\n");

    char chunk[4096];
    for (;;) {
        ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
        if (n > 0) {
            append_bytes(&buffer, &used, &capacity, chunk, (size_t)n);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(pipe_fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (used == 0) {
        append_string(&buffer, &used, &capacity, "capture produced no output\n");
    }
    send_response(fd, 200, "text/plain; charset=utf-8", buffer, used);
    free(buffer);
}

static void handle_request(int fd, const char *request) {
    char method[16] = {0};
    char target[1024] = {0};
    char version[16] = {0};
    if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3) {
        send_text(fd, 400, "bad request\n");
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_text(fd, 400, "unsupported method\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    bool head_only = strcmp(method, "HEAD") == 0;

    if (strcmp(target, "/") == 0 || strcmp(target, "/network-inspector.outer") == 0) {
        if (head_only) {
            send_head(fd, 200, "application/vnd.outerframe");
            return;
        }
        send_outer_descriptor(fd);
    } else if (strcmp(target, k_bundle_url_path) == 0) {
        if (head_only) {
            send_head(fd, 200, "text/plain; charset=utf-8");
            return;
        }
        send_text(fd, 200, "macos-arm\nmacos-x86\n");
    } else if (strcmp(target, k_bundle_url_path_macos_arm) == 0) {
        if (head_only) {
            send_head(fd, 200, "application/octet-stream");
            return;
        }
        send_bundle_file(fd, g_bundle_file_path_macos_arm);
    } else if (strcmp(target, k_bundle_url_path_macos_x86) == 0) {
        if (head_only) {
            send_head(fd, 200, "application/octet-stream");
            return;
        }
        send_bundle_file(fd, g_bundle_file_path_macos_x86);
    } else if (strcmp(target, "/api/sockets") == 0) {
        if (head_only) {
            send_head(fd, 200, "text/plain; charset=utf-8");
            return;
        }
        send_socket_snapshot(fd);
    } else if (strcmp(target, "/api/capture") == 0) {
        if (head_only) {
            send_head(fd, 200, "text/plain; charset=utf-8");
            return;
        }
        send_packet_capture(fd, query);
    } else {
        send_text(fd, 404, "not found\n");
    }
}

static int create_listener(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        exit(1);
    }

    if (listen(listen_fd, 16) != 0) {
        perror("listen");
        close(listen_fd);
        exit(1);
    }

    if (port == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd, (struct sockaddr *)&addr, &len) == 0) {
            fprintf(stderr, "Listening on http://127.0.0.1:%d/\n", ntohs(addr.sin_port));
        }
    } else {
        fprintf(stderr, "Listening on http://127.0.0.1:%d/\n", port);
    }

    return listen_fd;
}

static int create_unix_listener(const char *socket_path) {
    if (!socket_path || !socket_path[0]) {
        fprintf(stderr, "socket path is required\n");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "socket path is too long: %s\n", socket_path);
        return -1;
    }

    char directory[4096];
    snprintf(directory, sizeof(directory), "%s", socket_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (directory[0] && mkdir(directory, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "failed to create socket directory %s: %s\n", directory, strerror(errno));
            return -1;
        }
    }

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    unlink(socket_path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (chmod(socket_path, 0600) != 0) {
        perror("chmod");
        close(listen_fd);
        unlink(socket_path);
        return -1;
    }
    if (listen(listen_fd, 16) != 0) {
        perror("listen");
        close(listen_fd);
        unlink(socket_path);
        return -1;
    }
    snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    return listen_fd;
}

static int systemd_activated_listener(void) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (!listen_pid || !listen_fds) {
        return -1;
    }
    char *end = NULL;
    long pid = strtol(listen_pid, &end, 10);
    if (!end || *end != '\0' || pid != (long)getpid()) {
        return -1;
    }
    end = NULL;
    long fds = strtol(listen_fds, &end, 10);
    if (!end || *end != '\0' || fds < 1) {
        return -1;
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return 3;
}

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--bundles-dir DIR]\n", program);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int port = 7352;
    bool use_port = true;
    char socket_path[4096] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            snprintf(socket_path, sizeof(socket_path), "%s", argv[++i]);
            use_port = false;
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            const char *dir = argv[++i];
            snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
                     "%s/NetworkInspectorContent.bundle.macos-arm.aar", dir);
            snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
                     "%s/NetworkInspectorContent.bundle.macos-x86.aar", dir);
        } else if ((strcmp(argv[i], "--label") == 0 || strcmp(argv[i], "--icon-file") == 0) && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    bool using_socket_activation = false;
    int listen_fd = !use_port ? systemd_activated_listener() : -1;
    if (listen_fd >= 0) {
        using_socket_activation = true;
        if (socket_path[0]) {
            snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
        }
    } else {
        listen_fd = use_port ? create_listener(port) : create_unix_listener(socket_path);
    }
    if (listen_fd < 0) {
        return 1;
    }
    if (use_port) {
        fprintf(stderr, "NetworkInspectorBackend listening on http://127.0.0.1:%d/\n", port);
    } else {
        fprintf(stderr, "NetworkInspectorBackend listening on %s/\n", socket_path);
    }
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        struct timeval receive_timeout;
        receive_timeout.tv_sec = 1;
        receive_timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

        char request[REQUEST_LIMIT + 1];
        ssize_t n = read(client_fd, request, REQUEST_LIMIT);
        if (n > 0) {
            request[n] = '\0';
            handle_request(client_fd, request);
        }
        close(client_fd);
    }

    close(listen_fd);
    if (!use_port && g_listen_socket_path[0] && !using_socket_activation) {
        unlink(g_listen_socket_path);
    }
    return 0;
}
