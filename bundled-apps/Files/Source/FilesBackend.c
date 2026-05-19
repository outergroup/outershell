#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 7354
#define READ_BUFFER_SIZE 8192

static const char *kBundleUrlPath = "/bundles/FilesContent";
static const char *kBundleUrlPathMacosArm = "/bundles/FilesContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/FilesContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/FilesContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/FilesContent.bundle.macos-x86.aar";

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_backend_label[256] = "dev.outergroup.Files";
static char g_outerctl_path[PATH_MAX] = "";
static char g_app_icon_path[PATH_MAX] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

typedef struct {
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
    bool is_directory;
    uint64_t size;
    double modified;
    mode_t mode;
} FileEntry;

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
    if (g_listener_fd >= 0) {
        close((int)g_listener_fd);
    }
}

static void write_uint32_le(unsigned char *dst, uint32_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
    dst[2] = (unsigned char)((value >> 16) & 0xffu);
    dst[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void write_uint64_le(unsigned char *dst, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

static bool queue_all(int fd, const void *data, size_t len) {
    const char *bytes = (const char *)data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

static void send_response(int fd, int status, const char *status_text, const char *content_type,
                          const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n"
                              "\r\n",
                              status, status_text, content_type, body_len);
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        queue_all(fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        queue_all(fd, body, body_len);
    }
}

static void send_text_response(int fd, int status, const char *message) {
    const char *status_text = status == 200 ? "OK" :
                              status == 400 ? "Bad Request" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "Error";
    send_response(fd, status, status_text, "text/plain; charset=utf-8", message, strlen(message));
}

static void send_outer_descriptor(int fd) {
    const char *plugin_json = "{\"filesAPIPath\":\"/api/files\",\"rootPath\":\"~\"}";
    size_t path_len = strlen(kBundleUrlPath);
    size_t plugin_len = strlen(plugin_json);
    size_t header_len = 40;
    size_t data_offset = header_len + path_len;
    size_t total_len = data_offset + plugin_len;
    unsigned char *payload = malloc(total_len);
    if (!payload) {
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    payload[0] = 'O';
    payload[1] = 'U';
    payload[2] = 'T';
    payload[3] = 'R';
    write_uint32_le(payload + 4, 1);
    write_uint64_le(payload + 8, (uint64_t)header_len);
    write_uint64_le(payload + 16, (uint64_t)path_len);
    write_uint64_le(payload + 24, (uint64_t)data_offset);
    write_uint64_le(payload + 32, (uint64_t)plugin_len);
    memcpy(payload + header_len, kBundleUrlPath, path_len);
    memcpy(payload + data_offset, plugin_json, plugin_len);

    send_response(fd, 200, "OK", "application/vnd.outerframe", payload, total_len);
    free(payload);
}

static void send_bundle_file(int fd, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        send_text_response(fd, 404, message);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0) {
        close(file_fd);
        send_text_response(fd, 500, "failed to stat bundle\n");
        return;
    }

    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size);
    if (!data) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(file_fd, data + offset, size - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            close(file_fd);
            send_text_response(fd, 500, "failed to read bundle\n");
            return;
        }
        if (got == 0) {
            break;
        }
        offset += (size_t)got;
    }
    close(file_fd);
    send_response(fd, 200, "OK", "application/octet-stream", data, offset);
    free(data);
}

static bool sb_reserve(StringBuilder *builder, size_t additional) {
    if (builder->length + additional + 1 <= builder->capacity) {
        return true;
    }
    size_t new_capacity = builder->capacity ? builder->capacity * 2 : 4096;
    while (new_capacity < builder->length + additional + 1) {
        new_capacity *= 2;
    }
    char *new_data = realloc(builder->data, new_capacity);
    if (!new_data) {
        return false;
    }
    builder->data = new_data;
    builder->capacity = new_capacity;
    return true;
}

static bool sb_append_n(StringBuilder *builder, const char *text, size_t length) {
    if (!sb_reserve(builder, length)) {
        return false;
    }
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static bool sb_append(StringBuilder *builder, const char *text) {
    return sb_append_n(builder, text, strlen(text));
}

static bool sb_append_json_string(StringBuilder *builder, const char *text) {
    if (!sb_append(builder, "\"")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char escaped[8];
        switch (*p) {
        case '\\':
            if (!sb_append(builder, "\\\\")) return false;
            break;
        case '"':
            if (!sb_append(builder, "\\\"")) return false;
            break;
        case '\n':
            if (!sb_append(builder, "\\n")) return false;
            break;
        case '\r':
            if (!sb_append(builder, "\\r")) return false;
            break;
        case '\t':
            if (!sb_append(builder, "\\t")) return false;
            break;
        default:
            if (*p < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                if (!sb_append(builder, escaped)) return false;
            } else if (!sb_append_n(builder, (const char *)p, 1)) {
                return false;
            }
            break;
        }
    }
    return sb_append(builder, "\"");
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_size; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            int high = hex_value(src[i + 1]);
            int low = hex_value(src[i + 2]);
            dst[out++] = (char)((high << 4) | low);
            i += 2;
        } else if (src[i] == '+') {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool query_value(const char *query, const char *name, char *dst, size_t dst_size) {
    if (!query) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t pair_len = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', pair_len);
        if (equals && (size_t)(equals - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            char encoded[PATH_MAX * 3];
            size_t value_len = pair_len - name_len - 1;
            if (value_len >= sizeof(encoded)) {
                value_len = sizeof(encoded) - 1;
            }
            memcpy(encoded, equals + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(dst, dst_size, encoded);
            return true;
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        return home;
    }
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

static bool mkdir_p(const char *path) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
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

static void expand_tilde_path(const char *path, char *out, size_t out_size) {
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

static void default_socket_path(char *out, size_t out_size) {
    const char *label = g_backend_label[0] ? g_backend_label : "dev.outergroup.Files";
#ifdef __APPLE__
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
    } else {
        snprintf(out, out_size, "%s/Library/%s", home_directory(), label);
    }
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
    } else {
        snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), label);
    }
#endif
}

static void run_outerctl_announcement(const char *action, int port, const char *socket_path) {
    if (!g_outerctl_path[0] || !g_backend_label[0]) return;

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%d", port);
    pid_t child = fork();
    if (child == 0) {
        const char *arguments[24];
        size_t argument_count = 0;
        arguments[argument_count++] = g_outerctl_path;
        arguments[argument_count++] = "app";
        arguments[argument_count++] = action;
        arguments[argument_count++] = "--backend";
        arguments[argument_count++] = g_backend_label;
        if (socket_path && socket_path[0]) {
            arguments[argument_count++] = "--socket-path";
            arguments[argument_count++] = socket_path;
        } else if (port > 0) {
            arguments[argument_count++] = "--port";
            arguments[argument_count++] = port_buffer;
        } else {
            _exit(127);
        }
        if (strcmp(action, "add") == 0) {
            arguments[argument_count++] = "--name";
            arguments[argument_count++] = "Files";
            arguments[argument_count++] = "--url";
            arguments[argument_count++] = "/";
            if (g_app_icon_path[0]) {
                arguments[argument_count++] = "--icon-file";
                arguments[argument_count++] = g_app_icon_path;
            }
        }
        arguments[argument_count] = NULL;
        execv(g_outerctl_path, (char *const *)arguments);
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
}

static void cleanup_handler(void) {
    if (g_listen_socket_path[0] && !g_systemd_socket_activation) {
        run_outerctl_announcement("remove", 0, g_listen_socket_path);
        unlink(g_listen_socket_path);
    }
}

static void resolve_requested_path(const char *requested, char *resolved, size_t resolved_size) {
    const char *home = home_directory();
    if (!requested || requested[0] == '\0' || strcmp(requested, "~") == 0) {
        snprintf(resolved, resolved_size, "%s", home);
    } else if (requested[0] == '~' && requested[1] == '/') {
        snprintf(resolved, resolved_size, "%s/%s", home, requested + 2);
    } else {
        snprintf(resolved, resolved_size, "%s", requested);
    }
}

static void parent_path_for(const char *path, char *parent, size_t parent_size) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        snprintf(parent, parent_size, "/");
        return;
    }
    *slash = '\0';
    snprintf(parent, parent_size, "%s", copy);
}

static bool join_child_path(const char *directory, const char *name, char *out, size_t out_size) {
    int written;
    if (strcmp(directory, "/") == 0) {
        written = snprintf(out, out_size, "/%s", name);
    } else {
        written = snprintf(out, out_size, "%s/%s", directory, name);
    }
    return written >= 0 && (size_t)written < out_size;
}

static void mode_string(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    const mode_t bits[] = {
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH
    };
    const char chars[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        out[i + 1] = (mode & bits[i]) ? chars[i] : '-';
    }
    out[10] = '\0';
}

static int compare_entries(const void *lhs, const void *rhs) {
    const FileEntry *a = (const FileEntry *)lhs;
    const FileEntry *b = (const FileEntry *)rhs;
    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    return strcasecmp(a->name, b->name);
}

static bool append_entry_json(StringBuilder *builder, const FileEntry *entry) {
    char number[64];
    char mode[11];
    mode_string(entry->mode, mode);

    if (!sb_append(builder, "{\"name\":")) return false;
    if (!sb_append_json_string(builder, entry->name)) return false;
    if (!sb_append(builder, ",\"path\":")) return false;
    if (!sb_append_json_string(builder, entry->path)) return false;
    if (!sb_append(builder, ",\"isDirectory\":")) return false;
    if (!sb_append(builder, entry->is_directory ? "true" : "false")) return false;
    snprintf(number, sizeof(number), ",\"size\":%llu", (unsigned long long)entry->size);
    if (!sb_append(builder, number)) return false;
    snprintf(number, sizeof(number), ",\"modified\":%.3f", entry->modified);
    if (!sb_append(builder, number)) return false;
    if (!sb_append(builder, ",\"mode\":")) return false;
    if (!sb_append_json_string(builder, mode)) return false;
    return sb_append(builder, "}");
}

static void send_files_response(int fd, const char *query) {
    char requested[PATH_MAX];
    char path[PATH_MAX];
    if (!query_value(query, "path", requested, sizeof(requested))) {
        requested[0] = '\0';
    }
    resolve_requested_path(requested, path, sizeof(path));

    DIR *dir = opendir(path);
    if (!dir) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "failed to open %s: %s\n", path, strerror(errno));
        send_text_response(fd, 404, message);
        return;
    }

    FileEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 64;
            FileEntry *new_entries = realloc(entries, new_capacity * sizeof(FileEntry));
            if (!new_entries) {
                free(entries);
                closedir(dir);
                send_text_response(fd, 500, "out of memory\n");
                return;
            }
            entries = new_entries;
            capacity = new_capacity;
        }

        FileEntry *file = &entries[count];
        memset(file, 0, sizeof(*file));
        snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
        if (!join_child_path(path, entry->d_name, file->path, sizeof(file->path))) {
            continue;
        }

        struct stat st;
        if (stat(file->path, &st) != 0 && lstat(file->path, &st) != 0) {
            continue;
        }
        file->is_directory = S_ISDIR(st.st_mode);
        file->size = (uint64_t)st.st_size;
        file->modified = (double)st.st_mtime;
        file->mode = st.st_mode;
        count++;
    }
    closedir(dir);

    qsort(entries, count, sizeof(FileEntry), compare_entries);

    char parent[PATH_MAX];
    parent_path_for(path, parent, sizeof(parent));

    StringBuilder builder = {0};
    bool ok = sb_append(&builder, "{\"path\":") &&
              sb_append_json_string(&builder, path) &&
              sb_append(&builder, ",\"parent\":") &&
              sb_append_json_string(&builder, parent) &&
              sb_append(&builder, ",\"entries\":[");
    for (size_t i = 0; ok && i < count; i++) {
        if (i > 0) {
            ok = sb_append(&builder, ",");
        }
        if (ok) {
            ok = append_entry_json(&builder, &entries[i]);
        }
    }
    ok = ok && sb_append(&builder, "]}");
    free(entries);

    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8", builder.data, builder.length);
    free(builder.data);
}

static void send_download_response(int fd, const char *query) {
    char requested[PATH_MAX];
    char path[PATH_MAX];
    if (!query_value(query, "path", requested, sizeof(requested))) {
        send_text_response(fd, 400, "missing path\n");
        return;
    }
    resolve_requested_path(requested, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text_response(fd, 404, "file not found\n");
        return;
    }
    send_bundle_file(fd, path);
}

static bool safe_upload_name(const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            return false;
        }
    }
    return true;
}

static void send_upload_response(int fd, const char *query, const unsigned char *body, size_t body_len) {
    char requested_directory[PATH_MAX];
    char directory[PATH_MAX];
    char name[NAME_MAX + 1];
    char output_path[PATH_MAX];

    if (!query_value(query, "directory", requested_directory, sizeof(requested_directory)) ||
        !query_value(query, "name", name, sizeof(name))) {
        send_text_response(fd, 400, "missing directory or name\n");
        return;
    }
    if (!safe_upload_name(name)) {
        send_text_response(fd, 400, "invalid file name\n");
        return;
    }

    resolve_requested_path(requested_directory, directory, sizeof(directory));
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_text_response(fd, 404, "destination directory not found\n");
        return;
    }
    if (!join_child_path(directory, name, output_path, sizeof(output_path))) {
        send_text_response(fd, 400, "path is too long\n");
        return;
    }

    int file_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file_fd < 0) {
        send_text_response(fd, 500, "failed to open destination\n");
        return;
    }
    bool ok = queue_all(file_fd, body, body_len);
    if (close(file_fd) != 0) {
        ok = false;
    }
    if (!ok) {
        send_text_response(fd, 500, "failed to write destination\n");
        return;
    }
    send_text_response(fd, 200, "ok\n");
}

static void send_mkdir_response(int fd, const char *query) {
    char requested_directory[PATH_MAX];
    char directory[PATH_MAX];
    char name[NAME_MAX + 1];
    char output_path[PATH_MAX];

    if (!query_value(query, "directory", requested_directory, sizeof(requested_directory)) ||
        !query_value(query, "name", name, sizeof(name))) {
        send_text_response(fd, 400, "missing directory or name\n");
        return;
    }
    if (!safe_upload_name(name)) {
        send_text_response(fd, 400, "invalid directory name\n");
        return;
    }

    resolve_requested_path(requested_directory, directory, sizeof(directory));
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_text_response(fd, 404, "destination directory not found\n");
        return;
    }
    if (!join_child_path(directory, name, output_path, sizeof(output_path))) {
        send_text_response(fd, 400, "path is too long\n");
        return;
    }

    if (mkdir(output_path, 0755) != 0) {
        if (errno == EEXIST && stat(output_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            send_text_response(fd, 200, "ok\n");
            return;
        }
        send_text_response(fd, 500, "failed to create directory\n");
        return;
    }
    send_text_response(fd, 200, "ok\n");
}

static size_t request_content_length(const char *request) {
    const char *line = request;
    while ((line = strcasestr(line, "\r\nContent-Length:")) != NULL) {
        line += strlen("\r\nContent-Length:");
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        return (size_t)strtoull(line, NULL, 10);
    }
    return 0;
}

static unsigned char *read_request_body(int fd,
                                        const char *request,
                                        ssize_t request_len,
                                        size_t content_length) {
    char *headers_end = strstr((char *)request, "\r\n\r\n");
    if (!headers_end) {
        return NULL;
    }

    const char *initial_body = headers_end + 4;
    size_t header_len = (size_t)(initial_body - request);
    size_t initial_body_len = request_len > (ssize_t)header_len ? (size_t)request_len - header_len : 0;
    if (initial_body_len > content_length) {
        initial_body_len = content_length;
    }

    unsigned char *body = malloc(content_length ? content_length : 1);
    if (!body) {
        return NULL;
    }
    memcpy(body, initial_body, initial_body_len);

    size_t offset = initial_body_len;
    while (offset < content_length) {
        ssize_t got = read(fd, body + offset, content_length - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(body);
            return NULL;
        }
        if (got == 0) {
            free(body);
            return NULL;
        }
        offset += (size_t)got;
    }
    return body;
}

static void handle_client(int fd) {
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct pollfd poll_fd = {.fd = fd, .events = POLLIN, .revents = 0};
    int ready;
    do {
        ready = poll(&poll_fd, 1, 500);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || !(poll_fd.revents & POLLIN)) {
        return;
    }

    char request[READ_BUFFER_SIZE];
    ssize_t n = read(fd, request, sizeof(request) - 1);
    if (n <= 0) {
        return;
    }
    request[n] = '\0';

    char method[16], target[1024], version[16];
    if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3) {
        send_text_response(fd, 400, "bad request\n");
        return;
    }
    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "HEAD") != 0 && strcasecmp(method, "POST") != 0) {
        send_text_response(fd, 400, "unsupported method\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcasecmp(method, "POST") == 0) {
        if (strcmp(target, "/api/mkdir") == 0) {
            send_mkdir_response(fd, query);
            return;
        }
        if (strcmp(target, "/api/upload") != 0) {
            send_text_response(fd, 404, "not found\n");
            return;
        }
        size_t content_length = request_content_length(request);
        unsigned char *body = read_request_body(fd, request, n, content_length);
        if (!body && content_length > 0) {
            send_text_response(fd, 400, "failed to read request body\n");
            return;
        }
        send_upload_response(fd, query, body, content_length);
        free(body);
    } else if (strcmp(target, "/") == 0 || strcmp(target, "/files.outer") == 0) {
        send_outer_descriptor(fd);
    } else if (strcmp(target, kBundleUrlPath) == 0) {
        send_text_response(fd, 200, "macos-arm\nmacos-x86\n");
    } else if (strcmp(target, kBundleUrlPathMacosArm) == 0) {
        const char *path = g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
        send_bundle_file(fd, path);
    } else if (strcmp(target, kBundleUrlPathMacosX86) == 0) {
        const char *path = g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
        send_bundle_file(fd, path);
    } else if (strcmp(target, "/api/files") == 0) {
        send_files_response(fd, query);
    } else if (strcmp(target, "/api/download") == 0) {
        send_download_response(fd, query);
    } else {
        send_text_response(fd, 404, "not found\n");
    }
}

static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
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

    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", socket_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(directory)) {
            fprintf(stderr, "failed to create socket directory %s: %s\n", directory, strerror(errno));
            return -1;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    unlink(socket_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (chmod(socket_path, 0600) != 0) {
        perror("chmod");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    return fd;
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
    g_systemd_socket_activation = true;
    return 3;
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--label LABEL] [--bundles-dir DIR] [--icon-file PATH]\n", program);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    bool use_port = false;
    char socket_path[PATH_MAX] = "";
    const char *bundles_dir = "bundles";
    const char *outerctl_path = getenv("OUTERCTL_PATH");
    if (outerctl_path && outerctl_path[0]) {
        snprintf(g_outerctl_path, sizeof(g_outerctl_path), "%s", outerctl_path);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], socket_path, sizeof(socket_path));
            use_port = false;
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            snprintf(g_backend_label, sizeof(g_backend_label), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            bundles_dir = argv[++i];
        } else if (strcmp(argv[i], "--icon-file") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_app_icon_path, sizeof(g_app_icon_path));
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!use_port && !socket_path[0]) {
        default_socket_path(socket_path, sizeof(socket_path));
    }

    snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
             "%s/FilesContent.bundle.macos-arm.aar", bundles_dir);
    snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
             "%s/FilesContent.bundle.macos-x86.aar", bundles_dir);

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup_handler);

    int listener = !use_port ? systemd_activated_listener() : -1;
    if (listener < 0) {
        listener = use_port ? create_tcp_listener(port) : create_unix_listener(socket_path);
    } else if (socket_path[0]) {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    }
    if (listener < 0) {
        return 1;
    }
    g_listener_fd = listener;
    if (use_port) {
        fprintf(stderr, "FilesBackend listening on http://127.0.0.1:%d/\n", port);
        run_outerctl_announcement("add", port, NULL);
    } else {
        fprintf(stderr, "FilesBackend listening on %s/\n", socket_path);
        run_outerctl_announcement("add", 0, socket_path);
    }

    while (!g_shutdown_requested) {
        if (g_systemd_socket_activation) {
            struct pollfd poll_fd = {.fd = listener, .events = POLLIN};
            int poll_result = poll(&poll_fd, 1, 60000);
            if (poll_result == 0) {
                break;
            }
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("poll");
                break;
            }
        }
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(client);
        close(client);
    }

    close(listener);
    g_listener_fd = -1;
    return 0;
}
