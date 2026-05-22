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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
extern int launch_activate_socket(const char *name, int **fds, size_t *cnt);
#endif

#define DEFAULT_PORT 7354
#define READ_BUFFER_SIZE 65536
#define DEFAULT_LOG_BYTES 131072
#define MAX_LOG_BYTES 1048576
#define MAX_REACTOR_CLIENTS 128
#define CLIENT_IDLE_TIMEOUT_MS 10000
#define SYSTEMD_STATUS_CACHE_TTL_MS 1500
#define MAX_SYSTEMD_STATUS_ENTRIES 512

static const char *kHomeScreenServiceID = "dev.outergroup.HomeScreen";
static const char *kLegacyNavigatorServiceID = "dev.outergroup.Navigator";
static const char *kLegacyBackendsServiceID = "dev.outergroup.Backends";
static const char *kBundleUrlPath = "/bundles/BackendsContent";
static const char *kBundleUrlPathMacosArm = "/bundles/BackendsContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/BackendsContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/BackendsContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/BackendsContent.bundle.macos-x86.aar";
#ifdef __APPLE__
static const char *kSystemOuterAgentRoot = "/Library/dev.outergroup.OuterLoop";
#else
static const char *kSystemOuterAgentRoot = "/var/lib/outergroup/outeragent";
#endif

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_registry_database_path[PATH_MAX] = "";
static char g_system_registry_database_path[PATH_MAX] = "";
static char g_bundled_apps_directory[PATH_MAX] = "";
static char g_bundled_apps_base_url[2048] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static bool g_launchd_socket_activation = false;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;

typedef struct {
    int fd;
    char request[READ_BUFFER_SIZE];
    size_t length;
    int64_t last_activity_ms;
} ReactorClient;

typedef struct {
    char unit_name[256];
    char scope[16];
    char active_state[32];
} SystemdStatusEntry;

typedef struct {
    SystemdStatusEntry entries[MAX_SYSTEMD_STATUS_ENTRIES];
    size_t count;
    int64_t refreshed_at_ms;
} SystemdStatusCache;

static SystemdStatusCache g_systemd_status_cache = {0};

typedef struct {
    const char *service_id;
    const char *display_name;
    const char *unit_name;
    const char *stage_directory_name;
    const char *install_directory_name;
    const char *binary_name;
    const char *bundle_prefix;
    const char *icon_name;
    const char *source_name;
    const char *socket_name;
    bool socket_activated;
    const char *archive_name;
    const char *version;
} BundledAppDefinition;

static bool mkdir_p(const char *path);

static const BundledAppDefinition kBundledApps[] = {
    {
        .service_id = "dev.outergroup.Top",
        .display_name = "Top",
        .unit_name = "dev.outergroup.Top.service",
        .stage_directory_name = "Top",
        .install_directory_name = "dev.outergroup.Top",
        .binary_name = "TopBackend",
        .bundle_prefix = "TopContent",
        .icon_name = "app-icon.png",
        .source_name = "TopBackend.c",
        .socket_name = "dev.outergroup.Top",
        .socket_activated = false,
        .archive_name = "Top.tar.gz",
        .version = "1"
    },
    {
        .service_id = "dev.outergroup.Files",
        .display_name = "Files",
        .unit_name = "dev.outergroup.Files.service",
        .stage_directory_name = "Files",
        .install_directory_name = "dev.outergroup.Files",
        .binary_name = "FilesBackend",
        .bundle_prefix = "FilesContent",
        .icon_name = "app-icon.png",
        .source_name = "FilesBackend.c",
        .socket_name = "dev.outergroup.Files",
        .socket_activated = true,
        .archive_name = "Files.tar.gz",
        .version = "1"
    },
    {
        .service_id = "dev.outergroup.NetworkInspector",
        .display_name = "Network Inspector",
        .unit_name = "dev.outergroup.NetworkInspector.service",
        .stage_directory_name = "NetworkInspector",
        .install_directory_name = "dev.outergroup.NetworkInspector",
        .binary_name = "NetworkInspectorBackend",
        .bundle_prefix = "NetworkInspectorContent",
        .icon_name = "app-icon.png",
        .source_name = "NetworkInspectorBackend.c",
        .socket_name = "dev.outergroup.NetworkInspector",
        .socket_activated = true,
        .archive_name = "NetworkInspector.tar.gz",
        .version = "1"
    },
    {
        .service_id = "dev.outergroup.Firehose",
        .display_name = "Firehose",
        .unit_name = "dev.outergroup.Firehose.service",
        .stage_directory_name = "Firehose",
        .install_directory_name = "dev.outergroup.Firehose",
        .binary_name = "FirehoseBackend",
        .bundle_prefix = "TraceContent",
        .icon_name = "app-icon.png",
        .source_name = "TraceBackend.c",
        .socket_name = "dev.outergroup.Firehose",
        .socket_activated = true,
        .archive_name = "Firehose.tar.gz",
        .version = "1"
    }
};

static bool is_home_screen_service_id(const char *service_id) {
    return service_id &&
           (strcmp(service_id, kHomeScreenServiceID) == 0 ||
            strcmp(service_id, kLegacyNavigatorServiceID) == 0 ||
            strcmp(service_id, kLegacyBackendsServiceID) == 0);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
    if (g_listener_fd >= 0) {
        close((int)g_listener_fd);
    }
}

static bool queue_all(int fd, const void *data, size_t len) {
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

static int64_t monotonic_milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool set_fd_nonblocking(int fd, bool nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    int new_flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (new_flags == flags) return true;
    return fcntl(fd, F_SETFL, new_flags) == 0;
}

static void log_event(const char *format, ...) {
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) || !defined(__APPLE__)
    localtime_r(&now, &local_time);
#else
    struct tm *resolved_time = localtime(&now);
    if (resolved_time) {
        local_time = *resolved_time;
    } else {
        memset(&local_time, 0, sizeof(local_time));
    }
#endif
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local_time);
    fprintf(stderr, "[%s] ", timestamp);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fputc('\n', stderr);
    fflush(stderr);
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

static bool sb_reserve(StringBuilder *builder, size_t additional) {
    if (builder->length + additional + 1 <= builder->capacity) return true;
    size_t new_capacity = builder->capacity ? builder->capacity * 2 : 4096;
    while (new_capacity < builder->length + additional + 1) {
        new_capacity *= 2;
    }
    char *new_data = realloc(builder->data, new_capacity);
    if (!new_data) return false;
    builder->data = new_data;
    builder->capacity = new_capacity;
    return true;
}

static bool sb_append_n(StringBuilder *builder, const char *text, size_t length) {
    if (!sb_reserve(builder, length)) return false;
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static bool sb_append(StringBuilder *builder, const char *text) {
    return sb_append_n(builder, text, strlen(text));
}

static bool sb_append_json_string(StringBuilder *builder, const char *text) {
    if (!sb_append(builder, "\"")) return false;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        char escaped[8];
        switch (*p) {
        case '\\': if (!sb_append(builder, "\\\\")) return false; break;
        case '"': if (!sb_append(builder, "\\\"")) return false; break;
        case '\n': if (!sb_append(builder, "\\n")) return false; break;
        case '\r': if (!sb_append(builder, "\\r")) return false; break;
        case '\t': if (!sb_append(builder, "\\t")) return false; break;
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

static bool sb_append_base64_file_json_string(StringBuilder *builder, const char *path) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!path || !path[0]) {
        return sb_append_json_string(builder, "");
    }

    if (strncmp(path, "data:", 5) == 0) {
        const char *comma = strchr(path, ',');
        if (comma && strstr(path, ";base64")) {
            return sb_append_json_string(builder, comma + 1);
        }
        return sb_append_json_string(builder, "");
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        return sb_append_json_string(builder, "");
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return sb_append_json_string(builder, "");
    }

    unsigned char *bytes = malloc((size_t)st.st_size);
    if (!bytes) {
        fclose(file);
        return false;
    }
    size_t read_count = fread(bytes, 1, (size_t)st.st_size, file);
    fclose(file);
    if (read_count != (size_t)st.st_size) {
        free(bytes);
        return sb_append_json_string(builder, "");
    }

    bool ok = sb_append(builder, "\"");
    for (size_t i = 0; ok && i < read_count; i += 3) {
        unsigned int value = (unsigned int)bytes[i] << 16;
        bool has_second = i + 1 < read_count;
        bool has_third = i + 2 < read_count;
        if (has_second) value |= (unsigned int)bytes[i + 1] << 8;
        if (has_third) value |= (unsigned int)bytes[i + 2];

        char chunk[4] = {
            table[(value >> 18) & 0x3f],
            table[(value >> 12) & 0x3f],
            has_second ? table[(value >> 6) & 0x3f] : '=',
            has_third ? table[value & 0x3f] : '='
        };
        ok = sb_append_n(builder, chunk, sizeof(chunk));
    }
    free(bytes);
    return ok && sb_append(builder, "\"");
}

static bool sb_append_base64_bytes(StringBuilder *builder, const unsigned char *bytes, size_t length) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < length; i += 3) {
        unsigned int value = (unsigned int)bytes[i] << 16;
        bool has_second = i + 1 < length;
        bool has_third = i + 2 < length;
        if (has_second) value |= (unsigned int)bytes[i + 1] << 8;
        if (has_third) value |= (unsigned int)bytes[i + 2];

        char chunk[4] = {
            table[(value >> 18) & 0x3f],
            table[(value >> 12) & 0x3f],
            has_second ? table[(value >> 6) & 0x3f] : '=',
            has_third ? table[value & 0x3f] : '='
        };
        if (!sb_append_n(builder, chunk, sizeof(chunk))) {
            return false;
        }
    }
    return true;
}

static char *registry_icon_value(const char *path) {
    if (!path || !path[0]) {
        return NULL;
    }
    if (strncmp(path, "data:", 5) == 0) {
        return strdup(path);
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        return strdup(path);
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return strdup(path);
    }

    unsigned char *bytes = malloc((size_t)st.st_size);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    size_t read_count = fread(bytes, 1, (size_t)st.st_size, file);
    fclose(file);
    if (read_count != (size_t)st.st_size) {
        free(bytes);
        return strdup(path);
    }

    StringBuilder builder = {0};
    bool ok = sb_append(&builder, "data:image/png;base64,") &&
              sb_append_base64_bytes(&builder, bytes, read_count);
    free(bytes);
    if (!ok) {
        free(builder.data);
        return NULL;
    }
    return builder.data;
}

static bool sb_append_python_string(StringBuilder *builder, const char *text) {
    if (!sb_append(builder, "\"")) return false;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        char escaped[16];
        switch (*p) {
        case '\\': if (!sb_append(builder, "\\\\")) return false; break;
        case '"': if (!sb_append(builder, "\\\"")) return false; break;
        case '\n': if (!sb_append(builder, "\\n")) return false; break;
        case '\r': if (!sb_append(builder, "\\r")) return false; break;
        case '\t': if (!sb_append(builder, "\\t")) return false; break;
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

static bool sb_append_python_list(StringBuilder *builder, const char *const *items, size_t count) {
    if (!sb_append(builder, "[")) return false;
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && !sb_append(builder, ", ")) return false;
        if (!sb_append_python_string(builder, items[i])) return false;
    }
    return sb_append(builder, "]");
}

static void shell_quote(const char *value, char *out, size_t out_size) {
    size_t offset = 0;
    if (out_size == 0) return;
    out[offset++] = '\'';
    for (const char *p = value ? value : ""; *p && offset + 5 < out_size; p++) {
        if (*p == '\'') {
            memcpy(out + offset, "'\\''", 4);
            offset += 4;
        } else {
            out[offset++] = *p;
        }
    }
    if (offset + 1 < out_size) {
        out[offset++] = '\'';
    }
    out[offset] = '\0';
}

static void join_url_path(char *out, size_t out_size, const char *base_url, const char *path) {
    if (!out_size) return;
    out[0] = '\0';
    if (!base_url || !base_url[0] || !path || !path[0]) return;

    size_t base_length = strlen(base_url);
    while (base_length > 0 && base_url[base_length - 1] == '/') {
        base_length--;
    }
    snprintf(out, out_size, "%.*s/%s", (int)base_length, base_url, path);
}

static bool sb_append_xml_escaped(StringBuilder *builder, const char *text) {
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '&': if (!sb_append(builder, "&amp;")) return false; break;
        case '<': if (!sb_append(builder, "&lt;")) return false; break;
        case '>': if (!sb_append(builder, "&gt;")) return false; break;
        case '"': if (!sb_append(builder, "&quot;")) return false; break;
        case '\'': if (!sb_append(builder, "&apos;")) return false; break;
        default:
            if (!sb_append_n(builder, (const char *)p, 1)) return false;
            break;
        }
    }
    return true;
}

static bool safe_unit_name(const char *unit_name) {
    if (!unit_name || !unit_name[0]) return false;
    size_t len = strlen(unit_name);
    if (len > 240 || len < 9 || strcmp(unit_name + len - 8, ".service") != 0) return false;
    for (const char *p = unit_name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-' || *p == '@' || *p == ':')) {
            return false;
        }
    }
    return true;
}

static void sanitize_identifier_component(const char *value, char *out, size_t out_size) {
    size_t offset = 0;
    if (out_size == 0) return;
    for (const char *p = value ? value : ""; *p && offset + 1 < out_size; p++) {
        if (isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-') {
            out[offset++] = *p;
        } else if (offset == 0 || out[offset - 1] != '-') {
            out[offset++] = '-';
        }
    }
    while (offset > 0 && out[offset - 1] == '-') offset--;
    if (offset == 0) {
        snprintf(out, out_size, "backend");
    } else {
        out[offset] = '\0';
    }
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
    if (!query) return false;
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t pair_len = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', pair_len);
        if (equals && (size_t)(equals - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            char encoded[PATH_MAX * 3];
            size_t value_len = pair_len - name_len - 1;
            if (value_len >= sizeof(encoded)) value_len = sizeof(encoded) - 1;
            memcpy(encoded, equals + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(dst, dst_size, encoded);
            return true;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return false;
}

static bool query_value_any(const char *query, const char *body, const char *name, char *dst, size_t dst_size) {
    return query_value(query, name, dst, dst_size) || query_value(body, name, dst, dst_size);
}

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
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

static void append_path_component(char *out, size_t out_size, const char *base, const char *component) {
    snprintf(out, out_size, "%s/%s", base && base[0] ? base : "", component && component[0] ? component : "");
}

static bool resolve_user_script_path(const char *raw_path,
                                     const char *working_directory,
                                     char *out,
                                     size_t out_size) {
    char expanded[PATH_MAX];
    expand_tilde_path(raw_path, expanded, sizeof(expanded));
    if (!expanded[0]) return false;
    if (expanded[0] == '/') {
        snprintf(out, out_size, "%s", expanded);
    } else {
        append_path_component(out, out_size, working_directory && working_directory[0] ? working_directory : home_directory(), expanded);
    }
    return out[0] != '\0';
}

static bool sudo_failure_needs_password(const char *output, int exit_status);
static bool run_sudo_shell(const char *command, const char *password, char *output, size_t output_size, int *exit_status);

static const BundledAppDefinition *bundled_app_for_service_id(const char *service_id) {
    if (!service_id) return NULL;
    for (size_t i = 0; i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        if (strcmp(kBundledApps[i].service_id, service_id) == 0) {
#ifdef __APPLE__
            if (strcmp(kBundledApps[i].service_id, "dev.outergroup.Top") != 0) {
                return NULL;
            }
#endif
            return &kBundledApps[i];
        }
    }
    return NULL;
}

static bool bundled_app_is_available_on_platform(const BundledAppDefinition *app) {
    if (!app) return false;
#ifdef __APPLE__
    return strcmp(app->service_id, "dev.outergroup.Top") == 0;
#else
    return true;
#endif
}

static void systemd_socket_unit_name(const char *unit_name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", unit_name && unit_name[0] ? unit_name : "");
    size_t length = strlen(out);
    const char *suffix = ".service";
    size_t suffix_length = strlen(suffix);
    if (length > suffix_length && strcmp(out + length - suffix_length, suffix) == 0) {
        snprintf(out + length - suffix_length, out_size - (length - suffix_length), ".socket");
    }
}

static void bundled_socket_path_for_scope(const BundledAppDefinition *app,
                                          const char *scope,
                                          char *out,
                                          size_t out_size) {
    if (!app || !app->socket_name || !app->socket_name[0]) {
        out[0] = '\0';
        return;
    }
    if (scope && strcmp(scope, "system") == 0) {
#ifdef __APPLE__
        snprintf(out, out_size, "/var/run/%s", app->socket_name);
#else
        snprintf(out, out_size, "/run/%s", app->socket_name);
#endif
        return;
    }
#ifdef __APPLE__
    size_t required_length = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (required_length > 0) {
        char temp_dir[PATH_MAX];
        if (required_length < sizeof(temp_dir) &&
            confstr(_CS_DARWIN_USER_TEMP_DIR, temp_dir, required_length) > 0 &&
            temp_dir[0]) {
            size_t length = strlen(temp_dir);
            snprintf(out,
                     out_size,
                     "%s%s%s",
                     temp_dir,
                     length > 0 && temp_dir[length - 1] == '/' ? "" : "/",
                     app->socket_name);
            return;
        }
    }
    snprintf(out, out_size, "/tmp/%s", app->socket_name);
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, app->socket_name);
    } else {
        snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), app->socket_name);
    }
#endif
}

static bool safe_service_directory_name(const char *value) {
    if (!value || !value[0] || value[0] == '.') return false;
    for (const char *p = value; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

#ifndef __APPLE__
static bool remote_machine_architecture(char *out, size_t out_size) {
    struct utsname names;
    if (uname(&names) != 0) return false;
    if (strcmp(names.machine, "x86_64") == 0 || strcmp(names.machine, "amd64") == 0) {
        snprintf(out, out_size, "x86_64");
        return true;
    }
    if (strcmp(names.machine, "aarch64") == 0 || strcmp(names.machine, "arm64") == 0) {
        snprintf(out, out_size, "aarch64");
        return true;
    }
    snprintf(out, out_size, "%s", names.machine);
    return false;
}
#endif

static bool directory_exists(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#ifdef __APPLE__
static bool current_executable_path(char *out, size_t out_size) {
    uint32_t size = (uint32_t)out_size;
    if (_NSGetExecutablePath(out, &size) != 0) return false;
    char resolved[PATH_MAX];
    if (realpath(out, resolved)) {
        snprintf(out, out_size, "%s", resolved);
    }
    return out[0] != '\0';
}

static bool parent_directory(const char *path, char *out, size_t out_size) {
    if (!path || !path[0]) return false;
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return out[0] != '\0';
}
#endif

static void bundled_apps_root(char *out, size_t out_size) {
    if (g_bundled_apps_directory[0]) {
        snprintf(out, out_size, "%s", g_bundled_apps_directory);
        return;
    }
    const char *env_root = getenv("BACKENDS_BUNDLED_APPS_DIR");
    if (env_root && env_root[0]) {
        expand_tilde_path(env_root, out, out_size);
        return;
    }
    char cwd[PATH_MAX] = "";
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(out, out_size, "%s/bundled-apps", cwd);
        if (directory_exists(out)) return;
    }
#ifdef __APPLE__
    char executable[PATH_MAX];
    if (current_executable_path(executable, sizeof(executable))) {
        char directory[PATH_MAX];
        if (parent_directory(executable, directory, sizeof(directory))) {
            char candidate[PATH_MAX];
            snprintf(candidate, sizeof(candidate), "%s/bundled-apps", directory);
            if (directory_exists(candidate)) {
                snprintf(out, out_size, "%s", candidate);
                return;
            }

            char parent[PATH_MAX];
            char grandparent[PATH_MAX];
            char great_grandparent[PATH_MAX];
            if (parent_directory(directory, parent, sizeof(parent))) {
                snprintf(candidate, sizeof(candidate), "%s/run/bundled-apps", parent);
                if (directory_exists(candidate)) {
                    snprintf(out, out_size, "%s", candidate);
                    return;
                }
                snprintf(candidate, sizeof(candidate), "%s/bundled-apps", parent);
                if (directory_exists(candidate)) {
                    snprintf(out, out_size, "%s", candidate);
                    return;
                }
                if (parent_directory(parent, grandparent, sizeof(grandparent))) {
                    snprintf(candidate, sizeof(candidate), "%s/run/bundled-apps", grandparent);
                    if (directory_exists(candidate)) {
                        snprintf(out, out_size, "%s", candidate);
                        return;
                    }
                    snprintf(candidate, sizeof(candidate), "%s/bundled-apps", grandparent);
                    if (directory_exists(candidate)) {
                        snprintf(out, out_size, "%s", candidate);
                        return;
                    }
                    if (parent_directory(grandparent, great_grandparent, sizeof(great_grandparent))) {
                        snprintf(candidate, sizeof(candidate), "%s/bundled-apps", great_grandparent);
                        if (directory_exists(candidate)) {
                            snprintf(out, out_size, "%s", candidate);
                            return;
                        }
                    }
                }
            }
        }
    }
#endif
    if (cwd[0]) {
        snprintf(out, out_size, "%s/bundled-apps", cwd);
    } else {
        snprintf(out, out_size, "bundled-apps");
    }
}

static void bundled_app_stage_root(const BundledAppDefinition *app, char *out, size_t out_size) {
    char root[PATH_MAX];
    bundled_apps_root(root, sizeof(root));
    append_path_component(out, out_size, root, app->stage_directory_name);
}

static bool bundled_app_stage_has_expected_files(const BundledAppDefinition *app, const char *stage_root) {
    if (!app || !stage_root || !stage_root[0]) return false;
    struct stat st;

    char bundle_arm[PATH_MAX];
    snprintf(bundle_arm, sizeof(bundle_arm), "%s/bundles/%s.bundle.macos-arm.aar", stage_root, app->bundle_prefix);
    if (stat(bundle_arm, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    char bundle_x86[PATH_MAX];
    snprintf(bundle_x86, sizeof(bundle_x86), "%s/bundles/%s.bundle.macos-x86.aar", stage_root, app->bundle_prefix);
    if (stat(bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    if (app->icon_name && app->icon_name[0]) {
        char icon_path[PATH_MAX];
        snprintf(icon_path, sizeof(icon_path), "%s/%s", stage_root, app->icon_name);
        if (stat(icon_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    }

#ifdef __APPLE__
    char macos_binary[PATH_MAX];
    snprintf(macos_binary, sizeof(macos_binary), "%s/MacOS/%s", stage_root, app->binary_name);
    return stat(macos_binary, &st) == 0 && S_ISREG(st.st_mode);
#else
    if (app->source_name && app->source_name[0]) {
        char source_path[PATH_MAX];
        snprintf(source_path, sizeof(source_path), "%s/Source/%s", stage_root, app->source_name);
        if (stat(source_path, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }

    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) return false;
    char linux_binary[PATH_MAX];
    snprintf(linux_binary, sizeof(linux_binary), "%s/RemoteLinuxBinaries/%s/%s", stage_root, architecture, app->binary_name);
    return stat(linux_binary, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static bool download_bundled_app_stage(const BundledAppDefinition *app,
                                       char *out_stage_root,
                                       size_t out_stage_root_size,
                                       char *message,
                                       size_t message_size) {
    char archive_url[2048];
    join_url_path(archive_url, sizeof(archive_url), g_bundled_apps_base_url, app->archive_name);
    if (!archive_url[0]) {
        snprintf(message, message_size,
                 "No bundled app download URL is configured for %s. Pass --app-base-url or set HOME_SCREEN_APP_BASE_URL.",
                 app->display_name);
        return false;
    }

    log_event("Downloading bundled app %s from %s.",
              app->display_name,
              archive_url);

    const char *cache_home = getenv("XDG_CACHE_HOME");
    char cache_root[PATH_MAX];
    if (cache_home && cache_home[0]) {
        snprintf(cache_root, sizeof(cache_root), "%s/outerloop/home-screen/bundled-apps", cache_home);
    } else {
        snprintf(cache_root, sizeof(cache_root), "%s/.cache/outerloop/home-screen/bundled-apps", home_directory());
    }
    if (!mkdir_p(cache_root)) {
        snprintf(message, message_size, "Failed to create bundled app cache at %s: %s", cache_root, strerror(errno));
        return false;
    }

    char archive_path[PATH_MAX];
    snprintf(archive_path, sizeof(archive_path), "%s/%s.tar.gz", cache_root, app->stage_directory_name);

    char quoted_archive_path[PATH_MAX + 8];
    char quoted_cache_root[PATH_MAX + 8];
    char quoted_archive_url[2048];
    shell_quote(archive_path, quoted_archive_path, sizeof(quoted_archive_path));
    shell_quote(cache_root, quoted_cache_root, sizeof(quoted_cache_root));
    shell_quote(archive_url, quoted_archive_url, sizeof(quoted_archive_url));

    char command[4096];
    snprintf(command, sizeof(command),
             "set -eu; "
             "if command -v curl >/dev/null 2>&1; then "
             "curl -fsSL -o %s %s; "
             "elif command -v wget >/dev/null 2>&1; then "
             "wget -qO %s %s; "
             "else echo 'curl or wget is required' >&2; exit 127; fi; "
             "tar -xzf %s -C %s",
             quoted_archive_path, quoted_archive_url,
             quoted_archive_path, quoted_archive_url,
             quoted_archive_path, quoted_cache_root);

    int status = system(command);
    if (status != 0) {
        log_event("Failed to download bundled app %s.", app->display_name);
        snprintf(message, message_size, "Failed to download bundled %s from %s.",
                 app->display_name, archive_url);
        return false;
    }

    snprintf(out_stage_root, out_stage_root_size, "%s/%s", cache_root, app->stage_directory_name);
    if (!bundled_app_stage_has_expected_files(app, out_stage_root)) {
        log_event("Downloaded bundled app %s, but its payload is incomplete.", app->display_name);
        snprintf(message, message_size, "Downloaded bundled %s, but its payload is incomplete.", app->display_name);
        return false;
    }
    log_event("Downloaded bundled app %s to %s.", app->display_name, out_stage_root);
    return true;
}

static bool resolve_bundled_app_stage_root(const BundledAppDefinition *app,
                                           char *out_stage_root,
                                           size_t out_stage_root_size,
                                           char *message,
                                           size_t message_size) {
    bundled_app_stage_root(app, out_stage_root, out_stage_root_size);
    if (bundled_app_stage_has_expected_files(app, out_stage_root)) {
        return true;
    }

    const char *disable_download = getenv("BACKENDS_DISABLE_BUNDLED_APP_DOWNLOADS");
    if (disable_download && disable_download[0]) {
        snprintf(message, message_size, "Missing bundled %s payload at %s.", app->display_name, out_stage_root);
        return false;
    }

    return download_bundled_app_stage(app, out_stage_root, out_stage_root_size, message, message_size);
}

static void default_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("BACKENDS_REGISTRY_DB");
    if (!env_path || !env_path[0]) {
        env_path = getenv("OUTERLOOP_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/registry.sqlite3", home_directory());
#else
    snprintf(out, out_size, "%s/.outeragent/registry.sqlite3", home_directory());
#endif
}

static void default_system_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("BACKENDS_SYSTEM_REGISTRY_DB");
    if (!env_path || !env_path[0]) {
        env_path = getenv("OUTERLOOP_SYSTEM_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
#ifdef __APPLE__
    snprintf(out, out_size, "%s/registry.sqlite3", kSystemOuterAgentRoot);
#else
    snprintf(out, out_size, "%s/registry.sqlite3", kSystemOuterAgentRoot);
#endif
}

static sqlite3 *open_registry_readonly_at(const char *path, char *error, size_t error_size) {
    if (!path || !path[0]) {
        snprintf(error, error_size, "registry database path is empty");
        return NULL;
    }
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(path, &database,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open registry database");
        if (database) sqlite3_close(database);
        return NULL;
    }
    sqlite3_busy_timeout(database, 5000);
    return database;
}

static sqlite3 *open_registry_readonly(char *error, size_t error_size) {
    return open_registry_readonly_at(g_registry_database_path, error, error_size);
}

static sqlite3 *open_system_registry_readonly(char *error, size_t error_size) {
    return open_registry_readonly_at(g_system_registry_database_path, error, error_size);
}

static sqlite3 *open_registry_readwrite_at(const char *path, char *error, size_t error_size) {
    if (!path || !path[0]) {
        snprintf(error, error_size, "registry database path is empty");
        return NULL;
    }
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(path, &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open registry database");
        if (database) sqlite3_close(database);
        return NULL;
    }
    sqlite3_busy_timeout(database, 5000);
    return database;
}

static sqlite3 *open_registry_readwrite(char *error, size_t error_size) {
    return open_registry_readwrite_at(g_registry_database_path, error, error_size);
}

static bool sqlite_exec_ok(sqlite3 *database, const char *sql, char *error, size_t error_size) {
    char *raw_error = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &raw_error);
    if (result == SQLITE_OK) return true;
    snprintf(error, error_size, "%s", raw_error ? raw_error : sqlite3_errmsg(database));
    if (raw_error) sqlite3_free(raw_error);
    return false;
}

static bool frontends_has_column(sqlite3 *database, const char *name, char *error, size_t error_size) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(frontends);", -1, &statement, NULL) != SQLITE_OK) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        return false;
    }
    bool has_column = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *column_name = (const char *)sqlite3_column_text(statement, 1);
        if (column_name && strcmp(column_name, name) == 0) {
            has_column = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return has_column;
}

static bool ensure_registry_schema(sqlite3 *database, char *error, size_t error_size) {
    if (!sqlite_exec_ok(database,
                        "CREATE TABLE IF NOT EXISTS backends ("
                        "service_id TEXT PRIMARY KEY,"
                        "display_name TEXT NOT NULL DEFAULT '',"
                        "icon TEXT,"
                        "service_unit TEXT"
                        ");"
                        "CREATE TABLE IF NOT EXISTS frontends ("
                        "url TEXT PRIMARY KEY,"
                        "service_id TEXT,"
                        "name TEXT NOT NULL,"
                        "port INTEGER NOT NULL DEFAULT 0,"
                        "socket_path TEXT NOT NULL DEFAULT '',"
                        "icon TEXT,"
                        "is_home_screen INTEGER NOT NULL DEFAULT 0,"
                        "list TEXT"
                        ");"
                        "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);"
                        "CREATE TABLE IF NOT EXISTS log_files ("
                        "path TEXT PRIMARY KEY,"
                        "service_id TEXT NOT NULL"
                        ");"
                        "CREATE INDEX IF NOT EXISTS log_files_service_id_idx ON log_files(service_id);"
                        "CREATE TABLE IF NOT EXISTS systemd_backends ("
                        "service_id TEXT PRIMARY KEY,"
                        "unit_name TEXT NOT NULL,"
                        "scope TEXT NOT NULL DEFAULT 'user'"
                        ");"
                        "CREATE TABLE IF NOT EXISTS launchd_backends ("
                        "service_id TEXT PRIMARY KEY,"
                        "plist_path TEXT NOT NULL,"
                        "owns_plist INTEGER NOT NULL DEFAULT 0"
                        ");",
                        error,
                        error_size)) {
        return false;
    }
    if (!frontends_has_column(database, "is_home_screen", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN is_home_screen INTEGER NOT NULL DEFAULT 0;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!frontends_has_column(database, "list", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN list TEXT;",
                            error,
                            error_size)) {
            return false;
        }
    }
    return true;
}

static void migrate_registry_schema_if_writable(const char *path) {
    if (!path || !path[0] || access(path, W_OK) != 0) return;
    char error[512] = "";
    sqlite3 *database = open_registry_readwrite_at(path, error, sizeof(error));
    if (!database) return;
    (void)ensure_registry_schema(database, error, sizeof(error));
    sqlite3_close(database);
}

static const char *sqlite_column_text_or_empty(sqlite3_stmt *statement, int column) {
    const unsigned char *value = sqlite3_column_text(statement, column);
    return value ? (const char *)value : "";
}

static bool sqlite_table_exists(sqlite3 *database, const char *table_name) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, table_name, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return exists;
}

static void append_systemd_status_scope(const char *scope) {
    const char *scope_argument = (scope && strcmp(scope, "system") == 0) ? "--system" : "--user";
    char command[256];
    snprintf(command,
             sizeof(command),
             "timeout 2s systemctl %s list-units --all --type=service --type=socket --no-legend --no-pager --plain 2>/dev/null",
             scope_argument);

    FILE *pipe = popen(command, "r");
    if (!pipe) return;

    char line[2048];
    while (g_systemd_status_cache.count < MAX_SYSTEMD_STATUS_ENTRIES &&
           fgets(line, sizeof(line), pipe)) {
        char unit_name[256] = "";
        char load_state[32] = "";
        char active_state[32] = "";
        if (sscanf(line, "%255s %31s %31s", unit_name, load_state, active_state) != 3) {
            continue;
        }
        if (!safe_unit_name(unit_name)) {
            continue;
        }
        SystemdStatusEntry *entry = &g_systemd_status_cache.entries[g_systemd_status_cache.count++];
        snprintf(entry->unit_name, sizeof(entry->unit_name), "%s", unit_name);
        snprintf(entry->scope, sizeof(entry->scope), "%s", scope && scope[0] ? scope : "user");
        snprintf(entry->active_state, sizeof(entry->active_state), "%s", active_state);
    }
    pclose(pipe);
}

static void refresh_systemd_status_cache_if_needed(void) {
    int64_t now = monotonic_milliseconds();
    if (g_systemd_status_cache.refreshed_at_ms > 0 &&
        now - g_systemd_status_cache.refreshed_at_ms < SYSTEMD_STATUS_CACHE_TTL_MS) {
        return;
    }

    g_systemd_status_cache.count = 0;
    g_systemd_status_cache.refreshed_at_ms = now;
    append_systemd_status_scope("user");
#ifndef __APPLE__
    append_systemd_status_scope("system");
#endif
}

static const char *cached_systemd_active_state(const char *unit_name, const char *scope) {
    if (!safe_unit_name(unit_name)) return NULL;
    const char *normalized_scope = (scope && strcmp(scope, "system") == 0) ? "system" : "user";
    refresh_systemd_status_cache_if_needed();
    for (size_t i = 0; i < g_systemd_status_cache.count; i++) {
        SystemdStatusEntry *entry = &g_systemd_status_cache.entries[i];
        if (strcmp(entry->scope, normalized_scope) == 0 &&
            strcmp(entry->unit_name, unit_name) == 0) {
            return entry->active_state;
        }
    }
    return NULL;
}

static void systemd_status(const char *unit_name, const char *scope, char *out, size_t out_size) {
    if (!safe_unit_name(unit_name)) {
        snprintf(out, out_size, "unknown");
        return;
    }
    const char *active_state = cached_systemd_active_state(unit_name, scope);
    if (active_state && strcmp(active_state, "active") == 0) {
        snprintf(out, out_size, "running");
    } else if (!active_state ||
               strcmp(active_state, "inactive") == 0 ||
               strcmp(active_state, "failed") == 0) {
        for (size_t i = 0; i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
            if (!kBundledApps[i].socket_activated || strcmp(kBundledApps[i].unit_name, unit_name) != 0) {
                continue;
            }
            char socket_unit[256];
            systemd_socket_unit_name(unit_name, socket_unit, sizeof(socket_unit));
            if (safe_unit_name(socket_unit)) {
                const char *socket_active_state = cached_systemd_active_state(socket_unit, scope);
                if (socket_active_state && strcmp(socket_active_state, "active") == 0) {
                    snprintf(out, out_size, "available");
                    return;
                }
            }
        }
        snprintf(out, out_size, "stopped");
    } else {
        snprintf(out, out_size, "unknown");
    }
}

static void systemd_unit_path(const char *unit_name, const char *scope, char *out, size_t out_size) {
    if (!unit_name || !unit_name[0]) {
        out[0] = '\0';
        return;
    }
    if (strchr(unit_name, '/')) {
        snprintf(out, out_size, "%s", unit_name);
        return;
    }
    if (scope && strcmp(scope, "system") == 0) {
        snprintf(out, out_size, "/etc/systemd/system/%s", unit_name);
        return;
    }
    snprintf(out, out_size, "%s/.config/systemd/user/%s", home_directory(), unit_name);
}

#ifdef __APPLE__
static bool launchd_label_is_safe(const char *label) {
    if (!label || !label[0] || strlen(label) > 240) return false;
    for (const char *p = label; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static void launchd_domain_for_plist(const char *plist_path, char *out, size_t out_size) {
    if (plist_path && strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0) {
        snprintf(out, out_size, "system");
    } else {
        snprintf(out, out_size, "gui/%d", (int)getuid());
    }
}

static bool launchd_is_system_domain(const char *domain) {
    return domain && strcmp(domain, "system") == 0;
}

static void launchd_status(const char *label,
                           const char *plist_path,
                           char *out,
                           size_t out_size) {
    if (!launchd_label_is_safe(label)) {
        snprintf(out, out_size, "unknown");
        return;
    }

    char domain[64];
    launchd_domain_for_plist(plist_path, domain, sizeof(domain));

    char quoted_target[384];
    char target[320];
    snprintf(target, sizeof(target), "%s/%s", domain, label);
    shell_quote(target, quoted_target, sizeof(quoted_target));

    char command[512];
    snprintf(command, sizeof(command), "launchctl print %s 2>/dev/null | grep -q 'state = running'", quoted_target);
    int result = system(command);
    if (result == 0) {
        snprintf(out, out_size, "running");
        return;
    }

    snprintf(command, sizeof(command), "launchctl print %s 2>/dev/null | grep -q 'passive = 1'", quoted_target);
    result = system(command);
    if (result == 0) {
        snprintf(out, out_size, "awaiting");
        return;
    }

    if (plist_path && plist_path[0] && access(plist_path, F_OK) == 0) {
        snprintf(out, out_size, "stopped");
    } else {
        snprintf(out, out_size, "registered");
    }
}

static bool run_launchctl_capture(const char *command, char *message, size_t message_size) {
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        snprintf(message, message_size, "Failed to run launchctl: %s", strerror(errno));
        return false;
    }
    size_t offset = 0;
    while (offset + 1 < message_size) {
        size_t got = fread(message + offset, 1, message_size - offset - 1, pipe);
        offset += got;
        if (got == 0) break;
    }
    message[offset] = '\0';
    int status = pclose(pipe);
    return status == 0;
}

static void unload_launchd_service_if_needed(const char *label, const char *domain) {
    if (!launchd_label_is_safe(label) || launchd_is_system_domain(domain)) return;
    char target[320];
    char quoted_target[384];
    snprintf(target, sizeof(target), "%s/%s", domain, label);
    shell_quote(target, quoted_target, sizeof(quoted_target));
    char command[512];
    snprintf(command, sizeof(command), "launchctl bootout %s >/dev/null 2>&1 || true", quoted_target);
    system(command);
}

static bool run_launchd_operation(const char *label,
                                  const char *plist_path,
                                  const char *operation,
                                  char *message,
                                  size_t message_size) {
    if (!launchd_label_is_safe(label)) {
        snprintf(message, message_size, "Invalid launchd label.");
        return false;
    }

    char domain[64];
    launchd_domain_for_plist(plist_path, domain, sizeof(domain));
    if (launchd_is_system_domain(domain)) {
        snprintf(message, message_size, "System launchd services are not supported yet.");
        return false;
    }

    char target[320];
    char quoted_target[384];
    snprintf(target, sizeof(target), "%s/%s", domain, label);
    shell_quote(target, quoted_target, sizeof(quoted_target));

    if (strcmp(operation, "stop") == 0) {
        char command[512];
        snprintf(command, sizeof(command), "launchctl bootout %s 2>&1", quoted_target);
        if (run_launchctl_capture(command, message, message_size)) {
            if (message[0] == '\0') snprintf(message, message_size, "ok");
            return true;
        }
        if (strstr(message, "No such process") || strstr(message, "Could not find service")) {
            snprintf(message, message_size, "ok");
            return true;
        }
        if (message[0] == '\0') snprintf(message, message_size, "Failed to stop launchd service.");
        return false;
    }

    if (!(strcmp(operation, "start") == 0 || strcmp(operation, "restart") == 0)) {
        snprintf(message, message_size, "Unsupported operation.");
        return false;
    }
    if (!plist_path || !plist_path[0] || access(plist_path, R_OK) != 0) {
        snprintf(message, message_size, "LaunchAgent plist is missing or unreadable.");
        return false;
    }

    char quoted_domain[96];
    char quoted_plist[PATH_MAX + 8];
    shell_quote(domain, quoted_domain, sizeof(quoted_domain));
    shell_quote(plist_path, quoted_plist, sizeof(quoted_plist));
    unload_launchd_service_if_needed(label, domain);

    char command[PATH_MAX + 512];
    snprintf(command, sizeof(command), "launchctl enable %s >/dev/null 2>&1 || true", quoted_target);
    system(command);
    snprintf(command, sizeof(command), "launchctl bootstrap %s %s 2>&1", quoted_domain, quoted_plist);
    if (!run_launchctl_capture(command, message, message_size)) {
        if (message[0] == '\0') snprintf(message, message_size, "Failed to bootstrap launchd service.");
        return false;
    }

    snprintf(command, sizeof(command), "launchctl kickstart -k %s 2>&1", quoted_target);
    if (!run_launchctl_capture(command, message, message_size)) {
        if (message[0] == '\0') snprintf(message, message_size, "Failed to kickstart launchd service.");
        return false;
    }
    if (message[0] == '\0') snprintf(message, message_size, "ok");
    return true;
}

static bool run_launchd_operation_privileged(const char *label,
                                             const char *plist_path,
                                             const char *operation,
                                             const char *sudo_password,
                                             bool *needs_password,
                                             char *message,
                                             size_t message_size) {
    if (needs_password) *needs_password = false;
    if (!launchd_label_is_safe(label)) {
        snprintf(message, message_size, "Invalid launchd label.");
        return false;
    }

    char domain[64];
    launchd_domain_for_plist(plist_path, domain, sizeof(domain));
    if (!launchd_is_system_domain(domain)) {
        return run_launchd_operation(label, plist_path, operation, message, message_size);
    }

    char target[320];
    char quoted_target[384];
    snprintf(target, sizeof(target), "system/%s", label);
    shell_quote(target, quoted_target, sizeof(quoted_target));

    char command[PATH_MAX + 640];
    if (strcmp(operation, "stop") == 0) {
        snprintf(command, sizeof(command), "launchctl bootout %s 2>&1 || true", quoted_target);
    } else if (strcmp(operation, "start") == 0 || strcmp(operation, "restart") == 0) {
        if (!plist_path || !plist_path[0]) {
            snprintf(message, message_size, "LaunchDaemon plist path is missing.");
            return false;
        }
        char quoted_plist[PATH_MAX + 8];
        shell_quote(plist_path, quoted_plist, sizeof(quoted_plist));
        snprintf(command,
                 sizeof(command),
                 "launchctl bootout %s >/dev/null 2>&1 || true; "
                 "launchctl bootstrap system %s 2>&1; "
                 "launchctl kickstart -k %s 2>&1",
                 quoted_target,
                 quoted_plist,
                 quoted_target);
    } else {
        snprintf(message, message_size, "Unsupported operation.");
        return false;
    }

    int exit_status = -1;
    bool ok = run_sudo_shell(command, sudo_password, message, message_size, &exit_status);
    if (!ok && sudo_failure_needs_password(message, exit_status)) {
        if (needs_password) *needs_password = true;
        snprintf(message, message_size, "Administrator password required.");
    } else if (!ok && message[0] == '\0') {
        snprintf(message, message_size, "launchctl %s failed.", operation);
    } else if (ok && message[0] == '\0') {
        snprintf(message, message_size, "ok");
    }
    return ok;
}
#endif

static bool run_systemd_operation(const char *unit_name,
                                  const char *scope,
                                  const char *operation,
                                  const char *sudo_password,
                                  bool *needs_password,
                                  char *message,
                                  size_t message_size) {
    if (needs_password) *needs_password = false;
    if (!safe_unit_name(unit_name)) {
        snprintf(message, message_size, "Invalid systemd unit name.");
        return false;
    }
    if (!(strcmp(operation, "start") == 0 ||
          strcmp(operation, "stop") == 0 ||
          strcmp(operation, "restart") == 0)) {
        snprintf(message, message_size, "Unsupported operation.");
        return false;
    }

    char quoted_unit[320];
    shell_quote(unit_name, quoted_unit, sizeof(quoted_unit));
    char command[768];
    const char *scope_argument = (scope && strcmp(scope, "system") == 0) ? "--system" : "--user";
    snprintf(command, sizeof(command), "systemctl %s %s %s 2>&1", scope_argument, operation, quoted_unit);

    if (strcmp(scope_argument, "--system") == 0) {
        int exit_status = -1;
        bool ok = run_sudo_shell(command, sudo_password, message, message_size, &exit_status);
        if (!ok && sudo_failure_needs_password(message, exit_status)) {
            if (needs_password) *needs_password = true;
            snprintf(message, message_size, "Administrator password required.");
        } else if (!ok && message[0] == '\0') {
            snprintf(message, message_size, "systemctl %s failed.", operation);
        }
        if (ok && message[0] == '\0') snprintf(message, message_size, "ok");
        return ok;
    }

    FILE *pipe = popen(command, "r");
    if (!pipe) {
        snprintf(message, message_size, "Failed to run systemctl: %s", strerror(errno));
        return false;
    }

    size_t offset = 0;
    while (offset + 1 < message_size) {
        size_t got = fread(message + offset, 1, message_size - offset - 1, pipe);
        offset += got;
        if (got == 0) break;
    }
    message[offset] = '\0';
    int status = pclose(pipe);
    if (status == 0) {
        if (message[0] == '\0') snprintf(message, message_size, "ok");
        return true;
    }
    if (message[0] == '\0') {
        snprintf(message, message_size, "systemctl %s failed.", operation);
    }
    return false;
}

static bool lookup_systemd_backend(sqlite3 *database,
                                   const char *service_id,
                                   char *unit_name,
                                   size_t unit_name_size,
                                   char *scope,
                                   size_t scope_size) {
    if (!sqlite_table_exists(database, "systemd_backends")) return false;
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT unit_name, COALESCE(scope, 'user') FROM systemd_backends WHERE service_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) {
        snprintf(unit_name, unit_name_size, "%s", sqlite_column_text_or_empty(statement, 0));
        snprintf(scope, scope_size, "%s", sqlite_column_text_or_empty(statement, 1));
    }
    sqlite3_finalize(statement);
    return found;
}

static bool lookup_systemd_backend_any(const char *service_id,
                                       char *unit_name,
                                       size_t unit_name_size,
                                       char *scope,
                                       size_t scope_size) {
    char error[512] = "";
    sqlite3 *database = open_registry_readonly(error, sizeof(error));
    if (database) {
        bool found = lookup_systemd_backend(database, service_id, unit_name, unit_name_size, scope, scope_size);
        sqlite3_close(database);
        if (found) return true;
    }

    database = open_system_registry_readonly(error, sizeof(error));
    if (database) {
        bool found = lookup_systemd_backend(database, service_id, unit_name, unit_name_size, scope, scope_size);
        sqlite3_close(database);
        if (found) return true;
    }
    return false;
}

#ifdef __APPLE__
static bool lookup_launchd_backend(sqlite3 *database,
                                   const char *service_id,
                                   char *plist_path,
                                   size_t plist_path_size,
                                   int *owns_plist) {
    if (!sqlite_table_exists(database, "launchd_backends")) return false;
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT plist_path, COALESCE(owns_plist, 0) FROM launchd_backends WHERE service_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) {
        snprintf(plist_path, plist_path_size, "%s", sqlite_column_text_or_empty(statement, 0));
        if (owns_plist) *owns_plist = sqlite3_column_int(statement, 1);
    }
    sqlite3_finalize(statement);
    return found;
}

static bool lookup_launchd_backend_any(const char *service_id,
                                       char *plist_path,
                                       size_t plist_path_size,
                                       int *owns_plist) {
    char error[512] = "";
    sqlite3 *database = open_registry_readonly(error, sizeof(error));
    if (database) {
        bool found = lookup_launchd_backend(database, service_id, plist_path, plist_path_size, owns_plist);
        sqlite3_close(database);
        if (found) return true;
    }
    database = open_system_registry_readonly(error, sizeof(error));
    if (database) {
        bool found = lookup_launchd_backend(database, service_id, plist_path, plist_path_size, owns_plist);
        sqlite3_close(database);
        if (found) return true;
    }
    return false;
}
#endif

static bool append_frontends_json(StringBuilder *builder, sqlite3 *database, const char *service_id) {
    sqlite3_stmt *statement = NULL;
    char column_error[256] = "";
    bool has_home_screen_column = frontends_has_column(database, "is_home_screen", column_error, sizeof(column_error));
    bool has_list_column = frontends_has_column(database, "list", column_error, sizeof(column_error));
    const char *sql = has_home_screen_column && has_list_column ?
        "SELECT f.name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), COALESCE(f.is_home_screen, 0), COALESCE(NULLIF(f.icon, ''), COALESCE(b.icon, '')), COALESCE(f.list, '') "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY COALESCE(f.list, ''), f.name, f.url;" :
        has_home_screen_column ?
        "SELECT f.name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), COALESCE(f.is_home_screen, 0), COALESCE(NULLIF(f.icon, ''), COALESCE(b.icon, '')), '' "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY f.name, f.url;" :
        has_list_column ?
        "SELECT f.name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), 0, COALESCE(NULLIF(f.icon, ''), COALESCE(b.icon, '')), COALESCE(f.list, '') "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY COALESCE(f.list, ''), f.name, f.url;" :
        "SELECT f.name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), 0, COALESCE(NULLIF(f.icon, ''), COALESCE(b.icon, '')), '' "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY f.name, f.url;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);

    bool ok = sb_append(builder, "[");
    bool first = true;
    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        if (!first) ok = sb_append(builder, ",");
        first = false;
        char number[64];
        ok = ok && sb_append(builder, "{\"name\":");
        ok = ok && sb_append_json_string(builder, sqlite_column_text_or_empty(statement, 0));
        ok = ok && sb_append(builder, ",\"url\":");
        ok = ok && sb_append_json_string(builder, sqlite_column_text_or_empty(statement, 1));
        snprintf(number, sizeof(number), ",\"port\":%d", sqlite3_column_int(statement, 2));
        ok = ok && sb_append(builder, number);
        ok = ok && sb_append(builder, ",\"socketPath\":");
        ok = ok && sb_append_json_string(builder, sqlite_column_text_or_empty(statement, 3));
        ok = ok && sb_append(builder, ",\"isHomeScreen\":");
        ok = ok && sb_append(builder, sqlite3_column_int(statement, 4) != 0 ? "true" : "false");
        ok = ok && sb_append(builder, ",\"iconPath\":");
        ok = ok && sb_append_json_string(builder, sqlite_column_text_or_empty(statement, 5));
        ok = ok && sb_append(builder, ",\"iconData\":");
        ok = ok && sb_append_base64_file_json_string(builder, sqlite_column_text_or_empty(statement, 5));
        ok = ok && sb_append(builder, ",\"list\":");
        ok = ok && sb_append_json_string(builder, sqlite_column_text_or_empty(statement, 6));
        ok = ok && sb_append(builder, "}");
    }
    sqlite3_finalize(statement);
    return ok && sb_append(builder, "]");
}

static bool append_log_files_json(StringBuilder *builder, sqlite3 *database, const char *service_id) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT path FROM log_files WHERE service_id = ? ORDER BY path;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);

    bool ok = sb_append(builder, "[");
    bool first = true;
    int index = 0;
    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        const char *path = sqlite_column_text_or_empty(statement, 0);
        char expanded[PATH_MAX];
        expand_tilde_path(path, expanded, sizeof(expanded));
        struct stat st;
        bool has_stat = stat(expanded, &st) == 0;

        if (!first) ok = sb_append(builder, ",");
        first = false;
        char number[128];
        ok = ok && sb_append(builder, "{\"identifier\":");
        char identifier[PATH_MAX + 80];
        snprintf(identifier, sizeof(identifier), "backend-log-file:%s:%d", service_id, index);
        ok = ok && sb_append_json_string(builder, identifier);
        ok = ok && sb_append(builder, ",\"displayName\":");
        const char *last_slash = strrchr(path, '/');
        ok = ok && sb_append_json_string(builder, last_slash && last_slash[1] ? last_slash + 1 : path);
        ok = ok && sb_append(builder, ",\"path\":");
        ok = ok && sb_append_json_string(builder, path);
        snprintf(number, sizeof(number), ",\"size\":%llu,\"modified\":%.3f,\"readable\":%s",
                 has_stat ? (unsigned long long)st.st_size : 0ULL,
                 has_stat ? (double)st.st_mtime : 0.0,
                 has_stat ? "true" : "false");
        ok = ok && sb_append(builder, number);
        ok = ok && sb_append(builder, "}");
        index++;
    }
    sqlite3_finalize(statement);
    return ok && sb_append(builder, "]");
}

static bool append_registered_backends_json(StringBuilder *builder,
                                            sqlite3 *database,
                                            bool *first,
                                            bool *bundled_installed,
                                            size_t bundled_installed_count) {
    sqlite3_stmt *statement = NULL;
    bool has_launchd_table = sqlite_table_exists(database, "launchd_backends");
    bool has_systemd_table = sqlite_table_exists(database, "systemd_backends");
    char sql[2048];
    snprintf(sql, sizeof(sql),
             "SELECT b.service_id, COALESCE(b.display_name, ''), %s, %s, %s, %s "
             "FROM backends b %s %s "
             "ORDER BY COALESCE(NULLIF(b.display_name, ''), b.service_id) COLLATE NOCASE;",
             has_systemd_table
                 ? "COALESCE(NULLIF(b.service_unit, ''), COALESCE(s.unit_name, ''))"
                 : "COALESCE(b.service_unit, '')",
             has_launchd_table ? "COALESCE(l.plist_path, '')" : "''",
             has_launchd_table ? "COALESCE(l.owns_plist, 0)" : "0",
             has_systemd_table ? "COALESCE(s.scope, 'user')" : "''",
             has_systemd_table ? "LEFT JOIN systemd_backends s ON s.service_id = b.service_id" : "",
             has_launchd_table ? "LEFT JOIN launchd_backends l ON l.service_id = b.service_id" : "");
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;

    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        const char *service_id = sqlite_column_text_or_empty(statement, 0);
        const char *display_name = sqlite_column_text_or_empty(statement, 1);
        const char *service_unit = sqlite_column_text_or_empty(statement, 2);
        const char *plist_path = sqlite_column_text_or_empty(statement, 3);
        int owns_plist = sqlite3_column_int(statement, 4);
        const char *service_scope = sqlite_column_text_or_empty(statement, 5);
        char effective_service_scope[32];
        snprintf(effective_service_scope, sizeof(effective_service_scope), "%s", service_scope);
        const BundledAppDefinition *bundled_app = bundled_app_for_service_id(service_id);
        bool is_self = is_home_screen_service_id(service_id);
        if (bundled_app) {
            size_t bundled_index = (size_t)(bundled_app - kBundledApps);
            if (bundled_index < bundled_installed_count) {
                bundled_installed[bundled_index] = true;
            }
        }
        char status[32] = "unknown";
        bool has_systemd_unit = service_unit[0] && has_systemd_table;
        bool has_launchd_unit = plist_path[0] && has_launchd_table;
        bool can_control = (has_systemd_unit || has_launchd_unit) && !is_self;
        bool can_uninstall = (has_systemd_unit || has_launchd_unit) && !is_self;
#ifdef __APPLE__
        if (has_launchd_unit) {
            snprintf(effective_service_scope,
                     sizeof(effective_service_scope),
                     "%s",
                     strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0 ? "system" : "user");
        }
#endif
        if (has_systemd_unit) {
            systemd_status(service_unit, effective_service_scope, status, sizeof(status));
#ifdef __APPLE__
        } else if (has_launchd_unit) {
            launchd_status(service_id, plist_path, status, sizeof(status));
#endif
        }
        char service_unit_path[PATH_MAX] = "";
        if (service_unit[0]) {
            systemd_unit_path(service_unit, effective_service_scope, service_unit_path, sizeof(service_unit_path));
        } else if (plist_path[0]) {
            snprintf(service_unit_path, sizeof(service_unit_path), "%s", plist_path);
        }

        if (!*first) ok = sb_append(builder, ",");
        *first = false;
        ok = ok && sb_append(builder, "{\"serviceID\":");
        ok = ok && sb_append_json_string(builder, service_id);
        ok = ok && sb_append(builder, ",\"displayName\":");
        ok = ok && sb_append_json_string(builder, display_name[0] ? display_name : service_id);
        ok = ok && sb_append(builder, ",\"serviceUnit\":");
        ok = ok && sb_append_json_string(builder, service_unit);
        ok = ok && sb_append(builder, ",\"serviceUnitPath\":");
        ok = ok && sb_append_json_string(builder, service_unit_path);
        ok = ok && sb_append(builder, ",\"serviceScope\":");
        ok = ok && sb_append_json_string(builder, effective_service_scope);
        ok = ok && sb_append(builder, ",\"status\":");
        ok = ok && sb_append_json_string(builder, status);
        ok = ok && sb_append(builder, ",\"canControl\":");
        ok = ok && sb_append(builder, can_control ? "true" : "false");
        ok = ok && sb_append(builder, ",\"canUninstall\":");
        ok = ok && sb_append(builder, can_uninstall ? "true" : "false");
        ok = ok && sb_append(builder, ",\"isBundled\":");
        ok = ok && sb_append(builder, bundled_app ? "true" : "false");
        ok = ok && sb_append(builder, ",\"isInstalled\":true");
        ok = ok && sb_append(builder, ",\"launchdPlistPath\":");
        ok = ok && sb_append_json_string(builder, plist_path);
        ok = ok && sb_append(builder, ",\"ownsLaunchdPlist\":");
        ok = ok && sb_append(builder, owns_plist ? "true" : "false");
        ok = ok && sb_append(builder, ",\"frontends\":");
        ok = ok && append_frontends_json(builder, database, service_id);
        ok = ok && sb_append(builder, ",\"logFiles\":");
        ok = ok && append_log_files_json(builder, database, service_id);
        ok = ok && sb_append(builder, "}");
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static void send_backends_response(int fd) {
    StringBuilder builder = {0};
    char user_error[512] = "";
    char system_error[512] = "";
    migrate_registry_schema_if_writable(g_registry_database_path);
    migrate_registry_schema_if_writable(g_system_registry_database_path);
    sqlite3 *user_database = open_registry_readonly(user_error, sizeof(user_error));
    sqlite3 *system_database = open_system_registry_readonly(system_error, sizeof(system_error));

    bool ok = sb_append(&builder, "{\"databasePath\":") &&
              sb_append_json_string(&builder, g_registry_database_path) &&
              sb_append(&builder, ",\"systemDatabasePath\":") &&
              sb_append_json_string(&builder, g_system_registry_database_path) &&
              sb_append(&builder, ",\"error\":");
    if (!user_database && !system_database) {
        ok = ok && sb_append_json_string(&builder, user_error[0] ? user_error : system_error);
        ok = ok && sb_append(&builder, ",\"backends\":[]}");
        if (!ok) {
            free(builder.data);
            send_text_response(fd, 500, "out of memory\n");
            return;
        }
        send_response(fd, 200, "OK", "application/json; charset=utf-8", builder.data, builder.length);
        free(builder.data);
        return;
    }

    ok = ok && sb_append_json_string(&builder, "") && sb_append(&builder, ",\"backends\":[");
    bool first = true;
    bool bundled_installed[sizeof(kBundledApps) / sizeof(kBundledApps[0])] = {0};
    if (user_database) {
        ok = ok && append_registered_backends_json(&builder, user_database, &first, bundled_installed,
                                                   sizeof(bundled_installed) / sizeof(bundled_installed[0]));
    }
    if (system_database) {
        ok = ok && append_registered_backends_json(&builder, system_database, &first, bundled_installed,
                                                   sizeof(bundled_installed) / sizeof(bundled_installed[0]));
    }

    for (size_t i = 0; ok && i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        if (!bundled_app_is_available_on_platform(&kBundledApps[i])) continue;
        if (bundled_installed[i]) continue;
        const BundledAppDefinition *app = &kBundledApps[i];
        if (!first) ok = sb_append(&builder, ",");
        first = false;
        ok = ok && sb_append(&builder, "{\"serviceID\":");
        ok = ok && sb_append_json_string(&builder, app->service_id);
        ok = ok && sb_append(&builder, ",\"displayName\":");
        ok = ok && sb_append_json_string(&builder, app->display_name);
        ok = ok && sb_append(&builder, ",\"serviceUnit\":\"\",\"serviceUnitPath\":\"\",\"serviceScope\":\"user\",\"status\":\"available\",\"canControl\":true,\"canUninstall\":false,\"isBundled\":true,\"isInstalled\":false,\"launchdPlistPath\":\"\",\"ownsLaunchdPlist\":false,\"frontends\":[],\"logFiles\":[]}");
    }
    if (user_database) sqlite3_close(user_database);
    if (system_database) sqlite3_close(system_database);

    ok = ok && sb_append(&builder, "]}");
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "failed to read registry database\n");
        return;
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8", builder.data, builder.length);
    free(builder.data);
}

static bool resolve_log_path(sqlite3 *database, const char *service_id, int log_index,
                             char *path, size_t path_size) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT path FROM log_files WHERE service_id = ? ORDER BY path LIMIT 1 OFFSET ?;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, log_index < 0 ? 0 : log_index);
    bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) {
        snprintf(path, path_size, "%s", sqlite_column_text_or_empty(statement, 0));
    }
    sqlite3_finalize(statement);
    return found;
}

static bool resolve_log_path_any(const char *service_id, int log_index, char *path, size_t path_size, char *error, size_t error_size) {
    sqlite3 *database = open_registry_readonly(error, error_size);
    if (database) {
        bool found = resolve_log_path(database, service_id, log_index, path, path_size);
        sqlite3_close(database);
        if (found) return true;
    }

    database = open_system_registry_readonly(error, error_size);
    if (database) {
        bool found = resolve_log_path(database, service_id, log_index, path, path_size);
        sqlite3_close(database);
        if (found) return true;
    }
    return false;
}

static void send_log_json(int fd, const char *service_id, const char *path, const char *contents,
                          bool truncated, uint64_t file_size, double modified, const char *error) {
    StringBuilder builder = {0};
    char number[160];
    bool ok = sb_append(&builder, "{\"serviceID\":") &&
              sb_append_json_string(&builder, service_id ? service_id : "") &&
              sb_append(&builder, ",\"path\":") &&
              sb_append_json_string(&builder, path ? path : "") &&
              sb_append(&builder, ",\"contents\":") &&
              sb_append_json_string(&builder, contents ? contents : "") &&
              sb_append(&builder, ",\"isTruncated\":") &&
              sb_append(&builder, truncated ? "true" : "false");
    snprintf(number, sizeof(number), ",\"fileSize\":%llu,\"modified\":%.3f,\"error\":",
             (unsigned long long)file_size, modified);
    ok = ok && sb_append(&builder, number) &&
         sb_append_json_string(&builder, error ? error : "") &&
         sb_append(&builder, "}");
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8", builder.data, builder.length);
    free(builder.data);
}

static void send_action_json_ex(int fd, int status, bool ok_value, const char *message, bool needs_password) {
    StringBuilder builder = {0};
    bool ok = sb_append(&builder, "{\"ok\":") &&
              sb_append(&builder, ok_value ? "true" : "false") &&
              sb_append(&builder, ",\"message\":") &&
              sb_append_json_string(&builder, message ? message : "") &&
              sb_append(&builder, ",\"needsPassword\":") &&
              sb_append(&builder, needs_password ? "true" : "false") &&
              sb_append(&builder, "}");
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    const char *status_text = status == 200 ? "OK" :
                              status == 401 ? "Unauthorized" :
                              status == 400 ? "Bad Request" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "Error";
    send_response(fd, status, status_text, "application/json; charset=utf-8", builder.data, builder.length);
    free(builder.data);
}

static void send_action_json(int fd, int status, bool ok_value, const char *message) {
    send_action_json_ex(fd, status, ok_value, message, false);
}

static bool install_bundled_app(const BundledAppDefinition *app, const char *scope, const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool uninstall_backend(const char *service_id, const char *sudo_password, bool *needs_password, char *message, size_t message_size);

static bool clear_frontends_in_registry_at(const char *database_path, const char *service_id, char *error, size_t error_size) {
    sqlite3 *database = open_registry_readwrite_at(database_path, error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        sqlite3_close(database);
        return false;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    sqlite3_stmt *statement = NULL;
    if (ok) {
        ok = sqlite3_prepare_v2(database, "DELETE FROM frontends WHERE service_id = ?;", -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(database);
    return ok;
}

static void clear_frontends_in_system_registry_as_root(const char *service_id, const char *sudo_password) {
    char quoted_system_root[PATH_MAX + 8];
    char quoted_service_id[512];
    shell_quote(kSystemOuterAgentRoot, quoted_system_root, sizeof(quoted_system_root));
    shell_quote(service_id, quoted_service_id, sizeof(quoted_service_id));

    char command[8192];
    snprintf(command,
             sizeof(command),
             "REGISTRY_ROOT=%s SERVICE_ID=%s python3 - <<'__HOMESCREEN_CLEAR_FRONTENDS__'\n"
             "import os, sqlite3\n"
             "database_path = os.path.join(os.environ['REGISTRY_ROOT'], 'registry.sqlite3')\n"
             "if os.path.exists(database_path):\n"
             "    database = sqlite3.connect(database_path)\n"
             "    with database:\n"
             "        try:\n"
             "            database.execute('DELETE FROM frontends WHERE service_id = ?', (os.environ['SERVICE_ID'],))\n"
             "        except sqlite3.OperationalError:\n"
             "            pass\n"
             "    database.close()\n"
             "__HOMESCREEN_CLEAR_FRONTENDS__\n",
             quoted_system_root,
             quoted_service_id);

    char output[1024] = "";
    int exit_status = -1;
    (void)run_sudo_shell(command, sudo_password, output, sizeof(output), &exit_status);
}

static void clear_frontends_after_successful_stop(const char *service_id, bool system_scope, const char *sudo_password) {
    char error[512] = "";
    (void)clear_frontends_in_registry_at(g_registry_database_path, service_id, error, sizeof(error));
    error[0] = '\0';
    if (g_system_registry_database_path[0] && access(g_system_registry_database_path, W_OK) == 0) {
        (void)clear_frontends_in_registry_at(g_system_registry_database_path, service_id, error, sizeof(error));
    } else if (system_scope) {
        clear_frontends_in_system_registry_as_root(service_id, sudo_password);
    }
}

static void send_control_response(int fd, const char *query, const char *body) {
    char service_id[PATH_MAX] = "";
    char operation[32] = "";
    char sudo_password[PATH_MAX] = "";
    if (!query_value_any(query, body, "serviceID", service_id, sizeof(service_id)) ||
        !query_value_any(query, body, "operation", operation, sizeof(operation))) {
        send_action_json(fd, 400, false, "Missing serviceID or operation.");
        return;
    }
    query_value_any(query, body, "sudoPassword", sudo_password, sizeof(sudo_password));
    log_event("Control request operation=%s serviceID=%s.", operation, service_id);

    if (is_home_screen_service_id(service_id)) {
        log_event("Rejected control request for Home Screen itself: operation=%s.", operation);
        send_action_json(fd, 400, false, "Home Screen cannot stop, start, or uninstall itself.");
        return;
    }

    if (strcmp(operation, "run") == 0 || strcmp(operation, "install") == 0 ||
        strcmp(operation, "runRoot") == 0 || strcmp(operation, "installRoot") == 0 ||
        strcmp(operation, "runUser") == 0 || strcmp(operation, "installUser") == 0) {
        const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
        if (!app) {
            send_action_json(fd, 404, false, "This backend is not a bundled app.");
            return;
        }
        char message[4096] = "";
        bool needs_password = false;
        const char *scope = (strcmp(operation, "runRoot") == 0 || strcmp(operation, "installRoot") == 0) ? "system" : "user";
        if (strcmp(scope, "user") == 0) {
            char existing_unit[256] = "";
            char existing_scope[32] = "user";
            bool found = lookup_systemd_backend_any(service_id, existing_unit, sizeof(existing_unit), existing_scope, sizeof(existing_scope));
            if (found && strcmp(existing_scope, "system") == 0) {
                bool removed = uninstall_backend(service_id, sudo_password, &needs_password, message, sizeof(message));
                if (!removed) {
                    log_event("Failed to uninstall existing root install before reinstalling %s as user: %s", service_id, message);
                    send_action_json_ex(fd, needs_password ? 401 : 500, false, message, needs_password);
                    return;
                }
            }
#ifdef __APPLE__
            char existing_plist[PATH_MAX] = "";
            int owns_existing_plist = 0;
            if (lookup_launchd_backend_any(service_id, existing_plist, sizeof(existing_plist), &owns_existing_plist) &&
                strncmp(existing_plist, "/Library/LaunchDaemons/", 23) == 0) {
                bool removed = uninstall_backend(service_id, sudo_password, &needs_password, message, sizeof(message));
                if (!removed) {
                    log_event("Failed to uninstall existing root launchd install before reinstalling %s as user: %s", service_id, message);
                    send_action_json_ex(fd, needs_password ? 401 : 500, false, message, needs_password);
                    return;
                }
            }
#endif
        }
        bool ok = install_bundled_app(app, scope, sudo_password, &needs_password, message, sizeof(message));
        log_event("%s bundled app %s as %s: %s",
                  ok ? "Installed" : "Failed to install",
                  app->service_id,
                  scope,
                  message);
        send_action_json_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }

    if (strcmp(operation, "uninstall") == 0) {
        char message[4096] = "";
        bool needs_password = false;
        bool ok = uninstall_backend(service_id, sudo_password, &needs_password, message, sizeof(message));
        log_event("%s backend %s: %s", ok ? "Uninstalled" : "Failed to uninstall", service_id, message);
        send_action_json_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }

#ifdef __APPLE__
    char plist_path[PATH_MAX] = "";
    int owns_plist = 0;
    if (lookup_launchd_backend_any(service_id, plist_path, sizeof(plist_path), &owns_plist)) {
        (void)owns_plist;
        char message[4096] = "";
        bool needs_password = false;
        bool ok = run_launchd_operation_privileged(service_id,
                                                   plist_path,
                                                   operation,
                                                   sudo_password,
                                                   &needs_password,
                                                   message,
                                                   sizeof(message));
        if (ok && strcmp(operation, "stop") == 0) {
            clear_frontends_after_successful_stop(service_id,
                                                  strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0,
                                                  sudo_password);
        }
        log_event("%s launchd operation %s for %s: %s",
                  ok ? "Completed" : "Failed",
                  operation,
                  service_id,
                  message);
        send_action_json_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }
#endif

    char unit_name[256] = "";
    char scope[32] = "user";
    bool found = lookup_systemd_backend_any(service_id, unit_name, sizeof(unit_name), scope, sizeof(scope));
    if (!found) {
        send_action_json(fd, 404, false, "This backend does not have a registered systemd unit.");
        return;
    }

    char message[4096] = "";
    bool needs_password = false;
    bool ok = run_systemd_operation(unit_name, scope, operation, sudo_password, &needs_password, message, sizeof(message));
    if (ok && strcmp(operation, "stop") == 0) {
        clear_frontends_after_successful_stop(service_id, strcmp(scope, "system") == 0, sudo_password);
    }
    log_event("%s systemd operation %s for %s (%s): %s",
              ok ? "Completed" : "Failed",
              operation,
              service_id,
              scope,
              message);
    send_action_json_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
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

static bool write_text_file(const char *path, const char *contents, char *error, size_t error_size) {
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(directory)) {
            snprintf(error, error_size, "Failed to create %s: %s", directory, strerror(errno));
            return false;
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", path, strerror(errno));
        return false;
    }
    bool ok = queue_all(fd, contents, strlen(contents));
    if (close(fd) != 0) ok = false;
    if (!ok) {
        snprintf(error, error_size, "Failed to write %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool copy_file(const char *source, const char *destination, mode_t mode, char *error, size_t error_size) {
    int in_fd = open(source, O_RDONLY);
    if (in_fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", source, strerror(errno));
        return false;
    }

    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", destination);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(directory)) {
            close(in_fd);
            snprintf(error, error_size, "Failed to create %s: %s", directory, strerror(errno));
            return false;
        }
    }

    int out_fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out_fd < 0) {
        close(in_fd);
        snprintf(error, error_size, "Failed to open %s: %s", destination, strerror(errno));
        return false;
    }

    char buffer[65536];
    bool ok = true;
    for (;;) {
        ssize_t got = read(in_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            snprintf(error, error_size, "Failed to read %s: %s", source, strerror(errno));
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!queue_all(out_fd, buffer, (size_t)got)) {
            snprintf(error, error_size, "Failed to write %s: %s", destination, strerror(errno));
            ok = false;
            break;
        }
    }
    if (close(out_fd) != 0 && ok) {
        snprintf(error, error_size, "Failed to close %s: %s", destination, strerror(errno));
        ok = false;
    }
    close(in_fd);
    chmod(destination, mode);
    return ok;
}

static void run_shell_ignored(const char *command) {
    int result = system(command);
    (void)result;
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len &&
               p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool sudo_failure_needs_password(const char *output, int exit_status) {
    if (exit_status == 0) return false;
    return contains_case_insensitive(output, "password") ||
           contains_case_insensitive(output, "authentication") ||
           contains_case_insensitive(output, "try again") ||
           contains_case_insensitive(output, "sudo:");
}

static bool run_sudo_shell(const char *command, const char *password, char *output, size_t output_size, int *exit_status) {
    if (output_size > 0) output[0] = '\0';
    if (exit_status) *exit_status = -1;

    int stdin_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(output_pipe) != 0) {
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (output_pipe[1] >= 0) close(output_pipe[1]);
        snprintf(output, output_size, "Failed to create sudo pipes: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        snprintf(output, output_size, "Failed to fork sudo: %s", strerror(errno));
        return false;
    }

    bool has_password = password && password[0];
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (has_password) {
            execlp("sudo", "sudo", "-S", "-p", "", "sh", "-c", command, (char *)NULL);
        } else {
            execlp("sudo", "sudo", "-n", "sh", "-c", command, (char *)NULL);
        }
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(output_pipe[1]);
    if (has_password) {
        queue_all(stdin_pipe[1], password, strlen(password));
        queue_all(stdin_pipe[1], "\n", 1);
    }
    close(stdin_pipe[1]);

    size_t offset = 0;
    while (offset + 1 < output_size) {
        ssize_t got = read(output_pipe[0], output + offset, output_size - offset - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    if (output_size > 0) output[offset] = '\0';
    close(output_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) {
        if (exit_status) *exit_status = WEXITSTATUS(status);
        return WEXITSTATUS(status) == 0;
    }
    if (WIFSIGNALED(status)) {
        if (exit_status) *exit_status = 128 + WTERMSIG(status);
    }
    return false;
}

static bool run_root_script(const char *script_path, const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;
    char quoted_script[PATH_MAX + 8];
    shell_quote(script_path, quoted_script, sizeof(quoted_script));
    char command[PATH_MAX + 64];
    snprintf(command, sizeof(command), "sh %s", quoted_script);

    int exit_status = -1;
    bool ok = run_sudo_shell(command, sudo_password, message, message_size, &exit_status);
    if (!ok && sudo_failure_needs_password(message, exit_status)) {
        if (needs_password) *needs_password = true;
        snprintf(message, message_size, "Administrator password required.");
    } else if (!ok && message[0] == '\0') {
        snprintf(message, message_size, "Privileged operation failed.");
    }
    return ok;
}

static void unit_description_text(const char *value, char *out, size_t out_size) {
    size_t offset = 0;
    for (const char *p = value ? value : ""; *p && offset + 1 < out_size; p++) {
        out[offset++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    out[offset] = '\0';
}

static bool query_value_or_default(const char *query, const char *name, const char *default_value, char *dst, size_t dst_size) {
    if (query_value(query, name, dst, dst_size)) return true;
    snprintf(dst, dst_size, "%s", default_value ? default_value : "");
    return false;
}

static bool valid_port_text(const char *value) {
    if (!value || !value[0]) return false;
    char *end = NULL;
    long port = strtol(value, &end, 10);
    return end && *end == '\0' && port >= 1 && port <= 65535;
}

static bool make_blank_script(const char *service_id, const char *display_name, StringBuilder *builder) {
    char quoted_id[512];
    char quoted_name[1024];
    shell_quote(service_id, quoted_id, sizeof(quoted_id));
    shell_quote(display_name, quoted_name, sizeof(quoted_name));
    return sb_append(builder,
        "#!/bin/sh\n"
        "set -eu\n"
        "\n"
        "BACKEND_ID=") &&
        sb_append(builder, quoted_id) &&
        sb_append(builder, "\nDISPLAY_NAME=") &&
        sb_append(builder, quoted_name) &&
        sb_append(builder,
        "\n\n"
        "cleanup() {\n"
        "    if [ -n \"${OUTERCTL_PATH:-}\" ] && [ -x \"$OUTERCTL_PATH\" ]; then\n"
        "        \"$OUTERCTL_PATH\" app clear --backend \"$BACKEND_ID\" >/dev/null 2>&1 || true\n"
        "    fi\n"
        "}\n"
        "trap cleanup EXIT INT TERM\n"
        "\n"
        "# Outer Loop sets OUTERCTL_PATH before this script runs.\n"
        "# Keep your own startup logic here, in a file you control.\n"
        "# Example registration command, once your app is ready:\n"
        "# \"$OUTERCTL_PATH\" app add --backend \"$BACKEND_ID\" --port 9000 --name \"$DISPLAY_NAME\" --url \"127.0.0.1:9000/\"\n"
        "\n"
        "python3 -m http.server 9000\n");
}

static bool make_fixed_port_script(const char *service_id,
                                   const char *display_name,
                                   const char *port,
                                   const char *command,
                                   StringBuilder *builder) {
    char quoted_id[512];
    char quoted_name[1024];
    char quoted_port[64];
    shell_quote(service_id, quoted_id, sizeof(quoted_id));
    shell_quote(display_name, quoted_name, sizeof(quoted_name));
    shell_quote(port, quoted_port, sizeof(quoted_port));
    return sb_append(builder,
        "#!/bin/sh\n"
        "set -eu\n"
        "\n"
        "BACKEND_ID=") &&
        sb_append(builder, quoted_id) &&
        sb_append(builder, "\nDISPLAY_NAME=") &&
        sb_append(builder, quoted_name) &&
        sb_append(builder, "\nPORT=") &&
        sb_append(builder, quoted_port) &&
        sb_append(builder,
        "\nCMD_PID=''\n"
        "\n"
        "run_outerctl() {\n"
        "    if [ -n \"${OUTERCTL_PATH:-}\" ] && [ -x \"$OUTERCTL_PATH\" ]; then\n"
        "        \"$OUTERCTL_PATH\" \"$@\" >/dev/null 2>&1 || true\n"
        "    fi\n"
        "}\n"
        "\n"
        "server_ready() {\n"
        "    PORT=\"$PORT\" /bin/bash -c '\n"
        "        exec 3<>\"/dev/tcp/127.0.0.1/$PORT\" 2>/dev/null || exit 1\n"
        "        exec 3>&-\n"
        "        exec 3<&-\n"
        "    '\n"
        "}\n"
        "\n"
        "cleanup() {\n"
        "    if [ -n \"$CMD_PID\" ]; then\n"
        "        kill -TERM \"$CMD_PID\" 2>/dev/null || true\n"
        "        wait \"$CMD_PID\" 2>/dev/null || true\n"
        "    fi\n"
        "    run_outerctl app clear --backend \"$BACKEND_ID\"\n"
        "}\n"
        "trap cleanup EXIT INT TERM\n"
        "\n"
        "run_outerctl app clear --backend \"$BACKEND_ID\"\n"
        "\n") &&
        sb_append(builder, command) &&
        sb_append(builder,
        " &\n"
        "CMD_PID=$!\n"
        "\n"
        "while kill -0 \"$CMD_PID\" 2>/dev/null; do\n"
        "    if server_ready; then\n"
        "        run_outerctl app add --backend \"$BACKEND_ID\" --port \"$PORT\" --name \"$DISPLAY_NAME\" --url \"127.0.0.1:$PORT/\"\n"
        "        break\n"
        "    fi\n"
        "    sleep 0.25\n"
        "done\n"
        "\n"
        "if wait \"$CMD_PID\"; then\n"
        "    status=0\n"
        "else\n"
        "    status=$?\n"
        "fi\n"
        "CMD_PID=''\n"
        "exit \"$status\"\n");
}

static bool make_jupyter_script(const char *service_id,
                                const char *display_name,
                                const char *python,
                                const char *port,
                                bool use_unix_socket,
                                bool project_venv,
                                StringBuilder *builder) {
    const char *module_command[8];
    size_t command_count = 0;
    const char *probe_command[8];
    size_t probe_count = 0;
    char port_buffer[32];
    snprintf(port_buffer, sizeof(port_buffer), "%s", port ? port : "");

    if (project_venv) {
        module_command[command_count++] = ".venv/bin/jupyter-lab";
        module_command[command_count++] = "--no-browser";
        if (port_buffer[0]) {
            module_command[command_count++] = "--port";
            module_command[command_count++] = port_buffer;
        }
        probe_command[probe_count++] = ".venv/bin/jupyter-server";
        probe_command[probe_count++] = "list";
        probe_command[probe_count++] = "--json";
    } else {
        module_command[command_count++] = python;
        module_command[command_count++] = "-m";
        module_command[command_count++] = "jupyter";
        module_command[command_count++] = "lab";
        module_command[command_count++] = "--no-browser";
        if (port_buffer[0]) {
            module_command[command_count++] = "--port";
            module_command[command_count++] = port_buffer;
        }
        probe_command[probe_count++] = python;
        probe_command[probe_count++] = "-m";
        probe_command[probe_count++] = "jupyter";
        probe_command[probe_count++] = "server";
        probe_command[probe_count++] = "list";
        probe_command[probe_count++] = "--json";
    }

    bool ok = sb_append(builder,
        "#!/usr/bin/python3\n"
        "import json\n"
        "import os\n"
        "import signal\n"
        "import subprocess\n"
        "import time\n"
        "from urllib.parse import urlencode, urlsplit\n"
        "\n"
        "OUTERCTL_ENV_VAR = \"OUTERCTL_PATH\"\n"
        "BACKEND_ID = ");
    ok = ok && sb_append_python_string(builder, service_id);
    ok = ok && sb_append(builder, "\nDISPLAY_NAME = ");
    ok = ok && sb_append_python_string(builder, display_name);
    ok = ok && sb_append(builder, "\nBASE_COMMAND = ");
    ok = ok && sb_append_python_list(builder, module_command, command_count);
    ok = ok && sb_append(builder, "\nPROBE_COMMAND = ");
    ok = ok && sb_append_python_list(builder, probe_command, probe_count);
    ok = ok && sb_append(builder, "\nUSE_UNIX_SOCKET = ");
    ok = ok && sb_append(builder, use_unix_socket ? "True" : "False");
    ok = ok && sb_append(builder,
        "\nSOCKET_DIRECTORY = os.path.join(os.path.expanduser(\"~\"), \".outerloop\", \"run\")\n"
        "SOCKET_PATH = os.path.join(SOCKET_DIRECTORY, f\"{BACKEND_ID}.sock\")\n"
        "\n"
        "child = None\n"
        "\n"
        "def log(message):\n"
        "    print(f\"[HomeScreen Jupyter] {message}\", flush=True)\n"
        "\n"
        "def run_outerctl(*args: str) -> None:\n"
        "    outerctl_path = os.environ.get(OUTERCTL_ENV_VAR, \"\").strip()\n"
        "    if not outerctl_path or not os.access(outerctl_path, os.X_OK):\n"
        "        log(\"outerctl is unavailable; frontend metadata will not be announced\")\n"
        "        return\n"
        "    result = subprocess.run([outerctl_path, *args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)\n"
        "    if result.returncode != 0:\n"
        "        detail = result.stderr.strip()\n"
        "        log(f\"outerctl failed ({result.returncode})\" + (f\": {detail}\" if detail else \"\"))\n"
        "\n"
        "def build_command() -> list[str]:\n"
        "    command = list(BASE_COMMAND)\n"
        "    if USE_UNIX_SOCKET:\n"
        "        command.extend([\n"
        "            f\"--ServerApp.sock={SOCKET_PATH}\",\n"
        "            \"--ServerApp.sock_mode=0600\",\n"
        "            \"--IdentityProvider.token=\",\n"
        "            \"--ServerApp.password=\",\n"
        "        ])\n"
        "    return command\n"
        "\n"
        "def resource_python_path():\n"
        "    if BASE_COMMAND and BASE_COMMAND[0].endswith(\"jupyter-lab\"):\n"
        "        candidate = os.path.join(os.path.dirname(BASE_COMMAND[0]), \"python\")\n"
        "        if os.path.exists(candidate):\n"
        "            return candidate\n"
        "    if BASE_COMMAND:\n"
        "        return BASE_COMMAND[0]\n"
        "    return None\n"
        "\n"
        "def jupyter_icon_path():\n"
        "    python_path = resource_python_path()\n"
        "    if not python_path:\n"
        "        return None\n"
        "    code = \"\"\"\n"
        "from importlib import resources\n"
        "candidates = [\n"
        "    ('jupyter_server', ('static', 'favicon.ico')),\n"
        "    ('jupyter_server', ('static', 'favicons', 'favicon.ico')),\n"
        "    ('jupyter_server', ('static', 'logo', 'logo.png')),\n"
        "    ('ipykernel', ('resources', 'logo-64x64.png')),\n"
        "]\n"
        "for package, parts in candidates:\n"
        "    try:\n"
        "        item = resources.files(package).joinpath(*parts)\n"
        "        if item.is_file():\n"
        "            print(str(item))\n"
        "            raise SystemExit(0)\n"
        "    except Exception:\n"
        "        pass\n"
        "raise SystemExit(1)\n"
        "\"\"\"\n"
        "    try:\n"
        "        result = subprocess.run([python_path, \"-c\", code], capture_output=True, text=True, timeout=2.0)\n"
        "    except Exception as error:\n"
        "        log(f\"failed to resolve package favicon using {python_path}: {error}\")\n"
        "        return None\n"
        "    if result.returncode != 0:\n"
        "        detail = result.stderr.strip()\n"
        "        log(\"could not find Jupyter package favicon\" + (f\": {detail}\" if detail else \"\"))\n"
        "        return None\n"
        "    path = result.stdout.strip().splitlines()[0] if result.stdout.strip() else \"\"\n"
        "    if path and os.path.exists(path) and os.path.getsize(path) > 0:\n"
        "        log(f\"using Jupyter package favicon at {path}\")\n"
        "        return path\n"
        "    return None\n"
        "\n"
        "def probe_frontend():\n"
        "    probe_result = subprocess.run(PROBE_COMMAND, capture_output=True, text=True)\n"
        "    for line in probe_result.stdout.splitlines():\n"
        "        line = line.strip()\n"
        "        if not line:\n"
        "            continue\n"
        "        try:\n"
        "            entry = json.loads(line)\n"
        "        except json.JSONDecodeError:\n"
        "            continue\n"
        "        server_url = entry.get(\"url\")\n"
        "        token = entry.get(\"token\")\n"
        "        if not isinstance(server_url, str) or not server_url:\n"
        "            continue\n"
        "        frontend_url = (server_url if server_url.endswith(\"/\") else server_url + \"/\") + \"lab\"\n"
        "        if not USE_UNIX_SOCKET and isinstance(token, str) and token:\n"
        "            separator = \"&\" if \"?\" in frontend_url else \"?\"\n"
        "            frontend_url = frontend_url + separator + urlencode({\"token\": token})\n"
        "        parsed_frontend = urlsplit(frontend_url)\n"
        "        app_url = parsed_frontend.path or \"/\"\n"
        "        if parsed_frontend.query:\n"
        "            app_url += \"?\" + parsed_frontend.query\n"
        "        icon_path = jupyter_icon_path()\n"
        "        if USE_UNIX_SOCKET:\n"
        "            return app_url, icon_path\n"
        "        port = entry.get(\"port\")\n"
        "        try:\n"
        "            port = int(port)\n"
        "        except (TypeError, ValueError):\n"
        "            continue\n"
        "        return str(port), app_url, icon_path\n"
        "    return None\n"
        "\n"
        "def handle_signal(signum, _frame):\n"
        "    raise SystemExit(128 + signum)\n"
        "\n"
        "signal.signal(signal.SIGINT, handle_signal)\n"
        "signal.signal(signal.SIGTERM, handle_signal)\n"
        "probe_attempts = 0\n"
        "announced = False\n"
        "\n"
        "try:\n"
        "    run_outerctl(\"app\", \"clear\", \"--backend\", BACKEND_ID)\n"
        "    if USE_UNIX_SOCKET:\n"
        "        os.makedirs(SOCKET_DIRECTORY, exist_ok=True)\n"
        "        try:\n"
        "            os.unlink(SOCKET_PATH)\n"
        "        except FileNotFoundError:\n"
        "            pass\n"
        "    child = subprocess.Popen(build_command())\n"
        "    while True:\n"
        "        status = child.poll()\n"
        "        if status is not None:\n"
        "            raise SystemExit(status)\n"
        "        if not announced:\n"
        "            discovered = probe_frontend()\n"
        "            if discovered is not None:\n"
        "                if USE_UNIX_SOCKET:\n"
        "                    app_url, icon_path = discovered\n"
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--socket-path\", SOCKET_PATH, \"--name\", DISPLAY_NAME, \"--url\", app_url]\n"
        "                else:\n"
        "                    port, app_url, icon_path = discovered\n"
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--port\", port, \"--name\", DISPLAY_NAME, \"--url\", app_url]\n"
        "                add_args.extend([\"--list\", \"Jupyter\"])\n"
        "                if icon_path:\n"
        "                    add_args.extend([\"--icon-file\", icon_path])\n"
        "                run_outerctl(*add_args)\n"
        "                announced = True\n"
        "        time.sleep(0.25 if not announced and probe_attempts < 20 else 1.0)\n"
        "        probe_attempts += 1\n"
        "finally:\n"
        "    if child is not None and child.poll() is None:\n"
        "        child.terminate()\n"
        "        try:\n"
        "            child.wait(timeout=5)\n"
        "        except subprocess.TimeoutExpired:\n"
        "            child.kill()\n"
        "            child.wait()\n"
        "    if USE_UNIX_SOCKET:\n"
        "        try:\n"
        "            os.unlink(SOCKET_PATH)\n"
        "        except FileNotFoundError:\n"
        "            pass\n"
        "    run_outerctl(\"app\", \"clear\", \"--backend\", BACKEND_ID)\n");
    return ok;
}

static bool register_created_backend(const char *service_id,
                                     const char *display_name,
                                     const char *unit_name,
                                     const char *log_path,
                                     char *error,
                                     size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        sqlite3_close(database);
        return false;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    sqlite3_stmt *statement = NULL;
    if (ok) {
        const char *sql = "SELECT 1 FROM backends WHERE service_id = ? LIMIT 1;";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) == SQLITE_ROW) {
                snprintf(error, error_size, "A backend with this identifier already exists.");
                ok = false;
            }
        } else {
            snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        }
        if (statement) sqlite3_finalize(statement);
    }
    if (ok) {
        const char *sql =
            "INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, NULL, ?);";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, display_name, -1, SQLITE_TRANSIENT);
#ifdef __APPLE__
            sqlite3_bind_null(statement, 3);
#else
            sqlite3_bind_text(statement, 3, unit_name, -1, SQLITE_TRANSIENT);
#endif
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
        statement = NULL;
    }
    if (ok) {
#ifdef __APPLE__
        const char *sql =
            "INSERT INTO launchd_backends(service_id, plist_path, owns_plist) VALUES(?, ?, 1) "
            "ON CONFLICT(service_id) DO UPDATE SET plist_path=excluded.plist_path, owns_plist=excluded.owns_plist;";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, unit_name, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
#else
        const char *sql =
            "INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, 'user');";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, unit_name, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
#endif
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
        statement = NULL;
    }
    if (ok) {
        const char *sql =
            "INSERT INTO log_files(path, service_id) VALUES(?, ?);";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, log_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(database);
    return ok;
}

static bool bind_and_step(sqlite3 *database, const char *sql, const char *a, const char *b, const char *c, const char *d,
                          char *error, size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok && a) sqlite3_bind_text(statement, 1, a, -1, SQLITE_TRANSIENT);
    if (ok && b) sqlite3_bind_text(statement, 2, b, -1, SQLITE_TRANSIENT);
    if (ok && c) sqlite3_bind_text(statement, 3, c, -1, SQLITE_TRANSIENT);
    if (ok && d) sqlite3_bind_text(statement, 4, d, -1, SQLITE_TRANSIENT);
    if (ok) ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    if (statement) sqlite3_finalize(statement);
    return ok;
}

#ifndef __APPLE__
static bool upsert_systemd_backend_registry(const char *service_id,
                                            const char *display_name,
                                            const char *unit_name,
                                            const char *scope,
                                            const char *socket_path,
                                            const char *log_path,
                                            const char *icon_path,
                                            char *error,
                                            size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        sqlite3_close(database);
        return false;
    }

    char *icon_value = registry_icon_value(icon_path);
    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, ?, ?) "
                           "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon=excluded.icon, service_unit=excluded.service_unit;",
                           service_id, display_name, icon_value, unit_name, error, error_size);
        if (ok) {
            sqlite3_stmt *statement = NULL;
            const char *sql =
                "UPDATE backends SET service_unit = ? WHERE service_id = ?;";
            ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_text(statement, 1, unit_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(statement) == SQLITE_DONE;
            }
            if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
            if (statement) sqlite3_finalize(statement);
        }
    }
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, ?) "
                           "ON CONFLICT(service_id) DO UPDATE SET unit_name=excluded.unit_name, scope=excluded.scope;",
                           service_id, unit_name, scope && scope[0] ? scope : "user", NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database, "DELETE FROM frontends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok && socket_path && socket_path[0]) {
        sqlite3_stmt *statement = NULL;
        const char *sql =
            "INSERT INTO frontends(url, service_id, name, port, socket_path, icon) VALUES(?, ?, ?, 0, ?, ?);";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, socket_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, display_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, socket_path, -1, SQLITE_TRANSIENT);
            if (icon_value && icon_value[0]) {
                sqlite3_bind_text(statement, 5, icon_value, -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(statement, 5);
            }
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
    }
    if (ok) {
        ok = bind_and_step(database, "DELETE FROM log_files WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO log_files(path, service_id) VALUES(?, ?);",
                           log_path, service_id, NULL, NULL, error, error_size);
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(database);
    free(icon_value);
    return ok;
}
#endif

#ifdef __APPLE__
static bool upsert_launchd_backend_registry_at(const char *database_path,
                                               const char *service_id,
                                               const char *display_name,
                                               const char *plist_path,
                                               const char *socket_path,
                                               const char *log_path,
                                               const char *icon_path,
                                               char *error,
                                               size_t error_size) {
    sqlite3 *database = open_registry_readwrite_at(database_path, error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        sqlite3_close(database);
        return false;
    }

    char *icon_value = registry_icon_value(icon_path);
    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, ?, NULL) "
                           "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon=excluded.icon, service_unit=NULL;",
                           service_id, display_name, icon_value, NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO launchd_backends(service_id, plist_path, owns_plist) VALUES(?, ?, 1) "
                           "ON CONFLICT(service_id) DO UPDATE SET plist_path=excluded.plist_path, owns_plist=excluded.owns_plist;",
                           service_id, plist_path, NULL, NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database, "DELETE FROM systemd_backends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database, "DELETE FROM frontends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok && socket_path && socket_path[0]) {
        sqlite3_stmt *statement = NULL;
        const char *sql =
            "INSERT INTO frontends(url, service_id, name, port, socket_path, icon) VALUES(?, ?, ?, 0, ?, ?);";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, socket_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, display_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, socket_path, -1, SQLITE_TRANSIENT);
            if (icon_value && icon_value[0]) {
                sqlite3_bind_text(statement, 5, icon_value, -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(statement, 5);
            }
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
    }
    if (ok) {
        ok = bind_and_step(database, "DELETE FROM log_files WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO log_files(path, service_id) VALUES(?, ?);",
                           log_path, service_id, NULL, NULL, error, error_size);
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(database);
    free(icon_value);
    return ok;
}
#endif

static bool unregister_backend_records(const char *service_id, char *error, size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        sqlite3_close(database);
        return false;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) ok = bind_and_step(database, "DELETE FROM frontends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    if (ok) ok = bind_and_step(database, "DELETE FROM log_files WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    if (ok) ok = bind_and_step(database, "DELETE FROM systemd_backends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    if (ok && sqlite_table_exists(database, "launchd_backends")) {
        ok = bind_and_step(database, "DELETE FROM launchd_backends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
    }
    if (ok) ok = bind_and_step(database, "DELETE FROM backends WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_close(database);
    return ok;
}

static bool install_bundled_app_macos(const BundledAppDefinition *app,
                                      const char *scope,
                                      const char *sudo_password,
                                      bool *needs_password,
                                      char *message,
                                      size_t message_size);

static bool install_bundled_app(const BundledAppDefinition *app, const char *scope, const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
#ifdef __APPLE__
    return install_bundled_app_macos(app, scope, sudo_password, needs_password, message, message_size);
#else
    if (needs_password) *needs_password = false;
    if (!app) {
        snprintf(message, message_size, "Unknown bundled app.");
        return false;
    }
    bool install_as_root = scope && strcmp(scope, "system") == 0;
    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) {
        snprintf(message, message_size, "Unsupported machine architecture.");
        return false;
    }

    char stage_root[PATH_MAX];
    if (!resolve_bundled_app_stage_root(app, stage_root, sizeof(stage_root), message, message_size)) {
        return false;
    }
    char source_binary[PATH_MAX];
    snprintf(source_binary, sizeof(source_binary), "%s/RemoteLinuxBinaries/%s/%s", stage_root, architecture, app->binary_name);
    char source_code[PATH_MAX];
    if (app->source_name && app->source_name[0]) {
        snprintf(source_code, sizeof(source_code), "%s/Source/%s", stage_root, app->source_name);
    } else {
        source_code[0] = '\0';
    }
    char source_bundle_arm[PATH_MAX];
    snprintf(source_bundle_arm, sizeof(source_bundle_arm), "%s/bundles/%s.bundle.macos-arm.aar", stage_root, app->bundle_prefix);
    char source_bundle_x86[PATH_MAX];
    snprintf(source_bundle_x86, sizeof(source_bundle_x86), "%s/bundles/%s.bundle.macos-x86.aar", stage_root, app->bundle_prefix);
    char source_icon[PATH_MAX];
    if (app->icon_name && app->icon_name[0]) {
        snprintf(source_icon, sizeof(source_icon), "%s/%s", stage_root, app->icon_name);
    } else {
        source_icon[0] = '\0';
    }

    char error[1024] = "";
    struct stat st;
    bool has_source_binary = stat(source_binary, &st) == 0 && S_ISREG(st.st_mode);
    bool has_source_code = !has_source_binary && source_code[0] && stat(source_code, &st) == 0 && S_ISREG(st.st_mode);
    if (!has_source_binary && !has_source_code) {
        snprintf(message, message_size, "Missing bundled %s binary for %s at %s.", app->display_name, architecture, source_binary);
        return false;
    }
    if (stat(source_bundle_arm, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(source_bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing bundled %s content archives under %s/bundles.", app->display_name, stage_root);
        return false;
    }
    if (source_icon[0] && (stat(source_icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        snprintf(message, message_size, "Missing bundled %s icon at %s.", app->display_name, source_icon);
        return false;
    }

    if (install_as_root) {
        char user_name[128] = "";
        struct passwd *pw = getpwuid(getuid());
        snprintf(user_name, sizeof(user_name), "%s", pw && pw->pw_name ? pw->pw_name : "");

        char compiled_binary[PATH_MAX] = "";
        if (has_source_code) {
            char binary_template[] = "/tmp/backends-bundled-binary-XXXXXX";
            int binary_fd = mkstemp(binary_template);
            if (binary_fd < 0) {
                snprintf(message, message_size, "Failed to create temporary binary path: %s", strerror(errno));
                return false;
            }
            close(binary_fd);
            snprintf(compiled_binary, sizeof(compiled_binary), "%s", binary_template);

            char quoted_source[PATH_MAX + 8];
            char quoted_binary_tmp[PATH_MAX + 8];
            shell_quote(source_code, quoted_source, sizeof(quoted_source));
            shell_quote(compiled_binary, quoted_binary_tmp, sizeof(quoted_binary_tmp));
            char compile_command[PATH_MAX * 2 + 256];
            snprintf(compile_command, sizeof(compile_command),
                     "cc -std=gnu17 -Wall -Wextra -O2 -o %s %s -lm 2>&1",
                     quoted_binary_tmp, quoted_source);
            FILE *pipe = popen(compile_command, "r");
            if (!pipe) {
                unlink(compiled_binary);
                snprintf(message, message_size, "Failed to compile %s: %s", app->display_name, strerror(errno));
                return false;
            }
            size_t offset = 0;
            while (offset + 1 < message_size) {
                size_t got = fread(message + offset, 1, message_size - offset - 1, pipe);
                offset += got;
                if (got == 0) break;
            }
            message[offset] = '\0';
            int status = pclose(pipe);
            if (status != 0) {
                if (!message[0]) snprintf(message, message_size, "Failed to compile %s.", app->display_name);
                unlink(compiled_binary);
                return false;
            }
            chmod(compiled_binary, 0700);
        }

        char install_root[PATH_MAX];
        snprintf(install_root, sizeof(install_root), "/opt/outergroup/%s", app->install_directory_name);
        char bundles_dir[PATH_MAX];
        snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
        char target_binary[PATH_MAX];
        snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
        char target_bundle_arm[PATH_MAX];
        snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
        char target_bundle_x86[PATH_MAX];
        snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
        char target_icon[PATH_MAX];
        if (app->icon_name && app->icon_name[0]) {
            snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
        } else {
            target_icon[0] = '\0';
        }
        char version_path[PATH_MAX];
        snprintf(version_path, sizeof(version_path), "%s/version", install_root);
        char wrapper_path[PATH_MAX];
        snprintf(wrapper_path, sizeof(wrapper_path), "%s/outerctl-as-user", install_root);
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "/var/log/outergroup/%s.log", app->service_id);
        char unit_path[PATH_MAX];
        snprintf(unit_path, sizeof(unit_path), "/etc/systemd/system/%s", app->unit_name);
        char socket_unit_name[256] = "";
        char socket_unit_path[PATH_MAX] = "";
        if (app->socket_activated) {
            systemd_socket_unit_name(app->unit_name, socket_unit_name, sizeof(socket_unit_name));
            snprintf(socket_unit_path, sizeof(socket_unit_path), "/etc/systemd/system/%s", socket_unit_name);
        }

        char user_unit_path[PATH_MAX];
        snprintf(user_unit_path, sizeof(user_unit_path), "%s/.config/systemd/user/%s", home_directory(), app->unit_name);
        char quoted_unit[320];
        shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));
        char stop_user_command[512];
        snprintf(stop_user_command, sizeof(stop_user_command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_unit);
        run_shell_ignored(stop_user_command);
        unlink(user_unit_path);
        if (app->socket_activated) {
            char user_socket_unit[256];
            char quoted_user_socket_unit[320];
            char user_socket_path[PATH_MAX];
            systemd_socket_unit_name(app->unit_name, user_socket_unit, sizeof(user_socket_unit));
            shell_quote(user_socket_unit, quoted_user_socket_unit, sizeof(quoted_user_socket_unit));
            snprintf(stop_user_command, sizeof(stop_user_command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_user_socket_unit);
            run_shell_ignored(stop_user_command);
            snprintf(user_socket_path, sizeof(user_socket_path), "%s/.config/systemd/user/%s", home_directory(), user_socket_unit);
            unlink(user_socket_path);
        }
        run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1 || true");

        const char *binary_source = has_source_code ? compiled_binary : source_binary;
        char quoted_binary_source[PATH_MAX + 8];
        char quoted_source_bundle_arm[PATH_MAX + 8];
        char quoted_source_bundle_x86[PATH_MAX + 8];
        char quoted_source_icon[PATH_MAX + 8];
        char quoted_install_root[PATH_MAX + 8];
        char quoted_bundles_dir[PATH_MAX + 8];
        char quoted_target_binary[PATH_MAX + 8];
        char quoted_target_bundle_arm[PATH_MAX + 8];
        char quoted_target_bundle_x86[PATH_MAX + 8];
        char quoted_target_icon[PATH_MAX + 8];
        char quoted_version_path[PATH_MAX + 8];
        char quoted_wrapper_path[PATH_MAX + 8];
        char quoted_log_path[PATH_MAX + 8];
        char quoted_unit_path[PATH_MAX + 8];
        char quoted_socket_unit[320] = "";
        char quoted_socket_unit_path[PATH_MAX + 8] = "";
        char quoted_actual_socket_path[PATH_MAX + 8] = "";
        shell_quote(binary_source, quoted_binary_source, sizeof(quoted_binary_source));
        shell_quote(source_bundle_arm, quoted_source_bundle_arm, sizeof(quoted_source_bundle_arm));
        shell_quote(source_bundle_x86, quoted_source_bundle_x86, sizeof(quoted_source_bundle_x86));
        if (source_icon[0]) shell_quote(source_icon, quoted_source_icon, sizeof(quoted_source_icon)); else quoted_source_icon[0] = '\0';
        shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
        shell_quote(bundles_dir, quoted_bundles_dir, sizeof(quoted_bundles_dir));
        shell_quote(target_binary, quoted_target_binary, sizeof(quoted_target_binary));
        shell_quote(target_bundle_arm, quoted_target_bundle_arm, sizeof(quoted_target_bundle_arm));
        shell_quote(target_bundle_x86, quoted_target_bundle_x86, sizeof(quoted_target_bundle_x86));
        if (target_icon[0]) shell_quote(target_icon, quoted_target_icon, sizeof(quoted_target_icon)); else quoted_target_icon[0] = '\0';
        shell_quote(version_path, quoted_version_path, sizeof(quoted_version_path));
        shell_quote(wrapper_path, quoted_wrapper_path, sizeof(quoted_wrapper_path));
        shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
        shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
        if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
        if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';

        char quoted_binary[PATH_MAX + 8];
        char quoted_bundles[PATH_MAX + 8];
        char quoted_icon[PATH_MAX + 8];
        char quoted_log[PATH_MAX + 8];
        char quoted_service_id[512];
        char quoted_display_name[512];
        char quoted_target_icon_for_registry[PATH_MAX + 8];
        char quoted_socket_path[PATH_MAX + 32];
        char actual_socket_path[PATH_MAX] = "";
        char systemd_socket_path[PATH_MAX] = "";
        shell_quote(target_binary, quoted_binary, sizeof(quoted_binary));
        shell_quote(bundles_dir, quoted_bundles, sizeof(quoted_bundles));
        if (target_icon[0]) shell_quote(target_icon, quoted_icon, sizeof(quoted_icon)); else quoted_icon[0] = '\0';
        shell_quote(log_path, quoted_log, sizeof(quoted_log));
        shell_quote(app->service_id, quoted_service_id, sizeof(quoted_service_id));
        shell_quote(app->display_name, quoted_display_name, sizeof(quoted_display_name));
        shell_quote(target_icon, quoted_target_icon_for_registry, sizeof(quoted_target_icon_for_registry));
        if (app->socket_name && app->socket_name[0]) {
            snprintf(systemd_socket_path, sizeof(systemd_socket_path), "%%t/%s", app->socket_name);
            shell_quote(systemd_socket_path, quoted_socket_path, sizeof(quoted_socket_path));
            bundled_socket_path_for_scope(app, "system", actual_socket_path, sizeof(actual_socket_path));
            shell_quote(actual_socket_path, quoted_actual_socket_path, sizeof(quoted_actual_socket_path));
        } else {
            quoted_socket_path[0] = '\0';
            quoted_actual_socket_path[0] = '\0';
        }

        char run_script[PATH_MAX * 4];
        if (quoted_icon[0]) {
            if (quoted_socket_path[0]) {
                snprintf(run_script, sizeof(run_script),
                         "exec %s --label %s --socket-path %s --bundles-dir %s --icon-file %s >> %s 2>&1",
                         quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles, quoted_icon, quoted_log);
            } else {
                snprintf(run_script, sizeof(run_script),
                         "exec %s --label %s --bundles-dir %s --icon-file %s >> %s 2>&1",
                         quoted_binary, quoted_service_id, quoted_bundles, quoted_icon, quoted_log);
            }
        } else {
            if (quoted_socket_path[0]) {
                snprintf(run_script, sizeof(run_script),
                         "exec %s --label %s --socket-path %s --bundles-dir %s >> %s 2>&1",
                         quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles, quoted_log);
            } else {
                snprintf(run_script, sizeof(run_script),
                         "exec %s --label %s --bundles-dir %s >> %s 2>&1",
                         quoted_binary, quoted_service_id, quoted_bundles, quoted_log);
            }
        }
        char quoted_run_script[sizeof(run_script) + 16];
        shell_quote(run_script, quoted_run_script, sizeof(quoted_run_script));

        char description[256];
        unit_description_text(app->display_name, description, sizeof(description));
        char unit_contents[12000];
        snprintf(unit_contents, sizeof(unit_contents),
                 "[Unit]\n"
                 "Description=%s\n"
                 "After=network.target\n"
                 "\n"
                 "[Service]\n"
                 "Type=simple\n"
                 "WorkingDirectory=%s\n"
                 "Environment=HOME=%s\n"
                 "Environment=USER=%s\n"
                 "Environment=LOGNAME=%s\n"
                 "Environment=OUTERCTL_PATH=%s\n"
                 "ExecStart=/bin/sh -lc %s\n"
                 "Restart=on-failure\n"
                 "KillMode=control-group\n"
                 "\n"
                 "[Install]\n"
                 "WantedBy=multi-user.target\n",
                 description,
                 install_root,
                 home_directory(),
                 user_name,
                 user_name,
                 wrapper_path,
                 quoted_run_script);

        char socket_contents[2048] = "";
        if (app->socket_activated && quoted_socket_path[0]) {
            snprintf(socket_contents, sizeof(socket_contents),
                     "[Unit]\n"
                     "Description=%s Socket\n"
                     "\n"
                     "[Socket]\n"
                     "ListenStream=%s\n"
                     "SocketMode=%s\n"
                     "\n"
                     "[Install]\n"
                     "WantedBy=sockets.target\n",
                     description,
                     systemd_socket_path,
                     "0666");
        }

        char quoted_outerctl[PATH_MAX + 8];
        char quoted_system_outeragent_root[PATH_MAX + 8];
        char outerctl_path[PATH_MAX];
        snprintf(outerctl_path, sizeof(outerctl_path), "%s/.outeragent/outerctl", home_directory());
        shell_quote(outerctl_path, quoted_outerctl, sizeof(quoted_outerctl));
        shell_quote(kSystemOuterAgentRoot, quoted_system_outeragent_root, sizeof(quoted_system_outeragent_root));
        char wrapper_contents[4096];
        snprintf(wrapper_contents, sizeof(wrapper_contents),
                 "#!/bin/sh\n"
                 "exec env OUTERAGENT_ROOT=%s %s \"$@\"\n",
                 quoted_system_outeragent_root,
                 quoted_outerctl);

        char script_template[] = "/tmp/backends-root-install-XXXXXX";
        int script_fd = mkstemp(script_template);
        if (script_fd < 0) {
            if (compiled_binary[0]) unlink(compiled_binary);
            snprintf(message, message_size, "Failed to create privileged install script: %s", strerror(errno));
            return false;
        }
        FILE *script = fdopen(script_fd, "w");
        if (!script) {
            close(script_fd);
            unlink(script_template);
            if (compiled_binary[0]) unlink(compiled_binary);
            snprintf(message, message_size, "Failed to write privileged install script: %s", strerror(errno));
            return false;
        }
        fprintf(script,
                "set -eu\n"
                "systemctl --system stop %s >/dev/null 2>&1 || true\n"
                "systemctl --system reset-failed %s >/dev/null 2>&1 || true\n"
                "mkdir -p %s %s /var/log/outergroup %s\n"
                "chmod 0755 /var/lib/outergroup %s\n"
                "install -m 0755 %s %s\n"
                "install -m 0644 %s %s\n"
                "install -m 0644 %s %s\n",
                quoted_unit,
                quoted_unit,
                quoted_install_root,
                quoted_bundles_dir,
                quoted_system_outeragent_root,
                quoted_system_outeragent_root,
                quoted_binary_source,
                quoted_target_binary,
                quoted_source_bundle_arm,
                quoted_target_bundle_arm,
                quoted_source_bundle_x86,
                quoted_target_bundle_x86);
        if (quoted_socket_unit[0]) {
            fprintf(script,
                    "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
                    "systemctl --system reset-failed %s >/dev/null 2>&1 || true\n",
                    quoted_socket_unit,
                    quoted_socket_unit);
        }
        if (quoted_source_icon[0] && quoted_target_icon[0]) {
            fprintf(script, "install -m 0644 %s %s\n", quoted_source_icon, quoted_target_icon);
        }
        fprintf(script,
                "cat > %s <<'__BACKENDS_VERSION__'\n%s\n__BACKENDS_VERSION__\n"
                "chmod 0644 %s\n"
                "cat > %s <<'__BACKENDS_OUTERCTL__'\n%s__BACKENDS_OUTERCTL__\n"
                "chmod 0755 %s\n"
                "cat > %s <<'__BACKENDS_UNIT__'\n%s__BACKENDS_UNIT__\n"
                "chmod 0644 %s\n"
                "touch %s\n"
                "chmod 0644 %s\n"
                "REGISTRY_ROOT=%s SERVICE_ID=%s DISPLAY_NAME=%s UNIT_NAME=%s LOG_PATH=%s ICON_PATH=%s SOCKET_PATH=%s python3 - <<'__BACKENDS_REGISTRY__'\n"
                "import base64, os, sqlite3\n"
                "root = os.environ['REGISTRY_ROOT']\n"
                "service_id = os.environ['SERVICE_ID']\n"
                "display_name = os.environ['DISPLAY_NAME']\n"
                "unit_name = os.environ['UNIT_NAME']\n"
                "log_path = os.environ['LOG_PATH']\n"
                "icon_path = os.environ.get('ICON_PATH') or None\n"
                "socket_path = os.environ.get('SOCKET_PATH') or ''\n"
                "def registry_icon_value(path):\n"
                "    if not path:\n"
                "        return None\n"
                "    if path.startswith('data:'):\n"
                "        return path\n"
                "    try:\n"
                "        with open(path, 'rb') as file:\n"
                "            data = file.read(1024 * 1024 + 1)\n"
                "    except OSError:\n"
                "        return path\n"
                "    if not data or len(data) > 1024 * 1024:\n"
                "        return path\n"
                "    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')\n"
                "icon_value = registry_icon_value(icon_path)\n"
                "os.makedirs(root, exist_ok=True)\n"
                "database_path = os.path.join(root, 'registry.sqlite3')\n"
                "database = sqlite3.connect(database_path)\n"
                "database.executescript('''\n"
                "CREATE TABLE IF NOT EXISTS backends (\n"
                "service_id TEXT PRIMARY KEY,\n"
                "display_name TEXT NOT NULL DEFAULT '',\n"
                "icon TEXT,\n"
                "service_unit TEXT\n"
                ");\n"
                "CREATE TABLE IF NOT EXISTS frontends (\n"
                "url TEXT PRIMARY KEY,\n"
                "service_id TEXT,\n"
                "name TEXT NOT NULL,\n"
                "port INTEGER NOT NULL DEFAULT 0,\n"
                "socket_path TEXT NOT NULL DEFAULT '',\n"
                "icon TEXT,\n"
                "is_home_screen INTEGER NOT NULL DEFAULT 0,\n"
                "list TEXT\n"
                ");\n"
                "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);\n"
                "CREATE TABLE IF NOT EXISTS log_files (\n"
                "path TEXT PRIMARY KEY,\n"
                "service_id TEXT NOT NULL\n"
                ");\n"
                "CREATE INDEX IF NOT EXISTS log_files_service_id_idx ON log_files(service_id);\n"
                "CREATE TABLE IF NOT EXISTS systemd_backends (\n"
                "service_id TEXT PRIMARY KEY,\n"
                "unit_name TEXT NOT NULL,\n"
                "scope TEXT NOT NULL DEFAULT 'user'\n"
                ");\n"
                "''')\n"
                "columns = {row[1] for row in database.execute('PRAGMA table_info(frontends)')}\n"
                "if 'is_home_screen' not in columns:\n"
                "    database.execute('ALTER TABLE frontends ADD COLUMN is_home_screen INTEGER NOT NULL DEFAULT 0')\n"
                "if 'list' not in columns:\n"
                "    database.execute('ALTER TABLE frontends ADD COLUMN list TEXT')\n"
                "with database:\n"
                "    database.execute('INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, ?, ?) ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon=excluded.icon, service_unit=excluded.service_unit', (service_id, display_name, icon_value, unit_name))\n"
                "    database.execute('INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, ?) ON CONFLICT(service_id) DO UPDATE SET unit_name=excluded.unit_name, scope=excluded.scope', (service_id, unit_name, 'system'))\n"
                "    database.execute('DELETE FROM frontends WHERE service_id = ?', (service_id,))\n"
                "    if socket_path:\n"
                "        database.execute('INSERT INTO frontends(url, service_id, name, port, socket_path, icon) VALUES(?, ?, ?, 0, ?, ?)', (socket_path, service_id, display_name, socket_path, icon_value))\n"
                "    database.execute('DELETE FROM log_files WHERE service_id = ?', (service_id,))\n"
                "    database.execute('INSERT INTO log_files(path, service_id) VALUES(?, ?)', (log_path, service_id))\n"
                "database.close()\n"
                "os.chmod(database_path, 0o644)\n"
                "__BACKENDS_REGISTRY__\n"
                "chmod 0644 %s/registry.sqlite3 >/dev/null 2>&1 || true\n"
                "systemctl --system daemon-reload\n",
                quoted_version_path,
                app->version,
                quoted_version_path,
                quoted_wrapper_path,
                wrapper_contents,
                quoted_wrapper_path,
                quoted_unit_path,
                unit_contents,
                quoted_unit_path,
                quoted_log_path,
                quoted_log_path,
                quoted_system_outeragent_root,
                quoted_service_id,
                quoted_display_name,
                quoted_unit,
                quoted_log_path,
                quoted_target_icon_for_registry,
                quoted_actual_socket_path,
                quoted_system_outeragent_root);
        if (quoted_socket_unit[0] && quoted_socket_unit_path[0]) {
            fprintf(script,
                    "cat > %s <<'__BACKENDS_SOCKET__'\n%s__BACKENDS_SOCKET__\n"
                    "chmod 0644 %s\n"
                    "systemctl --system enable --now %s >/dev/null 2>&1\n",
                    quoted_socket_unit_path,
                    socket_contents,
                    quoted_socket_unit_path,
                    quoted_socket_unit);
        } else {
            fprintf(script,
                    "systemctl --system enable %s >/dev/null 2>&1 || true\n"
                    "systemctl --system restart %s || systemctl --system start %s\n",
                    quoted_unit,
                    quoted_unit,
                    quoted_unit);
        }
        fclose(script);
        chmod(script_template, 0700);

        bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
        unlink(script_template);
        if (compiled_binary[0]) unlink(compiled_binary);
        if (!root_ok) return false;

        if (!unregister_backend_records(app->service_id, error, sizeof(error))) {
            snprintf(message, message_size, "%s", error);
            return false;
        }

        char user_install_root[PATH_MAX];
        snprintf(user_install_root, sizeof(user_install_root), "%s/.outeragent/%s", home_directory(), app->install_directory_name);
        char quoted_user_install_root[PATH_MAX + 8];
        shell_quote(user_install_root, quoted_user_install_root, sizeof(quoted_user_install_root));
        char remove_user_install_command[PATH_MAX + 64];
        snprintf(remove_user_install_command, sizeof(remove_user_install_command), "rm -rf -- %s", quoted_user_install_root);
        run_shell_ignored(remove_user_install_command);

        snprintf(message, message_size, "Installed %s as root.", app->display_name);
        return true;
    }

    char install_root[PATH_MAX];
    snprintf(install_root, sizeof(install_root), "%s/.outeragent/%s", home_directory(), app->install_directory_name);
    char bundles_dir[PATH_MAX];
    snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
    char target_binary[PATH_MAX];
    snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
    char target_bundle_arm[PATH_MAX];
    snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
    char target_bundle_x86[PATH_MAX];
    snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
    char target_icon[PATH_MAX];
    if (app->icon_name && app->icon_name[0]) {
        snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
    } else {
        target_icon[0] = '\0';
    }
    char version_path[PATH_MAX];
    snprintf(version_path, sizeof(version_path), "%s/version", install_root);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/outeragent.log", install_root);
    char unit_path[PATH_MAX];
    snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), app->unit_name);
    char socket_unit_name[256] = "";
    char socket_unit_path[PATH_MAX] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit_name, sizeof(socket_unit_name));
        snprintf(socket_unit_path, sizeof(socket_unit_path), "%s/.config/systemd/user/%s", home_directory(), socket_unit_name);
    }

    char stop_command[512];
    char quoted_unit[320];
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));
    snprintf(stop_command, sizeof(stop_command), "systemctl --user stop %s >/dev/null 2>&1 || true", quoted_unit);
    run_shell_ignored(stop_command);
    snprintf(stop_command, sizeof(stop_command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_unit);
    run_shell_ignored(stop_command);
    if (socket_unit_name[0]) {
        char quoted_socket_unit[320];
        shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit));
        snprintf(stop_command, sizeof(stop_command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_socket_unit);
        run_shell_ignored(stop_command);
        snprintf(stop_command, sizeof(stop_command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_socket_unit);
        run_shell_ignored(stop_command);
    }

    if (!mkdir_p(bundles_dir)) {
        snprintf(message, message_size, "Failed to create %s: %s", bundles_dir, strerror(errno));
        return false;
    }
    if (has_source_binary && !copy_file(source_binary, target_binary, 0700, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }
    if (!has_source_binary) {
        char quoted_source[PATH_MAX + 8];
        char quoted_target[PATH_MAX + 8];
        shell_quote(source_code, quoted_source, sizeof(quoted_source));
        shell_quote(target_binary, quoted_target, sizeof(quoted_target));
        char compile_command[PATH_MAX * 2 + 256];
        snprintf(compile_command, sizeof(compile_command),
                 "cc -std=gnu17 -Wall -Wextra -O2 -o %s %s -lm 2>&1",
                 quoted_target, quoted_source);
        FILE *pipe = popen(compile_command, "r");
        if (!pipe) {
            snprintf(message, message_size, "Failed to compile %s: %s", app->display_name, strerror(errno));
            return false;
        }
        size_t offset = 0;
        while (offset + 1 < message_size) {
            size_t got = fread(message + offset, 1, message_size - offset - 1, pipe);
            offset += got;
            if (got == 0) break;
        }
        message[offset] = '\0';
        int status = pclose(pipe);
        if (status != 0) {
            if (!message[0]) snprintf(message, message_size, "Failed to compile %s.", app->display_name);
            return false;
        }
        chmod(target_binary, 0700);
    }
    if (!copy_file(source_bundle_arm, target_bundle_arm, 0600, error, sizeof(error)) ||
        !copy_file(source_bundle_x86, target_bundle_x86, 0600, error, sizeof(error)) ||
        (source_icon[0] && !copy_file(source_icon, target_icon, 0600, error, sizeof(error)))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }
    if (!write_text_file(version_path, app->version, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }

    char user_name[128] = "";
    struct passwd *pw = getpwuid(getuid());
    snprintf(user_name, sizeof(user_name), "%s", pw && pw->pw_name ? pw->pw_name : "");

    char quoted_binary[PATH_MAX + 8];
    char quoted_bundles[PATH_MAX + 8];
    char quoted_icon[PATH_MAX + 8];
    char quoted_log[PATH_MAX + 8];
    char quoted_service_id[512];
    char quoted_socket_path[PATH_MAX + 32];
    char actual_socket_path[PATH_MAX] = "";
    char systemd_socket_path[PATH_MAX] = "";
    shell_quote(target_binary, quoted_binary, sizeof(quoted_binary));
    shell_quote(bundles_dir, quoted_bundles, sizeof(quoted_bundles));
    if (target_icon[0]) {
        shell_quote(target_icon, quoted_icon, sizeof(quoted_icon));
    } else {
        quoted_icon[0] = '\0';
    }
    shell_quote(log_path, quoted_log, sizeof(quoted_log));
    shell_quote(app->service_id, quoted_service_id, sizeof(quoted_service_id));
    if (app->socket_name && app->socket_name[0]) {
        snprintf(systemd_socket_path, sizeof(systemd_socket_path), "%%t/%s", app->socket_name);
        shell_quote(systemd_socket_path, quoted_socket_path, sizeof(quoted_socket_path));
        bundled_socket_path_for_scope(app, "user", actual_socket_path, sizeof(actual_socket_path));
    } else {
        quoted_socket_path[0] = '\0';
    }

    char run_script[PATH_MAX * 4];
    if (quoted_icon[0]) {
        if (quoted_socket_path[0]) {
            snprintf(run_script, sizeof(run_script),
                     "exec %s --label %s --socket-path %s --bundles-dir %s --icon-file %s >> %s 2>&1",
                     quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles, quoted_icon, quoted_log);
        } else {
            snprintf(run_script, sizeof(run_script),
                     "exec %s --label %s --bundles-dir %s --icon-file %s >> %s 2>&1",
                     quoted_binary, quoted_service_id, quoted_bundles, quoted_icon, quoted_log);
        }
    } else {
        if (quoted_socket_path[0]) {
            snprintf(run_script, sizeof(run_script),
                     "exec %s --label %s --socket-path %s --bundles-dir %s >> %s 2>&1",
                     quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles, quoted_log);
        } else {
            snprintf(run_script, sizeof(run_script),
                     "exec %s --label %s --bundles-dir %s >> %s 2>&1",
                     quoted_binary, quoted_service_id, quoted_bundles, quoted_log);
        }
    }
    char quoted_run_script[sizeof(run_script) + 16];
    shell_quote(run_script, quoted_run_script, sizeof(quoted_run_script));

    char description[256];
    unit_description_text(app->display_name, description, sizeof(description));
    char unit_contents[12000];
    snprintf(unit_contents, sizeof(unit_contents),
             "[Unit]\n"
             "Description=%s\n"
             "After=network.target\n"
             "\n"
             "[Service]\n"
             "Type=simple\n"
             "WorkingDirectory=%s\n"
             "Environment=HOME=%s\n"
             "Environment=USER=%s\n"
             "Environment=LOGNAME=%s\n"
             "Environment=OUTERCTL_PATH=%s/.outeragent/outerctl\n"
             "ExecStart=/bin/sh -lc %s\n"
             "Restart=on-failure\n"
             "KillMode=control-group\n"
             "\n"
             "[Install]\n"
             "WantedBy=default.target\n",
             description,
             install_root,
             home_directory(),
             user_name,
             user_name,
             home_directory(),
             quoted_run_script);
    char socket_contents[2048] = "";
    if (app->socket_activated && quoted_socket_path[0]) {
        snprintf(socket_contents, sizeof(socket_contents),
                 "[Unit]\n"
                 "Description=%s Socket\n"
                 "\n"
                 "[Socket]\n"
                 "ListenStream=%s\n"
                 "SocketMode=0600\n"
                 "\n"
                 "[Install]\n"
                 "WantedBy=sockets.target\n",
                 description,
                 systemd_socket_path);
    }
    if (!write_text_file(unit_path, unit_contents, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }
    if (socket_unit_path[0] && !write_text_file(socket_unit_path, socket_contents, error, sizeof(error))) {
        unlink(unit_path);
        snprintf(message, message_size, "%s", error);
        return false;
    }
    if (!upsert_systemd_backend_registry(app->service_id, app->display_name, app->unit_name, "user", actual_socket_path, log_path, target_icon, error, sizeof(error))) {
        unlink(unit_path);
        if (socket_unit_path[0]) unlink(socket_unit_path);
        snprintf(message, message_size, "%s", error);
        return false;
    }

    char enable_command[512];
    run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1");
    if (socket_unit_name[0]) {
        char quoted_socket_unit[320];
        shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit));
        snprintf(enable_command, sizeof(enable_command), "systemctl --user enable --now %s >/dev/null 2>&1", quoted_socket_unit);
        int status = system(enable_command);
        if (status != 0) {
            snprintf(message, message_size, "Installed %s, but failed to enable its socket.", app->display_name);
            return false;
        }
    } else {
        snprintf(enable_command, sizeof(enable_command), "systemctl --user enable %s >/dev/null 2>&1 || true", quoted_unit);
        run_shell_ignored(enable_command);
        char systemd_message[4096] = "";
        bool started = run_systemd_operation(app->unit_name, "user", "restart", NULL, NULL, systemd_message, sizeof(systemd_message));
        if (!started) {
            started = run_systemd_operation(app->unit_name, "user", "start", NULL, NULL, systemd_message, sizeof(systemd_message));
        }
        if (!started) {
            snprintf(message, message_size, "Installed %s, but failed to start it: %s", app->display_name, systemd_message);
            return false;
        }
    }

    snprintf(message, message_size, "Installed %s.", app->display_name);
    return true;
#endif
}

#ifdef __APPLE__
static bool append_xml_string_element(StringBuilder *builder, const char *value) {
    return sb_append(builder, "        <string>") &&
           sb_append_xml_escaped(builder, value ? value : "") &&
           sb_append(builder, "</string>\n");
}

static bool make_bundled_launchd_plist(const char *label,
                                       const char *binary_path,
                                       const char *bundles_dir,
                                       const char *icon_path,
                                       const char *socket_path,
                                       int socket_mode,
                                       const char *working_directory,
                                       const char *outerctl_path,
                                       const char *log_path,
                                       StringBuilder *builder) {
    bool ok = sb_append(builder,
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                        "<plist version=\"1.0\">\n"
                        "<dict>\n"
                        "    <key>Label</key>\n"
                        "    <string>");
    ok = ok && sb_append_xml_escaped(builder, label);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>ProgramArguments</key>\n"
                         "    <array>\n");
    ok = ok && append_xml_string_element(builder, binary_path);
    ok = ok && append_xml_string_element(builder, "--label");
    ok = ok && append_xml_string_element(builder, label);
    if (socket_path && socket_path[0]) {
        ok = ok && append_xml_string_element(builder, "--socket-path");
        ok = ok && append_xml_string_element(builder, socket_path);
        ok = ok && append_xml_string_element(builder, "--launchd-socket-name");
        ok = ok && append_xml_string_element(builder, "Listener");
    }
    ok = ok && append_xml_string_element(builder, "--bundles-dir");
    ok = ok && append_xml_string_element(builder, bundles_dir);
    if (icon_path && icon_path[0]) {
        ok = ok && append_xml_string_element(builder, "--icon-file");
        ok = ok && append_xml_string_element(builder, icon_path);
    }
    ok = ok && sb_append(builder,
                         "    </array>\n"
                         "    <key>EnvironmentVariables</key>\n"
                         "    <dict>\n"
                         "        <key>OUTERCTL_PATH</key>\n"
                         "        <string>");
    ok = ok && sb_append_xml_escaped(builder, outerctl_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    </dict>\n"
                         "    <key>WorkingDirectory</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, working_directory);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>StandardOutPath</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, log_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>StandardErrorPath</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, log_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>Sockets</key>\n"
                         "    <dict>\n"
                         "        <key>Listener</key>\n"
                         "        <dict>\n"
                         "            <key>SockPathName</key>\n"
                         "            <string>");
    ok = ok && sb_append_xml_escaped(builder, socket_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "            <key>SockPathMode</key>\n"
                         "            <integer>");
    char socket_mode_string[16];
    snprintf(socket_mode_string, sizeof(socket_mode_string), "%d", socket_mode > 0 ? socket_mode : 0600);
    ok = ok && sb_append(builder, socket_mode_string);
    ok = ok && sb_append(builder,
                         "</integer>\n"
                         "        </dict>\n"
                         "    </dict>\n"
                         "    <key>RunAtLoad</key>\n"
                         "    <true/>\n"
                         "</dict>\n"
                         "</plist>\n");
    return ok;
}

static bool install_bundled_app_macos(const BundledAppDefinition *app,
                                      const char *scope,
                                      const char *sudo_password,
                                      bool *needs_password,
                                      char *message,
                                      size_t message_size) {
    if (needs_password) *needs_password = false;
    if (!bundled_app_is_available_on_platform(app)) {
        snprintf(message, message_size, "This bundled app is not available on localhost.");
        return false;
    }

    bool install_as_root = scope && strcmp(scope, "system") == 0;
    char stage_root[PATH_MAX];
    if (!resolve_bundled_app_stage_root(app, stage_root, sizeof(stage_root), message, message_size)) {
        return false;
    }
    char source_binary[PATH_MAX];
    snprintf(source_binary, sizeof(source_binary), "%s/MacOS/%s", stage_root, app->binary_name);
    char source_bundle_arm[PATH_MAX];
    snprintf(source_bundle_arm, sizeof(source_bundle_arm), "%s/bundles/%s.bundle.macos-arm.aar", stage_root, app->bundle_prefix);
    char source_bundle_x86[PATH_MAX];
    snprintf(source_bundle_x86, sizeof(source_bundle_x86), "%s/bundles/%s.bundle.macos-x86.aar", stage_root, app->bundle_prefix);
    char source_icon[PATH_MAX] = "";
    if (app->icon_name && app->icon_name[0]) {
        snprintf(source_icon, sizeof(source_icon), "%s/%s", stage_root, app->icon_name);
    }

    struct stat st;
    if (stat(source_binary, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing bundled %s backend at %s.", app->display_name, source_binary);
        return false;
    }
    if (stat(source_bundle_arm, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(source_bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing bundled %s content archives under %s/bundles.", app->display_name, stage_root);
        return false;
    }
    if (source_icon[0] && (stat(source_icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        snprintf(message, message_size, "Missing bundled %s icon at %s.", app->display_name, source_icon);
        return false;
    }

    char socket_path[PATH_MAX] = "";
    bundled_socket_path_for_scope(app, install_as_root ? "system" : "user", socket_path, sizeof(socket_path));

    if (install_as_root) {
        char install_root[PATH_MAX];
        snprintf(install_root, sizeof(install_root), "/Library/Application Support/OuterLoopServerTools/%s", app->install_directory_name);
        char bundles_dir[PATH_MAX];
        snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
        char target_binary[PATH_MAX];
        snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
        char target_bundle_arm[PATH_MAX];
        snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
        char target_bundle_x86[PATH_MAX];
        snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
        char target_icon[PATH_MAX] = "";
        if (source_icon[0]) snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
        char wrapper_path[PATH_MAX];
        snprintf(wrapper_path, sizeof(wrapper_path), "%s/outerctl-system", install_root);
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "/Library/Logs/%s.log", app->service_id);
        char plist_path[PATH_MAX];
        snprintf(plist_path, sizeof(plist_path), "/Library/LaunchDaemons/%s.plist", app->service_id);

        char outerctl_path[PATH_MAX];
        snprintf(outerctl_path, sizeof(outerctl_path), "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());
        char quoted_system_root[PATH_MAX + 8];
        char quoted_outerctl[PATH_MAX + 8];
        shell_quote(kSystemOuterAgentRoot, quoted_system_root, sizeof(quoted_system_root));
        shell_quote(outerctl_path, quoted_outerctl, sizeof(quoted_outerctl));
        char wrapper_contents[4096];
        snprintf(wrapper_contents, sizeof(wrapper_contents),
                 "#!/bin/sh\n"
                 "exec env OUTERAGENT_ROOT=%s %s \"$@\"\n",
                 quoted_system_root,
                 quoted_outerctl);

        StringBuilder plist = {0};
        if (!make_bundled_launchd_plist(app->service_id,
                                        target_binary,
                                        bundles_dir,
                                        target_icon,
                                        socket_path,
                                        0666,
                                        install_root,
                                        wrapper_path,
                                        log_path,
                                        &plist)) {
            free(plist.data);
            snprintf(message, message_size, "Failed to generate LaunchDaemon plist.");
            return false;
        }

        char quoted_source_binary[PATH_MAX + 8];
        char quoted_source_bundle_arm[PATH_MAX + 8];
        char quoted_source_bundle_x86[PATH_MAX + 8];
        char quoted_source_icon[PATH_MAX + 8] = "";
        char quoted_install_root[PATH_MAX + 8];
        char quoted_bundles_dir[PATH_MAX + 8];
        char quoted_target_binary[PATH_MAX + 8];
        char quoted_target_bundle_arm[PATH_MAX + 8];
        char quoted_target_bundle_x86[PATH_MAX + 8];
        char quoted_target_icon[PATH_MAX + 8] = "";
        char quoted_wrapper_path[PATH_MAX + 8];
        char quoted_log_path[PATH_MAX + 8];
        char quoted_plist_path[PATH_MAX + 8];
        char quoted_service_id[512];
        char quoted_display_name[512];
        char quoted_socket_path[PATH_MAX + 8];
        shell_quote(source_binary, quoted_source_binary, sizeof(quoted_source_binary));
        shell_quote(source_bundle_arm, quoted_source_bundle_arm, sizeof(quoted_source_bundle_arm));
        shell_quote(source_bundle_x86, quoted_source_bundle_x86, sizeof(quoted_source_bundle_x86));
        if (source_icon[0]) shell_quote(source_icon, quoted_source_icon, sizeof(quoted_source_icon));
        shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
        shell_quote(bundles_dir, quoted_bundles_dir, sizeof(quoted_bundles_dir));
        shell_quote(target_binary, quoted_target_binary, sizeof(quoted_target_binary));
        shell_quote(target_bundle_arm, quoted_target_bundle_arm, sizeof(quoted_target_bundle_arm));
        shell_quote(target_bundle_x86, quoted_target_bundle_x86, sizeof(quoted_target_bundle_x86));
        if (target_icon[0]) shell_quote(target_icon, quoted_target_icon, sizeof(quoted_target_icon));
        shell_quote(wrapper_path, quoted_wrapper_path, sizeof(quoted_wrapper_path));
        shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
        shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));
        shell_quote(app->service_id, quoted_service_id, sizeof(quoted_service_id));
        shell_quote(app->display_name, quoted_display_name, sizeof(quoted_display_name));
        shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path));

        char user_plist_path[PATH_MAX];
        snprintf(user_plist_path, sizeof(user_plist_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), app->service_id);
        char user_install_root[PATH_MAX];
        snprintf(user_install_root, sizeof(user_install_root), "%s/Library/dev.outergroup.OuterLoop/backends/%s", home_directory(), app->install_directory_name);
        char quoted_user_plist_path[PATH_MAX + 8];
        char quoted_user_install_root[PATH_MAX + 8];
        shell_quote(user_plist_path, quoted_user_plist_path, sizeof(quoted_user_plist_path));
        shell_quote(user_install_root, quoted_user_install_root, sizeof(quoted_user_install_root));

        char script_template[] = "/tmp/backends-root-install-XXXXXX";
        int script_fd = mkstemp(script_template);
        if (script_fd < 0) {
            free(plist.data);
            snprintf(message, message_size, "Failed to create privileged install script: %s", strerror(errno));
            return false;
        }
        FILE *script = fdopen(script_fd, "w");
        if (!script) {
            close(script_fd);
            unlink(script_template);
            free(plist.data);
            snprintf(message, message_size, "Failed to write privileged install script: %s", strerror(errno));
            return false;
        }
        fprintf(script,
                "set -eu\n"
                "launchctl bootout system/%s >/dev/null 2>&1 || true\n"
                "mkdir -p %s %s /Library/LaunchDaemons /Library/Logs %s\n"
                "chmod 0755 %s\n"
                "install -m 0755 %s %s\n"
                "install -m 0644 %s %s\n"
                "install -m 0644 %s %s\n",
                app->service_id,
                quoted_install_root,
                quoted_bundles_dir,
                quoted_system_root,
                quoted_system_root,
                quoted_source_binary,
                quoted_target_binary,
                quoted_source_bundle_arm,
                quoted_target_bundle_arm,
                quoted_source_bundle_x86,
                quoted_target_bundle_x86);
        if (quoted_source_icon[0] && quoted_target_icon[0]) {
            fprintf(script, "install -m 0644 %s %s\n", quoted_source_icon, quoted_target_icon);
        }
        fprintf(script,
                "cat > %s <<'__BACKENDS_OUTERCTL__'\n%s__BACKENDS_OUTERCTL__\n"
                "chmod 0755 %s\n"
                "cat > %s <<'__BACKENDS_PLIST__'\n%s__BACKENDS_PLIST__\n"
                "chmod 0644 %s\n"
                "touch %s\n"
                "chmod 0644 %s\n"
                "REGISTRY_ROOT=%s SERVICE_ID=%s DISPLAY_NAME=%s PLIST_PATH=%s LOG_PATH=%s ICON_PATH=%s SOCKET_PATH=%s python3 - <<'__BACKENDS_REGISTRY__'\n"
                "import base64, os, sqlite3\n"
                "root = os.environ['REGISTRY_ROOT']\n"
                "service_id = os.environ['SERVICE_ID']\n"
                "display_name = os.environ['DISPLAY_NAME']\n"
                "plist_path = os.environ['PLIST_PATH']\n"
                "log_path = os.environ['LOG_PATH']\n"
                "icon_path = os.environ.get('ICON_PATH') or None\n"
                "socket_path = os.environ.get('SOCKET_PATH') or ''\n"
                "def registry_icon_value(path):\n"
                "    if not path:\n"
                "        return None\n"
                "    if path.startswith('data:'):\n"
                "        return path\n"
                "    try:\n"
                "        with open(path, 'rb') as file:\n"
                "            data = file.read(1024 * 1024 + 1)\n"
                "    except OSError:\n"
                "        return path\n"
                "    if not data or len(data) > 1024 * 1024:\n"
                "        return path\n"
                "    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')\n"
                "icon_value = registry_icon_value(icon_path)\n"
                "os.makedirs(root, exist_ok=True)\n"
                "database_path = os.path.join(root, 'registry.sqlite3')\n"
                "database = sqlite3.connect(database_path)\n"
                "database.executescript('''\n"
                "CREATE TABLE IF NOT EXISTS backends (service_id TEXT PRIMARY KEY, display_name TEXT NOT NULL DEFAULT '', icon TEXT, service_unit TEXT);\n"
                "CREATE TABLE IF NOT EXISTS frontends (url TEXT PRIMARY KEY, service_id TEXT, name TEXT NOT NULL, port INTEGER NOT NULL DEFAULT 0, socket_path TEXT NOT NULL DEFAULT '', icon TEXT, is_home_screen INTEGER NOT NULL DEFAULT 0, list TEXT);\n"
                "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);\n"
                "CREATE TABLE IF NOT EXISTS log_files (path TEXT PRIMARY KEY, service_id TEXT NOT NULL);\n"
                "CREATE INDEX IF NOT EXISTS log_files_service_id_idx ON log_files(service_id);\n"
                "CREATE TABLE IF NOT EXISTS launchd_backends (service_id TEXT PRIMARY KEY, plist_path TEXT NOT NULL, owns_plist INTEGER NOT NULL DEFAULT 0);\n"
                "CREATE TABLE IF NOT EXISTS systemd_backends (service_id TEXT PRIMARY KEY, unit_name TEXT NOT NULL, scope TEXT NOT NULL DEFAULT 'user');\n"
                "''')\n"
                "columns = {row[1] for row in database.execute('PRAGMA table_info(frontends)')}\n"
                "if 'is_home_screen' not in columns:\n"
                "    database.execute('ALTER TABLE frontends ADD COLUMN is_home_screen INTEGER NOT NULL DEFAULT 0')\n"
                "if 'list' not in columns:\n"
                "    database.execute('ALTER TABLE frontends ADD COLUMN list TEXT')\n"
                "with database:\n"
                "    database.execute('INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, ?, NULL) ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon=excluded.icon, service_unit=NULL', (service_id, display_name, icon_value))\n"
                "    database.execute('INSERT INTO launchd_backends(service_id, plist_path, owns_plist) VALUES(?, ?, 1) ON CONFLICT(service_id) DO UPDATE SET plist_path=excluded.plist_path, owns_plist=excluded.owns_plist', (service_id, plist_path))\n"
                "    database.execute('DELETE FROM systemd_backends WHERE service_id = ?', (service_id,))\n"
                "    database.execute('DELETE FROM frontends WHERE service_id = ?', (service_id,))\n"
                "    if socket_path:\n"
                "        database.execute('INSERT INTO frontends(url, service_id, name, port, socket_path, icon) VALUES(?, ?, ?, 0, ?, ?)', (socket_path, service_id, display_name, socket_path, icon_value))\n"
                "    database.execute('DELETE FROM log_files WHERE service_id = ?', (service_id,))\n"
                "    database.execute('INSERT INTO log_files(path, service_id) VALUES(?, ?)', (log_path, service_id))\n"
                "database.close()\n"
                "os.chmod(database_path, 0o644)\n"
                "__BACKENDS_REGISTRY__\n"
                "chmod 0644 %s/registry.sqlite3 >/dev/null 2>&1 || true\n"
                "launchctl bootstrap system %s\n"
                "launchctl kickstart -k system/%s\n",
                quoted_wrapper_path,
                wrapper_contents,
                quoted_wrapper_path,
                quoted_plist_path,
                plist.data ? plist.data : "",
                quoted_plist_path,
                quoted_log_path,
                quoted_log_path,
                quoted_system_root,
                quoted_service_id,
                quoted_display_name,
                quoted_plist_path,
                quoted_log_path,
                quoted_target_icon[0] ? quoted_target_icon : "''",
                quoted_socket_path,
                quoted_system_root,
                quoted_plist_path,
                app->service_id);
        fclose(script);
        free(plist.data);
        chmod(script_template, 0700);

        bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
        unlink(script_template);
        if (!root_ok) return false;

        char stop_user_target[384];
        char quoted_stop_user_target[512];
        snprintf(stop_user_target, sizeof(stop_user_target), "gui/%d/%s", (int)getuid(), app->service_id);
        shell_quote(stop_user_target, quoted_stop_user_target, sizeof(quoted_stop_user_target));
        char cleanup_command[PATH_MAX * 2 + 512];
        snprintf(cleanup_command,
                 sizeof(cleanup_command),
                 "launchctl bootout %s >/dev/null 2>&1 || true; rm -f -- %s; rm -rf -- %s",
                 quoted_stop_user_target,
                 quoted_user_plist_path,
                 quoted_user_install_root);
        run_shell_ignored(cleanup_command);
        char error[1024] = "";
        if (!unregister_backend_records(app->service_id, error, sizeof(error))) {
            snprintf(message, message_size, "%s", error);
            return false;
        }

        snprintf(message, message_size, "Installed %s as root.", app->display_name);
        return true;
    }

    char install_root[PATH_MAX];
    snprintf(install_root, sizeof(install_root), "%s/Library/dev.outergroup.OuterLoop/backends/%s", home_directory(), app->install_directory_name);
    char bundles_dir[PATH_MAX];
    snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
    char target_binary[PATH_MAX];
    snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
    char target_bundle_arm[PATH_MAX];
    snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
    char target_bundle_x86[PATH_MAX];
    snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
    char target_icon[PATH_MAX] = "";
    if (source_icon[0]) snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), app->service_id);
    char plist_path[PATH_MAX];
    snprintf(plist_path, sizeof(plist_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), app->service_id);
    char outerctl_path[PATH_MAX];
    snprintf(outerctl_path, sizeof(outerctl_path), "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());

    char error[1024] = "";
    if (!mkdir_p(bundles_dir)) {
        snprintf(message, message_size, "Failed to create %s: %s", bundles_dir, strerror(errno));
        return false;
    }
    char log_dir[PATH_MAX];
    snprintf(log_dir, sizeof(log_dir), "%s", log_path);
    char *slash = strrchr(log_dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(log_dir)) {
            snprintf(message, message_size, "Failed to create %s: %s", log_dir, strerror(errno));
            return false;
        }
    }
    char launch_agents_dir[PATH_MAX];
    snprintf(launch_agents_dir, sizeof(launch_agents_dir), "%s/Library/LaunchAgents", home_directory());
    if (!mkdir_p(launch_agents_dir)) {
        snprintf(message, message_size, "Failed to create %s: %s", launch_agents_dir, strerror(errno));
        return false;
    }

    char target[320];
    char quoted_target[384];
    snprintf(target, sizeof(target), "gui/%d/%s", (int)getuid(), app->service_id);
    shell_quote(target, quoted_target, sizeof(quoted_target));
    char bootout_command[512];
    snprintf(bootout_command, sizeof(bootout_command), "launchctl bootout %s >/dev/null 2>&1 || true", quoted_target);
    run_shell_ignored(bootout_command);

    if (!copy_file(source_binary, target_binary, 0700, error, sizeof(error)) ||
        !copy_file(source_bundle_arm, target_bundle_arm, 0600, error, sizeof(error)) ||
        !copy_file(source_bundle_x86, target_bundle_x86, 0600, error, sizeof(error)) ||
        (source_icon[0] && !copy_file(source_icon, target_icon, 0600, error, sizeof(error)))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }

    StringBuilder plist = {0};
    if (!make_bundled_launchd_plist(app->service_id,
                                    target_binary,
                                    bundles_dir,
                                    target_icon,
                                    socket_path,
                                    0600,
                                    install_root,
                                    outerctl_path,
                                    log_path,
                                    &plist)) {
        free(plist.data);
        snprintf(message, message_size, "Failed to generate LaunchAgent plist.");
        return false;
    }
    if (!write_text_file(plist_path, plist.data ? plist.data : "", error, sizeof(error))) {
        free(plist.data);
        snprintf(message, message_size, "%s", error);
        return false;
    }
    free(plist.data);

    if (!upsert_launchd_backend_registry_at(g_registry_database_path,
                                            app->service_id,
                                            app->display_name,
                                            plist_path,
                                            socket_path,
                                            log_path,
                                            target_icon,
                                            error,
                                            sizeof(error))) {
        unlink(plist_path);
        snprintf(message, message_size, "%s", error);
        return false;
    }

    char start_message[4096] = "";
    bool started = run_launchd_operation(app->service_id, plist_path, "start", start_message, sizeof(start_message));
    if (!started) {
        snprintf(message, message_size, "Installed %s, but failed to start it: %s", app->display_name, start_message);
        return false;
    }

    snprintf(message, message_size, "Installed %s.", app->display_name);
    return true;
}
#endif

static bool uninstall_backend(const char *service_id, const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;
    char error[1024] = "";
    char unit_name[256] = "";
    char scope[32] = "user";
    bool found = lookup_systemd_backend_any(service_id, unit_name, sizeof(unit_name), scope, sizeof(scope));

#ifdef __APPLE__
    char plist_path[PATH_MAX] = "";
    int owns_plist = 0;
    if (!found && lookup_launchd_backend_any(service_id, plist_path, sizeof(plist_path), &owns_plist)) {
        if (strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0) {
            const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
            char install_name[PATH_MAX];
            snprintf(install_name, sizeof(install_name), "%s", app ? app->install_directory_name : service_id);
            char install_root[PATH_MAX] = "";
            if (safe_service_directory_name(install_name)) {
                snprintf(install_root, sizeof(install_root), "/Library/Application Support/OuterLoopServerTools/%s", install_name);
            }
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "/Library/Logs/%s.log", service_id);
            char socket_path[PATH_MAX] = "";
            if (app) {
                bundled_socket_path_for_scope(app, "system", socket_path, sizeof(socket_path));
            }

            char quoted_plist_path[PATH_MAX + 8];
            char quoted_install_root[PATH_MAX + 8] = "";
            char quoted_log_path[PATH_MAX + 8];
            char quoted_socket_path[PATH_MAX + 8] = "";
            char quoted_system_root[PATH_MAX + 8];
            char quoted_service_id[512];
            shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));
            if (install_root[0]) shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
            shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
            if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path));
            shell_quote(kSystemOuterAgentRoot, quoted_system_root, sizeof(quoted_system_root));
            shell_quote(service_id, quoted_service_id, sizeof(quoted_service_id));

            char script_template[] = "/tmp/backends-root-uninstall-XXXXXX";
            int script_fd = mkstemp(script_template);
            if (script_fd < 0) {
                snprintf(message, message_size, "Failed to create privileged uninstall script: %s", strerror(errno));
                return false;
            }
            FILE *script = fdopen(script_fd, "w");
            if (!script) {
                close(script_fd);
                unlink(script_template);
                snprintf(message, message_size, "Failed to write privileged uninstall script: %s", strerror(errno));
                return false;
            }
            fprintf(script,
                    "set -eu\n"
                    "launchctl bootout system/%s >/dev/null 2>&1 || true\n"
                    "rm -f -- %s %s\n"
                    "REGISTRY_ROOT=%s SERVICE_ID=%s python3 - <<'__BACKENDS_REGISTRY__'\n"
                    "import os, sqlite3\n"
                    "root = os.environ['REGISTRY_ROOT']\n"
                    "service_id = os.environ['SERVICE_ID']\n"
                    "database_path = os.path.join(root, 'registry.sqlite3')\n"
                    "if os.path.exists(database_path):\n"
                    "    database = sqlite3.connect(database_path)\n"
                    "    with database:\n"
                    "        for table in ('frontends', 'log_files', 'launchd_backends', 'systemd_backends', 'backends'):\n"
                    "            try:\n"
                    "                key = 'service_id'\n"
                    "                database.execute(f'DELETE FROM {table} WHERE {key} = ?', (service_id,))\n"
                    "            except sqlite3.OperationalError:\n"
                    "                pass\n"
                    "    database.close()\n"
                    "__BACKENDS_REGISTRY__\n",
                    service_id,
                    owns_plist && plist_path[0] ? quoted_plist_path : "''",
                    quoted_socket_path[0] ? quoted_socket_path : "''",
                    quoted_system_root,
                    quoted_service_id);
            if (quoted_install_root[0]) {
                fprintf(script, "rm -rf -- %s\n", quoted_install_root);
            }
            fprintf(script, "rm -f -- %s\n", quoted_log_path);
            fclose(script);
            chmod(script_template, 0700);
            bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
            unlink(script_template);
            if (!root_ok) return false;
        } else {
            char stop_message[1024] = "";
            (void)run_launchd_operation(service_id, plist_path, "stop", stop_message, sizeof(stop_message));
            if (owns_plist && plist_path[0]) {
                unlink(plist_path);
            }
        }
    }
#endif

    if (found && safe_unit_name(unit_name)) {
        char quoted_unit[320];
        shell_quote(unit_name, quoted_unit, sizeof(quoted_unit));
        if (strcmp(scope, "system") == 0) {
            const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
            char install_name[PATH_MAX];
            snprintf(install_name, sizeof(install_name), "%s", app ? app->install_directory_name : service_id);
            char install_root[PATH_MAX] = "";
            if (safe_service_directory_name(install_name)) {
                snprintf(install_root, sizeof(install_root), "/opt/outergroup/%s", install_name);
            }
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "/var/log/outergroup/%s.log", service_id);
            char unit_path[PATH_MAX];
            snprintf(unit_path, sizeof(unit_path), "/etc/systemd/system/%s", unit_name);
            char socket_unit_name[256] = "";
            char socket_unit_path[PATH_MAX] = "";
            if (app && app->socket_activated) {
                systemd_socket_unit_name(unit_name, socket_unit_name, sizeof(socket_unit_name));
                snprintf(socket_unit_path, sizeof(socket_unit_path), "/etc/systemd/system/%s", socket_unit_name);
            }
            char wrapper_path[PATH_MAX] = "";
            if (install_root[0]) {
                snprintf(wrapper_path, sizeof(wrapper_path), "%s/outerctl-as-user", install_root);
            }

            char quoted_unit_path[PATH_MAX + 8];
            char quoted_socket_unit[320] = "";
            char quoted_socket_unit_path[PATH_MAX + 8] = "";
            char quoted_install_root[PATH_MAX + 8];
            char quoted_log_path[PATH_MAX + 8];
            char quoted_wrapper_path[PATH_MAX + 8];
            char quoted_outerctl_path[PATH_MAX + 8];
            char quoted_system_outeragent_root[PATH_MAX + 8];
            char quoted_service_id[512];
            shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
            if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
            if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';
            if (install_root[0]) shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root)); else quoted_install_root[0] = '\0';
            shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
            if (wrapper_path[0]) shell_quote(wrapper_path, quoted_wrapper_path, sizeof(quoted_wrapper_path)); else quoted_wrapper_path[0] = '\0';
            char outerctl_path[PATH_MAX];
            snprintf(outerctl_path, sizeof(outerctl_path), "%s/.outeragent/outerctl", home_directory());
            shell_quote(outerctl_path, quoted_outerctl_path, sizeof(quoted_outerctl_path));
            shell_quote(kSystemOuterAgentRoot, quoted_system_outeragent_root, sizeof(quoted_system_outeragent_root));
            shell_quote(service_id, quoted_service_id, sizeof(quoted_service_id));

            char script_template[] = "/tmp/backends-root-uninstall-XXXXXX";
            int script_fd = mkstemp(script_template);
            if (script_fd < 0) {
                snprintf(message, message_size, "Failed to create privileged uninstall script: %s", strerror(errno));
                return false;
            }
            FILE *script = fdopen(script_fd, "w");
            if (!script) {
                close(script_fd);
                unlink(script_template);
                snprintf(message, message_size, "Failed to write privileged uninstall script: %s", strerror(errno));
                return false;
            }
            fprintf(script,
                    "set -eu\n"
                    "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
                    "%s%s%s"
                    "if [ -x %s ]; then\n"
                    "  %s app clear --backend %s >/dev/null 2>&1 || true\n"
                    "  %s log clear --backend %s >/dev/null 2>&1 || true\n"
                    "  %s systemd clear --backend %s >/dev/null 2>&1 || true\n"
                    "  %s backend remove --backend %s >/dev/null 2>&1 || true\n"
                    "elif [ -x %s ]; then\n"
                    "  env OUTERAGENT_ROOT=%s %s app clear --backend %s >/dev/null 2>&1 || true\n"
                    "  env OUTERAGENT_ROOT=%s %s log clear --backend %s >/dev/null 2>&1 || true\n"
                    "  env OUTERAGENT_ROOT=%s %s systemd clear --backend %s >/dev/null 2>&1 || true\n"
                    "  env OUTERAGENT_ROOT=%s %s backend remove --backend %s >/dev/null 2>&1 || true\n"
                    "fi\n"
                    "rm -f -- %s %s\n",
                    quoted_unit,
                    quoted_socket_unit[0] ? "systemctl --system disable --now " : "",
                    quoted_socket_unit[0] ? quoted_socket_unit : "",
                    quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
                    quoted_wrapper_path[0] ? quoted_wrapper_path : "''",
                    quoted_wrapper_path[0] ? quoted_wrapper_path : "''",
                    quoted_service_id,
                    quoted_wrapper_path[0] ? quoted_wrapper_path : "''",
                    quoted_service_id,
                    quoted_wrapper_path[0] ? quoted_wrapper_path : "''",
                    quoted_service_id,
                    quoted_wrapper_path[0] ? quoted_wrapper_path : "''",
                    quoted_service_id,
                    quoted_outerctl_path,
                    quoted_system_outeragent_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_outeragent_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_outeragent_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_outeragent_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_unit_path,
                    quoted_socket_unit_path[0] ? quoted_socket_unit_path : "''");
            if (quoted_install_root[0]) {
                fprintf(script, "rm -rf -- %s\n", quoted_install_root);
            }
            fprintf(script,
                    "rm -f -- %s\n"
                    "systemctl --system daemon-reload\n",
                    quoted_log_path);
            fclose(script);
            chmod(script_template, 0700);
            bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
            unlink(script_template);
            if (!root_ok) return false;
        } else {
            char command[768];
            snprintf(command, sizeof(command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_unit);
            run_shell_ignored(command);
            char unit_path[PATH_MAX];
            snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), unit_name);
            unlink(unit_path);
            const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
            if (app && app->socket_activated) {
                char socket_unit[256];
                char quoted_socket_unit[320];
                char socket_unit_path[PATH_MAX];
                systemd_socket_unit_name(unit_name, socket_unit, sizeof(socket_unit));
                if (safe_unit_name(socket_unit)) {
                    shell_quote(socket_unit, quoted_socket_unit, sizeof(quoted_socket_unit));
                    snprintf(command, sizeof(command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_socket_unit);
                    run_shell_ignored(command);
                    snprintf(socket_unit_path, sizeof(socket_unit_path), "%s/.config/systemd/user/%s", home_directory(), socket_unit);
                    unlink(socket_unit_path);
                }
            }
            run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1 || true");
        }
    }

    if (!unregister_backend_records(service_id, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }

    const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
    if (app || safe_service_directory_name(service_id)) {
        char install_name[PATH_MAX];
        snprintf(install_name, sizeof(install_name), "%s", app ? app->install_directory_name : service_id);
        if (safe_service_directory_name(install_name)) {
            char install_root[PATH_MAX];
#ifdef __APPLE__
            snprintf(install_root, sizeof(install_root), "%s/Library/dev.outergroup.OuterLoop/backends/%s", home_directory(), install_name);
#else
            snprintf(install_root, sizeof(install_root), "%s/.outeragent/%s", home_directory(), install_name);
#endif
            char quoted_root[PATH_MAX + 8];
            shell_quote(install_root, quoted_root, sizeof(quoted_root));
            char command[PATH_MAX + 64];
            snprintf(command, sizeof(command), "rm -rf -- %s", quoted_root);
            run_shell_ignored(command);
        }
    }

    snprintf(message, message_size, "Uninstalled %s.", service_id);
    return true;
}

static bool path_is_executable_file(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

typedef struct {
    char paths[256][PATH_MAX];
    size_t count;
} PathSet;

static bool path_set_contains(PathSet *set, const char *path) {
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->paths[i], path) == 0) return true;
    }
    return false;
}

static bool path_set_insert(PathSet *set, const char *path) {
    if (path_set_contains(set, path)) return false;
    if (set->count < sizeof(set->paths) / sizeof(set->paths[0])) {
        snprintf(set->paths[set->count], sizeof(set->paths[set->count]), "%s", path);
        set->count++;
    }
    return true;
}

static bool append_python_suggestion(StringBuilder *builder, const char *path, bool *first, PathSet *seen) {
    if (!path_is_executable_file(path)) return true;
    char canonical[PATH_MAX];
    const char *display_path = path;
    if (realpath(path, canonical)) {
        display_path = canonical;
    }
    if (!path_set_insert(seen, display_path)) return true;
    if (!*first && !sb_append(builder, ",")) return false;
    *first = false;
    return sb_append_json_string(builder, display_path);
}

static bool append_python_env_suggestions(StringBuilder *builder, const char *base, bool *first, PathSet *seen) {
    char envs_dir[PATH_MAX];
    append_path_component(envs_dir, sizeof(envs_dir), base, "envs");
    DIR *dir = opendir(envs_dir);
    if (!dir) return true;
    struct dirent *entry;
    bool ok = true;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s/bin/python", envs_dir, entry->d_name);
        ok = append_python_suggestion(builder, path, first, seen);
    }
    closedir(dir);
    return ok;
}

static bool append_pyenv_suggestions(StringBuilder *builder, bool *first, PathSet *seen) {
    char versions_dir[PATH_MAX];
    snprintf(versions_dir, sizeof(versions_dir), "%s/.pyenv/versions", home_directory());
    DIR *dir = opendir(versions_dir);
    if (!dir) return true;
    struct dirent *entry;
    bool ok = true;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s/bin/python", versions_dir, entry->d_name);
        ok = append_python_suggestion(builder, path, first, seen);
    }
    closedir(dir);
    return ok;
}

static bool append_path_python_suggestions(StringBuilder *builder, bool *first, PathSet *seen) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) return true;
    char copy[8192];
    snprintf(copy, sizeof(copy), "%s", path_env);
    bool ok = true;
    for (char *dir = strtok(copy, ":"); ok && dir; dir = strtok(NULL, ":")) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/python3", dir);
        ok = append_python_suggestion(builder, candidate, first, seen);
        snprintf(candidate, sizeof(candidate), "%s/python", dir);
        ok = ok && append_python_suggestion(builder, candidate, first, seen);
    }
    return ok;
}

static bool append_field_json(StringBuilder *builder,
                              const char *key,
                              const char *label,
                              const char *default_value,
                              const char *field_type,
                              const char *placeholder,
                              const char *suggestions_json,
                              const char *choices_json) {
    bool ok = sb_append(builder, "{\"key\":") &&
              sb_append_json_string(builder, key) &&
              sb_append(builder, ",\"label\":") &&
              sb_append_json_string(builder, label) &&
              sb_append(builder, ",\"defaultValue\":") &&
              sb_append_json_string(builder, default_value) &&
              sb_append(builder, ",\"fieldType\":") &&
              sb_append_json_string(builder, field_type) &&
              sb_append(builder, ",\"placeholder\":") &&
              sb_append_json_string(builder, placeholder) &&
              sb_append(builder, ",\"suggestions\":") &&
              sb_append(builder, suggestions_json ? suggestions_json : "[]") &&
              sb_append(builder, ",\"choices\":") &&
              sb_append(builder, choices_json ? choices_json : "[]") &&
              sb_append(builder, "}");
    return ok;
}

static bool append_recipe_json(StringBuilder *builder,
                               const char *identifier,
                               const char *display_name,
                               const char *summary,
                               bool *first_recipe,
                               bool (*append_fields)(StringBuilder *builder, const char *python_suggestions_json),
                               const char *python_suggestions_json) {
    if (!*first_recipe && !sb_append(builder, ",")) return false;
    *first_recipe = false;
    bool ok = sb_append(builder, "{\"identifier\":") &&
              sb_append_json_string(builder, identifier) &&
              sb_append(builder, ",\"displayName\":") &&
              sb_append_json_string(builder, display_name) &&
              sb_append(builder, ",\"summary\":") &&
              sb_append_json_string(builder, summary) &&
              sb_append(builder, ",\"fields\":[");
    ok = ok && append_fields(builder, python_suggestions_json);
    return ok && sb_append(builder, "]}");
}

static bool append_command_recipe_fields(StringBuilder *builder, const char *python_suggestions_json) {
    (void)python_suggestions_json;
    bool ok = append_field_json(builder, "command", "Command", "", "text", "bundle exec jekyll serve --host 0.0.0.0", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "scriptPath", "Script Path", "", "file", "~/dev/run-service.sh", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "port", "Port", "", "text", "4000", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "name", "Display Name", "", "text", "My Service", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "identifier", "Identifier", "", "text", "my-service", NULL, NULL);
    return ok;
}

static bool append_blank_recipe_fields(StringBuilder *builder, const char *python_suggestions_json) {
    (void)python_suggestions_json;
    bool ok = append_field_json(builder, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "scriptPath", "Script Path", "", "file", "~/dev/run-service.sh", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "name", "Display Name", "", "text", "My Service", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "identifier", "Identifier", "", "text", "my-service", NULL, NULL);
    return ok;
}

static bool append_jupyter_recipe_fields(StringBuilder *builder, const char *python_suggestions_json) {
    const char *choices = "[{\"title\":\"Port\",\"value\":\"port\"},{\"title\":\"Unix Socket\",\"value\":\"unixSocket\"}]";
    bool ok = append_field_json(builder, "python", "Python", "/usr/bin/python3", "file", "/usr/bin/python3", python_suggestions_json, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "workdir", "Working Dir", "~/dev", "directory", "~/dev", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "scriptPath", "Script Path", "", "file", "~/dev/run-jupyter.py", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "frontendTransport", "Connection", "port", "choice", "", NULL, choices);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "port", "Port", "", "text", "Auto", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "name", "Display Name", "Jupyter Lab", "text", "Jupyter Lab", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "identifier", "Identifier", "jupyter", "text", "jupyter", NULL, NULL);
    return ok;
}

static bool append_jupyter_uv_recipe_fields(StringBuilder *builder, const char *python_suggestions_json) {
    (void)python_suggestions_json;
    const char *choices = "[{\"title\":\"Port\",\"value\":\"port\"},{\"title\":\"Unix Socket\",\"value\":\"unixSocket\"}]";
    bool ok = append_field_json(builder, "projectDir", "Project Dir", "~", "directory", "~/dev/my-project", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "scriptPath", "Script Path", "", "file", "~/dev/my-project/run-jupyter.py", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "frontendTransport", "Connection", "port", "choice", "", NULL, choices);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "port", "Port", "", "text", "Auto", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "name", "Display Name", "Jupyter Lab", "text", "Jupyter Lab", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "identifier", "Identifier", "jupyter-uv", "text", "jupyter-uv", NULL, NULL);
    return ok;
}

static bool append_existing_recipe_fields(StringBuilder *builder, const char *python_suggestions_json) {
    (void)python_suggestions_json;
    bool ok = append_field_json(builder, "executablePath", "Executable", "", "file", "~/path/to/executable.sh", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "name", "Display Name", "", "text", "My Service", NULL, NULL);
    ok = ok && sb_append(builder, ",") && append_field_json(builder, "identifier", "Identifier", "", "text", "my-service", NULL, NULL);
    return ok;
}

static void send_recipes_response(int fd) {
    StringBuilder suggestions = {0};
    bool first = true;
    PathSet seen = {0};
    bool ok = sb_append(&suggestions, "[");
    char path[PATH_MAX];
    const char *conda_dirs[] = {"miniforge3", "mambaforge", "miniconda3", "anaconda3"};
    for (size_t i = 0; ok && i < sizeof(conda_dirs) / sizeof(conda_dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/bin/python", home_directory(), conda_dirs[i]);
        ok = append_python_suggestion(&suggestions, path, &first, &seen);
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s/%s", home_directory(), conda_dirs[i]);
        ok = ok && append_python_env_suggestions(&suggestions, base, &first, &seen);
    }
    ok = ok && append_pyenv_suggestions(&suggestions, &first, &seen);
    ok = ok && append_python_suggestion(&suggestions, "/opt/homebrew/bin/python3", &first, &seen);
    ok = ok && append_python_suggestion(&suggestions, "/usr/local/bin/python3", &first, &seen);
    ok = ok && append_python_suggestion(&suggestions, "/usr/bin/python3", &first, &seen);
    ok = ok && append_path_python_suggestions(&suggestions, &first, &seen);
    ok = ok && sb_append(&suggestions, "]");
    if (!ok) {
        free(suggestions.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    StringBuilder builder = {0};
    bool first_recipe = true;
    ok = sb_append(&builder, "{\"pythonSuggestions\":") &&
         sb_append(&builder, suggestions.data) &&
         sb_append(&builder, ",\"recipes\":[");
    ok = ok && append_recipe_json(&builder, "command-port", "Run a command, use a hardcoded port number",
                                  "Create a script that runs a command you choose and registers a frontend on a hardcoded port number.",
                                  &first_recipe, append_command_recipe_fields, suggestions.data);
    ok = ok && append_recipe_json(&builder, "custom", "Blank Script",
                                  "Create a minimal script that shows how to use OUTERCTL_PATH yourself.",
                                  &first_recipe, append_blank_recipe_fields, suggestions.data);
    ok = ok && append_recipe_json(&builder, "jupyter", "Jupyter Lab",
                                  "Create a script that launches Jupyter Lab and finds its browser URL with jupyter server list.",
                                  &first_recipe, append_jupyter_recipe_fields, suggestions.data);
    ok = ok && append_recipe_json(&builder, "jupyter-uv", "Jupyter Lab (uv or .venv)",
                                  "Create a script that launches Jupyter Lab from .venv and finds its browser URL with jupyter server list.",
                                  &first_recipe, append_jupyter_uv_recipe_fields, suggestions.data);
    ok = ok && append_recipe_json(&builder, "existing-executable", "Use Existing Executable",
                                  "Choose a script or executable you already keep in your own folders.",
                                  &first_recipe, append_existing_recipe_fields, suggestions.data);
    ok = ok && sb_append(&builder, "]}");
    free(suggestions.data);
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_response(fd, 200, "OK", "application/json; charset=utf-8", builder.data, builder.length);
    free(builder.data);
}

#ifdef __APPLE__
static bool make_launchd_plist(const char *label,
                               const char *program_path,
                               const char *working_directory,
                               const char *outerctl_path,
                               const char *log_path,
                               StringBuilder *builder) {
    bool ok = sb_append(builder,
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                        "<plist version=\"1.0\">\n"
                        "<dict>\n"
                        "    <key>Label</key>\n"
                        "    <string>");
    ok = ok && sb_append_xml_escaped(builder, label);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>ProgramArguments</key>\n"
                         "    <array>\n"
                         "        <string>");
    ok = ok && sb_append_xml_escaped(builder, program_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    </array>\n"
                         "    <key>EnvironmentVariables</key>\n"
                         "    <dict>\n"
                         "        <key>OUTERCTL_PATH</key>\n"
                         "        <string>");
    ok = ok && sb_append_xml_escaped(builder, outerctl_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    </dict>\n"
                         "    <key>WorkingDirectory</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, working_directory);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>StandardOutPath</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, log_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>StandardErrorPath</key>\n"
                         "    <string>");
    ok = ok && sb_append_xml_escaped(builder, log_path);
    ok = ok && sb_append(builder,
                         "</string>\n"
                         "    <key>RunAtLoad</key>\n"
                         "    <false/>\n"
                         "</dict>\n"
                         "</plist>\n");
    return ok;
}
#endif

static void send_create_response(int fd, const char *query) {
    char recipe[128] = "";
    char display_name[512] = "";
    char service_id[256] = "";
    char command[8192] = "";
    char working_directory[PATH_MAX];
    snprintf(working_directory, sizeof(working_directory), "%s", home_directory());

    query_value(query, "recipe", recipe, sizeof(recipe));
    if (!recipe[0]) {
        query_value(query, "displayName", display_name, sizeof(display_name));
        query_value(query, "serviceID", service_id, sizeof(service_id));
        query_value(query, "command", command, sizeof(command));
    } else {
        char name_value[512] = "";
        char identifier_value[256] = "";
        query_value_or_default(query, "name", "", name_value, sizeof(name_value));
        query_value_or_default(query, "identifier", "", identifier_value, sizeof(identifier_value));
        if (strcmp(recipe, "jupyter") == 0 || strcmp(recipe, "jupyter-uv") == 0) {
            if (!name_value[0]) snprintf(name_value, sizeof(name_value), "Jupyter Lab");
            if (!identifier_value[0]) snprintf(identifier_value, sizeof(identifier_value), "%s", strcmp(recipe, "jupyter") == 0 ? "jupyter" : "jupyter-uv");
        } else if (!name_value[0]) {
            snprintf(name_value, sizeof(name_value), "My Service");
        }
        snprintf(display_name, sizeof(display_name), "%s", name_value);
        snprintf(service_id, sizeof(service_id), "%s", identifier_value);
    }

    if (!recipe[0] && (!display_name[0] || !command[0])) {
        send_action_json(fd, 400, false, "Missing displayName or command.");
        return;
    }
    if (!display_name[0] || !command[0]) {
        if (!recipe[0]) {
            send_action_json(fd, 400, false, "Display name and command are required.");
            return;
        }
    }
    if (!service_id[0]) {
        sanitize_identifier_component(display_name, service_id, sizeof(service_id));
    } else {
        char sanitized[256];
        sanitize_identifier_component(service_id, sanitized, sizeof(sanitized));
        snprintf(service_id, sizeof(service_id), "%s", sanitized);
    }
    log_event("Create request recipe=%s serviceID=%s displayName=%s.",
              recipe[0] ? recipe : "command",
              service_id,
              display_name);

    char unit_stem[256];
    sanitize_identifier_component(service_id, unit_stem, sizeof(unit_stem));
    char unit_name[300];
#ifdef __APPLE__
    snprintf(unit_name, sizeof(unit_name), "%s", unit_stem);
    if (!launchd_label_is_safe(unit_name)) {
        send_action_json(fd, 400, false, "Could not construct a safe launchd label.");
        return;
    }
#else
    snprintf(unit_name, sizeof(unit_name), "%s.service", unit_stem);
    if (!safe_unit_name(unit_name)) {
        send_action_json(fd, 400, false, "Could not construct a safe systemd unit name.");
        return;
    }
#endif

    char backend_dir[PATH_MAX];
    char log_path[PATH_MAX];
    char unit_path[PATH_MAX];
#ifdef __APPLE__
    snprintf(backend_dir, sizeof(backend_dir), "%s/Library/dev.outergroup.OuterLoop/backends/%s", home_directory(), service_id);
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), service_id);
    snprintf(unit_path, sizeof(unit_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), unit_name);
#else
    snprintf(backend_dir, sizeof(backend_dir), "%s/.outeragent/%s", home_directory(), service_id);
    snprintf(log_path, sizeof(log_path), "%s/output.log", backend_dir);
    snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), unit_name);
#endif

    char error[4096] = "";

    if (recipe[0]) {
        char workdir_raw[PATH_MAX] = "~";
        if (strcmp(recipe, "jupyter-uv") == 0) {
            query_value_or_default(query, "projectDir", "~", workdir_raw, sizeof(workdir_raw));
        } else {
            query_value_or_default(query, "workdir", "~", workdir_raw, sizeof(workdir_raw));
        }
        expand_tilde_path(workdir_raw[0] ? workdir_raw : "~", working_directory, sizeof(working_directory));

        char script_path[PATH_MAX] = "";
        bool generated_script_recipe = strcmp(recipe, "command-port") == 0 ||
                                       strcmp(recipe, "custom") == 0 ||
                                       strcmp(recipe, "jupyter") == 0 ||
                                       strcmp(recipe, "jupyter-uv") == 0;
        if (generated_script_recipe) {
            char script_path_raw[PATH_MAX] = "";
            query_value(query, "scriptPath", script_path_raw, sizeof(script_path_raw));
            if (!resolve_user_script_path(script_path_raw, working_directory, script_path, sizeof(script_path))) {
                send_action_json(fd, 400, false, "Script Path is required.");
                return;
            }
        }

        if (strcmp(recipe, "command-port") == 0) {
            char port[32] = "";
            query_value(query, "command", command, sizeof(command));
            query_value(query, "port", port, sizeof(port));
            if (!command[0] || !valid_port_text(port)) {
                send_action_json(fd, 400, false, "Command and a valid port are required.");
                return;
            }
            StringBuilder script = {0};
            if (!make_fixed_port_script(service_id, display_name, port, command, &script)) {
                free(script.data);
                send_action_json(fd, 500, false, "Failed to generate script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_json(fd, 500, false, error);
                return;
            }
            free(script.data);
            chmod(script_path, 0755);
#ifdef __APPLE__
            snprintf(command, sizeof(command), "%s", script_path);
#else
            shell_quote(script_path, command, sizeof(command));
#endif
        } else if (strcmp(recipe, "custom") == 0) {
            StringBuilder script = {0};
            if (!make_blank_script(service_id, display_name, &script)) {
                free(script.data);
                send_action_json(fd, 500, false, "Failed to generate script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_json(fd, 500, false, error);
                return;
            }
            free(script.data);
            chmod(script_path, 0755);
#ifdef __APPLE__
            snprintf(command, sizeof(command), "%s", script_path);
#else
            shell_quote(script_path, command, sizeof(command));
#endif
        } else if (strcmp(recipe, "jupyter") == 0 || strcmp(recipe, "jupyter-uv") == 0) {
            char python[PATH_MAX] = "/usr/bin/python3";
            char port[32] = "";
            char transport[64] = "port";
            query_value_or_default(query, "python", "/usr/bin/python3", python, sizeof(python));
            query_value(query, "port", port, sizeof(port));
            query_value_or_default(query, "frontendTransport", "port", transport, sizeof(transport));
            if (port[0] && !valid_port_text(port)) {
                send_action_json(fd, 400, false, "Port must be empty or a valid TCP port.");
                return;
            }
            StringBuilder script = {0};
            if (!make_jupyter_script(service_id, display_name, python, port,
                                     strcmp(transport, "unixSocket") == 0,
                                     strcmp(recipe, "jupyter-uv") == 0,
                                     &script)) {
                free(script.data);
                send_action_json(fd, 500, false, "Failed to generate Jupyter script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_json(fd, 500, false, error);
                return;
            }
            free(script.data);
            chmod(script_path, 0755);
#ifdef __APPLE__
            snprintf(command, sizeof(command), "%s", script_path);
#else
            shell_quote(script_path, command, sizeof(command));
#endif
        } else if (strcmp(recipe, "existing-executable") == 0) {
            char executable[PATH_MAX] = "";
            query_value(query, "executablePath", executable, sizeof(executable));
            if (!executable[0]) {
                send_action_json(fd, 400, false, "Executable is required.");
                return;
            }
            char expanded_executable[PATH_MAX];
            expand_tilde_path(executable, expanded_executable, sizeof(expanded_executable));
#ifdef __APPLE__
            snprintf(command, sizeof(command), "%s", expanded_executable);
#else
            shell_quote(expanded_executable, command, sizeof(command));
#endif
        } else {
            send_action_json(fd, 400, false, "Unknown recipe.");
            return;
        }
    }

#ifdef __APPLE__
    if (!recipe[0]) {
        char script_path[PATH_MAX];
        snprintf(script_path, sizeof(script_path), "%s/run.sh", backend_dir);
        StringBuilder script = {0};
        bool script_ok = sb_append(&script, "#!/bin/sh\nset -eu\nexec /bin/sh -lc ");
        char quoted_raw_command[9000];
        shell_quote(command, quoted_raw_command, sizeof(quoted_raw_command));
        script_ok = script_ok && sb_append(&script, quoted_raw_command) && sb_append(&script, "\n");
        if (!script_ok || !write_text_file(script_path, script.data ? script.data : "", error, sizeof(error))) {
            free(script.data);
            send_action_json(fd, 500, false, error[0] ? error : "Failed to generate script.");
            return;
        }
        free(script.data);
        chmod(script_path, 0755);
        snprintf(command, sizeof(command), "%s", script_path);
    }
#endif

    char quoted_command[9000];
    if (recipe[0]) {
        snprintf(quoted_command, sizeof(quoted_command), "%s", command);
    } else {
        shell_quote(command, quoted_command, sizeof(quoted_command));
    }
    char description[512];
    unit_description_text(display_name, description, sizeof(description));
    char unit_contents[12000];
    snprintf(unit_contents, sizeof(unit_contents),
             "[Unit]\n"
             "Description=%s\n"
             "After=network.target\n"
             "\n"
             "[Service]\n"
             "Type=simple\n"
             "WorkingDirectory=%s\n"
             "Environment=OUTERCTL_PATH=%s/.outeragent/outerctl\n"
             "ExecStart=/bin/sh -lc %s\n"
             "Restart=on-failure\n"
             "RestartSec=2\n"
             "StandardOutput=append:%s\n"
             "StandardError=append:%s\n"
             "SuccessExitStatus=143 SIGTERM\n"
             "\n"
             "[Install]\n"
             "WantedBy=default.target\n",
             description,
             working_directory,
             home_directory(),
             quoted_command,
             log_path,
             log_path);

    if (!mkdir_p(backend_dir)) {
        snprintf(error, sizeof(error), "Failed to create %s: %s", backend_dir, strerror(errno));
        send_action_json(fd, 500, false, error);
        return;
    }

#ifdef __APPLE__
    char log_dir[PATH_MAX];
    snprintf(log_dir, sizeof(log_dir), "%s", log_path);
    char *log_slash = strrchr(log_dir, '/');
    if (log_slash) {
        *log_slash = '\0';
        if (!mkdir_p(log_dir)) {
            snprintf(error, sizeof(error), "Failed to create %s: %s", log_dir, strerror(errno));
            send_action_json(fd, 500, false, error);
            return;
        }
    }
    char launch_agents_dir[PATH_MAX];
    snprintf(launch_agents_dir, sizeof(launch_agents_dir), "%s/Library/LaunchAgents", home_directory());
    if (!mkdir_p(launch_agents_dir)) {
        snprintf(error, sizeof(error), "Failed to create %s: %s", launch_agents_dir, strerror(errno));
        send_action_json(fd, 500, false, error);
        return;
    }
    char outerctl_path[PATH_MAX];
    snprintf(outerctl_path, sizeof(outerctl_path), "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());
    StringBuilder plist = {0};
    if (!make_launchd_plist(unit_name, command, working_directory, outerctl_path, log_path, &plist)) {
        free(plist.data);
        send_action_json(fd, 500, false, "Failed to generate LaunchAgent plist.");
        return;
    }
    if (!write_text_file(unit_path, plist.data, error, sizeof(error))) {
        free(plist.data);
        send_action_json(fd, 500, false, error);
        return;
    }
    free(plist.data);
#else
    if (!write_text_file(unit_path, unit_contents, error, sizeof(error))) {
        send_action_json(fd, 500, false, error);
        return;
    }
#endif
    if (!register_created_backend(service_id, display_name,
#ifdef __APPLE__
                                  unit_path,
#else
                                  unit_name,
#endif
                                  log_path, error, sizeof(error))) {
        unlink(unit_path);
        send_action_json(fd, 500, false, error);
        return;
    }

#ifdef __APPLE__
    char message[4096] = "";
    bool started = run_launchd_operation(unit_name, unit_path, "start", message, sizeof(message));
#else
    char quoted_unit[320];
    shell_quote(unit_name, quoted_unit, sizeof(quoted_unit));
    char enable_command[512];
    snprintf(enable_command, sizeof(enable_command), "systemctl --user enable %s >/dev/null 2>&1", quoted_unit);
    system("systemctl --user daemon-reload >/dev/null 2>&1");
    system(enable_command);
    char message[4096] = "";
    bool started = run_systemd_operation(unit_name, "user", "start", NULL, NULL, message, sizeof(message));
#endif
    if (!started) {
        char response[4600];
        snprintf(response, sizeof(response), "Created %s, but failed to start it: %s", display_name, message);
        log_event("Created backend %s but failed to start it: %s", service_id, message);
        send_action_json(fd, 500, false, response);
        return;
    }

    char response[512];
    snprintf(response, sizeof(response), "Created %s.", display_name);
    log_event("Created and started backend %s.", service_id);
    send_action_json(fd, 200, true, response);
}

static void send_logs_response(int fd, const char *query) {
    char service_id[PATH_MAX] = "";
    char raw_path[PATH_MAX] = "";
    char bytes_value[64] = "";
    char index_value[64] = "";
    int requested_bytes = DEFAULT_LOG_BYTES;
    int log_index = 0;

    query_value(query, "serviceID", service_id, sizeof(service_id));
    query_value(query, "path", raw_path, sizeof(raw_path));
    if (query_value(query, "bytes", bytes_value, sizeof(bytes_value))) {
        requested_bytes = atoi(bytes_value);
        if (requested_bytes < 1024) requested_bytes = 1024;
        if (requested_bytes > MAX_LOG_BYTES) requested_bytes = MAX_LOG_BYTES;
    }
    if (query_value(query, "logIndex", index_value, sizeof(index_value))) {
        log_index = atoi(index_value);
        if (log_index < 0) log_index = 0;
    }

    if (!raw_path[0]) {
        if (!service_id[0]) {
            send_log_json(fd, "", "", "", false, 0, 0, "missing serviceID or path");
            return;
        }
        char error[512] = "";
        bool found = resolve_log_path_any(service_id, log_index, raw_path, sizeof(raw_path), error, sizeof(error));
        if (!found) {
            send_log_json(fd, service_id, "", "", false, 0, 0, "no registered log file for this backend");
            return;
        }
    }

    char path[PATH_MAX];
    expand_tilde_path(raw_path, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        char message[PATH_MAX + 80];
        snprintf(message, sizeof(message), "failed to stat log file: %s", strerror(errno));
        send_log_json(fd, service_id, raw_path, "", false, 0, 0, message);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        send_log_json(fd, service_id, raw_path, "", false, 0, 0, "log path is not a regular file");
        return;
    }

    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t bytes_to_read = file_size < (uint64_t)requested_bytes ? file_size : (uint64_t)requested_bytes;
    uint64_t start_offset = file_size > bytes_to_read ? file_size - bytes_to_read : 0;
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_log_json(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
        return;
    }
    if (start_offset > 0 && lseek(file_fd, (off_t)start_offset, SEEK_SET) < 0) {
        close(file_fd);
        send_log_json(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
        return;
    }

    char *buffer = calloc((size_t)bytes_to_read + 1, 1);
    if (!buffer) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    size_t offset = 0;
    while (offset < (size_t)bytes_to_read) {
        ssize_t got = read(file_fd, buffer + offset, (size_t)bytes_to_read - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            close(file_fd);
            send_log_json(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    buffer[offset] = '\0';
    send_log_json(fd, service_id, raw_path, buffer, start_offset > 0, file_size, (double)st.st_mtime, "");
    free(buffer);
}

static void send_outer_descriptor(int fd) {
    const char *plugin_json = "{\"backendsAPIPath\":\"/api/backends\",\"logsAPIPath\":\"/api/logs\",\"controlAPIPath\":\"/api/control\",\"createAPIPath\":\"/api/create\",\"recipesAPIPath\":\"/api/recipes\"}";
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
            if (errno == EINTR) continue;
            free(data);
            close(file_fd);
            send_text_response(fd, 500, "failed to read bundle\n");
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    send_response(fd, 200, "OK", "application/octet-stream", data, offset);
    free(data);
}

static bool is_navigator_route(const char *target) {
    return strcmp(target, "/") == 0 ||
           strcmp(target, "/apps") == 0 ||
           strcmp(target, "/backends") == 0 ||
           strcmp(target, "/new") == 0 ||
           strcmp(target, "/backends.outer") == 0;
}

static bool parsed_content_length(const char *request,
                                  size_t header_length,
                                  size_t *content_length) {
    *content_length = 0;
    char header[READ_BUFFER_SIZE];
    if (header_length >= sizeof(header)) {
        return false;
    }
    memcpy(header, request, header_length);
    header[header_length] = '\0';

    char *content_length_header = strcasestr(header, "\r\nContent-Length:");
    if (!content_length_header) {
        return true;
    }
    *content_length = (size_t)strtoull(content_length_header + 17, NULL, 10);
    return true;
}

static bool request_is_complete(const char *request, size_t length, size_t *complete_length) {
    *complete_length = 0;
    const char *body_separator = NULL;
    for (size_t i = 0; i + 3 < length; i++) {
        if (request[i] == '\r' &&
            request[i + 1] == '\n' &&
            request[i + 2] == '\r' &&
            request[i + 3] == '\n') {
            body_separator = request + i;
            break;
        }
    }
    if (!body_separator) {
        return false;
    }

    size_t header_length = (size_t)(body_separator + 4 - request);
    size_t content_length = 0;
    if (!parsed_content_length(request, header_length, &content_length)) {
        return false;
    }
    if (content_length > READ_BUFFER_SIZE || header_length + content_length > READ_BUFFER_SIZE) {
        *complete_length = READ_BUFFER_SIZE + 1;
        return true;
    }
    if (length < header_length + content_length) {
        return false;
    }
    *complete_length = header_length + content_length;
    return true;
}

static void process_client_request(int fd, char *request, size_t n) {
    request[n] = '\0';

    char *body = strstr(request, "\r\n\r\n");
    size_t header_length = body ? (size_t)(body + 4 - request) : (size_t)n;
    size_t body_length = body ? (size_t)n - header_length : 0;
    size_t content_length = 0;
    char *content_length_header = strcasestr(request, "\r\nContent-Length:");
    if (content_length_header && (!body || content_length_header < body)) {
        content_length = (size_t)strtoull(content_length_header + 17, NULL, 10);
    }
    while (body && body_length < content_length && (size_t)n < sizeof(request) - 1) {
        ssize_t more = read(fd, request + n, sizeof(request) - 1 - (size_t)n);
        if (more <= 0) break;
        n += more;
        request[n] = '\0';
        body_length += (size_t)more;
    }
    if (body) {
        *body = '\0';
        body += 4;
    } else {
        body = "";
    }

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
        if (strcmp(target, "/api/control") == 0) {
            send_control_response(fd, query, body);
        } else if (strcmp(target, "/api/create") == 0) {
            send_create_response(fd, query);
        } else {
            send_text_response(fd, 404, "not found\n");
        }
    } else if (is_navigator_route(target)) {
        send_outer_descriptor(fd);
    } else if (strcmp(target, kBundleUrlPath) == 0) {
        send_text_response(fd, 200, "macos-arm\nmacos-x86\n");
    } else if (strcmp(target, kBundleUrlPathMacosArm) == 0) {
        const char *path = g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
        send_bundle_file(fd, path);
    } else if (strcmp(target, kBundleUrlPathMacosX86) == 0) {
        const char *path = g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
        send_bundle_file(fd, path);
    } else if (strcmp(target, "/api/backends") == 0) {
        send_backends_response(fd);
    } else if (strcmp(target, "/api/logs") == 0) {
        send_logs_response(fd, query);
    } else if (strcmp(target, "/api/recipes") == 0) {
        send_recipes_response(fd);
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

static bool socket_activation_enabled(void) {
    return g_systemd_socket_activation || g_launchd_socket_activation;
}

#ifdef __APPLE__
static int launchd_activated_listener(const char *socket_name) {
    const char *resolved_name = (socket_name && socket_name[0]) ? socket_name : "Listener";
    int *fds = NULL;
    size_t count = 0;
    int result = launch_activate_socket(resolved_name, &fds, &count);
    if (result != 0) {
        if (result != ENOENT && result != ESRCH && result != EALREADY) {
            fprintf(stderr, "launch_activate_socket(%s) failed: %s\n", resolved_name, strerror(result));
        }
        return -1;
    }
    if (!fds || count == 0) {
        free(fds);
        return -1;
    }
    int listener = fds[0];
    for (size_t i = 1; i < count; i++) {
        close(fds[i]);
    }
    free(fds);
    g_launchd_socket_activation = true;
    return listener;
}
#else
static int launchd_activated_listener(const char *socket_name) {
    (void)socket_name;
    return -1;
}
#endif

static void close_reactor_client(ReactorClient *clients, size_t *client_count, size_t index) {
    if (index >= *client_count) return;
    close(clients[index].fd);
    if (index + 1 < *client_count) {
        memmove(&clients[index],
                &clients[index + 1],
                (*client_count - index - 1) * sizeof(clients[0]));
    }
    (*client_count)--;
}

static void add_reactor_client(ReactorClient *clients, size_t *client_count, int client_fd) {
    if (*client_count >= MAX_REACTOR_CLIENTS) {
        close(client_fd);
        return;
    }
    set_fd_nonblocking(client_fd, true);
    ReactorClient *client = &clients[(*client_count)++];
    memset(client, 0, sizeof(*client));
    client->fd = client_fd;
    client->last_activity_ms = monotonic_milliseconds();
}

static bool read_reactor_client(ReactorClient *client,
                                size_t *complete_length,
                                bool *should_close) {
    *complete_length = 0;
    *should_close = false;

    for (;;) {
        if (client->length >= sizeof(client->request) - 1) {
            *complete_length = READ_BUFFER_SIZE + 1;
            return true;
        }

        ssize_t got = read(client->fd,
                           client->request + client->length,
                           sizeof(client->request) - client->length - 1);
        if (got > 0) {
            client->length += (size_t)got;
            client->request[client->length] = '\0';
            client->last_activity_ms = monotonic_milliseconds();
            if (request_is_complete(client->request, client->length, complete_length)) {
                return true;
            }
            continue;
        }
        if (got == 0) {
            *should_close = true;
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        *should_close = true;
        return false;
    }
}

static void accept_ready_clients(int listener, ReactorClient *clients, size_t *client_count) {
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            perror("accept");
            g_shutdown_requested = 1;
            return;
        }
        add_reactor_client(clients, client_count, client);
    }
}

static void run_reactor(int listener) {
    ReactorClient *clients = calloc(MAX_REACTOR_CLIENTS, sizeof(ReactorClient));
    if (!clients) {
        fprintf(stderr, "failed to allocate reactor clients\n");
        return;
    }
    size_t client_count = 0;

    set_fd_nonblocking(listener, true);
    while (!g_shutdown_requested) {
        struct pollfd poll_fds[MAX_REACTOR_CLIENTS + 1];
        size_t polled_client_count = client_count;
        poll_fds[0] = (struct pollfd){.fd = listener, .events = POLLIN, .revents = 0};
        for (size_t i = 0; i < polled_client_count; i++) {
            poll_fds[i + 1] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN, .revents = 0};
        }

        int timeout_ms = 1000;
        if (socket_activation_enabled() && polled_client_count == 0) {
            timeout_ms = 60000;
        }

        int poll_result = poll(poll_fds, (nfds_t)(polled_client_count + 1), timeout_ms);
        if (poll_result == 0) {
            if (socket_activation_enabled() && client_count == 0) {
                break;
            }
        } else if (poll_result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        } else {
            if (poll_fds[0].revents & POLLIN) {
                accept_ready_clients(listener, clients, &client_count);
            } else if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                break;
            }

            for (size_t i = polled_client_count; i > 0; i--) {
                size_t index = i - 1;
                short revents = poll_fds[index + 1].revents;
                if (revents == 0) continue;

                if (revents & POLLIN) {
                    size_t complete_length = 0;
                    bool should_close = false;
                    bool complete = read_reactor_client(&clients[index], &complete_length, &should_close);
                    if (complete) {
                        set_fd_nonblocking(clients[index].fd, false);
                        if (complete_length > READ_BUFFER_SIZE) {
                            send_text_response(clients[index].fd, 400, "request too large\n");
                        } else {
                            process_client_request(clients[index].fd,
                                                   clients[index].request,
                                                   complete_length);
                        }
                        close_reactor_client(clients, &client_count, index);
                    } else if (should_close) {
                        close_reactor_client(clients, &client_count, index);
                    }
                } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    close_reactor_client(clients, &client_count, index);
                }
            }
        }

        int64_t now = monotonic_milliseconds();
        for (size_t i = client_count; i > 0; i--) {
            size_t index = i - 1;
            if (now - clients[index].last_activity_ms > CLIENT_IDLE_TIMEOUT_MS) {
                close_reactor_client(clients, &client_count, index);
            }
        }
    }

    for (size_t i = 0; i < client_count; i++) {
        close(clients[i].fd);
    }
    free(clients);
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--launchd-socket-name NAME] [--bundles-dir DIR] [--bundled-apps-dir DIR] [--app-base-url URL] [--database PATH] [--system-database PATH]\n", program);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    bool use_port = true;
    char socket_path[PATH_MAX] = "";
    char launchd_socket_name[128] = "Listener";
    const char *bundles_dir = "bundles";
    const char *app_base_url = getenv("HOME_SCREEN_APP_BASE_URL");
    if (app_base_url && app_base_url[0]) {
        snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", app_base_url);
    }
    default_registry_database_path(g_registry_database_path, sizeof(g_registry_database_path));
    default_system_registry_database_path(g_system_registry_database_path, sizeof(g_system_registry_database_path));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], socket_path, sizeof(socket_path));
            use_port = false;
        } else if (strcmp(argv[i], "--launchd-socket-name") == 0 && i + 1 < argc) {
            snprintf(launchd_socket_name, sizeof(launchd_socket_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            bundles_dir = argv[++i];
        } else if (strcmp(argv[i], "--bundled-apps-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_bundled_apps_directory, sizeof(g_bundled_apps_directory));
        } else if (strcmp(argv[i], "--app-base-url") == 0 && i + 1 < argc) {
            snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--database") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_registry_database_path, sizeof(g_registry_database_path));
        } else if (strcmp(argv[i], "--system-database") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_system_registry_database_path, sizeof(g_system_registry_database_path));
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
             "%s/BackendsContent.bundle.macos-arm.aar", bundles_dir);
    snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
             "%s/BackendsContent.bundle.macos-x86.aar", bundles_dir);

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

    int listener = !use_port ? systemd_activated_listener() : -1;
    if (listener < 0 && !use_port) {
        listener = launchd_activated_listener(launchd_socket_name);
    }
    if (listener < 0) {
        listener = use_port ? create_tcp_listener(port) : create_unix_listener(socket_path);
    } else if (socket_path[0]) {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    }
    if (listener < 0) return 1;
    g_listener_fd = listener;
    if (use_port) {
        fprintf(stderr, "HomeScreenBackend listening on http://127.0.0.1:%d/\n", port);
    } else {
        fprintf(stderr, "HomeScreenBackend listening on %s/\n", socket_path);
    }
    fprintf(stderr, "Registry database: %s\n", g_registry_database_path);
    if (g_system_registry_database_path[0]) {
        fprintf(stderr, "System registry database: %s\n", g_system_registry_database_path);
    }
    char resolved_bundled_apps_root[PATH_MAX];
    bundled_apps_root(resolved_bundled_apps_root, sizeof(resolved_bundled_apps_root));
    fprintf(stderr, "Bundled apps directory: %s\n", resolved_bundled_apps_root);
    if (g_bundled_apps_base_url[0]) {
        fprintf(stderr, "Bundled apps base URL: %s\n", g_bundled_apps_base_url);
    }

    run_reactor(listener);

    close(listener);
    g_listener_fd = -1;
    if (!use_port && g_listen_socket_path[0] && !socket_activation_enabled()) {
        unlink(g_listen_socket_path);
    }
    return 0;
}
