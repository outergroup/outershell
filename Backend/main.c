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
#include <sys/file.h>
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

static const char *kOuterShellServiceID = "org.outershell.OuterShell";
static const char *kLegacyNavigatorServiceID = "dev.outergroup.Navigator";
static const char *kLegacyBackendsServiceID = "dev.outergroup.Backends";
static const char *kMigrationServiceID = "dev.outergroup.OuterwebappsMigration";
static const char *kBundleUrlPath = "/bundles/BackendsContent";
static const char *kBundleUrlPathMacosArm = "/bundles/BackendsContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/BackendsContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/BackendsContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/BackendsContent.bundle.macos-x86.aar";
#ifdef __APPLE__
static const char *kSystemOuterWebappsRoot = "/Library/Application Support/outerwebapps";
#else
static const char *kSystemOuterWebappsRoot = "/var/lib/outerwebapps";
#endif

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_registry_database_path[PATH_MAX] = "";
static char g_system_registry_database_path[PATH_MAX] = "";
static char g_bundled_apps_directory[PATH_MAX] = "";
static char g_bundled_apps_base_url[2048] = "";
static char g_home_screen_public_base_url[2048] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static char g_api_socket_path[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static bool g_api_systemd_socket_activation = false;
static bool g_launchd_socket_activation = false;
static bool g_stay_alive_when_socket_idle = false;
#ifndef __APPLE__
static uid_t g_root_helper_owner_uid = (uid_t)-1;
#endif
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;
static volatile sig_atomic_t g_api_listener_fd = -1;
static int g_registry_write_lock_fd = -1;
static char g_registry_write_lock_path[PATH_MAX] = "";

typedef struct {
    int fd;
    bool is_api;
    uid_t peer_uid;
    bool has_peer_uid;
    char request[READ_BUFFER_SIZE];
    size_t length;
    int64_t last_activity_ms;
    bool waiting_for_events;
    int64_t event_deadline_ms;
    uint64_t event_since_backends;
    uint64_t event_since_log;
    char event_log_service_id[PATH_MAX];
    int event_log_index;
} ReactorClient;

typedef struct {
    char unit_name[256];
    char scope[16];
    char active_state[32];
} SystemdStatusEntry;

typedef struct {
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    bool is_directory;
    uint64_t size;
    double modified;
    mode_t mode;
} FilePickerEntry;

typedef struct {
    SystemdStatusEntry entries[MAX_SYSTEMD_STATUS_ENTRIES];
    size_t count;
    int64_t refreshed_at_ms;
} SystemdStatusCache;

#define FRONTEND_FLAG_RUNNING 0x01u

static SystemdStatusCache g_systemd_status_cache = {0};
static uint64_t g_backend_event_sequence = 1;

typedef struct {
    const char *service_id;
    const char *display_name;
    const char *unit_name;
    const char *stage_directory_name;
    const char *install_directory_name;
    const char *binary_name;
    const char *bundle_prefix;
    const char *icon_symbol_name;
    const char *icon_name;
    const char *source_name;
    const char *socket_name;
    bool socket_activated;
    bool supports_root;
    bool root_only;
    const char *archive_name;
    const char *version;
} BundledAppDefinition;

typedef struct {
    const char *old_text;
    const char *new_text;
} TextReplacement;

static bool mkdir_p(const char *path);
static void run_shell_ignored(const char *command);
static char *read_text_file_alloc(const char *path, size_t *out_size);
static bool ensure_registry_schema(sqlite3 *database, char *error, size_t error_size);
static bool export_registry_binary_from_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size);
static bool import_registry_binary_into_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size);
static bool registry_binary_output_path(const char *sqlite_path, char *out, size_t out_size);
static int registry_binary_lock(const char *registry_path, int operation, char *error, size_t error_size);
static bool registry_storage_exists_at(const char *database_path);
static void rewrite_files_in_directory_replacing_text(const char *directory,
                                                      const TextReplacement *replacements,
                                                      size_t replacement_count,
                                                      bool recursive);
static void mark_backend_event_changed(void);

static const BundledAppDefinition kBundledApps[] = {
    {
        .service_id = "dev.outergroup.Top",
        .display_name = "Top",
        .unit_name = "dev.outergroup.Top.service",
        .stage_directory_name = "Top",
        .install_directory_name = "dev.outergroup.Top",
        .binary_name = "TopBackend",
        .bundle_prefix = "TopContent",
        .icon_symbol_name = "chart.bar.xaxis",
        .icon_name = "app-icon.png",
        .source_name = "TopBackend.c",
        .socket_name = "dev.outergroup.Top",
        .socket_activated = false,
        .supports_root = true,
        .root_only = false,
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
        .icon_symbol_name = "folder",
        .icon_name = "app-icon.png",
        .source_name = "FilesBackend.c",
        .socket_name = "dev.outergroup.Files",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .archive_name = "Files.tar.gz",
        .version = "1"
    },
    {
        .service_id = "dev.outergroup.Plaintext",
        .display_name = "Plaintext",
        .unit_name = "dev.outergroup.Plaintext.service",
        .stage_directory_name = "Plaintext",
        .install_directory_name = "dev.outergroup.Plaintext",
        .binary_name = "PlaintextBackend",
        .bundle_prefix = "PlaintextContent",
        .icon_symbol_name = "doc.plaintext",
        .icon_name = NULL,
        .source_name = "PlaintextBackend.c",
        .socket_name = "dev.outergroup.Plaintext",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .archive_name = "Plaintext.tar.gz",
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
        .icon_symbol_name = "network",
        .icon_name = "app-icon.png",
        .source_name = "NetworkInspectorBackend.c",
        .socket_name = "dev.outergroup.NetworkInspector",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
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
        .icon_symbol_name = "text.line.last.and.arrowtriangle.forward",
        .icon_name = "app-icon.png",
        .source_name = "TraceBackend.c",
        .socket_name = "dev.outergroup.Firehose",
        .socket_activated = true,
        .supports_root = true,
        .root_only = true,
        .archive_name = "Firehose.tar.gz",
        .version = "1"
    }
};

static bool is_home_screen_service_id(const char *service_id) {
    return service_id &&
           (strcmp(service_id, kOuterShellServiceID) == 0 ||
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
    if (g_api_listener_fd >= 0) {
        close((int)g_api_listener_fd);
    }
}

void OuterShellBackendRequestShutdown(void) {
    handle_shutdown_signal(SIGTERM);
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

static void write_uint16_le(unsigned char *dst, uint16_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_uint64_le(unsigned char *dst, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

static uint32_t read_uint32_le(const unsigned char *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint16_t read_uint16_le(const unsigned char *src) {
    return (uint16_t)(((uint16_t)src[0]) | ((uint16_t)src[1] << 8));
}

static uint64_t read_uint64_le(const unsigned char *src) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | (uint64_t)src[i];
    }
    return value;
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

static bool binary_reserve(StringBuilder *builder, size_t additional) {
    return sb_reserve(builder, additional);
}

static bool binary_append_zero(StringBuilder *builder, size_t length) {
    if (!binary_reserve(builder, length)) return false;
    memset(builder->data + builder->length, 0, length);
    builder->length += length;
    return true;
}

static bool binary_write_u32_at(StringBuilder *builder, size_t offset, uint32_t value) {
    if (!builder || offset + 4 > builder->length) return false;
    write_uint32_le((unsigned char *)builder->data + offset, value);
    return true;
}

static bool binary_write_u64_at(StringBuilder *builder, size_t offset, uint64_t value) {
    if (!builder || offset + 8 > builder->length) return false;
    write_uint64_le((unsigned char *)builder->data + offset, value);
    return true;
}

static bool binary_write_f64_at(StringBuilder *builder, size_t offset, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return binary_write_u64_at(builder, offset, bits);
}

static bool binary_write_ref32_at(StringBuilder *builder, size_t ref_offset, size_t data_offset, size_t data_length) {
    if (data_offset > UINT32_MAX || data_length > UINT32_MAX) return false;
    return binary_write_u32_at(builder, ref_offset, (uint32_t)data_offset) &&
           binary_write_u32_at(builder, ref_offset + 4, (uint32_t)data_length);
}

static bool binary_append_data_ref_at(StringBuilder *builder, size_t ref_offset, const void *data, size_t data_length) {
    size_t data_offset = builder->length;
    if (data_length > 0 && !sb_append_n(builder, (const char *)data, data_length)) return false;
    return binary_write_ref32_at(builder, ref_offset, data_offset, data_length);
}

static bool binary_append_string_ref_at(StringBuilder *builder, size_t ref_offset, const char *text) {
    const char *safe_text = text ? text : "";
    return binary_append_data_ref_at(builder, ref_offset, safe_text, strlen(safe_text));
}

static bool binary_write_u16_at(StringBuilder *builder, size_t offset, uint16_t value) {
    if (!builder || offset + 2 > builder->length) return false;
    write_uint16_le((unsigned char *)builder->data + offset, value);
    return true;
}

static bool binary_append_child_ref_at(StringBuilder *builder, size_t ref_offset, StringBuilder *child) {
    return binary_append_data_ref_at(builder, ref_offset, child && child->data ? child->data : NULL, child ? child->length : 0);
}

static bool binary_append_file_ref_at(StringBuilder *builder, size_t ref_offset, const char *path) {
    if (!path || !path[0]) return binary_write_ref32_at(builder, ref_offset, builder->length, 0);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        return binary_write_ref32_at(builder, ref_offset, builder->length, 0);
    }
    FILE *file = fopen(path, "rb");
    if (!file) return binary_write_ref32_at(builder, ref_offset, builder->length, 0);
    unsigned char *bytes = malloc((size_t)st.st_size);
    if (!bytes) {
        fclose(file);
        return false;
    }
    size_t read_count = fread(bytes, 1, (size_t)st.st_size, file);
    fclose(file);
    bool ok = read_count == (size_t)st.st_size &&
              binary_append_data_ref_at(builder, ref_offset, bytes, read_count);
    if (read_count != (size_t)st.st_size) {
        ok = binary_write_ref32_at(builder, ref_offset, builder->length, 0);
    }
    free(bytes);
    return ok;
}

typedef struct {
    StringBuilder *items;
    size_t count;
    size_t capacity;
} BinaryPayloadList;

static void binary_payload_list_free(BinaryPayloadList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].data);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool binary_payload_list_append(BinaryPayloadList *list, StringBuilder *payload) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? list->capacity * 2 : 16;
        StringBuilder *new_items = realloc(list->items, new_capacity * sizeof(StringBuilder));
        if (!new_items) return false;
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *payload;
    payload->data = NULL;
    payload->length = 0;
    payload->capacity = 0;
    return true;
}

static bool binary_build_payload_array(BinaryPayloadList *list, StringBuilder *out) {
    size_t fixed_size = 4 + list->count * 8;
    if (!binary_append_zero(out, fixed_size)) return false;
    if (!binary_write_u32_at(out, 0, (uint32_t)list->count)) return false;
    for (size_t i = 0; i < list->count; i++) {
        if (!binary_append_child_ref_at(out, 4 + i * 8, &list->items[i])) return false;
    }
    return true;
}

static void send_binary_response(int fd, int status, StringBuilder *builder) {
    const char *status_text = status == 200 ? "OK" :
                              status == 401 ? "Unauthorized" :
                              status == 400 ? "Bad Request" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "Error";
    send_response(fd, status, status_text, "application/octet-stream", builder->data, builder->length);
}

static char *registry_icon_path_value(const char *path) {
    if (!path || !path[0]) {
        return NULL;
    }
    return strncmp(path, "data:", 5) == 0 ? NULL : strdup(path);
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
    bool has_service_suffix = len >= 9 && strcmp(unit_name + len - 8, ".service") == 0;
    bool has_socket_suffix = len >= 8 && strcmp(unit_name + len - 7, ".socket") == 0;
    if (len > 240 || (!has_service_suffix && !has_socket_suffix)) return false;
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

static uint64_t parse_u64_or_zero(const char *text) {
    if (!text || !text[0]) return 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (!end || *end != '\0') return 0;
    return (uint64_t)value;
}

static uint64_t mix_u64(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash ? hash : 1;
}

static uint64_t file_state_token(const char *path) {
    if (!path || !path[0]) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t token = (uint64_t)st.st_size;
#ifdef __APPLE__
    token = mix_u64(token, (uint64_t)st.st_mtimespec.tv_sec);
    token = mix_u64(token, (uint64_t)st.st_mtimespec.tv_nsec);
#else
    token = mix_u64(token, (uint64_t)st.st_mtim.tv_sec);
    token = mix_u64(token, (uint64_t)st.st_mtim.tv_nsec);
#endif
    token = mix_u64(token, (uint64_t)st.st_ino);
    return token ? token : 1;
}

static const char *home_directory(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

static void default_user_outerwebapps_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    const char *override_root = getenv("OUTERWEBAPPS_HOME");
    if (override_root && override_root[0]) {
        if (strcmp(override_root, "~") == 0) {
            snprintf(out, out_size, "%s", home_directory());
        } else if (override_root[0] == '~' && override_root[1] == '/') {
            snprintf(out, out_size, "%s/%s", home_directory(), override_root + 2);
        } else {
            snprintf(out, out_size, "%s", override_root);
        }
        return;
    }
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/Application Support/outerwebapps", home_directory());
#else
    const char *state_home = getenv("XDG_STATE_HOME");
    if (state_home && state_home[0]) {
        snprintf(out, out_size, "%s/outerwebapps", state_home);
    } else {
        snprintf(out, out_size, "%s/.local/state/outerwebapps", home_directory());
    }
#endif
}

static void default_user_outerctl_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outerwebapps_root(root, sizeof(root));
    snprintf(out, out_size, "%s/bin/outerctl", root);
}

static void default_user_home_screen_install_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outerwebapps_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outer-shell", root);
}

static void default_user_outerwebapps_app_root(const char *install_name, char *out, size_t out_size) {
    char root[PATH_MAX];
    default_user_outerwebapps_root(root, sizeof(root));
    snprintf(out, out_size, "%s/apps/%s", root, install_name && install_name[0] ? install_name : "");
}

static void default_user_outerwebapps_apps_root(char *out, size_t out_size) {
    char root[PATH_MAX];
    default_user_outerwebapps_root(root, sizeof(root));
    snprintf(out, out_size, "%s/apps", root);
}

static void legacy_user_registry_database_path(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/registry.sqlite3", home_directory());
#else
    snprintf(out, out_size, "%s/.outeragent/registry.sqlite3", home_directory());
#endif
}

static void legacy_user_apps_root(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/backends", home_directory());
#else
    snprintf(out, out_size, "%s/.outeragent", home_directory());
#endif
}

static void legacy_user_outerctl_path(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());
#else
    snprintf(out, out_size, "%s/.outeragent/outerctl", home_directory());
#endif
}

static void legacy_home_screen_outerctl_path(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());
#else
    snprintf(out, out_size, "%s/.outerloop/outer-shell/bin/outerctl", home_directory());
#endif
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
static bool process_api_client_request(ReactorClient *client, char *request, size_t n);
#ifndef __APPLE__
static bool ensure_root_helper_installed(const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool root_helper_outerctl(int argc,
                                 char **argv,
                                 const char *sudo_password,
                                 bool *needs_password,
                                 char *message,
                                 size_t message_size);
static bool root_helper_registry_upsert_systemd(const char *service_id,
                                                const char *display_name,
                                                const char *unit_name,
                                                const char *socket_path,
                                                const char *log_path,
                                                const char *icon_path,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size);
static bool root_helper_registry_remove_backend(const char *service_id,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size);
static bool root_helper_registry_clear_frontends(const char *service_id,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size);
static bool remove_bundled_root_support(const BundledAppDefinition *app,
                                        const char *sudo_password,
                                        bool *needs_password,
                                        char *message,
                                        size_t message_size);
#endif

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

static void runtime_socket_path(const char *name, char *out, size_t out_size) {
#ifdef __APPLE__
    size_t required_length = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (required_length > 0) {
        char temp_dir[PATH_MAX];
        if (required_length < sizeof(temp_dir) &&
            confstr(_CS_DARWIN_USER_TEMP_DIR, temp_dir, required_length) > 0 &&
            temp_dir[0]) {
            size_t length = strlen(temp_dir);
            snprintf(out, out_size, "%s%s%s", temp_dir, length > 0 && temp_dir[length - 1] == '/' ? "" : "/", name);
            return;
        }
    }
    snprintf(out, out_size, "/tmp/%s", name);
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, name);
    } else {
        snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), name);
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

static bool current_executable_path(char *out, size_t out_size) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)out_size;
    if (_NSGetExecutablePath(out, &size) != 0) return false;
    char resolved[PATH_MAX];
    if (realpath(out, resolved)) {
        snprintf(out, out_size, "%s", resolved);
    }
    return out[0] != '\0';
#else
    ssize_t length = readlink("/proc/self/exe", out, out_size > 0 ? out_size - 1 : 0);
    if (length < 0 || out_size == 0) return false;
    out[length] = '\0';
    return out[0] != '\0';
#endif
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
                 "No app download URL is configured for %s. Pass --app-base-url or set OUTER_SHELL_APP_BASE_URL.",
                 app->display_name);
        return false;
    }

    log_event("Downloading app %s from %s.",
              app->display_name,
              archive_url);

    const char *cache_home = getenv("XDG_CACHE_HOME");
    char cache_root[PATH_MAX];
    if (cache_home && cache_home[0]) {
        snprintf(cache_root, sizeof(cache_root), "%s/outerwebapps/outer-shell/bundled-apps", cache_home);
    } else {
        snprintf(cache_root, sizeof(cache_root), "%s/.cache/outerwebapps/outer-shell/bundled-apps", home_directory());
    }
    if (!mkdir_p(cache_root)) {
        snprintf(message, message_size, "Failed to create app download cache at %s: %s", cache_root, strerror(errno));
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
        log_event("Failed to download app %s.", app->display_name);
        snprintf(message, message_size, "Failed to download %s from %s.",
                 app->display_name, archive_url);
        return false;
    }

    snprintf(out_stage_root, out_stage_root_size, "%s/%s", cache_root, app->stage_directory_name);
    if (!bundled_app_stage_has_expected_files(app, out_stage_root)) {
        log_event("Downloaded app %s, but its payload is incomplete.", app->display_name);
        snprintf(message, message_size, "Downloaded %s, but its payload is incomplete.", app->display_name);
        return false;
    }
    log_event("Downloaded app %s to %s.", app->display_name, out_stage_root);
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
        snprintf(message, message_size, "Missing %s payload at %s.", app->display_name, out_stage_root);
        return false;
    }

    return download_bundled_app_stage(app, out_stage_root, out_stage_root_size, message, message_size);
}

static void default_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERWEBAPPS_REGISTRY");
    if (!env_path || !env_path[0]) {
        env_path = getenv("BACKENDS_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
    char root[PATH_MAX];
    default_user_outerwebapps_root(root, sizeof(root));
    snprintf(out, out_size, "%s/registry.sqlite3", root);
}

static void default_system_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERWEBAPPS_SYSTEM_REGISTRY");
    if (!env_path || !env_path[0]) {
        env_path = getenv("BACKENDS_SYSTEM_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
    snprintf(out, out_size, "%s/registry.orwa", kSystemOuterWebappsRoot);
}

static void default_api_socket_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERSHELLD_API_SOCKET");
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
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

#ifndef __APPLE__
static void root_helper_socket_path_for_uid(uid_t uid, char *out, size_t out_size) {
    snprintf(out, out_size, "/run/outershelld-root-helper-%ld.sock", (long)uid);
}

static void root_helper_unit_name_for_uid(uid_t uid, const char *suffix, char *out, size_t out_size) {
    snprintf(out, out_size, "outershelld-root-helper-%ld.%s", (long)uid, suffix && suffix[0] ? suffix : "service");
}
#endif

static bool ensure_parent_directory(const char *path, char *error, size_t error_size) {
    char directory[PATH_MAX];
    if (!path || !path[0]) {
        snprintf(error, error_size, "path is empty");
        return false;
    }
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (!slash) return true;
    if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    if (!mkdir_p(directory)) {
        snprintf(error, error_size, "failed to create %s: %s", directory, strerror(errno));
        return false;
    }
    return true;
}

static sqlite3 *open_legacy_sqlite_registry_at(const char *path, char *error, size_t error_size) {
    if (!path || !path[0]) {
        snprintf(error, error_size, "registry database path is empty");
        return NULL;
    }
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(path, &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI, NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open registry database");
        if (database) sqlite3_close(database);
        return NULL;
    }
    sqlite3_busy_timeout(database, 5000);
    return database;
}

static bool migrate_sqlite_registry_to_binary_if_needed(const char *sqlite_path, const char *binary_path, char *error, size_t error_size) {
    struct stat binary_stat;
    if (stat(binary_path, &binary_stat) == 0 && S_ISREG(binary_stat.st_mode)) {
        return true;
    }
    if (errno != ENOENT) {
        snprintf(error, error_size, "failed to inspect %s: %s", binary_path, strerror(errno));
        return false;
    }
    struct stat sqlite_stat;
    if (stat(sqlite_path, &sqlite_stat) != 0) {
        if (errno == ENOENT) return true;
        snprintf(error, error_size, "failed to inspect %s: %s", sqlite_path, strerror(errno));
        return false;
    }
    if (!S_ISREG(sqlite_stat.st_mode)) return true;

    sqlite3 *database = open_legacy_sqlite_registry_at(sqlite_path, error, error_size);
    if (!database) return false;
    bool ok = ensure_registry_schema(database, error, error_size) &&
              export_registry_binary_from_sqlite(database, sqlite_path, error, error_size);
    sqlite3_close(database);
    if (ok) {
        log_event("Migrated registry.sqlite3 to registry.orwa at %s.", binary_path);
    }
    return ok;
}

static sqlite3 *open_registry_memory_at(const char *path, bool writable, char *error, size_t error_size) {
    if (!path || !path[0]) {
        snprintf(error, error_size, "registry database path is empty");
        return NULL;
    }
    char binary_path[PATH_MAX];
    if (!registry_binary_output_path(path, binary_path, sizeof(binary_path))) {
        snprintf(error, error_size, "registry path is too long");
        return NULL;
    }
    struct stat binary_stat;
    struct stat sqlite_stat;
    bool have_binary = stat(binary_path, &binary_stat) == 0 && S_ISREG(binary_stat.st_mode);
    bool have_sqlite = stat(path, &sqlite_stat) == 0 && S_ISREG(sqlite_stat.st_mode);
    if (!writable && !have_binary && !have_sqlite) {
        sqlite3 *database = NULL;
        int result = sqlite3_open_v2(":memory:", &database,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
        if (result != SQLITE_OK) {
            snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open in-memory registry database");
            if (database) sqlite3_close(database);
            return NULL;
        }
        if (!ensure_registry_schema(database, error, error_size)) {
            sqlite3_close(database);
            return NULL;
        }
        return database;
    }
    if ((writable || have_sqlite) && !ensure_parent_directory(binary_path, error, error_size)) {
        return NULL;
    }
    if (!migrate_sqlite_registry_to_binary_if_needed(path, binary_path, error, error_size)) {
        return NULL;
    }

    int lock_fd = -1;
    if (writable) {
        lock_fd = registry_binary_lock(binary_path, LOCK_EX, error, error_size);
        if (lock_fd < 0) return NULL;
    }

    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(":memory:", &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open in-memory registry database");
        if (database) sqlite3_close(database);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return NULL;
    }
    sqlite3_busy_timeout(database, 5000);
    bool ok = ensure_registry_schema(database, error, error_size) &&
              import_registry_binary_into_sqlite(database, path, error, error_size);
    if (!ok) {
        sqlite3_close(database);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return NULL;
    }

    if (writable) {
        if (g_registry_write_lock_fd >= 0) {
            snprintf(error, error_size, "registry writer lock is already held");
            sqlite3_close(database);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            return NULL;
        }
        g_registry_write_lock_fd = lock_fd;
        snprintf(g_registry_write_lock_path, sizeof(g_registry_write_lock_path), "%s", binary_path);
    }
    return database;
}

static bool close_registry_readwrite_at(sqlite3 *database, const char *path, bool commit, char *error, size_t error_size) {
    bool ok = true;
    if (database && commit) {
        ok = export_registry_binary_from_sqlite(database, path, error, error_size);
    }
    if (database && sqlite3_close(database) != SQLITE_OK && ok) {
        snprintf(error, error_size, "failed to close registry database");
        ok = false;
    }
    if (g_registry_write_lock_fd >= 0) {
        flock(g_registry_write_lock_fd, LOCK_UN);
        close(g_registry_write_lock_fd);
        g_registry_write_lock_fd = -1;
        g_registry_write_lock_path[0] = '\0';
    }
    return ok;
}

static bool close_registry_readwrite(sqlite3 *database, bool commit, char *error, size_t error_size) {
    return close_registry_readwrite_at(database, g_registry_database_path, commit, error, error_size);
}

static sqlite3 *open_registry_readonly_at(const char *path, char *error, size_t error_size) {
    return open_registry_memory_at(path, false, error, error_size);
}

static sqlite3 *open_registry_readonly(char *error, size_t error_size) {
    return open_registry_readonly_at(g_registry_database_path, error, error_size);
}

static sqlite3 *open_system_registry_readonly(char *error, size_t error_size) {
    return open_registry_readonly_at(g_system_registry_database_path, error, error_size);
}

static sqlite3 *open_registry_readwrite_at(const char *path, char *error, size_t error_size) {
    return open_registry_memory_at(path, true, error, error_size);
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

static bool table_has_column(sqlite3 *database, const char *table_name, const char *name, char *error, size_t error_size) {
    sqlite3_stmt *statement = NULL;
    char sql[160];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
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

static bool table_column_is_primary_key(sqlite3 *database, const char *table_name, const char *name, bool *is_primary_key, char *error, size_t error_size) {
    *is_primary_key = false;
    sqlite3_stmt *statement = NULL;
    char sql[160];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *column_name = (const char *)sqlite3_column_text(statement, 1);
        if (column_name && strcmp(column_name, name) == 0) {
            *is_primary_key = sqlite3_column_int(statement, 5) != 0;
            break;
        }
    }
    sqlite3_finalize(statement);
    return true;
}

static bool frontends_has_column(sqlite3 *database, const char *name, char *error, size_t error_size) {
    return table_has_column(database, "frontends", name, error, error_size);
}

static bool sqlite_table_exists(sqlite3 *database, const char *table_name);

static bool ensure_registry_schema(sqlite3 *database, char *error, size_t error_size) {
    if (!sqlite_exec_ok(database,
                        "CREATE TABLE IF NOT EXISTS backends ("
                        "service_id TEXT PRIMARY KEY,"
                        "display_name TEXT NOT NULL DEFAULT '',"
                        "icon TEXT,"
                        "icon_path TEXT,"
                        "service_unit TEXT"
                        ");"
                        "CREATE TABLE IF NOT EXISTS frontends ("
                        "frontend_id TEXT PRIMARY KEY,"
                        "url TEXT NOT NULL DEFAULT '',"
                        "service_id TEXT,"
                        "display_name TEXT NOT NULL DEFAULT '',"
                        "port INTEGER NOT NULL DEFAULT 0,"
                        "socket_path TEXT NOT NULL DEFAULT '',"
                        "icon TEXT,"
                        "icon_path TEXT,"
                        "list TEXT,"
                        "running INTEGER NOT NULL DEFAULT 1"
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
                        ");"
                        "CREATE TABLE IF NOT EXISTS file_openers ("
                        "extension TEXT NOT NULL,"
                        "service_id TEXT NOT NULL,"
                        "display_name TEXT NOT NULL DEFAULT '',"
                        "socket_path TEXT NOT NULL DEFAULT '',"
                        "url_template TEXT NOT NULL DEFAULT '?file={file}',"
                        "rank INTEGER NOT NULL DEFAULT 0,"
                        "PRIMARY KEY(extension, service_id)"
                        ");",
                        error,
                        error_size)) {
        return false;
    }
    if (!sqlite_exec_ok(database,
                        "CREATE INDEX IF NOT EXISTS file_openers_extension_idx ON file_openers(extension, rank, display_name);"
                        "CREATE INDEX IF NOT EXISTS file_openers_service_id_idx ON file_openers(service_id);",
                        error,
                        error_size)) {
        return false;
    }
    if (!table_has_column(database, "backends", "icon_path", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE backends ADD COLUMN icon_path TEXT;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!sqlite_exec_ok(database,
                        "UPDATE backends SET icon_path = icon "
                        "WHERE (icon_path IS NULL OR icon_path = '') "
                        "AND icon IS NOT NULL AND icon != '' AND substr(icon, 1, 5) != 'data:';",
                        error,
                        error_size)) {
        return false;
    }
    bool has_frontend_display_name_column = frontends_has_column(database, "display_name", error, error_size);
    bool has_frontend_name_column = frontends_has_column(database, "name", error, error_size);
    bool had_frontend_layouts_table = sqlite_table_exists(database, "frontend_layouts");
    if (!has_frontend_display_name_column) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN display_name TEXT NOT NULL DEFAULT '';",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (has_frontend_name_column) {
        if (!sqlite_exec_ok(database,
                            "UPDATE frontends SET display_name = name WHERE display_name = '';",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!frontends_has_column(database, "icon_path", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN icon_path TEXT;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!sqlite_exec_ok(database,
                        "UPDATE frontends SET icon_path = icon "
                        "WHERE (icon_path IS NULL OR icon_path = '') "
                        "AND icon IS NOT NULL AND icon != '' AND substr(icon, 1, 5) != 'data:';",
                        error,
                        error_size)) {
        return false;
    }
    if (!frontends_has_column(database, "list", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN list TEXT;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!frontends_has_column(database, "frontend_id", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN frontend_id TEXT NOT NULL DEFAULT '';"
                            "UPDATE frontends SET frontend_id = COALESCE(NULLIF(service_id, ''), 'app') || ':' || rowid WHERE frontend_id = '';",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!frontends_has_column(database, "running", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontends ADD COLUMN running INTEGER NOT NULL DEFAULT 1;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!sqlite_exec_ok(database,
                        "UPDATE frontends SET frontend_id = COALESCE(NULLIF(service_id, ''), 'app') || ':' || rowid WHERE frontend_id = '';",
                        error,
                        error_size)) {
        return false;
    }
    bool frontend_id_is_primary_key = false;
    if (!table_column_is_primary_key(database, "frontends", "frontend_id", &frontend_id_is_primary_key, error, error_size)) {
        return false;
    }
    if (has_frontend_name_column || !frontend_id_is_primary_key) {
        const char *copy_frontends_sql = has_frontend_name_column
            ? "INSERT OR REPLACE INTO frontends_new(frontend_id, url, service_id, display_name, port, socket_path, icon, icon_path, list, running) "
              "SELECT frontend_id, COALESCE(url, ''), service_id, COALESCE(NULLIF(display_name, ''), name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, icon_path, list, COALESCE(running, 1) FROM frontends;"
            : "INSERT OR REPLACE INTO frontends_new(frontend_id, url, service_id, display_name, port, socket_path, icon, icon_path, list, running) "
              "SELECT frontend_id, COALESCE(url, ''), service_id, COALESCE(display_name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, icon_path, list, COALESCE(running, 1) FROM frontends;";
        if (!sqlite_exec_ok(database,
                            "DROP INDEX IF EXISTS frontends_service_id_idx;"
                            "DROP INDEX IF EXISTS frontends_frontend_id_unique;"
                            "CREATE TABLE frontends_new ("
                            "frontend_id TEXT PRIMARY KEY,"
                            "url TEXT NOT NULL DEFAULT '',"
                            "service_id TEXT,"
                            "display_name TEXT NOT NULL DEFAULT '',"
                            "port INTEGER NOT NULL DEFAULT 0,"
                            "socket_path TEXT NOT NULL DEFAULT '',"
                            "icon TEXT,"
                            "icon_path TEXT,"
                            "list TEXT,"
                            "running INTEGER NOT NULL DEFAULT 1"
                            ");",
                            error,
                            error_size) ||
            !sqlite_exec_ok(database, copy_frontends_sql, error, error_size) ||
            !sqlite_exec_ok(database,
                            "DROP TABLE frontends;"
                            "ALTER TABLE frontends_new RENAME TO frontends;"
                            "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);",
                            error,
                            error_size)) {
            return false;
        }
    } else if (!sqlite_exec_ok(database,
                               "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);",
                               error,
                               error_size)) {
        return false;
    }
    if (!sqlite_exec_ok(database,
                        "CREATE TABLE IF NOT EXISTS frontend_layouts ("
                        "url TEXT PRIMARY KEY,"
                        "list TEXT NOT NULL DEFAULT '',"
                        "frontend_id TEXT"
                        ");",
                        error,
                        error_size)) {
        return false;
    }
    if (!table_has_column(database, "frontend_layouts", "frontend_id", error, error_size)) {
        if (!sqlite_exec_ok(database,
                            "ALTER TABLE frontend_layouts ADD COLUMN frontend_id TEXT;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!sqlite_exec_ok(database,
                        "UPDATE frontend_layouts "
                        "SET frontend_id = (SELECT f.frontend_id FROM frontends f WHERE f.url = frontend_layouts.url LIMIT 1) "
                        "WHERE frontend_id IS NULL OR frontend_id = '';"
                        "CREATE INDEX IF NOT EXISTS frontend_layouts_frontend_id_idx ON frontend_layouts(frontend_id);",
                        error,
                        error_size)) {
        return false;
    }
    if (!had_frontend_layouts_table) {
        if (!sqlite_exec_ok(database,
                            "INSERT OR IGNORE INTO frontend_layouts(url, list) "
                            "SELECT url, COALESCE(list, '') FROM frontends;",
                            error,
                            error_size)) {
            return false;
        }
    }
    if (!sqlite_exec_ok(database,
                        "DELETE FROM frontend_layouts "
                        "WHERE url IN ("
                        "  SELECT raw.url FROM frontends raw "
                        "  JOIN frontends canonical "
                        "    ON canonical.service_id = raw.service_id "
                        "   AND canonical.socket_path = raw.socket_path "
                        "   AND canonical.url = raw.url || '/' "
                        "  WHERE raw.socket_path != '' AND raw.url = raw.socket_path"
                        ");"
                        "UPDATE frontend_layouts "
                        "SET url = url || '/' "
                        "WHERE EXISTS ("
                        "  SELECT 1 FROM frontends f "
                        "  WHERE f.url = frontend_layouts.url "
                        "    AND f.socket_path != '' "
                        "    AND f.url = f.socket_path"
                        ") "
                        "AND NOT EXISTS ("
                        "  SELECT 1 FROM frontend_layouts existing "
                        "  WHERE existing.url = frontend_layouts.url || '/'"
                        ");"
                        "DELETE FROM frontends "
                        "WHERE socket_path != '' "
                        "AND url = socket_path "
                        "AND EXISTS ("
                        "  SELECT 1 FROM frontends canonical "
                        "  WHERE canonical.service_id = frontends.service_id "
                        "    AND canonical.socket_path = frontends.socket_path "
                        "    AND canonical.url = frontends.url || '/'"
                        ");"
                        "UPDATE frontends "
                        "SET url = url || '/' "
                        "WHERE socket_path != '' AND url = socket_path;",
                        error,
                        error_size)) {
        return false;
    }
    return true;
}

static void migrate_registry_schema_if_writable(const char *path) {
    if (!path || !path[0]) return;
    char error[512] = "";
    sqlite3 *database = open_registry_readwrite_at(path, error, sizeof(error));
    if (!database) return;
    bool ok = ensure_registry_schema(database, error, sizeof(error));
    (void)close_registry_readwrite_at(database, path, ok, error, sizeof(error));
}

static bool sqlite_exec_formatted(sqlite3 *database, char *error, size_t error_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *sql = sqlite3_vmprintf(format, args);
    va_end(args);
    if (!sql) {
        snprintf(error, error_size, "Out of memory.");
        return false;
    }
    bool ok = sqlite_exec_ok(database, sql, error, error_size);
    sqlite3_free(sql);
    return ok;
}

static bool sqlite_file_uri_for_readonly_immutable_path(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    size_t offset = 0;
    int written = snprintf(out, out_size, "file:");
    if (written < 0 || (size_t)written >= out_size) return false;
    offset = (size_t)written;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;
        bool literal = (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') ||
                       c == '/' || c == '.' || c == '_' || c == '-' || c == '~';
        if (literal) {
            if (offset + 1 >= out_size) return false;
            out[offset++] = (char)c;
        } else {
            if (offset + 3 >= out_size) return false;
            out[offset++] = '%';
            out[offset++] = hex[(c >> 4) & 0xF];
            out[offset++] = hex[c & 0xF];
        }
    }
    written = snprintf(out + offset, out_size - offset, "?mode=ro&immutable=1");
    return written >= 0 && (size_t)written < out_size - offset;
}

static sqlite3 *open_readonly_immutable_sqlite_database(const char *path, char *error, size_t error_size) {
    char uri[PATH_MAX * 3 + 64];
    if (!sqlite_file_uri_for_readonly_immutable_path(path, uri, sizeof(uri))) {
        snprintf(error, error_size, "Could not build read-only SQLite URI for %s.", path);
        return NULL;
    }
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(uri,
                                 &database,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI,
                                 NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", database ? sqlite3_errmsg(database) : "failed to open source registry database");
        if (database) sqlite3_close(database);
        return NULL;
    }
    return database;
}

static bool copy_registry_rows(sqlite3 *source,
                               sqlite3 *destination,
                               const char *select_sql,
                               const char *insert_sql,
                               char *error,
                               size_t error_size) {
    sqlite3_stmt *select_statement = NULL;
    int result = sqlite3_prepare_v2(source, select_sql, -1, &select_statement, NULL);
    if (result != SQLITE_OK) {
        return true;
    }
    sqlite3_stmt *insert_statement = NULL;
    result = sqlite3_prepare_v2(destination, insert_sql, -1, &insert_statement, NULL);
    if (result != SQLITE_OK) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(destination));
        sqlite3_finalize(select_statement);
        return false;
    }
    bool ok = true;
    int column_count = sqlite3_column_count(select_statement);
    while ((result = sqlite3_step(select_statement)) == SQLITE_ROW) {
        sqlite3_reset(insert_statement);
        sqlite3_clear_bindings(insert_statement);
        for (int i = 0; i < column_count; i++) {
            sqlite3_bind_value(insert_statement, i + 1, sqlite3_column_value(select_statement, i));
        }
        int insert_result = sqlite3_step(insert_statement);
        if (insert_result != SQLITE_DONE) {
            snprintf(error, error_size, "%s", sqlite3_errmsg(destination));
            ok = false;
            break;
        }
    }
    if (ok && result != SQLITE_DONE) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(source));
        ok = false;
    }
    sqlite3_finalize(insert_statement);
    sqlite3_finalize(select_statement);
    return ok;
}

static bool merge_registry_database(const char *old_path,
                                    const char *new_path,
                                    const TextReplacement *replacements,
                                    size_t replacement_count,
                                    char *error,
                                    size_t error_size) {
    if (!old_path || !new_path || strcmp(old_path, new_path) == 0 || access(old_path, R_OK) != 0) {
        return true;
    }
    sqlite3 *database = open_registry_readwrite_at(new_path, error, error_size);
    if (!database) return false;
    sqlite3 *old_database = open_readonly_immutable_sqlite_database(old_path, error, error_size);
    if (!old_database) {
        close_registry_readwrite_at(database, new_path, false, error, error_size);
        return false;
    }
    bool old_frontends_have_display_name = frontends_has_column(old_database, "display_name", error, error_size);
    bool old_frontends_have_name = frontends_has_column(old_database, "name", error, error_size);
    const char *old_frontend_display_name_expression = old_frontends_have_display_name
        ? (old_frontends_have_name ? "COALESCE(NULLIF(display_name, ''), name, '')" : "COALESCE(display_name, '')")
        : (old_frontends_have_name ? "COALESCE(name, '')" : "''");
    char *old_frontends_sql = sqlite3_mprintf("SELECT url, service_id, %s, COALESCE(port, 0), COALESCE(socket_path, ''), icon, CASE WHEN icon IS NOT NULL AND substr(icon, 1, 5) != 'data:' THEN icon ELSE NULL END, list FROM frontends;",
                                             old_frontend_display_name_expression);
    if (!old_frontends_sql) {
        snprintf(error, error_size, "Out of memory.");
        sqlite3_close(old_database);
        close_registry_readwrite_at(database, new_path, false, error, error_size);
        return false;
    }
    bool ok = ensure_registry_schema(database, error, error_size);
    if (ok) ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    "SELECT service_id, COALESCE(display_name, ''), icon, CASE WHEN icon IS NOT NULL AND substr(icon, 1, 5) != 'data:' THEN icon ELSE NULL END, service_unit FROM backends;",
                                    "INSERT OR REPLACE INTO backends(service_id, display_name, icon, icon_path, service_unit) VALUES (?, ?, ?, ?, ?);",
                                    error, error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    old_frontends_sql,
                                    "INSERT OR REPLACE INTO frontends(url, service_id, display_name, port, socket_path, icon, icon_path, list) VALUES (?, ?, ?, ?, ?, ?, ?, ?);",
                                    error, error_size);
    if (ok) ok = sqlite_exec_ok(database,
                                "INSERT OR REPLACE INTO frontend_layouts(url, list) "
                                "SELECT url, COALESCE(list, '') FROM frontends;",
                                error,
                                error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    "SELECT path, service_id FROM log_files;",
                                    "INSERT OR REPLACE INTO log_files(path, service_id) VALUES (?, ?);",
                                    error, error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    "SELECT service_id, unit_name, COALESCE(scope, 'user') FROM systemd_backends;",
                                    "INSERT OR REPLACE INTO systemd_backends(service_id, unit_name, scope) VALUES (?, ?, ?);",
                                    error, error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    "SELECT service_id, plist_path, COALESCE(owns_plist, 0) FROM launchd_backends;",
                                    "INSERT OR REPLACE INTO launchd_backends(service_id, plist_path, owns_plist) VALUES (?, ?, ?);",
                                    error, error_size);
    for (size_t i = 0; ok && i < replacement_count; i++) {
        const char *old_text = replacements[i].old_text;
        const char *new_text = replacements[i].new_text ? replacements[i].new_text : "";
        if (!old_text || !old_text[0]) continue;
        ok = sqlite_exec_formatted(database, error, error_size,
                                   "UPDATE log_files SET path = replace(path, %Q, %Q);"
                                   "UPDATE frontends SET url = replace(url, %Q, %Q), socket_path = replace(socket_path, %Q, %Q);"
                                   "UPDATE launchd_backends SET plist_path = replace(plist_path, %Q, %Q);",
                                   old_text, new_text,
                                   old_text, new_text,
                                   old_text, new_text,
                                   old_text, new_text);
    }
    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_free(old_frontends_sql);
    sqlite3_close(old_database);
    return close_registry_readwrite_at(database, new_path, ok, error, error_size) && ok;
}

static void rename_if_possible(const char *old_path, const char *new_path) {
    if (!old_path || !new_path || access(old_path, F_OK) != 0 || access(new_path, F_OK) == 0) return;
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", new_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        (void)mkdir_p(directory);
    }
    (void)rename(old_path, new_path);
}

static void migrate_user_app_directories(const char *old_apps_root, const char *new_apps_root) {
    DIR *dir = opendir(old_apps_root);
    if (!dir) return;
    (void)mkdir_p(new_apps_root);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, "registry.sqlite3") == 0 ||
            strcmp(entry->d_name, "registry.lock") == 0 ||
            strcmp(entry->d_name, "registry.bin") == 0 ||
            strcmp(entry->d_name, "outerctl") == 0) {
            continue;
        }
        char old_path[PATH_MAX];
        snprintf(old_path, sizeof(old_path), "%s/%s", old_apps_root, entry->d_name);
        struct stat st;
        if (lstat(old_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char new_path[PATH_MAX];
        snprintf(new_path, sizeof(new_path), "%s/%s", new_apps_root, entry->d_name);
        rename_if_possible(old_path, new_path);
        char old_log[PATH_MAX];
        char new_log[PATH_MAX];
        snprintf(old_log, sizeof(old_log), "%s/outeragent.log", access(new_path, F_OK) == 0 ? new_path : old_path);
        snprintf(new_log, sizeof(new_log), "%s/backend.log", access(new_path, F_OK) == 0 ? new_path : old_path);
        rename_if_possible(old_log, new_log);
    }
    closedir(dir);
}

static void migrate_user_outerwebapps_state(void) {
    char old_registry[PATH_MAX];
    char old_apps_root[PATH_MAX];
    char new_apps_root[PATH_MAX];
    char old_outerctl[PATH_MAX];
    char old_home_screen_outerctl[PATH_MAX];
    char new_outerctl[PATH_MAX];
    legacy_user_registry_database_path(old_registry, sizeof(old_registry));
    legacy_user_apps_root(old_apps_root, sizeof(old_apps_root));
    default_user_outerwebapps_apps_root(new_apps_root, sizeof(new_apps_root));
    legacy_user_outerctl_path(old_outerctl, sizeof(old_outerctl));
    legacy_home_screen_outerctl_path(old_home_screen_outerctl, sizeof(old_home_screen_outerctl));
    default_user_outerctl_path(new_outerctl, sizeof(new_outerctl));
    char new_root[PATH_MAX];
    default_user_outerwebapps_root(new_root, sizeof(new_root));
    (void)mkdir_p(new_root);
    (void)mkdir_p(new_apps_root);

    TextReplacement replacements[] = {
        {old_home_screen_outerctl, new_outerctl},
        {old_outerctl, new_outerctl},
        {old_apps_root, new_apps_root},
        {"outeragent.log", "backend.log"},
        {"OUTERAGENT_ROOT", "OUTERWEBAPPS_HOME"},
        {"/var/lib/outergroup/outeragent", kSystemOuterWebappsRoot}
    };

    char error[1024] = "";
    char binary_registry[PATH_MAX] = "";
    bool new_binary_registry_exists = registry_binary_output_path(g_registry_database_path,
                                                                  binary_registry,
                                                                  sizeof(binary_registry)) &&
                                      access(binary_registry, F_OK) == 0;
    if (!new_binary_registry_exists && access(old_registry, R_OK) == 0) {
        if (merge_registry_database(old_registry,
                                    g_registry_database_path,
                                    replacements,
                                    sizeof(replacements) / sizeof(replacements[0]),
                                    error,
                                    sizeof(error))) {
            log_event("Migrated user outerwebapps registry from %s to %s.", old_registry, g_registry_database_path);
        } else {
            log_event("Failed to migrate user registry from %s: %s", old_registry, error);
        }
    }

    migrate_user_app_directories(old_apps_root, new_apps_root);

#ifndef __APPLE__
    char user_units_dir[PATH_MAX];
    snprintf(user_units_dir, sizeof(user_units_dir), "%s/.config/systemd/user", home_directory());
    rewrite_files_in_directory_replacing_text(user_units_dir, replacements, sizeof(replacements) / sizeof(replacements[0]), false);
    run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1 || true");
#else
    char launch_agents_dir[PATH_MAX];
    snprintf(launch_agents_dir, sizeof(launch_agents_dir), "%s/Library/LaunchAgents", home_directory());
    rewrite_files_in_directory_replacing_text(launch_agents_dir, replacements, sizeof(replacements) / sizeof(replacements[0]), false);
#endif
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

enum {
    ORWA_TABLE_BACKENDS = 0,
    ORWA_TABLE_FRONTENDS = 1,
    ORWA_TABLE_FRONTEND_LAYOUTS = 2,
    ORWA_TABLE_LOG_FILES = 3,
    ORWA_TABLE_FILE_OPENERS = 4,
    ORWA_TABLE_COUNT = 5,
    ORWA_LEGACY_FOUR_TABLE_COUNT = 4,
    ORWA_LEGACY_THREE_TABLE_COUNT = 3,
    ORWA_TABLE_DESCRIPTOR_SIZE = 20,
    ORWA_HEADER_SIZE = 8 + ORWA_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_FOUR_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_FOUR_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_THREE_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_THREE_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_BACKENDS_ROW_SIZE = 84,
    ORWA_LEGACY_FRONTENDS_ROW_SIZE = 97,
    ORWA_FRONTENDS_ROW_SIZE = 117,
    ORWA_FRONTEND_LAYOUTS_ROW_SIZE = 32,
    ORWA_LOG_FILES_ROW_SIZE = 32,
    ORWA_FILE_OPENERS_ROW_SIZE = 88
};

typedef struct {
    char *bytes;
    size_t length;
    uint64_t offset;
} RegistryBinaryStringEntry;

typedef struct {
    RegistryBinaryStringEntry *entries;
    size_t count;
    size_t capacity;
    uint64_t variable_base_offset;
} RegistryBinaryStringPool;

typedef struct {
    uint64_t offset;
    uint64_t row_count;
    uint32_t row_size;
} RegistryBinaryTableDescriptor;

static bool binary_append_u32(StringBuilder *builder, uint32_t value) {
    unsigned char bytes[4];
    write_uint32_le(bytes, value);
    return sb_append_n(builder, (const char *)bytes, sizeof(bytes));
}

static bool binary_append_u64(StringBuilder *builder, uint64_t value) {
    unsigned char bytes[8];
    write_uint64_le(bytes, value);
    return sb_append_n(builder, (const char *)bytes, sizeof(bytes));
}

static bool binary_append_string_ref(StringBuilder *builder, uint64_t offset, uint64_t length) {
    return binary_append_u64(builder, offset) && binary_append_u64(builder, length);
}

static void registry_binary_string_pool_free(RegistryBinaryStringPool *pool) {
    if (!pool) return;
    for (size_t i = 0; i < pool->count; i++) {
        free(pool->entries[i].bytes);
    }
    free(pool->entries);
    pool->entries = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

static bool registry_binary_string_ref(RegistryBinaryStringPool *pool,
                                       StringBuilder *variable_region,
                                       const char *text,
                                       uint64_t *offset,
                                       uint64_t *length) {
    if (offset) *offset = 0;
    if (length) *length = 0;
    if (!text || !text[0]) return true;

    size_t text_length = strlen(text);
    for (size_t i = 0; i < pool->count; i++) {
        RegistryBinaryStringEntry *entry = &pool->entries[i];
        if (entry->length == text_length && memcmp(entry->bytes, text, text_length) == 0) {
            if (offset) *offset = entry->offset;
            if (length) *length = (uint64_t)entry->length;
            return true;
        }
    }

    if (pool->count == pool->capacity) {
        size_t new_capacity = pool->capacity ? pool->capacity * 2 : 128;
        RegistryBinaryStringEntry *new_entries = realloc(pool->entries, new_capacity * sizeof(*new_entries));
        if (!new_entries) return false;
        pool->entries = new_entries;
        pool->capacity = new_capacity;
    }

    char *copy = malloc(text_length);
    if (!copy) return false;
    memcpy(copy, text, text_length);

    uint64_t absolute_offset = pool->variable_base_offset + (uint64_t)variable_region->length;
    if (!sb_append_n(variable_region, text, text_length)) {
        free(copy);
        return false;
    }

    RegistryBinaryStringEntry *entry = &pool->entries[pool->count++];
    entry->bytes = copy;
    entry->length = text_length;
    entry->offset = absolute_offset;
    if (offset) *offset = absolute_offset;
    if (length) *length = (uint64_t)text_length;
    return true;
}

static bool registry_binary_append_string_ref(RegistryBinaryStringPool *pool,
                                              StringBuilder *variable_region,
                                              StringBuilder *rows,
                                              const char *text) {
    uint64_t offset = 0;
    uint64_t length = 0;
    return registry_binary_string_ref(pool, variable_region, text, &offset, &length) &&
           binary_append_string_ref(rows, offset, length);
}

static uint64_t registry_binary_count_rows(sqlite3 *database, const char *table_name) {
    if (!sqlite_table_exists(database, table_name)) return 0;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", table_name);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return 0;
    }
    uint64_t count = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        sqlite3_int64 value = sqlite3_column_int64(statement, 0);
        count = value > 0 ? (uint64_t)value : 0;
    }
    sqlite3_finalize(statement);
    return count;
}

static bool registry_binary_output_path(const char *sqlite_path, char *out, size_t out_size) {
    if (!sqlite_path || !sqlite_path[0]) return false;
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", sqlite_path);
    char *slash = strrchr(directory, '/');
    if (!slash) {
        return snprintf(out, out_size, "registry.orwa") > 0;
    }
    if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int written = snprintf(out, out_size, "%s/registry.orwa", directory);
    return written >= 0 && (size_t)written < out_size;
}

static bool registry_binary_lock_path(const char *registry_path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s.lock", registry_path);
    return written >= 0 && (size_t)written < out_size;
}

static int registry_binary_lock(const char *registry_path, int operation, char *error, size_t error_size) {
    char lock_path[PATH_MAX];
    if (!registry_binary_lock_path(registry_path, lock_path, sizeof(lock_path))) {
        snprintf(error, error_size, "Registry lock path is too long.");
        return -1;
    }
    int fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", lock_path, strerror(errno));
        return -1;
    }
    (void)fchmod(fd, 0666);
    if (flock(fd, operation) != 0) {
        snprintf(error, error_size, "Failed to lock %s: %s", lock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool registry_binary_append_query(sqlite3 *database,
                                         const char *sql,
                                         int expected_columns,
                                         bool (*append_row)(sqlite3_stmt *,
                                                            RegistryBinaryStringPool *,
                                                            StringBuilder *,
                                                            StringBuilder *),
                                         RegistryBinaryStringPool *pool,
                                         StringBuilder *variable_region,
                                         StringBuilder *rows,
                                         char *error,
                                         size_t error_size) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        return false;
    }
    if (sqlite3_column_count(statement) != expected_columns) {
        sqlite3_finalize(statement);
        snprintf(error, error_size, "Unexpected column count while exporting registry.");
        return false;
    }
    bool ok = true;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (!append_row(statement, pool, variable_region, rows)) {
            snprintf(error, error_size, "Out of memory while exporting registry.");
            ok = false;
            break;
        }
    }
    if (ok && step_result != SQLITE_DONE) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        ok = false;
    }
    sqlite3_finalize(statement);
    return ok;
}

static bool registry_binary_append_backend_row(sqlite3_stmt *statement,
                                               RegistryBinaryStringPool *pool,
                                               StringBuilder *variable_region,
                                               StringBuilder *rows) {
    uint32_t flags = sqlite3_column_int(statement, 5) != 0 ? 1u : 0u;
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 2)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 3)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 4)) &&
           binary_append_u32(rows, flags);
}

static bool registry_binary_append_frontend_row(sqlite3_stmt *statement,
                                                RegistryBinaryStringPool *pool,
                                                StringBuilder *variable_region,
                                                StringBuilder *rows) {
    const char *socket_path = sqlite_column_text_or_empty(statement, 4);
    uint32_t port = (uint32_t)sqlite3_column_int(statement, 3);
    unsigned char endpoint_kind = socket_path[0] ? 2u : (port ? 1u : 0u);
    unsigned char zero_padding[12] = {0};
    unsigned char empty_payload[16] = {0};
    bool ok = registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
              registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1)) &&
              registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 2)) &&
              registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 5)) &&
              registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 6)) &&
              sb_append_n(rows, (const char *)&endpoint_kind, sizeof(endpoint_kind));
    if (!ok) return false;
    if (endpoint_kind == 2u) {
        ok = registry_binary_append_string_ref(pool, variable_region, rows, socket_path);
    } else if (endpoint_kind == 1u) {
        ok = binary_append_u32(rows, port) &&
             sb_append_n(rows, (const char *)zero_padding, sizeof(zero_padding));
    } else {
        ok = sb_append_n(rows, (const char *)empty_payload, sizeof(empty_payload));
    }
    if (!ok) return false;
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 7)) &&
           binary_append_u32(rows, sqlite3_column_int(statement, 8) ? FRONTEND_FLAG_RUNNING : 0);
}

static bool registry_binary_append_frontend_layout_row(sqlite3_stmt *statement,
                                                       RegistryBinaryStringPool *pool,
                                                       StringBuilder *variable_region,
                                                       StringBuilder *rows) {
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1));
}

static bool registry_binary_append_log_file_row(sqlite3_stmt *statement,
                                                RegistryBinaryStringPool *pool,
                                                StringBuilder *variable_region,
                                                StringBuilder *rows) {
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1));
}

static bool registry_binary_append_file_opener_row(sqlite3_stmt *statement,
                                                   RegistryBinaryStringPool *pool,
                                                   StringBuilder *variable_region,
                                                   StringBuilder *rows) {
    unsigned char padding[4] = {0};
    uint32_t rank = (uint32_t)sqlite3_column_int(statement, 5);
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 2)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 3)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 4)) &&
           binary_append_u32(rows, rank) &&
           sb_append_n(rows, (const char *)padding, sizeof(padding));
}

static bool registry_binary_write_file(const char *path, const void *data, size_t length, char *error, size_t error_size) {
    char temp_path[PATH_MAX];
    bool owns_lock = false;
    int lock_fd = -1;
    if (g_registry_write_lock_fd >= 0 && strcmp(g_registry_write_lock_path, path) == 0) {
        lock_fd = g_registry_write_lock_fd;
    } else {
        lock_fd = registry_binary_lock(path, LOCK_EX, error, error_size);
        if (lock_fd < 0) return false;
        owns_lock = true;
    }

    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        snprintf(error, error_size, "Registry binary path is too long.");
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", temp_path, strerror(errno));
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    if (fchmod(fd, 0644) != 0) {
        snprintf(error, error_size, "Failed to chmod %s: %s", temp_path, strerror(errno));
        close(fd);
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    bool ok = queue_all(fd, data, length);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        snprintf(error, error_size, "Failed to write %s: %s", temp_path, strerror(errno));
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    if (rename(temp_path, path) != 0) {
        snprintf(error, error_size, "Failed to rename %s to %s: %s", temp_path, path, strerror(errno));
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    char directory_path[PATH_MAX];
    snprintf(directory_path, sizeof(directory_path), "%s", path);
    char *slash = strrchr(directory_path, '/');
    if (slash) {
        if (slash == directory_path) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        int dir_fd = open(directory_path, O_RDONLY);
        if (dir_fd >= 0) {
            (void)fsync(dir_fd);
            close(dir_fd);
        }
    }
    if (owns_lock) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return true;
}

static bool registry_binary_read_string(const unsigned char *file,
                                        size_t file_size,
                                        uint64_t variable_offset,
                                        uint64_t offset,
                                        uint64_t length,
                                        char **out,
                                        char *error,
                                        size_t error_size) {
    *out = NULL;
    if (offset == 0 && length == 0) {
        *out = strdup("");
        if (!*out) snprintf(error, error_size, "Out of memory.");
        return *out != NULL;
    }
    if (offset < variable_offset ||
        offset > (uint64_t)file_size ||
        length > (uint64_t)file_size - offset ||
        length > SIZE_MAX - 1) {
        snprintf(error, error_size, "Registry binary string reference is out of bounds.");
        return false;
    }
    char *value = malloc((size_t)length + 1);
    if (!value) {
        snprintf(error, error_size, "Out of memory.");
        return false;
    }
    memcpy(value, file + offset, (size_t)length);
    value[length] = '\0';
    *out = value;
    return true;
}

static bool registry_binary_step(sqlite3 *database, sqlite3_stmt *statement, char *error, size_t error_size) {
    int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) return true;
    snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    return false;
}

static bool registry_binary_import_backend(sqlite3 *database,
                                           const char *service_id,
                                           const char *display_name,
                                           const char *icon_path,
                                           const char *unit_name,
                                           const char *unit_path,
                                           bool owns_unit,
                                           char *error,
                                           size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO backends(service_id, display_name, icon_path, service_unit) VALUES(?, ?, NULLIF(?, ''), NULLIF(?, '')) "
                                 "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon_path=excluded.icon_path, service_unit=excluded.service_unit;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, icon_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, unit_name, -1, SQLITE_TRANSIENT);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    if (!ok) return false;

    if (unit_path && unit_path[0]) {
        ok = sqlite3_prepare_v2(database,
                                "INSERT INTO launchd_backends(service_id, plist_path, owns_plist) VALUES(?, ?, ?) "
                                "ON CONFLICT(service_id) DO UPDATE SET plist_path=excluded.plist_path, owns_plist=excluded.owns_plist;",
                                -1,
                                &statement,
                                NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, unit_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 3, owns_unit ? 1 : 0);
            ok = registry_binary_step(database, statement, error, error_size);
        } else {
            snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        }
        if (statement) sqlite3_finalize(statement);
        return ok;
    }
    if (unit_name && unit_name[0]) {
        ok = sqlite3_prepare_v2(database,
                                "INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, 'user') "
                                "ON CONFLICT(service_id) DO UPDATE SET unit_name=excluded.unit_name;",
                                -1,
                                &statement,
                                NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, unit_name, -1, SQLITE_TRANSIENT);
            ok = registry_binary_step(database, statement, error, error_size);
        } else {
            snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        }
        if (statement) sqlite3_finalize(statement);
    }
    return ok;
}

static bool registry_binary_import_frontend(sqlite3 *database,
                                            const char *frontend_id,
                                            const char *url,
                                            const char *service_id,
                                            const char *display_name,
                                            uint32_t port,
                                            const char *socket_path,
                                            const char *icon_path,
                                            const char *list,
                                            bool running,
                                            char *error,
                                            size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, list, running) VALUES(?, ?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), ?) "
                                 "ON CONFLICT(frontend_id) DO UPDATE SET url=excluded.url, service_id=excluded.service_id, display_name=excluded.display_name, port=excluded.port, socket_path=excluded.socket_path, icon_path=excluded.icon_path, list=excluded.list, running=excluded.running;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, frontend_id && frontend_id[0] ? frontend_id : service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, url, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, (int)port);
        sqlite3_bind_text(statement, 6, socket_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 7, icon_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 8, list, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 9, running ? 1 : 0);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool registry_binary_import_frontend_layout(sqlite3 *database,
                                                   const char *url,
                                                   const char *list,
                                                   char *error,
                                                   size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT OR REPLACE INTO frontend_layouts(url, list) VALUES(?, ?);",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, list ? list : "", -1, SQLITE_TRANSIENT);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool registry_binary_import_log_file(sqlite3 *database,
                                            const char *path,
                                            const char *service_id,
                                            char *error,
                                            size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT OR REPLACE INTO log_files(path, service_id) VALUES(?, ?);",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool registry_binary_import_file_opener(sqlite3 *database,
                                               const char *extension,
                                               const char *service_id,
                                               const char *display_name,
                                               const char *socket_path,
                                               const char *url_template,
                                               uint32_t rank,
                                               char *error,
                                               size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO file_openers(extension, service_id, display_name, socket_path, url_template, rank) VALUES(?, ?, ?, ?, ?, ?) "
                                 "ON CONFLICT(extension, service_id) DO UPDATE SET display_name=excluded.display_name, socket_path=excluded.socket_path, url_template=excluded.url_template, rank=excluded.rank;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, extension, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, socket_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 5, url_template, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 6, (int)rank);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool import_registry_binary_into_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size) {
    char path[PATH_MAX];
    if (!registry_binary_output_path(sqlite_path, path, sizeof(path))) {
        snprintf(error, error_size, "Could not build registry.orwa path.");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return true;
        snprintf(error, error_size, "Failed to inspect %s: %s", path, strerror(errno));
        return false;
    }
    if (!S_ISREG(st.st_mode)) return true;

    size_t file_size = 0;
    char *file_data = read_text_file_alloc(path, &file_size);
    if (!file_data) {
        snprintf(error, error_size, "Failed to read %s.", path);
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)file_data;
    if (file_size < ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ||
        memcmp(file_data, "ORWA", 4) != 0 ||
        read_uint32_le(bytes + 4) != 1) {
        free(file_data);
        snprintf(error, error_size, "Registry binary has an unsupported header.");
        return false;
    }

    RegistryBinaryTableDescriptor descriptors[ORWA_TABLE_COUNT] = {0};
    uint64_t first_table_offset = read_uint64_le(bytes + 8);
    size_t table_count = first_table_offset == ORWA_HEADER_SIZE ? ORWA_TABLE_COUNT :
                         first_table_offset == ORWA_LEGACY_FOUR_TABLE_HEADER_SIZE ? ORWA_LEGACY_FOUR_TABLE_COUNT :
                         first_table_offset == ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ? ORWA_LEGACY_THREE_TABLE_COUNT :
                         0;
    if (table_count == 0) {
        free(file_data);
        snprintf(error, error_size, "Registry binary has an unsupported table layout.");
        return false;
    }
    if (file_size < 8 + table_count * ORWA_TABLE_DESCRIPTOR_SIZE) {
        free(file_data);
        snprintf(error, error_size, "Registry binary table descriptors are truncated.");
        return false;
    }
    uint64_t variable_offset = 0;
    for (size_t i = 0; i < table_count; i++) {
        size_t descriptor_offset = 8 + i * ORWA_TABLE_DESCRIPTOR_SIZE;
        descriptors[i].offset = read_uint64_le(bytes + descriptor_offset);
        descriptors[i].row_count = read_uint64_le(bytes + descriptor_offset + 8);
        descriptors[i].row_size = read_uint32_le(bytes + descriptor_offset + 16);
        uint32_t expected_row_size = i == ORWA_TABLE_BACKENDS ? ORWA_BACKENDS_ROW_SIZE :
                                     i == ORWA_TABLE_FRONTENDS ? descriptors[i].row_size :
                                     i == ORWA_TABLE_FRONTEND_LAYOUTS && table_count != ORWA_LEGACY_THREE_TABLE_COUNT ? ORWA_FRONTEND_LAYOUTS_ROW_SIZE :
                                     i == ORWA_TABLE_FILE_OPENERS ? ORWA_FILE_OPENERS_ROW_SIZE :
                                     ORWA_LOG_FILES_ROW_SIZE;
        if ((i == ORWA_TABLE_FRONTENDS
                ? (descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE && descriptors[i].row_size != ORWA_LEGACY_FRONTENDS_ROW_SIZE)
                : descriptors[i].row_size != expected_row_size) ||
            descriptors[i].offset > (uint64_t)file_size ||
            descriptors[i].row_count > UINT64_MAX / descriptors[i].row_size ||
            descriptors[i].row_count * descriptors[i].row_size > (uint64_t)file_size - descriptors[i].offset) {
            free(file_data);
            snprintf(error, error_size, "Registry binary table descriptor is invalid.");
            return false;
        }
        uint64_t table_end = descriptors[i].offset + descriptors[i].row_count * descriptors[i].row_size;
        if (table_end > variable_offset) variable_offset = table_end;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size) &&
              sqlite_exec_ok(database, "DELETE FROM file_openers; DELETE FROM frontend_layouts; DELETE FROM frontends; DELETE FROM log_files; DELETE FROM systemd_backends; DELETE FROM launchd_backends; DELETE FROM backends;", error, error_size);

    for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_BACKENDS].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_BACKENDS].offset + row * ORWA_BACKENDS_ROW_SIZE;
        char *service_id = NULL, *display_name = NULL, *icon_path = NULL, *unit_name = NULL, *unit_path = NULL;
        ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &service_id, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &display_name, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 32), read_uint64_le(row_bytes + 40), &icon_path, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 48), read_uint64_le(row_bytes + 56), &unit_name, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 64), read_uint64_le(row_bytes + 72), &unit_path, error, error_size);
        if (ok) {
            bool owns_unit = (read_uint32_le(row_bytes + 80) & 1u) != 0;
            ok = registry_binary_import_backend(database, service_id, display_name, icon_path, unit_name, unit_path, owns_unit, error, error_size);
        }
        free(service_id);
        free(display_name);
        free(icon_path);
        free(unit_name);
        free(unit_path);
    }

    for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FRONTENDS].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FRONTENDS].offset + row * descriptors[ORWA_TABLE_FRONTENDS].row_size;
        char *url = NULL, *service_id = NULL, *display_name = NULL, *icon_path = NULL, *list = NULL, *socket_path = NULL, *frontend_id = NULL;
        uint32_t port = 0;
        uint32_t flags = FRONTEND_FLAG_RUNNING;
        ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &url, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &service_id, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 32), read_uint64_le(row_bytes + 40), &display_name, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 48), read_uint64_le(row_bytes + 56), &icon_path, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 64), read_uint64_le(row_bytes + 72), &list, error, error_size);
        if (ok) {
            uint8_t endpoint_kind = row_bytes[80];
            if (endpoint_kind == 1u) {
                port = read_uint32_le(row_bytes + 81);
                socket_path = strdup("");
                if (!socket_path) {
                    snprintf(error, error_size, "Out of memory.");
                    ok = false;
                }
            } else if (endpoint_kind == 2u) {
                ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 81), read_uint64_le(row_bytes + 89), &socket_path, error, error_size);
            } else if (endpoint_kind == 0u) {
                socket_path = strdup("");
                if (!socket_path) {
                    snprintf(error, error_size, "Out of memory.");
                    ok = false;
                }
            } else {
                snprintf(error, error_size, "Registry binary frontend endpoint kind is unsupported.");
                ok = false;
            }
        }
        if (ok && descriptors[ORWA_TABLE_FRONTENDS].row_size >= ORWA_FRONTENDS_ROW_SIZE) {
            ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 97), read_uint64_le(row_bytes + 105), &frontend_id, error, error_size);
            flags = read_uint32_le(row_bytes + 113);
        } else if (ok) {
            size_t needed = strlen(service_id ? service_id : "") + strlen(url ? url : "") + 2;
            frontend_id = (char *)malloc(needed);
            if (frontend_id) {
                snprintf(frontend_id, needed, "%s:%s", service_id ? service_id : "app", url && url[0] ? url : "main");
            } else {
                snprintf(error, error_size, "Out of memory.");
                ok = false;
            }
        }
        if (ok) {
            ok = registry_binary_import_frontend(database, frontend_id, url, service_id, display_name, port, socket_path, icon_path, list, (flags & FRONTEND_FLAG_RUNNING) != 0, error, error_size);
        }
        if (ok && table_count == ORWA_LEGACY_THREE_TABLE_COUNT) {
            ok = registry_binary_import_frontend_layout(database, url, list, error, error_size);
        }
        free(url);
        free(service_id);
        free(display_name);
        free(icon_path);
        free(list);
        free(socket_path);
        free(frontend_id);
    }

    if (table_count != ORWA_LEGACY_THREE_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FRONTEND_LAYOUTS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FRONTEND_LAYOUTS].offset + row * ORWA_FRONTEND_LAYOUTS_ROW_SIZE;
            char *url = NULL, *list = NULL;
            ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &url, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &list, error, error_size);
            if (ok) {
                ok = registry_binary_import_frontend_layout(database, url, list, error, error_size);
            }
            free(url);
            free(list);
        }
    }

    size_t log_table_index = table_count == ORWA_LEGACY_THREE_TABLE_COUNT ? 2 : ORWA_TABLE_LOG_FILES;
    for (uint64_t row = 0; ok && row < descriptors[log_table_index].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[log_table_index].offset + row * ORWA_LOG_FILES_ROW_SIZE;
        char *path_value = NULL, *service_id = NULL;
        ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &path_value, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &service_id, error, error_size);
        if (ok) {
            ok = registry_binary_import_log_file(database, path_value, service_id, error, error_size);
        }
        free(path_value);
        free(service_id);
    }

    if (table_count == ORWA_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FILE_OPENERS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FILE_OPENERS].offset + row * ORWA_FILE_OPENERS_ROW_SIZE;
            char *extension = NULL, *service_id = NULL, *display_name = NULL, *socket_path = NULL, *url_template = NULL;
            ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &extension, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &service_id, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 32), read_uint64_le(row_bytes + 40), &display_name, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 48), read_uint64_le(row_bytes + 56), &socket_path, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 64), read_uint64_le(row_bytes + 72), &url_template, error, error_size);
            if (ok) {
                ok = registry_binary_import_file_opener(database,
                                                        extension,
                                                        service_id,
                                                        display_name,
                                                        socket_path,
                                                        url_template,
                                                        read_uint32_le(row_bytes + 80),
                                                        error,
                                                        error_size);
            }
            free(extension);
            free(service_id);
            free(display_name);
            free(socket_path);
            free(url_template);
        }
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    free(file_data);
    return ok;
}

static bool export_registry_binary_from_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size) {
    if (!database || !sqlite_path || !sqlite_path[0]) return false;

    RegistryBinaryTableDescriptor descriptors[ORWA_TABLE_COUNT] = {
        {.row_count = registry_binary_count_rows(database, "backends"), .row_size = ORWA_BACKENDS_ROW_SIZE},
        {.row_count = registry_binary_count_rows(database, "frontends"), .row_size = ORWA_FRONTENDS_ROW_SIZE},
        {.row_count = registry_binary_count_rows(database, "frontend_layouts"), .row_size = ORWA_FRONTEND_LAYOUTS_ROW_SIZE},
        {.row_count = registry_binary_count_rows(database, "log_files"), .row_size = ORWA_LOG_FILES_ROW_SIZE},
        {.row_count = registry_binary_count_rows(database, "file_openers"), .row_size = ORWA_FILE_OPENERS_ROW_SIZE},
    };

    uint64_t offset = ORWA_HEADER_SIZE;
    for (size_t i = 0; i < ORWA_TABLE_COUNT; i++) {
        descriptors[i].offset = offset;
        offset += descriptors[i].row_count * descriptors[i].row_size;
    }
    uint64_t variable_region_offset = offset;

    StringBuilder rows = {0};
    StringBuilder variable_region = {0};
    RegistryBinaryStringPool pool = {.variable_base_offset = variable_region_offset};
    bool ok = true;

    if (ok && descriptors[ORWA_TABLE_BACKENDS].row_count > 0) {
        bool has_systemd_table = sqlite_table_exists(database, "systemd_backends");
        bool has_launchd_table = sqlite_table_exists(database, "launchd_backends");
        const char *systemd_join = has_systemd_table ? "LEFT JOIN systemd_backends s ON s.service_id = b.service_id" : "";
        const char *launchd_join = has_launchd_table ? "LEFT JOIN launchd_backends l ON l.service_id = b.service_id" : "";
        const char *unit_name_expression = "COALESCE(NULLIF(b.service_unit, ''), '')";
        const char *unit_path_expression = "''";
        const char *owns_unit_expression = "CASE WHEN COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        if (has_systemd_table && has_launchd_table) {
            unit_name_expression = "CASE WHEN COALESCE(l.plist_path, '') != '' THEN b.service_id ELSE COALESCE(NULLIF(s.unit_name, ''), NULLIF(b.service_unit, ''), '') END";
            unit_path_expression = "COALESCE(l.plist_path, '')";
            owns_unit_expression = "CASE WHEN COALESCE(l.plist_path, '') != '' THEN COALESCE(l.owns_plist, 0) WHEN COALESCE(s.unit_name, '') != '' OR COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        } else if (has_systemd_table) {
            unit_name_expression = "COALESCE(NULLIF(s.unit_name, ''), NULLIF(b.service_unit, ''), '')";
            owns_unit_expression = "CASE WHEN COALESCE(s.unit_name, '') != '' OR COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        } else if (has_launchd_table) {
            unit_name_expression = "CASE WHEN COALESCE(l.plist_path, '') != '' THEN b.service_id ELSE COALESCE(NULLIF(b.service_unit, ''), '') END";
            unit_path_expression = "COALESCE(l.plist_path, '')";
            owns_unit_expression = "CASE WHEN COALESCE(l.plist_path, '') != '' THEN COALESCE(l.owns_plist, 0) WHEN COALESCE(b.service_unit, '') != '' THEN 1 ELSE 0 END";
        }
        char *sql = sqlite3_mprintf("SELECT b.service_id, COALESCE(b.display_name, ''), COALESCE(b.icon_path, ''), %s, %s, %s FROM backends b %s %s ORDER BY b.service_id;",
                                    unit_name_expression,
                                    unit_path_expression,
                                    owns_unit_expression,
                                    systemd_join,
                                    launchd_join);
        if (!sql) {
            snprintf(error, error_size, "Out of memory while exporting registry.");
            ok = false;
        }
        if (ok) {
            ok = registry_binary_append_query(database,
                                              sql,
                                              6,
                                              registry_binary_append_backend_row,
                                              &pool,
                                              &variable_region,
                                              &rows,
                                              error,
                                              error_size);
        }
        sqlite3_free(sql);
    }
    if (ok && descriptors[ORWA_TABLE_FRONTENDS].row_count > 0) {
        ok = registry_binary_append_query(database,
                                          "SELECT url, COALESCE(service_id, ''), COALESCE(display_name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), COALESCE(icon_path, ''), COALESCE(list, ''), COALESCE(frontend_id, ''), COALESCE(running, 0) FROM frontends ORDER BY service_id, COALESCE(list, ''), display_name, url;",
                                          9,
                                          registry_binary_append_frontend_row,
                                          &pool,
                                          &variable_region,
                                          &rows,
                                          error,
                                          error_size);
    }
    if (ok && descriptors[ORWA_TABLE_FRONTEND_LAYOUTS].row_count > 0) {
        ok = registry_binary_append_query(database,
                                          "SELECT url, COALESCE(list, '') FROM frontend_layouts ORDER BY url;",
                                          2,
                                          registry_binary_append_frontend_layout_row,
                                          &pool,
                                          &variable_region,
                                          &rows,
                                          error,
                                          error_size);
    }
    if (ok && descriptors[ORWA_TABLE_LOG_FILES].row_count > 0) {
        ok = registry_binary_append_query(database,
                                          "SELECT path, service_id FROM log_files ORDER BY service_id, path;",
                                          2,
                                          registry_binary_append_log_file_row,
                                          &pool,
                                          &variable_region,
                                          &rows,
                                          error,
                                          error_size);
    }
    if (ok && descriptors[ORWA_TABLE_FILE_OPENERS].row_count > 0) {
        ok = registry_binary_append_query(database,
                                          "SELECT extension, service_id, COALESCE(display_name, ''), COALESCE(socket_path, ''), COALESCE(url_template, ''), COALESCE(rank, 0) FROM file_openers ORDER BY extension, rank, display_name, service_id;",
                                          6,
                                          registry_binary_append_file_opener_row,
                                          &pool,
                                          &variable_region,
                                          &rows,
                                          error,
                                          error_size);
    }
    uint64_t expected_rows_length = variable_region_offset - ORWA_HEADER_SIZE;
    if (ok && rows.length != expected_rows_length) {
        snprintf(error, error_size, "Registry binary row length mismatch.");
        ok = false;
    }

    StringBuilder file = {0};
    if (ok) {
        ok = sb_append_n(&file, "ORWA", 4) &&
             binary_append_u32(&file, 1);
    }
    for (size_t i = 0; ok && i < ORWA_TABLE_COUNT; i++) {
        ok = binary_append_u64(&file, descriptors[i].offset) &&
             binary_append_u64(&file, descriptors[i].row_count) &&
             binary_append_u32(&file, descriptors[i].row_size);
    }
    if (ok && file.length != ORWA_HEADER_SIZE) {
        snprintf(error, error_size, "Registry binary header length mismatch.");
        ok = false;
    }
    if (ok) {
        ok = sb_append_n(&file, rows.data ? rows.data : "", rows.length) &&
             sb_append_n(&file, variable_region.data ? variable_region.data : "", variable_region.length);
    }

    if (ok) {
        char output_path[PATH_MAX];
        ok = registry_binary_output_path(sqlite_path, output_path, sizeof(output_path));
        if (!ok) {
            snprintf(error, error_size, "Could not build registry.orwa path.");
        } else {
            ok = registry_binary_write_file(output_path, file.data, file.length, error, error_size);
        }
    }

    registry_binary_string_pool_free(&pool);
    free(rows.data);
    free(variable_region.data);
    free(file.data);
    return ok;
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
        if (found) {
#ifndef __APPLE__
            snprintf(scope, scope_size, "system");
#endif
            return true;
        }
    }
    return false;
}

static bool lookup_systemd_backend_any_for_scope(const char *service_id,
                                                 const char *requested_scope,
                                                 char *unit_name,
                                                 size_t unit_name_size,
                                                 char *scope,
                                                 size_t scope_size) {
    bool wants_system = requested_scope && strcmp(requested_scope, "system") == 0;
    bool wants_user = requested_scope && strcmp(requested_scope, "user") == 0;
    char error[512] = "";

    if (!wants_system) {
        sqlite3 *database = open_registry_readonly(error, sizeof(error));
        if (database) {
            bool found = lookup_systemd_backend(database, service_id, unit_name, unit_name_size, scope, scope_size);
            sqlite3_close(database);
            if (found) return true;
        }
    }

    if (!wants_user) {
        sqlite3 *database = open_system_registry_readonly(error, sizeof(error));
        if (database) {
            bool found = lookup_systemd_backend(database, service_id, unit_name, unit_name_size, scope, scope_size);
            sqlite3_close(database);
            if (found) {
#ifndef __APPLE__
                snprintf(scope, scope_size, "system");
#endif
                return true;
            }
        }
    }

    return false;
}

#ifndef __APPLE__
static bool system_registry_has_backend(const char *service_id, bool *exists, char *error, size_t error_size) {
    if (exists) *exists = false;
    if (!service_id || !service_id[0] || !g_system_registry_database_path[0] ||
        !registry_storage_exists_at(g_system_registry_database_path)) {
        return true;
    }

    sqlite3 *database = open_system_registry_readonly(error, error_size);
    if (!database) return false;
    if (!sqlite_table_exists(database, "backends")) {
        sqlite3_close(database);
        return true;
    }

    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, "SELECT 1 FROM backends WHERE service_id = ? LIMIT 1;", -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        if (exists) *exists = sqlite3_step(statement) == SQLITE_ROW;
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    sqlite3_close(database);
    return ok;
}
#endif

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

#define BACKEND_FLAG_CAN_CONTROL 0x01u
#define BACKEND_FLAG_CAN_UNINSTALL 0x02u
#define BACKEND_FLAG_IS_BUNDLED 0x04u
#define BACKEND_FLAG_IS_INSTALLED 0x08u
#define BACKEND_FLAG_IS_MIGRATION 0x10u
#define BACKEND_FLAG_OWNS_LAUNCHD_PLIST 0x20u
#define BACKEND_FLAG_SUPPORTS_ROOT 0x40u
#define BACKEND_FLAG_ROOT_ONLY 0x80u
#define BACKEND_FLAG_HAS_ROOT_SUPPORT 0x100u
#define LOG_FILE_FLAG_READABLE 0x01u
#define FILE_PICKER_FLAG_IS_DIRECTORY 0x01u
#define ACTION_FLAG_OK 0x01u
#define ACTION_FLAG_NEEDS_PASSWORD 0x02u

static bool root_outerwebapps_migration_pending(void);
static void trim_whitespace_in_place(char *value);
static bool installed_home_screen_version(char *out, size_t out_size);
static bool fetch_home_screen_available_version(const char *heartbeat, char *out, size_t out_size, char *message, size_t message_size);
static void mark_update_check_completed(void);
static int compare_versions(const char *installed, const char *available);
static bool home_screen_base_url(char *out, size_t out_size);
static bool home_screen_update_available(char *available, size_t available_size);
static bool run_home_screen_install_script(const char *subcommand, char *message, size_t message_size);
static bool uninstall_local_home_screen(char *message, size_t message_size);

static bool build_frontend_payload(const char *name,
                                   const char *frontend_id,
                                   const char *url,
                                   int port,
                                   const char *socket_path,
                                   const char *icon_path,
                                   const char *list,
                                   bool running,
                                   StringBuilder *payload) {
    if (!binary_append_zero(payload, 64)) return false;
    return binary_append_string_ref_at(payload, 0, name) &&
           binary_append_string_ref_at(payload, 8, url) &&
           binary_append_string_ref_at(payload, 16, socket_path) &&
           binary_append_string_ref_at(payload, 24, icon_path) &&
           binary_append_file_ref_at(payload, 32, icon_path) &&
           binary_append_string_ref_at(payload, 40, list) &&
           binary_write_u32_at(payload, 48, (uint32_t)(port < 0 ? 0 : port)) &&
           binary_append_string_ref_at(payload, 52, frontend_id) &&
           binary_write_u32_at(payload, 60, running ? FRONTEND_FLAG_RUNNING : 0);
}

static bool build_log_file_payload(const char *service_id,
                                   const char *path,
                                   int index,
                                   StringBuilder *payload) {
    char expanded[PATH_MAX];
    expand_tilde_path(path, expanded, sizeof(expanded));
    struct stat st;
    bool has_stat = stat(expanded, &st) == 0;

    char identifier[PATH_MAX + 80];
    snprintf(identifier, sizeof(identifier), "backend-log-file:%s:%d", service_id, index);
    const char *last_slash = strrchr(path, '/');
    const char *display_name = last_slash && last_slash[1] ? last_slash + 1 : path;

    if (!binary_append_zero(payload, 44)) return false;
    return binary_append_string_ref_at(payload, 0, identifier) &&
           binary_append_string_ref_at(payload, 8, display_name) &&
           binary_append_string_ref_at(payload, 16, path) &&
           binary_write_u64_at(payload, 24, has_stat ? (uint64_t)st.st_size : 0) &&
           binary_write_f64_at(payload, 32, has_stat ? (double)st.st_mtime : 0.0) &&
           binary_write_u32_at(payload, 40, has_stat ? LOG_FILE_FLAG_READABLE : 0);
}

static bool lookup_frontend_layout(sqlite3 *database,
                                   const char *frontend_id,
                                   const char *url,
                                   char *list,
                                   size_t list_size,
                                   bool *found) {
    if (found) *found = false;
    if (list && list_size) list[0] = '\0';
    if (!database || !sqlite_table_exists(database, "frontend_layouts")) return true;
    sqlite3_stmt *statement = NULL;
    const char *sql = (frontend_id && frontend_id[0])
        ? "SELECT list FROM frontend_layouts WHERE frontend_id = ? LIMIT 1;"
        : "SELECT list FROM frontend_layouts WHERE url = ? LIMIT 1;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, (frontend_id && frontend_id[0]) ? frontend_id : (url ? url : ""), -1, SQLITE_TRANSIENT);
    bool has_row = sqlite3_step(statement) == SQLITE_ROW;
    if (has_row) {
        if (found) *found = true;
        snprintf(list, list_size, "%s", sqlite_column_text_or_empty(statement, 0));
    }
    sqlite3_finalize(statement);
    if (!has_row && (!frontend_id || !frontend_id[0]) && url && url[0]) {
        size_t length = strlen(url);
        if (length > 1 && url[length - 1] == '/') {
            char without_trailing_slash[PATH_MAX];
            if (length < sizeof(without_trailing_slash)) {
                memcpy(without_trailing_slash, url, length - 1);
                without_trailing_slash[length - 1] = '\0';
                statement = NULL;
                if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
                    return false;
                }
                sqlite3_bind_text(statement, 1, without_trailing_slash, -1, SQLITE_TRANSIENT);
                has_row = sqlite3_step(statement) == SQLITE_ROW;
                if (has_row) {
                    if (found) *found = true;
                    snprintf(list, list_size, "%s", sqlite_column_text_or_empty(statement, 0));
                }
                sqlite3_finalize(statement);
            }
        }
    }
    return true;
}

static bool build_frontends_array_payload(sqlite3 *database, sqlite3 *layout_database, const char *service_id, StringBuilder *out) {
    BinaryPayloadList list = {0};
    sqlite3_stmt *statement = NULL;
    char column_error[256] = "";
    bool has_list_column = frontends_has_column(database, "list", column_error, sizeof(column_error));
    const char *sql = has_list_column ?
        "SELECT f.display_name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), COALESCE(NULLIF(f.icon_path, ''), COALESCE(b.icon_path, '')), COALESCE(f.list, ''), COALESCE(f.frontend_id, ''), COALESCE(f.running, 0) "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY COALESCE(f.list, ''), f.display_name, f.url;" :
        "SELECT f.display_name, COALESCE(f.url, ''), COALESCE(f.port, 0), COALESCE(f.socket_path, ''), COALESCE(NULLIF(f.icon_path, ''), COALESCE(b.icon_path, '')), '', COALESCE(f.frontend_id, ''), COALESCE(f.running, 0) "
        "FROM frontends f LEFT JOIN backends b ON b.service_id = f.service_id WHERE f.service_id = ? "
        "ORDER BY f.display_name, f.url;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);

    bool ok = true;
    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        StringBuilder payload = {0};
        const char *url = sqlite_column_text_or_empty(statement, 1);
        const char *frontend_id = sqlite_column_text_or_empty(statement, 6);
        const char *suggested_list = sqlite_column_text_or_empty(statement, 5);
        char layout_list[PATH_MAX] = "";
        bool has_layout = false;
        ok = lookup_frontend_layout(layout_database ? layout_database : database,
                                    frontend_id,
                                    url,
                                    layout_list,
                                    sizeof(layout_list),
                                    &has_layout);
        if (!ok) {
            free(payload.data);
            break;
        }
        ok = build_frontend_payload(sqlite_column_text_or_empty(statement, 0),
                                    frontend_id,
                                    url,
                                    sqlite3_column_int(statement, 2),
                                    sqlite_column_text_or_empty(statement, 3),
                                    sqlite_column_text_or_empty(statement, 4),
                                    has_layout ? layout_list : suggested_list,
                                    sqlite3_column_int(statement, 7) != 0,
                                    &payload) &&
             binary_payload_list_append(&list, &payload);
        if (!ok) free(payload.data);
    }
    sqlite3_finalize(statement);
    if (ok) ok = binary_build_payload_array(&list, out);
    binary_payload_list_free(&list);
    return ok;
}

static bool build_log_files_array_payload(sqlite3 *database, const char *service_id, StringBuilder *out) {
    BinaryPayloadList list = {0};
    sqlite3_stmt *statement = NULL;
    const char *sql = "SELECT path FROM log_files WHERE service_id = ? ORDER BY path;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);

    bool ok = true;
    int index = 0;
    while (ok && sqlite3_step(statement) == SQLITE_ROW) {
        StringBuilder payload = {0};
        ok = build_log_file_payload(service_id, sqlite_column_text_or_empty(statement, 0), index, &payload) &&
             binary_payload_list_append(&list, &payload);
        if (!ok) free(payload.data);
        index++;
    }
    sqlite3_finalize(statement);
    if (ok) ok = binary_build_payload_array(&list, out);
    binary_payload_list_free(&list);
    return ok;
}

static bool build_empty_array_payload(StringBuilder *out) {
    return binary_append_zero(out, 4) && binary_write_u32_at(out, 0, 0);
}

static bool build_backend_payload(const char *service_id,
                                  const char *display_name,
                                  const char *service_unit,
                                  const char *service_unit_path,
                                  const char *service_scope,
                                  const char *status,
                                  uint32_t flags,
                                  const char *icon_symbol_name,
                                  const char *launchd_plist_path,
                                  StringBuilder *frontends_array,
                                  StringBuilder *log_files_array,
                                  StringBuilder *payload) {
    if (!binary_append_zero(payload, 84)) return false;
    return binary_append_string_ref_at(payload, 0, service_id) &&
           binary_append_string_ref_at(payload, 8, display_name && display_name[0] ? display_name : service_id) &&
           binary_append_string_ref_at(payload, 16, service_unit) &&
           binary_append_string_ref_at(payload, 24, service_unit_path) &&
           binary_append_string_ref_at(payload, 32, service_scope) &&
           binary_append_string_ref_at(payload, 40, status) &&
           binary_append_string_ref_at(payload, 48, icon_symbol_name) &&
           binary_append_string_ref_at(payload, 56, launchd_plist_path) &&
           binary_write_u32_at(payload, 64, flags) &&
           binary_append_child_ref_at(payload, 68, frontends_array) &&
           binary_append_child_ref_at(payload, 76, log_files_array);
}

static bool append_registered_backend_payloads(sqlite3 *database,
                                               sqlite3 *layout_database,
                                               BinaryPayloadList *payloads,
                                               bool *bundled_installed,
                                               size_t bundled_installed_count,
                                               const char *registry_scope) {
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
#ifndef __APPLE__
        if (registry_scope && strcmp(registry_scope, "system") == 0) {
            snprintf(effective_service_scope, sizeof(effective_service_scope), "system");
        }
#endif
        const BundledAppDefinition *bundled_app = bundled_app_for_service_id(service_id);
        bool is_self = is_home_screen_service_id(service_id);
        if (bundled_app) {
            size_t bundled_index = (size_t)(bundled_app - kBundledApps);
            if (bundled_index < bundled_installed_count) bundled_installed[bundled_index] = true;
        }

        char status[32] = "unknown";
        bool has_launchd_unit = plist_path[0] && has_launchd_table;
        bool has_systemd_unit = service_unit[0] && has_systemd_table;
#ifdef __APPLE__
        if (has_launchd_unit) has_systemd_unit = false;
        if (has_launchd_unit) {
            snprintf(effective_service_scope,
                     sizeof(effective_service_scope),
                     "%s",
                     strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0 ? "system" : "user");
            launchd_status(service_id, plist_path, status, sizeof(status));
        } else
#endif
        if (has_systemd_unit) {
            systemd_status(service_unit, effective_service_scope, status, sizeof(status));
        }

        char service_unit_path[PATH_MAX] = "";
#ifdef __APPLE__
        if (plist_path[0]) {
            snprintf(service_unit_path, sizeof(service_unit_path), "%s", plist_path);
        } else
#endif
        if (service_unit[0]) {
            systemd_unit_path(service_unit, effective_service_scope, service_unit_path, sizeof(service_unit_path));
        } else if (plist_path[0]) {
            snprintf(service_unit_path, sizeof(service_unit_path), "%s", plist_path);
        }

        uint32_t flags = BACKEND_FLAG_IS_INSTALLED;
        if ((has_systemd_unit || has_launchd_unit) && !is_self) flags |= BACKEND_FLAG_CAN_CONTROL | BACKEND_FLAG_CAN_UNINSTALL;
        if (is_self) {
            flags |= BACKEND_FLAG_CAN_UNINSTALL;
            char available_version[128] = "";
            if (home_screen_update_available(available_version, sizeof(available_version))) {
                snprintf(status, sizeof(status), "update available");
            }
        }
        if (bundled_app) flags |= BACKEND_FLAG_IS_BUNDLED;
        if (bundled_app && bundled_app->supports_root) flags |= BACKEND_FLAG_SUPPORTS_ROOT;
        if (bundled_app && bundled_app->root_only) flags |= BACKEND_FLAG_ROOT_ONLY;
#ifndef __APPLE__
        if (bundled_app && bundled_app->supports_root) {
            bool has_root_support = registry_scope && strcmp(registry_scope, "system") == 0;
            if (!has_root_support) {
                char ignored_error[256] = "";
                (void)system_registry_has_backend(service_id, &has_root_support, ignored_error, sizeof(ignored_error));
            }
            if (has_root_support) flags |= BACKEND_FLAG_HAS_ROOT_SUPPORT;
        }
#endif
        if (owns_plist) flags |= BACKEND_FLAG_OWNS_LAUNCHD_PLIST;

        StringBuilder frontends = {0};
        StringBuilder logs = {0};
        StringBuilder payload = {0};
        ok = build_frontends_array_payload(database, layout_database ? layout_database : database, service_id, &frontends) &&
             build_log_files_array_payload(database, service_id, &logs) &&
             build_backend_payload(service_id, display_name, service_unit, service_unit_path,
                                   effective_service_scope, status, flags, "",
                                   plist_path, &frontends, &logs, &payload) &&
             binary_payload_list_append(payloads, &payload);
        free(frontends.data);
        free(logs.data);
        if (!ok) free(payload.data);
    }
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool append_root_migration_backend_payload(BinaryPayloadList *payloads) {
    if (!root_outerwebapps_migration_pending()) return true;
    StringBuilder frontends = {0};
    StringBuilder logs = {0};
    StringBuilder payload = {0};
#ifdef __APPLE__
    const char *path = "/Library/dev.outergroup.OuterLoop";
#else
    const char *path = "/var/lib/outergroup/outeragent";
#endif
    bool ok = build_empty_array_payload(&frontends) &&
              build_empty_array_payload(&logs) &&
              build_backend_payload(kMigrationServiceID,
                                    "Migrate old root services",
                                    "",
                                    path,
                                    "system",
                                    "pending",
                                    BACKEND_FLAG_CAN_CONTROL | BACKEND_FLAG_IS_INSTALLED | BACKEND_FLAG_IS_MIGRATION,
                                    "",
                                    "",
                                    &frontends,
                                    &logs,
                                    &payload) &&
              binary_payload_list_append(payloads, &payload);
    free(frontends.data);
    free(logs.data);
    if (!ok) free(payload.data);
    return ok;
}

static bool append_bundled_backend_placeholder_payload(BinaryPayloadList *payloads, const BundledAppDefinition *app) {
    StringBuilder frontends = {0};
    StringBuilder logs = {0};
    StringBuilder payload = {0};
    bool ok = build_empty_array_payload(&frontends) &&
              build_empty_array_payload(&logs) &&
              build_backend_payload(app->service_id,
                                    app->display_name,
                                    "",
                                    "",
                                    "user",
                                    "available",
                                    BACKEND_FLAG_CAN_CONTROL |
                                        BACKEND_FLAG_IS_BUNDLED |
                                        (app->supports_root ? BACKEND_FLAG_SUPPORTS_ROOT : 0) |
                                        (app->root_only ? BACKEND_FLAG_ROOT_ONLY : 0),
                                    app->icon_symbol_name ? app->icon_symbol_name : "",
                                    "",
                                    &frontends,
                                    &logs,
                                    &payload) &&
              binary_payload_list_append(payloads, &payload);
    free(frontends.data);
    free(logs.data);
    if (!ok) free(payload.data);
    return ok;
}

static bool file_contains_any_legacy_outerwebapps_text(const char *path) {
    size_t size = 0;
    char *contents = read_text_file_alloc(path, &size);
    (void)size;
    if (!contents) return false;
    bool found = strstr(contents, "OUTERAGENT_ROOT") ||
                 strstr(contents, ".outeragent/outerctl") ||
                 strstr(contents, ".outerloop/outer-shell/bin/outerctl") ||
                 strstr(contents, "/var/lib/outergroup/outeragent") ||
                 strstr(contents, "outeragent.log");
    free(contents);
    return found;
}

static bool directory_contains_legacy_outerwebapps_text(const char *directory, bool recursive) {
    DIR *dir = opendir(directory);
    if (!dir) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = recursive && directory_contains_legacy_outerwebapps_text(path, true);
        } else if (S_ISREG(st.st_mode)) {
            found = file_contains_any_legacy_outerwebapps_text(path);
        }
    }
    closedir(dir);
    return found;
}

static bool root_outerwebapps_migration_pending(void) {
#ifdef __APPLE__
    struct stat st;
    return stat("/Library/dev.outergroup.OuterLoop/registry.sqlite3", &st) == 0 ||
           stat("/Library/dev.outergroup.OuterLoop", &st) == 0 ||
           directory_contains_legacy_outerwebapps_text("/Library/LaunchDaemons", false);
#else
    struct stat st;
    return stat("/var/lib/outergroup/outeragent/registry.sqlite3", &st) == 0 ||
           stat("/var/lib/outergroup/outeragent", &st) == 0 ||
           directory_contains_legacy_outerwebapps_text("/opt/outergroup", true) ||
           directory_contains_legacy_outerwebapps_text("/etc/systemd/system", false);
#endif
}

static void send_backends_response(int fd) {
    BinaryPayloadList payloads = {0};
    char user_error[512] = "";
    char system_error[512] = "";
    migrate_registry_schema_if_writable(g_registry_database_path);
    migrate_registry_schema_if_writable(g_system_registry_database_path);
    sqlite3 *user_database = open_registry_readonly(user_error, sizeof(user_error));
    sqlite3 *system_database = open_system_registry_readonly(system_error, sizeof(system_error));

    bool ok = true;
    const char *error = "";
    if (!user_database && !system_database) {
        error = user_error[0] ? user_error : system_error;
    }
    bool bundled_installed[sizeof(kBundledApps) / sizeof(kBundledApps[0])] = {0};
    if (user_database) {
        ok = ok && append_registered_backend_payloads(user_database, user_database, &payloads, bundled_installed,
                                                       sizeof(bundled_installed) / sizeof(bundled_installed[0]),
                                                       "user");
    }
    if (system_database) {
        ok = ok && append_registered_backend_payloads(system_database, user_database, &payloads, bundled_installed,
                                                       sizeof(bundled_installed) / sizeof(bundled_installed[0]),
                                                       "system");
    }
    ok = ok && append_root_migration_backend_payload(&payloads);

    for (size_t i = 0; ok && i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        if (!bundled_app_is_available_on_platform(&kBundledApps[i])) continue;
        if (bundled_installed[i]) continue;
        ok = ok && append_bundled_backend_placeholder_payload(&payloads, &kBundledApps[i]);
    }
    if (user_database) sqlite3_close(user_database);
    if (system_database) sqlite3_close(system_database);

    StringBuilder builder = {0};
    size_t fixed_size = 12 + payloads.count * 8;
    ok = ok && binary_append_zero(&builder, fixed_size) &&
         binary_write_u32_at(&builder, 8, (uint32_t)payloads.count) &&
         binary_append_string_ref_at(&builder, 0, error);
    for (size_t i = 0; ok && i < payloads.count; i++) {
        ok = binary_append_child_ref_at(&builder, 12 + i * 8, &payloads.items[i]);
    }
    if (!ok) {
        free(builder.data);
        binary_payload_list_free(&payloads);
        send_text_response(fd, 500, "failed to read registry database\n");
        return;
    }
    send_binary_response(fd, 200, &builder);
    free(builder.data);
    binary_payload_list_free(&payloads);
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

static uint64_t registry_file_state_token(const char *sqlite_path) {
    uint64_t token = file_state_token(sqlite_path);
    char binary_path[PATH_MAX] = "";
    if (registry_binary_output_path(sqlite_path, binary_path, sizeof(binary_path))) {
        token = mix_u64(token, file_state_token(binary_path));
    }
    return token;
}

static uint64_t current_backends_event_version(void) {
    uint64_t version = g_backend_event_sequence;
    version = mix_u64(version, registry_file_state_token(g_registry_database_path));
    version = mix_u64(version, registry_file_state_token(g_system_registry_database_path));
    return version ? version : 1;
}

static uint64_t current_log_event_version(const char *service_id, int log_index) {
    if (!service_id || !service_id[0]) return 0;
    char error[512] = "";
    char raw_path[PATH_MAX] = "";
    if (!resolve_log_path_any(service_id, log_index, raw_path, sizeof(raw_path), error, sizeof(error))) {
        return mix_u64(current_backends_event_version(), 0);
    }
    char path[PATH_MAX];
    expand_tilde_path(raw_path, path, sizeof(path));
    return mix_u64(current_backends_event_version(), file_state_token(path));
}

static void mark_backend_event_changed(void) {
    g_backend_event_sequence++;
    if (g_backend_event_sequence == 0) g_backend_event_sequence = 1;
}

static void send_events_response(int fd,
                                 bool backends_changed,
                                 bool log_changed,
                                 bool timed_out,
                                 uint64_t backends_version,
                                 uint64_t log_version) {
    StringBuilder builder = {0};
    uint32_t flags = (backends_changed ? 1u : 0u) |
                     (log_changed ? 2u : 0u) |
                     (timed_out ? 4u : 0u);
    bool ok = binary_append_zero(&builder, 24) &&
              binary_write_u32_at(&builder, 0, flags) &&
              binary_write_u64_at(&builder, 8, backends_version) &&
              binary_write_u64_at(&builder, 16, log_version);
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_binary_response(fd, 200, &builder);
    free(builder.data);
}

static void send_log_response(int fd, const char *service_id, const char *path, const char *contents,
                          bool truncated, uint64_t file_size, double modified, const char *error) {
    StringBuilder builder = {0};
    bool ok = binary_append_zero(&builder, 52) &&
              binary_append_string_ref_at(&builder, 0, service_id ? service_id : "") &&
              binary_append_string_ref_at(&builder, 8, path ? path : "") &&
              binary_append_string_ref_at(&builder, 16, contents ? contents : "") &&
              binary_write_u32_at(&builder, 24, truncated ? 1u : 0u) &&
              binary_write_u64_at(&builder, 28, file_size) &&
              binary_write_f64_at(&builder, 36, modified) &&
              binary_append_string_ref_at(&builder, 44, error ? error : "");
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_binary_response(fd, 200, &builder);
    free(builder.data);
}

static void send_action_response_ex(int fd, int status, bool ok_value, const char *message, bool needs_password) {
    StringBuilder builder = {0};
    uint32_t flags = (ok_value ? ACTION_FLAG_OK : 0) |
                     (needs_password ? ACTION_FLAG_NEEDS_PASSWORD : 0);
    bool ok = binary_append_zero(&builder, 12) &&
              binary_write_u32_at(&builder, 0, flags) &&
              binary_append_string_ref_at(&builder, 4, message ? message : "");
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_binary_response(fd, status, &builder);
    free(builder.data);
}

static void send_action_response(int fd, int status, bool ok_value, const char *message) {
    send_action_response_ex(fd, status, ok_value, message, false);
}

static bool install_bundled_app(const BundledAppDefinition *app, const char *scope, const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool uninstall_backend(const char *service_id, const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool run_root_outerwebapps_migration(const char *sudo_password, bool *needs_password, char *message, size_t message_size);

static bool registry_storage_exists_at(const char *database_path) {
    struct stat st;
    if (database_path && database_path[0] && stat(database_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    char binary_path[PATH_MAX];
    return database_path &&
           registry_binary_output_path(database_path, binary_path, sizeof(binary_path)) &&
           stat(binary_path, &st) == 0 &&
           S_ISREG(st.st_mode);
}

static bool frontend_exists_in_registry_at(const char *database_path,
                                           const char *service_id,
                                           const char *frontend_id,
                                           const char *frontend_url,
                                           bool *found,
                                           char *error,
                                           size_t error_size) {
    if (found) *found = false;
    sqlite3 *database = open_registry_readonly_at(database_path, error, error_size);
    if (!database) return false;
    sqlite3_stmt *statement = NULL;
    bool use_frontend_id = frontend_id && frontend_id[0];
    bool ok = sqlite3_prepare_v2(database,
                                 use_frontend_id
                                     ? "SELECT 1 FROM frontends WHERE service_id = ? AND frontend_id = ? LIMIT 1;"
                                     : "SELECT 1 FROM frontends WHERE service_id = ? AND url = ? LIMIT 1;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, use_frontend_id ? frontend_id : frontend_url, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            if (found) *found = true;
        } else if (step != SQLITE_DONE) {
            ok = false;
        }
    }
    if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    if (statement) sqlite3_finalize(statement);
    sqlite3_close(database);
    return ok;
}

static bool update_frontend_layout_in_user_registry(const char *frontend_id,
                                                    const char *frontend_url,
                                                    const char *list_name,
                                                    char *error,
                                                    size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        close_registry_readwrite(database, false, error, error_size);
        return false;
    }
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO frontend_layouts(url, list, frontend_id) VALUES(?, ?, NULLIF(?, '')) "
                                 "ON CONFLICT(url) DO UPDATE SET list = excluded.list, frontend_id = excluded.frontend_id;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, (frontend_url && frontend_url[0]) ? frontend_url : (frontend_id ? frontend_id : ""), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, list_name ? list_name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, frontend_id ? frontend_id : "", -1, SQLITE_TRANSIENT);
        ok = registry_binary_step(database, statement, error, error_size);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }
    if (statement) sqlite3_finalize(statement);
    return close_registry_readwrite(database, ok, error, error_size) && ok;
}

static bool update_frontend_list_any_registry(const char *service_id,
                                              const char *frontend_id,
                                              const char *frontend_url,
                                              const char *list_name,
                                              char *message,
                                              size_t message_size) {
    bool found = false;
    char error[512] = "";
    if (!frontend_exists_in_registry_at(g_registry_database_path,
                                        service_id,
                                        frontend_id,
                                        frontend_url,
                                        &found,
                                        error,
                                        sizeof(error))) {
        snprintf(message, message_size, "Could not read user registry: %s", error);
        return false;
    }

    if (!found && g_system_registry_database_path[0] && registry_storage_exists_at(g_system_registry_database_path)) {
        error[0] = '\0';
        if (!frontend_exists_in_registry_at(g_system_registry_database_path,
                                            service_id,
                                            frontend_id,
                                            frontend_url,
                                            &found,
                                            error,
                                            sizeof(error))) {
            snprintf(message, message_size, "Could not read system registry: %s", error);
            return false;
        }
    }

    if (found) {
        if (update_frontend_layout_in_user_registry(frontend_id, frontend_url, list_name, error, sizeof(error))) {
            snprintf(message, message_size, "Updated app list.");
            return true;
        }
        snprintf(message, message_size, "Could not update app layout: %s", error);
        return false;
    }

    snprintf(message, message_size, "Frontend was not found.");
    return false;
}

static bool clear_frontends_in_registry_at(const char *database_path, const char *service_id, char *error, size_t error_size) {
    sqlite3 *database = open_registry_readwrite_at(database_path, error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        close_registry_readwrite_at(database, database_path, false, error, error_size);
        return false;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    sqlite3_stmt *statement = NULL;
    if (ok) {
        ok = sqlite3_prepare_v2(database, "UPDATE frontends SET running = 0, url = '', port = 0 WHERE service_id = ?;", -1, &statement, NULL) == SQLITE_OK;
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
    return close_registry_readwrite_at(database, database_path, ok, error, error_size) && ok;
}

static void clear_frontends_in_system_registry_as_root(const char *service_id, const char *sudo_password) {
#ifdef __APPLE__
    char quoted_system_root[PATH_MAX + 8];
    char quoted_service_id[512];
    char outerctl_path[PATH_MAX];
    char quoted_outerctl[PATH_MAX + 8];
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
    shell_quote(outerctl_path, quoted_outerctl, sizeof(quoted_outerctl));
    shell_quote(kSystemOuterWebappsRoot, quoted_system_root, sizeof(quoted_system_root));
    shell_quote(service_id, quoted_service_id, sizeof(quoted_service_id));
    char command[8192];
    snprintf(command,
             sizeof(command),
             "OUTERWEBAPPS_HOME=%s %s app clear --backend %s >/dev/null 2>&1 || true\n",
             quoted_system_root,
             quoted_outerctl,
             quoted_service_id);
    char output[1024] = "";
    int exit_status = -1;
    (void)run_sudo_shell(command, sudo_password, output, sizeof(output), &exit_status);
#else
    char output[1024] = "";
    bool needs_password = false;
    if (!ensure_root_helper_installed(sudo_password, &needs_password, output, sizeof(output))) return;
    (void)root_helper_registry_clear_frontends(service_id, sudo_password, &needs_password, output, sizeof(output));
#endif
}

static void clear_frontends_after_successful_stop(const char *service_id, bool system_scope, const char *sudo_password) {
    char error[512] = "";
    if (!system_scope) {
        (void)clear_frontends_in_registry_at(g_registry_database_path, service_id, error, sizeof(error));
        return;
    }
    if (g_system_registry_database_path[0] && access(g_system_registry_database_path, W_OK) == 0) {
        (void)clear_frontends_in_registry_at(g_system_registry_database_path, service_id, error, sizeof(error));
    } else {
        clear_frontends_in_system_registry_as_root(service_id, sudo_password);
    }
}

static void send_control_response(int fd, const char *query, const char *body) {
    char service_id[PATH_MAX] = "";
    char operation[32] = "";
    char requested_scope[32] = "";
    char sudo_password[PATH_MAX] = "";
    if (!query_value_any(query, body, "serviceID", service_id, sizeof(service_id)) ||
        !query_value_any(query, body, "operation", operation, sizeof(operation))) {
        send_action_response(fd, 400, false, "Missing serviceID or operation.");
        return;
    }
    query_value_any(query, body, "scope", requested_scope, sizeof(requested_scope));
    query_value_any(query, body, "sudoPassword", sudo_password, sizeof(sudo_password));
    trim_whitespace_in_place(service_id);
    trim_whitespace_in_place(operation);
    trim_whitespace_in_place(requested_scope);
    log_event("Control request operation=%s serviceID=%s scope=%s.", operation, service_id, requested_scope);

    if (strcmp(operation, "setFrontendList") == 0) {
        char frontend_id[PATH_MAX] = "";
        char frontend_url[PATH_MAX] = "";
        char list_name[PATH_MAX] = "";
        query_value_any(query, body, "frontendID", frontend_id, sizeof(frontend_id));
        query_value_any(query, body, "frontendURL", frontend_url, sizeof(frontend_url));
        if (!frontend_id[0] && !frontend_url[0]) {
            send_action_response(fd, 400, false, "Missing frontend identifier.");
            return;
        }
        query_value_any(query, body, "list", list_name, sizeof(list_name));
        char message[1024] = "";
        bool ok = update_frontend_list_any_registry(service_id, frontend_id, frontend_url, list_name, message, sizeof(message));
        log_event("%s frontend list update for %s frontend=%s url=%s list=%s: %s",
                  ok ? "Completed" : "Failed",
                  service_id,
                  frontend_id,
                  frontend_url,
                  list_name,
                  message);
        if (ok) mark_backend_event_changed();
        send_action_response(fd, ok ? 200 : 500, ok, message);
        return;
    }

    if (strcmp(service_id, kMigrationServiceID) == 0) {
        if (strcmp(operation, "migrateRoot") != 0 && strcmp(operation, "start") != 0) {
            send_action_response(fd, 400, false, "Unsupported migration operation.");
            return;
        }
        char message[4096] = "";
        bool needs_password = false;
        bool ok = run_root_outerwebapps_migration(sudo_password, &needs_password, message, sizeof(message));
        log_event("%s root outerwebapps migration: %s", ok ? "Completed" : "Failed", message);
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }

    if (is_home_screen_service_id(service_id)) {
        char message[4096] = "";
        if (strcmp(operation, "checkUpdate") == 0 || strcmp(operation, "checkOuterShellUpdate") == 0) {
            char installed[128] = "";
            char latest[128] = "";
            installed_home_screen_version(installed, sizeof(installed));
            bool ok = fetch_home_screen_available_version("manual", latest, sizeof(latest), message, sizeof(message));
            if (ok) {
                mark_update_check_completed();
                if (compare_versions(installed, latest) < 0) {
                    snprintf(message, sizeof(message),
                             "Outer Shell %s is available. Installed version: %s.",
                             latest,
                             installed[0] ? installed : "unknown");
                } else {
                    snprintf(message, sizeof(message),
                             "Outer Shell is up to date. Installed version: %s.",
                             installed[0] ? installed : latest);
                }
            }
            log_event("%s Outer Shell update check: %s", ok ? "Completed" : "Failed", message);
            send_action_response(fd, ok ? 200 : 500, ok, message);
            return;
        }
        if (strcmp(operation, "update") == 0 || strcmp(operation, "updateOuterShell") == 0 ||
            strcmp(operation, "uninstall") == 0 || strcmp(operation, "uninstallOuterShell") == 0) {
            const char *installer_command = (strncmp(operation, "uninstall", 9) == 0) ? "uninstall" : "update";
            bool ok = false;
#ifdef __APPLE__
            char base_url[2048] = "";
            if (strcmp(installer_command, "uninstall") == 0 &&
                !home_screen_base_url(base_url, sizeof(base_url))) {
                ok = uninstall_local_home_screen(message, sizeof(message));
            } else
#endif
            {
                ok = run_home_screen_install_script(installer_command, message, sizeof(message));
            }
            log_event("%s Outer Shell %s: %s", ok ? "Completed" : "Failed", installer_command, message);
            if (ok) mark_backend_event_changed();
            send_action_response(fd, ok ? 200 : 500, ok, message);
            return;
        }
        log_event("Rejected control request for Outer Shell itself: operation=%s.", operation);
        send_action_response(fd, 400, false, "Outer Shell cannot start or stop itself.");
        return;
    }

    if (strcmp(operation, "run") == 0 || strcmp(operation, "install") == 0 ||
        strcmp(operation, "runRoot") == 0 || strcmp(operation, "installRoot") == 0 ||
        strcmp(operation, "runUser") == 0 || strcmp(operation, "installUser") == 0) {
        const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
        if (!app) {
            send_action_response(fd, 404, false, "This app cannot be installed by Outer Shell.");
            return;
        }
        char message[4096] = "";
        bool needs_password = false;
        const char *scope = (strcmp(operation, "runRoot") == 0 || strcmp(operation, "installRoot") == 0) ? "system" : "user";
        if (strcmp(scope, "system") == 0 && !app->supports_root) {
            send_action_response(fd, 400, false, "This app does not support running as root.");
            return;
        }
        if (strcmp(scope, "user") == 0 && app->root_only) {
            send_action_response(fd, 400, false, "This app can only run as root.");
            return;
        }
#ifdef __APPLE__
        if (strcmp(scope, "user") == 0) {
            char existing_plist[PATH_MAX] = "";
            int owns_existing_plist = 0;
            if (lookup_launchd_backend_any(service_id, existing_plist, sizeof(existing_plist), &owns_existing_plist) &&
                strncmp(existing_plist, "/Library/LaunchDaemons/", 23) == 0) {
                bool removed = uninstall_backend(service_id, sudo_password, &needs_password, message, sizeof(message));
                if (!removed) {
                    log_event("Failed to remove existing root launchd install before installing %s: %s", service_id, message);
                    send_action_response_ex(fd, needs_password ? 401 : 500, false, message, needs_password);
                    return;
                }
            }
        }
#endif
        bool ok = install_bundled_app(app, scope, sudo_password, &needs_password, message, sizeof(message));
        log_event("%s app %s as %s: %s",
                  ok ? "Installed" : "Failed to install",
                  app->service_id,
                  scope,
                  message);
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }

#ifndef __APPLE__
    if (strcmp(operation, "addRootSupport") == 0 || strcmp(operation, "removeRootSupport") == 0) {
        const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
        if (!app || !app->supports_root) {
            send_action_response(fd, 404, false, "This app does not support root support.");
            return;
        }
        char message[4096] = "";
        bool needs_password = false;
        bool ok = false;
        if (strcmp(operation, "addRootSupport") == 0) {
            ok = install_bundled_app(app, "system", sudo_password, &needs_password, message, sizeof(message));
        } else {
            if (!app->root_only) {
                char user_unit[256] = "";
                char user_scope[32] = "user";
                sqlite3 *database = open_registry_readonly(message, sizeof(message));
                bool has_user_install = false;
                if (database) {
                    has_user_install = lookup_systemd_backend(database, service_id, user_unit, sizeof(user_unit), user_scope, sizeof(user_scope));
                    sqlite3_close(database);
                }
                if (has_user_install) {
                    ok = install_bundled_app(app, "user", NULL, NULL, message, sizeof(message));
                    if (!ok) {
                        log_event("Failed to restore user install before removing root support for %s: %s", service_id, message);
                        send_action_response(fd, 500, false, message);
                        return;
                    }
                }
            }
            ok = remove_bundled_root_support(app, sudo_password, &needs_password, message, sizeof(message));
        }
        log_event("%s root support for app %s: %s",
                  ok ? (strcmp(operation, "addRootSupport") == 0 ? "Added" : "Removed") : "Failed to change",
                  app->service_id,
                  message);
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }
#endif

    if (strcmp(operation, "uninstall") == 0) {
        char message[4096] = "";
        bool needs_password = false;
        bool ok = uninstall_backend(service_id, sudo_password, &needs_password, message, sizeof(message));
        log_event("%s backend %s: %s", ok ? "Uninstalled" : "Failed to uninstall", service_id, message);
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
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
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }
#endif

    char unit_name[256] = "";
    char scope[32] = "user";
    bool found = lookup_systemd_backend_any_for_scope(service_id,
                                                      requested_scope,
                                                      unit_name,
                                                      sizeof(unit_name),
                                                      scope,
                                                      sizeof(scope));
    if (!found) {
        send_action_response(fd, 404, false, "This backend does not have a registered systemd unit.");
        return;
    }

    char message[4096] = "";
    bool needs_password = false;
    bool ok = true;
    char socket_unit[256] = "";
    systemd_socket_unit_name(unit_name, socket_unit, sizeof(socket_unit));
    if (safe_unit_name(socket_unit) && (strcmp(operation, "start") == 0 || strcmp(operation, "stop") == 0)) {
        log_event("Running socket-backed systemd operation %s for %s (%s): socket=%s service=%s",
                  operation,
                  service_id,
                  scope,
                  socket_unit,
                  unit_name);
        bool ignored_needs_password = false;
        char ignored_message[1024] = "";
        if (strcmp(operation, "stop") == 0) {
            (void)ignored_needs_password;
            (void)ignored_message;
            ok = run_systemd_operation(unit_name, scope, "stop", sudo_password, &needs_password, message, sizeof(message));
        } else {
            (void)run_systemd_operation(socket_unit,
                                        scope,
                                        "start",
                                        sudo_password,
                                        &ignored_needs_password,
                                        ignored_message,
                                        sizeof(ignored_message));
            ok = run_systemd_operation(unit_name, scope, "start", sudo_password, &needs_password, message, sizeof(message));
        }
    } else {
        ok = run_systemd_operation(unit_name, scope, operation, sudo_password, &needs_password, message, sizeof(message));
    }
    if (ok && strcmp(operation, "stop") == 0) {
        clear_frontends_after_successful_stop(service_id, strcmp(scope, "system") == 0, sudo_password);
    }
    log_event("%s systemd operation %s for %s (%s): %s",
              ok ? "Completed" : "Failed",
              operation,
              service_id,
              scope,
              message);
    if (ok) mark_backend_event_changed();
    send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
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

static char *read_text_file_alloc(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 1024 * 1024 * 8) {
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (!data) {
        close(fd);
        return NULL;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(fd, data + offset, size - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(data);
            close(fd);
            return NULL;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(fd);
    data[offset] = '\0';
    if (out_size) *out_size = offset;
    return data;
}

static bool append_replaced_text(StringBuilder *builder, const char *text, const TextReplacement *replacements, size_t replacement_count, bool *changed) {
    const char *cursor = text ? text : "";
    while (*cursor) {
        size_t best_index = replacement_count;
        size_t best_length = 0;
        for (size_t i = 0; i < replacement_count; i++) {
            const char *old_text = replacements[i].old_text;
            if (!old_text || !old_text[0]) continue;
            size_t old_length = strlen(old_text);
            if (strncmp(cursor, old_text, old_length) == 0 && old_length > best_length) {
                best_index = i;
                best_length = old_length;
            }
        }
        if (best_index < replacement_count) {
            if (!sb_append(builder, replacements[best_index].new_text ? replacements[best_index].new_text : "")) return false;
            cursor += best_length;
            if (changed) *changed = true;
        } else {
            if (!sb_append_n(builder, cursor, 1)) return false;
            cursor++;
        }
    }
    return true;
}

static bool rewrite_file_replacing_text(const char *path, const TextReplacement *replacements, size_t replacement_count) {
    size_t size = 0;
    char *contents = read_text_file_alloc(path, &size);
    if (!contents) return false;
    bool changed = false;
    StringBuilder builder = {0};
    bool ok = append_replaced_text(&builder, contents, replacements, replacement_count, &changed);
    free(contents);
    if (ok && changed) {
        char error[512] = "";
        ok = write_text_file(path, builder.data ? builder.data : "", error, sizeof(error));
    }
    free(builder.data);
    return ok;
}

static void rewrite_files_in_directory_replacing_text(const char *directory,
                                                      const TextReplacement *replacements,
                                                      size_t replacement_count,
                                                      bool recursive) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (recursive) rewrite_files_in_directory_replacing_text(path, replacements, replacement_count, true);
        } else if (S_ISREG(st.st_mode)) {
            (void)rewrite_file_replacing_text(path, replacements, replacement_count);
        }
    }
    closedir(dir);
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

static void trim_whitespace_in_place(char *value) {
    if (!value) return;
    char *start = value;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != value) memmove(value, start, strlen(start) + 1);
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[--len] = '\0';
    }
}

static bool read_first_line_file(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = fgets(out, (int)out_size, file) != NULL;
    fclose(file);
    if (ok) trim_whitespace_in_place(out);
    return ok;
}

static bool write_text_file_simple(const char *path, const char *contents) {
    char error[256] = "";
    if (!ensure_parent_directory(path, error, sizeof(error))) {
        return false;
    }
    FILE *file = fopen(path, "w");
    if (!file) return false;
    if (contents && contents[0]) fputs(contents, file);
    bool ok = fclose(file) == 0;
    return ok;
}

static void append_url_encoded(StringBuilder *builder, const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    if (!builder || !value) return;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char ch = *p;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            char one[2] = {(char)ch, '\0'};
            sb_append(builder, one);
        } else {
            char escaped[4] = {'%', hex[ch >> 4], hex[ch & 0x0f], '\0'};
            sb_append(builder, escaped);
        }
    }
}

static bool home_screen_base_url(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    const char *env = getenv("OUTER_SHELL_PUBLIC_BASE_URL");
    const char *value = env && env[0] ? env : g_home_screen_public_base_url;
    if (!value || !value[0]) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_size, "%s", value);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
    return out[0] != '\0';
}

static void home_screen_install_root(char *out, size_t out_size) {
    default_user_home_screen_install_root(out, out_size);
}

static void home_screen_version_path(char *out, size_t out_size) {
    char root[PATH_MAX];
    home_screen_install_root(root, sizeof(root));
    snprintf(out, out_size, "%s/version", root);
}

static void home_screen_last_update_check_path(char *out, size_t out_size) {
    char root[PATH_MAX];
    home_screen_install_root(root, sizeof(root));
    snprintf(out, out_size, "%s/last-update-check", root);
}

static bool installed_home_screen_version(char *out, size_t out_size) {
    char path[PATH_MAX];
    home_screen_version_path(path, sizeof(path));
    return read_first_line_file(path, out, out_size);
}

static bool update_check_due(void) {
    char path[PATH_MAX];
    char timestamp[64] = "";
    home_screen_last_update_check_path(path, sizeof(path));
    if (!read_first_line_file(path, timestamp, sizeof(timestamp))) return true;
    char *end = NULL;
    long long last = strtoll(timestamp, &end, 10);
    if (end == timestamp || last <= 0) return true;
    time_t now = time(NULL);
    return now <= 0 || (long long)now - last >= 24 * 60 * 60;
}

static void mark_update_check_completed(void) {
    char path[PATH_MAX];
    char value[64];
    home_screen_last_update_check_path(path, sizeof(path));
    snprintf(value, sizeof(value), "%lld\n", (long long)time(NULL));
    (void)write_text_file_simple(path, value);
}

static int version_label_rank(const char *label) {
    if (!label || !label[0]) return 3;
    if (strcasecmp(label, "DEV") == 0) return 0;
    if (strcasecmp(label, "ALPHA") == 0) return 0;
    if (strcasecmp(label, "BETA") == 0) return 1;
    if (strcasecmp(label, "RC") == 0) return 2;
    return 3;
}

static void parse_version_component(const char *component, int *number, char *label, size_t label_size) {
    if (number) *number = 0;
    if (label && label_size > 0) label[0] = '\0';
    if (!component) return;
    char *end = NULL;
    long value = strtol(component, &end, 10);
    if (number && end != component && value >= 0 && value <= 1000000) {
        *number = (int)value;
    }
    if (label && label_size > 0 && end && *end) {
        while (*end == '-' || *end == '_' || *end == '+') end++;
        snprintf(label, label_size, "%s", end);
    }
}

static int compare_versions(const char *installed, const char *available) {
    char a[128], b[128];
    snprintf(a, sizeof(a), "%s", installed ? installed : "");
    snprintf(b, sizeof(b), "%s", available ? available : "");
    trim_whitespace_in_place(a);
    trim_whitespace_in_place(b);
    char *save_a = NULL;
    char *save_b = NULL;
    char *token_a = strtok_r(a, ".", &save_a);
    char *token_b = strtok_r(b, ".", &save_b);
    for (int i = 0; i < 8; i++) {
        int number_a = 0;
        int number_b = 0;
        char label_a[64] = "";
        char label_b[64] = "";
        parse_version_component(token_a, &number_a, label_a, sizeof(label_a));
        parse_version_component(token_b, &number_b, label_b, sizeof(label_b));
        if (number_a < number_b) return -1;
        if (number_a > number_b) return 1;
        int rank_a = version_label_rank(label_a);
        int rank_b = version_label_rank(label_b);
        if (rank_a < rank_b) return -1;
        if (rank_a > rank_b) return 1;
        int label_compare = strcasecmp(label_a, label_b);
        if (label_compare < 0) return -1;
        if (label_compare > 0) return 1;
        token_a = token_a ? strtok_r(NULL, ".", &save_a) : NULL;
        token_b = token_b ? strtok_r(NULL, ".", &save_b) : NULL;
        if (!token_a && !token_b) return 0;
    }
    return 0;
}

static bool fetch_home_screen_available_version(const char *heartbeat, char *out, size_t out_size, char *message, size_t message_size) {
    if (out && out_size > 0) out[0] = '\0';
    char base_url[2048];
    if (!home_screen_base_url(base_url, sizeof(base_url))) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    StringBuilder url = {0};
    bool ok = sb_append(&url, base_url) &&
              sb_append(&url, "/latest/version.txt?heartbeat=") &&
              (append_url_encoded(&url, heartbeat && heartbeat[0] ? heartbeat : "manual"), true);
    if (!ok) {
        free(url.data);
        snprintf(message, message_size, "Failed to build update URL.");
        return false;
    }

    char quoted_url[4096];
    shell_quote(url.data, quoted_url, sizeof(quoted_url));
    free(url.data);
    char command[8192];
    snprintf(command,
             sizeof(command),
             "if command -v curl >/dev/null 2>&1; then "
             "curl -fsSL %s; "
             "elif command -v wget >/dev/null 2>&1; then "
             "wget -qO- %s; "
             "else echo 'curl or wget is required' >&2; exit 127; fi",
             quoted_url,
             quoted_url);
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        snprintf(message, message_size, "Failed to run update check: %s", strerror(errno));
        return false;
    }
    char buffer[256] = "";
    bool got = fgets(buffer, sizeof(buffer), pipe) != NULL;
    int status = pclose(pipe);
    if (status != 0 || !got) {
        snprintf(message, message_size, "Could not fetch Outer Shell version.");
        return false;
    }
    trim_whitespace_in_place(buffer);
    if (!buffer[0]) {
        snprintf(message, message_size, "Outer Shell version file was empty.");
        return false;
    }
    snprintf(out, out_size, "%s", buffer);
    return true;
}

static bool home_screen_update_available(char *available, size_t available_size) {
    if (!update_check_due()) return false;
    char base_url[2048] = "";
    if (!home_screen_base_url(base_url, sizeof(base_url))) return false;
    char installed[128] = "";
    char latest[128] = "";
    char message[256] = "";
    if (!installed_home_screen_version(installed, sizeof(installed))) return false;
    if (!fetch_home_screen_available_version("daily", latest, sizeof(latest), message, sizeof(message))) {
        log_event("Outer Shell update check failed: %s", message);
        return false;
    }
    mark_update_check_completed();
    if (compare_versions(installed, latest) < 0) {
        snprintf(available, available_size, "%s", latest);
        return true;
    }
    return false;
}

static bool run_home_screen_install_script(const char *subcommand, char *message, size_t message_size) {
    char base_url[2048];
    if (!home_screen_base_url(base_url, sizeof(base_url))) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    char script_url[4096];
    snprintf(script_url, sizeof(script_url), "%s/latest/install.sh?heartbeat=manual", base_url);
    char quoted_url[4096];
    char quoted_subcommand[64];
    shell_quote(script_url, quoted_url, sizeof(quoted_url));
    shell_quote(subcommand && subcommand[0] ? subcommand : "install", quoted_subcommand, sizeof(quoted_subcommand));
    char command[8192];
    snprintf(command,
             sizeof(command),
             "if command -v curl >/dev/null 2>&1; then "
             "curl -fsSL %s | sh -s -- %s; "
             "elif command -v wget >/dev/null 2>&1; then "
             "wget -qO- %s | sh -s -- %s; "
             "else echo 'curl or wget is required' >&2; exit 127; fi",
             quoted_url,
             quoted_subcommand,
             quoted_url,
             quoted_subcommand);
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        snprintf(message, message_size, "Failed to run Outer Shell %s: %s", subcommand, strerror(errno));
        return false;
    }
    size_t offset = 0;
    while (offset + 1 < message_size) {
        size_t got = fread(message + offset, 1, message_size - offset - 1, pipe);
        offset += got;
        if (got == 0) break;
    }
    message[offset] = '\0';
    trim_whitespace_in_place(message);
    int status = pclose(pipe);
    if (status == 0) {
        if (!message[0]) snprintf(message, message_size, "Outer Shell %s completed.", subcommand);
        return true;
    }
    if (!message[0]) snprintf(message, message_size, "Outer Shell %s failed.", subcommand);
    return false;
}

static bool sudo_failure_needs_password(const char *output, int exit_status) {
    if (exit_status == 0) return false;
    return contains_case_insensitive(output, "password") ||
           contains_case_insensitive(output, "authentication") ||
           contains_case_insensitive(output, "try again") ||
           contains_case_insensitive(output, "sudo:");
}

static bool read_exact_with_timeout(int fd, void *buffer, size_t length, int timeout_ms) {
    unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (poll_result == 0 || (pfd.revents & (POLLERR | POLLNVAL))) {
            return false;
        }
        if (!(pfd.revents & POLLIN)) continue;
        ssize_t got = read(fd, bytes + offset, length - offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        offset += (size_t)got;
    }
    return true;
}

static void service_nested_api_client_once(void) {
    if (g_api_listener_fd < 0) return;
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    int client_fd = accept((int)g_api_listener_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) return;

    char request[READ_BUFFER_SIZE];
    unsigned char length_bytes[4];
    bool ok = read_exact_with_timeout(client_fd, length_bytes, sizeof(length_bytes), 5000);
    uint32_t message_length = ok ? read_uint32_le(length_bytes) : 0;
    if (ok && message_length <= READ_BUFFER_SIZE - 4) {
        memcpy(request, length_bytes, sizeof(length_bytes));
        ok = read_exact_with_timeout(client_fd, request + 4, message_length, 5000);
    } else {
        ok = false;
    }

    if (ok) {
        ReactorClient client = {0};
        client.fd = client_fd;
        client.is_api = true;
        (void)process_api_client_request(&client, request, (size_t)message_length + 4);
    }
    close(client_fd);
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

    int status = 0;
    bool child_exited = false;
    size_t offset = 0;
    while (!child_exited) {
        struct pollfd poll_fds[2];
        nfds_t poll_count = 0;
        poll_fds[poll_count++] = (struct pollfd){.fd = output_pipe[0], .events = POLLIN, .revents = 0};
        if (g_api_listener_fd >= 0) {
            poll_fds[poll_count++] = (struct pollfd){.fd = (int)g_api_listener_fd, .events = POLLIN, .revents = 0};
        }
        int poll_result = poll(poll_fds, poll_count, 100);
        if (poll_result > 0) {
            if (poll_fds[0].revents & POLLIN) {
                char buffer[1024];
                ssize_t got = read(output_pipe[0], buffer, sizeof(buffer));
                if (got > 0) {
                    size_t copy = (size_t)got;
                    if (output_size > 0 && offset + copy >= output_size) {
                        copy = output_size - offset - 1;
                    }
                    if (copy > 0 && output_size > 0) {
                        memcpy(output + offset, buffer, copy);
                        offset += copy;
                    }
                }
            }
            if (poll_count > 1 && (poll_fds[1].revents & POLLIN)) {
                service_nested_api_client_once();
            }
        } else if (poll_result < 0 && errno != EINTR) {
            break;
        }
        pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            child_exited = true;
        } else if (wait_result < 0 && errno != EINTR) {
            child_exited = true;
        }
    }
    for (;;) {
        char buffer[1024];
        ssize_t got = read(output_pipe[0], buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        size_t copy = (size_t)got;
        if (output_size > 0 && offset + copy >= output_size) {
            copy = output_size - offset - 1;
        }
        if (copy > 0 && output_size > 0) {
            memcpy(output + offset, buffer, copy);
            offset += copy;
        }
    }
    if (output_size > 0) output[offset] = '\0';
    close(output_pipe[0]);

    if (!child_exited) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
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

#ifndef __APPLE__
static bool ensure_root_helper_installed(const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;

    char executable[PATH_MAX];
    if (!current_executable_path(executable, sizeof(executable))) {
        snprintf(message, message_size, "Could not resolve outershelld executable path.");
        return false;
    }

    uid_t owner_uid = getuid();
    struct passwd *pw = getpwuid(owner_uid);
    const char *owner_name = pw && pw->pw_name ? pw->pw_name : "";
    if (!owner_name[0]) {
        snprintf(message, message_size, "Could not resolve current user name.");
        return false;
    }

    char service_name[256];
    char socket_name[256];
    root_helper_unit_name_for_uid(owner_uid, "service", service_name, sizeof(service_name));
    root_helper_unit_name_for_uid(owner_uid, "socket", socket_name, sizeof(socket_name));

    char quoted_executable[PATH_MAX + 8];
    char quoted_service_name[320];
    char quoted_socket_name[320];
    shell_quote(executable, quoted_executable, sizeof(quoted_executable));
    shell_quote(service_name, quoted_service_name, sizeof(quoted_service_name));
    shell_quote(socket_name, quoted_socket_name, sizeof(quoted_socket_name));
    (void)owner_name;

    char script_template[] = "/tmp/outershelld-root-helper-install-XXXXXX";
    int script_fd = mkstemp(script_template);
    if (script_fd < 0) {
        snprintf(message, message_size, "Failed to create root helper install script: %s", strerror(errno));
        return false;
    }
    FILE *script = fdopen(script_fd, "w");
    if (!script) {
        close(script_fd);
        unlink(script_template);
        snprintf(message, message_size, "Failed to write root helper install script: %s", strerror(errno));
        return false;
    }
    fprintf(script,
            "set -eu\n"
            "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
            "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
            "rm -f /etc/systemd/system/%s /etc/systemd/system/%s\n"
            "systemctl --system daemon-reload >/dev/null 2>&1 || true\n"
            "mkdir -p /usr/local/libexec %s\n"
            "rm -f /usr/local/libexec/outershelld-root-helper\n"
            "install -m 0755 %s /usr/local/libexec/outershelld-root-tool\n"
            "chmod 0755 /usr/local/libexec/outershelld-root-tool\n",
            quoted_service_name,
            quoted_socket_name,
            service_name,
            socket_name,
            kSystemOuterWebappsRoot,
            quoted_executable);
    fclose(script);
    chmod(script_template, 0700);

    bool ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
    unlink(script_template);
    if (!ok) return false;

    snprintf(message, message_size, "Root helper installed.");
    return true;
}
#else
static bool ensure_root_helper_installed(const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    (void)sudo_password;
    if (needs_password) *needs_password = false;
    snprintf(message, message_size, "Root helper is only implemented on Linux.");
    return false;
}

static bool root_helper_registry_remove_backend(const char *service_id,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size) {
    (void)service_id;
    (void)sudo_password;
    if (needs_password) *needs_password = false;
    snprintf(message, message_size, "Root helper is only implemented on Linux.");
    return false;
}
#endif

static bool run_root_outerwebapps_migration(const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;
    char old_outerctl[PATH_MAX];
    char old_home_screen_outerctl[PATH_MAX];
    char new_outerctl[PATH_MAX];
    char old_user_apps[PATH_MAX];
    char new_user_apps[PATH_MAX];
    legacy_user_outerctl_path(old_outerctl, sizeof(old_outerctl));
    legacy_home_screen_outerctl_path(old_home_screen_outerctl, sizeof(old_home_screen_outerctl));
    default_user_outerctl_path(new_outerctl, sizeof(new_outerctl));
    legacy_user_apps_root(old_user_apps, sizeof(old_user_apps));
    default_user_outerwebapps_apps_root(new_user_apps, sizeof(new_user_apps));

    const char *legacy_system_root =
#ifdef __APPLE__
        "/Library/dev.outergroup.OuterLoop";
#else
        "/var/lib/outergroup/outeragent";
#endif

    char legacy_system_apps_root[PATH_MAX];
    char new_system_apps_root[PATH_MAX];
#ifdef __APPLE__
    snprintf(legacy_system_apps_root, sizeof(legacy_system_apps_root), "%s/backends", legacy_system_root);
#else
    snprintf(legacy_system_apps_root, sizeof(legacy_system_apps_root), "%s", legacy_system_root);
#endif
    snprintf(new_system_apps_root, sizeof(new_system_apps_root), "%s/apps", kSystemOuterWebappsRoot);

    char quoted_old_outerctl[PATH_MAX + 8];
    char quoted_old_home_screen_outerctl[PATH_MAX + 8];
    char quoted_new_outerctl[PATH_MAX + 8];
    char quoted_old_user_apps[PATH_MAX + 8];
    char quoted_new_user_apps[PATH_MAX + 8];
    char quoted_legacy_system_root[PATH_MAX + 8];
    char quoted_legacy_system_apps_root[PATH_MAX + 8];
    char quoted_new_root[PATH_MAX + 8];
    char quoted_new_system_apps_root[PATH_MAX + 8];
    shell_quote(old_outerctl, quoted_old_outerctl, sizeof(quoted_old_outerctl));
    shell_quote(old_home_screen_outerctl, quoted_old_home_screen_outerctl, sizeof(quoted_old_home_screen_outerctl));
    shell_quote(new_outerctl, quoted_new_outerctl, sizeof(quoted_new_outerctl));
    shell_quote(old_user_apps, quoted_old_user_apps, sizeof(quoted_old_user_apps));
    shell_quote(new_user_apps, quoted_new_user_apps, sizeof(quoted_new_user_apps));
    shell_quote(legacy_system_root, quoted_legacy_system_root, sizeof(quoted_legacy_system_root));
    shell_quote(legacy_system_apps_root, quoted_legacy_system_apps_root, sizeof(quoted_legacy_system_apps_root));
    shell_quote(kSystemOuterWebappsRoot, quoted_new_root, sizeof(quoted_new_root));
    shell_quote(new_system_apps_root, quoted_new_system_apps_root, sizeof(quoted_new_system_apps_root));

    char script_template[] = "/tmp/homescreen-root-migration-XXXXXX";
    int script_fd = mkstemp(script_template);
    if (script_fd < 0) {
        snprintf(message, message_size, "Failed to create privileged migration script: %s", strerror(errno));
        return false;
    }
    FILE *script = fdopen(script_fd, "w");
    if (!script) {
        close(script_fd);
        unlink(script_template);
        snprintf(message, message_size, "Failed to write privileged migration script: %s", strerror(errno));
        return false;
    }
    fprintf(script,
            "set -eu\n"
            "mkdir -p %s\n"
            "OLD_ROOT=%s\n"
            "NEW_ROOT=%s\n"
            "OLD_SYSTEM_APPS=%s\n"
            "NEW_SYSTEM_APPS=%s\n"
            "OLD_DB=\"$OLD_ROOT/registry.sqlite3\"\n"
            "NEW_DB=\"$NEW_ROOT/registry.sqlite3\"\n"
            "OLD_OUTERCTL=%s\n"
            "OLD_OUTER_SHELL_OUTERCTL=%s\n"
            "NEW_OUTERCTL=%s\n"
            "OLD_USER_APPS=%s\n"
            "NEW_USER_APPS=%s\n"
            "mkdir -p \"$NEW_SYSTEM_APPS\"\n"
            "if [ -d \"$OLD_SYSTEM_APPS\" ]; then\n"
            "  for child in \"$OLD_SYSTEM_APPS\"/*; do\n"
            "    [ -e \"$child\" ] || continue\n"
            "    name=$(basename \"$child\")\n"
            "    if [ ! -e \"$NEW_SYSTEM_APPS/$name\" ]; then mv \"$child\" \"$NEW_SYSTEM_APPS/$name\"; fi\n"
            "  done\n"
            "fi\n"
            "find \"$NEW_SYSTEM_APPS\" -name outeragent.log -type f -exec sh -c 'for path do mv \"$path\" \"$(dirname \"$path\")/backend.log\" 2>/dev/null || true; done' sh {} + 2>/dev/null || true\n"
            "export OLD_DB NEW_DB OLD_ROOT NEW_ROOT OLD_SYSTEM_APPS NEW_SYSTEM_APPS OLD_OUTERCTL OLD_OUTER_SHELL_OUTERCTL NEW_OUTERCTL OLD_USER_APPS NEW_USER_APPS\n"
            "python3 - <<'__HOMESCREEN_ROOT_MIGRATION__'\n"
            "import os, sqlite3, urllib.parse\n"
            "old_db = os.environ['OLD_DB']\n"
            "new_db = os.environ['NEW_DB']\n"
            "replacements = [\n"
            "    (os.environ['OLD_OUTERCTL'], os.environ['NEW_OUTERCTL']),\n"
            "    (os.environ['OLD_OUTER_SHELL_OUTERCTL'], os.environ['NEW_OUTERCTL']),\n"
            "    (os.environ['OLD_USER_APPS'], os.environ['NEW_USER_APPS']),\n"
            "    (os.environ['OLD_SYSTEM_APPS'], os.environ['NEW_SYSTEM_APPS']),\n"
            "    (os.environ['OLD_ROOT'], os.environ['NEW_ROOT']),\n"
            "    ('OUTERAGENT_ROOT', 'OUTERWEBAPPS_HOME'),\n"
            "    ('outeragent.log', 'backend.log'),\n"
            "]\n"
            "os.makedirs(os.path.dirname(new_db), exist_ok=True)\n"
            "db = sqlite3.connect(new_db)\n"
            "db.executescript('''\n"
            "CREATE TABLE IF NOT EXISTS backends (service_id TEXT PRIMARY KEY, display_name TEXT NOT NULL DEFAULT '', icon TEXT, icon_path TEXT, service_unit TEXT);\n"
            "CREATE TABLE IF NOT EXISTS frontends (url TEXT PRIMARY KEY, service_id TEXT, display_name TEXT NOT NULL DEFAULT '', port INTEGER NOT NULL DEFAULT 0, socket_path TEXT NOT NULL DEFAULT '', icon TEXT, icon_path TEXT, list TEXT);\n"
            "CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);\n"
            "CREATE TABLE IF NOT EXISTS frontend_layouts (url TEXT PRIMARY KEY, list TEXT NOT NULL DEFAULT '');\n"
            "CREATE TABLE IF NOT EXISTS log_files (path TEXT PRIMARY KEY, service_id TEXT NOT NULL);\n"
            "CREATE INDEX IF NOT EXISTS log_files_service_id_idx ON log_files(service_id);\n"
            "CREATE TABLE IF NOT EXISTS systemd_backends (service_id TEXT PRIMARY KEY, unit_name TEXT NOT NULL, scope TEXT NOT NULL DEFAULT 'user');\n"
            "CREATE TABLE IF NOT EXISTS launchd_backends (service_id TEXT PRIMARY KEY, plist_path TEXT NOT NULL, owns_plist INTEGER NOT NULL DEFAULT 0);\n"
            "CREATE TABLE IF NOT EXISTS file_openers (extension TEXT NOT NULL, service_id TEXT NOT NULL, display_name TEXT NOT NULL DEFAULT '', socket_path TEXT NOT NULL DEFAULT '', url_template TEXT NOT NULL DEFAULT '?file={file}', rank INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(extension, service_id));\n"
            "CREATE INDEX IF NOT EXISTS file_openers_extension_idx ON file_openers(extension, rank, display_name);\n"
            "CREATE INDEX IF NOT EXISTS file_openers_service_id_idx ON file_openers(service_id);\n"
            "''')\n"
            "def open_old_registry(path):\n"
            "    uri = 'file:' + urllib.parse.quote(path) + '?mode=ro&immutable=1'\n"
            "    old = sqlite3.connect(uri, uri=True)\n"
            "    old.row_factory = sqlite3.Row\n"
            "    return old\n"
            "def old_columns(old, table):\n"
            "    try:\n"
            "        return {row['name'] for row in old.execute(f'PRAGMA table_info({table})')}\n"
            "    except sqlite3.OperationalError:\n"
            "        return set()\n"
            "def old_rows(old, table):\n"
            "    try:\n"
            "        return old.execute(f'SELECT * FROM {table}')\n"
            "    except sqlite3.OperationalError:\n"
            "        return []\n"
            "if os.path.exists(old_db):\n"
            "    old = open_old_registry(old_db)\n"
            "    try:\n"
            "        for row in old_rows(old, 'backends'):\n"
            "            icon = row['icon'] if 'icon' in row.keys() and row['icon'] is not None else None\n"
            "            icon_path = icon if icon and not str(icon).startswith('data:') else None\n"
            "            db.execute('INSERT OR REPLACE INTO backends(service_id, display_name, icon, icon_path, service_unit) VALUES (?, ?, ?, ?, ?)',\n"
            "                       (row['service_id'], row['display_name'] if 'display_name' in row.keys() and row['display_name'] is not None else '', icon, icon_path, row['service_unit'] if 'service_unit' in row.keys() else None))\n"
            "        for row in old_rows(old, 'frontends'):\n"
            "            icon = row['icon'] if 'icon' in row.keys() and row['icon'] is not None else None\n"
            "            icon_path = icon if icon and not str(icon).startswith('data:') else None\n"
            "            display_name = row['display_name'] if 'display_name' in row.keys() and row['display_name'] is not None else (row['name'] if 'name' in row.keys() and row['name'] is not None else '')\n"
            "            db.execute('INSERT OR REPLACE INTO frontends(url, service_id, display_name, port, socket_path, icon, icon_path, list) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',\n"
            "                       (row['url'], row['service_id'] if 'service_id' in row.keys() else None, display_name, row['port'] if 'port' in row.keys() and row['port'] is not None else 0, row['socket_path'] if 'socket_path' in row.keys() and row['socket_path'] is not None else '', icon, icon_path, row['list'] if 'list' in row.keys() else None))\n"
            "            db.execute('INSERT OR REPLACE INTO frontend_layouts(url, list) VALUES (?, ?)',\n"
            "                       (row['url'], row['list'] if 'list' in row.keys() and row['list'] is not None else ''))\n"
            "        for row in old_rows(old, 'log_files'):\n"
            "            db.execute('INSERT OR REPLACE INTO log_files(path, service_id) VALUES (?, ?)', (row['path'], row['service_id']))\n"
            "        for row in old_rows(old, 'systemd_backends'):\n"
            "            db.execute('INSERT OR REPLACE INTO systemd_backends(service_id, unit_name, scope) VALUES (?, ?, ?)',\n"
            "                       (row['service_id'], row['unit_name'], row['scope'] if 'scope' in row.keys() and row['scope'] is not None else 'system'))\n"
            "        for row in old_rows(old, 'launchd_backends'):\n"
            "            db.execute('INSERT OR REPLACE INTO launchd_backends(service_id, plist_path, owns_plist) VALUES (?, ?, ?)',\n"
            "                       (row['service_id'], row['plist_path'], row['owns_plist'] if 'owns_plist' in row.keys() and row['owns_plist'] is not None else 1))\n"
            "        for row in old_rows(old, 'file_openers'):\n"
            "            db.execute('INSERT OR REPLACE INTO file_openers(extension, service_id, display_name, socket_path, url_template, rank) VALUES (?, ?, ?, ?, ?, ?)',\n"
            "                       (row['extension'], row['service_id'], row['display_name'] if 'display_name' in row.keys() and row['display_name'] is not None else '', row['socket_path'] if 'socket_path' in row.keys() and row['socket_path'] is not None else '', row['url_template'] if 'url_template' in row.keys() and row['url_template'] is not None else '?file={file}', row['rank'] if 'rank' in row.keys() and row['rank'] is not None else 0))\n"
            "    finally:\n"
            "        old.close()\n"
            "for old, new in replacements:\n"
            "    if not old:\n"
            "        continue\n"
            "    for sql in [\n"
            "        'UPDATE log_files SET path = replace(path, ?, ?)',\n"
            "        'UPDATE frontends SET url = replace(url, ?, ?), socket_path = replace(socket_path, ?, ?)',\n"
            "        'UPDATE file_openers SET socket_path = replace(socket_path, ?, ?)',\n"
            "        'UPDATE launchd_backends SET plist_path = replace(plist_path, ?, ?)',\n"
            "    ]:\n"
            "        try:\n"
            "            if sql.count('?') == 2:\n"
            "                db.execute(sql, (old, new))\n"
            "            else:\n"
            "                db.execute(sql, (old, new, old, new))\n"
            "        except sqlite3.OperationalError:\n"
            "            pass\n"
            "db.commit()\n"
            "db.close()\n"
            "for root in (os.environ['NEW_SYSTEM_APPS'], '/opt/outergroup', '/etc/systemd/system', '/Library/LaunchDaemons'):\n"
            "    if not os.path.isdir(root):\n"
            "        continue\n"
            "    for dirpath, _, filenames in os.walk(root):\n"
            "        for filename in filenames:\n"
            "            path = os.path.join(dirpath, filename)\n"
            "            try:\n"
            "                with open(path, 'r', encoding='utf-8') as f:\n"
            "                    text = f.read()\n"
            "            except (OSError, UnicodeDecodeError):\n"
            "                continue\n"
            "            new_text = text\n"
            "            for old, new in replacements:\n"
            "                if old:\n"
            "                    new_text = new_text.replace(old, new)\n"
            "            if new_text != text:\n"
            "                with open(path, 'w', encoding='utf-8') as f:\n"
            "                    f.write(new_text)\n"
            "__HOMESCREEN_ROOT_MIGRATION__\n"
            "chmod 0755 \"$NEW_ROOT\" >/dev/null 2>&1 || true\n"
            "if [ -d \"$OLD_ROOT\" ]; then mv \"$OLD_ROOT\" \"$OLD_ROOT.migrated.$(date +%%s)\" >/dev/null 2>&1 || true; fi\n"
            "chmod 0644 \"$NEW_DB\" >/dev/null 2>&1 || true\n"
            "if command -v launchctl >/dev/null 2>&1; then\n"
            "  for plist in /Library/LaunchDaemons/*.plist; do\n"
            "    [ -f \"$plist\" ] || continue\n"
            "    if grep -E -q 'outerwebapps|dev[.]outergroup|outergroup' \"$plist\" 2>/dev/null; then\n"
            "      label=$(/usr/libexec/PlistBuddy -c 'Print :Label' \"$plist\" 2>/dev/null || true)\n"
            "      [ -n \"$label\" ] || continue\n"
            "      launchctl bootout \"system/$label\" >/dev/null 2>&1 || true\n"
            "      launchctl bootstrap system \"$plist\" >/dev/null 2>&1 || true\n"
            "      launchctl kickstart -k \"system/$label\" >/dev/null 2>&1 || true\n"
            "    fi\n"
            "  done\n"
            "fi\n"
            "systemctl --system daemon-reload >/dev/null 2>&1 || true\n",
            quoted_new_root,
            quoted_legacy_system_root,
            quoted_new_root,
            quoted_legacy_system_apps_root,
            quoted_new_system_apps_root,
            quoted_old_outerctl,
            quoted_old_home_screen_outerctl,
            quoted_new_outerctl,
            quoted_old_user_apps,
            quoted_new_user_apps);
    fclose(script);
    chmod(script_template, 0700);
    bool ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
    unlink(script_template);
    if (ok) {
        snprintf(message, message_size, "Migrated old root registry and wrappers.");
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

static void parent_path_for(const char *path, char *parent, size_t parent_size) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path && path[0] ? path : "/");
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

static bool path_has_extension(const char *name, const char *extension) {
    if (!extension || !extension[0]) return true;
    char expected[64];
    const char *dot_extension = extension;
    if (extension[0] != '.') {
        snprintf(expected, sizeof(expected), ".%s", extension);
        dot_extension = expected;
    }
    size_t name_len = strlen(name);
    size_t extension_len = strlen(dot_extension);
    return name_len >= extension_len && strcasecmp(name + name_len - extension_len, dot_extension) == 0;
}

static int compare_file_picker_entries(const void *lhs, const void *rhs) {
    const FilePickerEntry *a = (const FilePickerEntry *)lhs;
    const FilePickerEntry *b = (const FilePickerEntry *)rhs;
    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    return strcasecmp(a->name, b->name);
}

static bool build_file_picker_entry_payload(const FilePickerEntry *entry, StringBuilder *payload) {
    if (!binary_append_zero(payload, 40)) return false;
    return binary_append_string_ref_at(payload, 0, entry->name) &&
           binary_append_string_ref_at(payload, 8, entry->path) &&
           binary_write_u32_at(payload, 16, entry->is_directory ? FILE_PICKER_FLAG_IS_DIRECTORY : 0) &&
           binary_write_u64_at(payload, 20, entry->size) &&
           binary_write_f64_at(payload, 28, entry->modified);
}

static void send_file_picker_response(int fd, const char *query) {
    char requested[PATH_MAX] = "";
    char extension[64] = "";
    char directories_only_raw[16] = "";
    char path[PATH_MAX];
    query_value(query, "path", requested, sizeof(requested));
    query_value(query, "extension", extension, sizeof(extension));
    query_value(query, "directoriesOnly", directories_only_raw, sizeof(directories_only_raw));
    bool directories_only = strcmp(directories_only_raw, "1") == 0 ||
                            strcasecmp(directories_only_raw, "true") == 0;
    expand_tilde_path(requested[0] ? requested : "~", path, sizeof(path));

    DIR *dir = opendir(path);
    if (!dir && directories_only) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s", path);
        while (strcmp(candidate, "/") != 0) {
            char parent[PATH_MAX];
            parent_path_for(candidate, parent, sizeof(parent));
            if (strcmp(parent, candidate) == 0) {
                break;
            }
            DIR *parent_dir = opendir(parent);
            if (parent_dir) {
                snprintf(path, sizeof(path), "%s", parent);
                dir = parent_dir;
                break;
            }
            snprintf(candidate, sizeof(candidate), "%s", parent);
        }
    }
    if (!dir) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "failed to open %s: %s\n", path, strerror(errno));
        send_text_response(fd, 404, message);
        return;
    }

    FilePickerEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[PATH_MAX];
        if (!join_child_path(path, entry->d_name, child_path, sizeof(child_path))) {
            continue;
        }

        struct stat st;
        if (stat(child_path, &st) != 0 && lstat(child_path, &st) != 0) {
            continue;
        }
        bool is_directory = S_ISDIR(st.st_mode);
        if (directories_only && !is_directory) {
            continue;
        }
        if (!is_directory && !path_has_extension(entry->d_name, extension)) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 64;
            FilePickerEntry *new_entries = realloc(entries, new_capacity * sizeof(FilePickerEntry));
            if (!new_entries) {
                free(entries);
                closedir(dir);
                send_text_response(fd, 500, "out of memory\n");
                return;
            }
            entries = new_entries;
            capacity = new_capacity;
        }

        FilePickerEntry *file = &entries[count++];
        memset(file, 0, sizeof(*file));
        snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
        snprintf(file->path, sizeof(file->path), "%s", child_path);
        file->is_directory = is_directory;
        file->size = (uint64_t)st.st_size;
        file->modified = (double)st.st_mtime;
        file->mode = st.st_mode;
    }
    closedir(dir);

    qsort(entries, count, sizeof(FilePickerEntry), compare_file_picker_entries);

    char parent[PATH_MAX];
    parent_path_for(path, parent, sizeof(parent));

    BinaryPayloadList entry_payloads = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        StringBuilder payload = {0};
        ok = build_file_picker_entry_payload(&entries[i], &payload) &&
             binary_payload_list_append(&entry_payloads, &payload);
        if (!ok) free(payload.data);
    }
    free(entries);

    StringBuilder builder = {0};
    size_t fixed_size = 20 + entry_payloads.count * 8;
    ok = ok && binary_append_zero(&builder, fixed_size) &&
         binary_write_u32_at(&builder, 16, (uint32_t)entry_payloads.count) &&
         binary_append_string_ref_at(&builder, 0, path) &&
         binary_append_string_ref_at(&builder, 8, parent);
    for (size_t i = 0; ok && i < entry_payloads.count; i++) {
        ok = binary_append_child_ref_at(&builder, 20 + i * 8, &entry_payloads.items[i]);
    }
    if (!ok) {
        free(builder.data);
        binary_payload_list_free(&entry_payloads);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_binary_response(fd, 200, &builder);
    free(builder.data);
    binary_payload_list_free(&entry_payloads);
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
        "        run_outerctl app add --backend \"$BACKEND_ID\" --frontend-id \"$BACKEND_ID:main\" --port \"$PORT\" --name \"$DISPLAY_NAME\" --url \"127.0.0.1:$PORT/\" --running\n"
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
        "import sys\n"
        "import tempfile\n"
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
        "\n"
        "def runtime_socket_directory():\n"
        "    if sys.platform == \"darwin\":\n"
        "        return tempfile.gettempdir()\n"
        "    runtime_dir = os.environ.get(\"XDG_RUNTIME_DIR\", \"\").strip()\n"
        "    if runtime_dir:\n"
        "        return runtime_dir\n"
        "    if sys.platform.startswith(\"linux\"):\n"
        "        return f\"/run/user/{os.getuid()}\"\n"
        "    return tempfile.gettempdir()\n"
        "\n"
        "SOCKET_DIRECTORY = runtime_socket_directory()\n"
        "SOCKET_PATH = os.path.join(SOCKET_DIRECTORY, BACKEND_ID)\n"
        "\n"
        "child = None\n"
        "\n"
        "def log(message):\n"
        "    print(f\"[OuterShell Jupyter] {message}\", flush=True)\n"
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
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--frontend-id\", f\"{BACKEND_ID}:main\", \"--socket-path\", SOCKET_PATH, \"--name\", DISPLAY_NAME, \"--url\", app_url, \"--running\"]\n"
        "                else:\n"
        "                    port, app_url, icon_path = discovered\n"
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--frontend-id\", f\"{BACKEND_ID}:main\", \"--port\", port, \"--name\", DISPLAY_NAME, \"--url\", app_url, \"--running\"]\n"
        "                add_args.extend([\"--list\", \"Jupyter\"])\n"
        "                if icon_path:\n"
        "                    add_args.extend([\"--icon-path\", icon_path])\n"
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
                                     const char *frontend_id,
                                     const char *frontend_url,
                                     int frontend_port,
                                     const char *frontend_socket_path,
                                     const char *frontend_list,
                                     char *error,
                                     size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        close_registry_readwrite(database, false, error, error_size);
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
            "INSERT INTO backends(service_id, display_name, icon_path, service_unit) VALUES(?, ?, NULL, ?);";
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
    if (ok && frontend_id && frontend_id[0]) {
        const char *sql =
            "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, list, running) "
            "VALUES(?, ?, ?, ?, ?, ?, NULL, NULLIF(?, ''), 0) "
            "ON CONFLICT(frontend_id) DO UPDATE SET url=excluded.url, service_id=excluded.service_id, display_name=excluded.display_name, port=excluded.port, socket_path=excluded.socket_path, list=excluded.list, running=0;";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(statement, 1, frontend_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, frontend_url ? frontend_url : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, service_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, display_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 5, frontend_port > 0 ? frontend_port : 0);
            sqlite3_bind_text(statement, 6, frontend_socket_path ? frontend_socket_path : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 7, frontend_list ? frontend_list : "", -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
        if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
        statement = NULL;
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, error_size);
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    return close_registry_readwrite(database, ok, error, error_size) && ok;
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

static bool outerctl_tsv_field(StringBuilder *out, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p; p++) {
        if (*p == '\t') {
            if (!sb_append(out, "\\t")) return false;
        } else if (*p == '\n') {
            if (!sb_append(out, "\\n")) return false;
        } else if (*p == '\r') {
            if (!sb_append(out, "\\r")) return false;
        } else if (*p == '\\') {
            if (!sb_append(out, "\\\\")) return false;
        } else if (!sb_append_n(out, (const char *)p, 1)) {
            return false;
        }
    }
    return true;
}

static bool outerctl_print_query(sqlite3 *database,
                                 const char *sql,
                                 const char *backend_filter,
                                 StringBuilder *out,
                                 char *error,
                                 size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        if (backend_filter && backend_filter[0]) {
            sqlite3_bind_text(statement, 1, backend_filter, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(statement, 1);
        }
    }
    if (!ok) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    int column_count = sqlite3_column_count(statement);
    for (int i = 0; i < column_count; i++) {
        if (i > 0 && !sb_append(out, "\t")) ok = false;
        if (ok && !outerctl_tsv_field(out, sqlite3_column_name(statement, i))) ok = false;
    }
    if (ok && !sb_append(out, "\n")) ok = false;

    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(statement)) == SQLITE_ROW) {
        for (int i = 0; i < column_count; i++) {
            if (i > 0 && !sb_append(out, "\t")) ok = false;
            if (ok && !outerctl_tsv_field(out, sqlite_column_text_or_empty(statement, i))) ok = false;
        }
        if (ok && !sb_append(out, "\n")) ok = false;
    }
    if (ok && step != SQLITE_DONE) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        ok = false;
    }
    if (!ok && !error[0]) {
        snprintf(error, error_size, "out of memory");
    }
    sqlite3_finalize(statement);
    return ok;
}

static bool outerctl_backend_exists(sqlite3 *database, const char *service_id, bool *exists, char *error, size_t error_size) {
    if (exists) *exists = false;
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, "SELECT 1 FROM backends WHERE service_id = ? LIMIT 1;", -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            if (exists) *exists = true;
        } else if (step != SQLITE_DONE) {
            ok = false;
        }
    }
    if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool outerctl_require_backend(sqlite3 *database, const char *service_id, char *error, size_t error_size) {
    bool exists = false;
    if (!outerctl_backend_exists(database, service_id, &exists, error, error_size)) return false;
    if (!exists) {
        snprintf(error, error_size, "Backend not registered. Run outerctl backend upsert first.");
        return false;
    }
    return true;
}

static bool outerctl_count_rows(sqlite3 *database, const char *sql, const char *service_id, int *count, char *error, size_t error_size) {
    if (count) *count = 0;
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            if (count) *count = sqlite3_column_int(statement, 0);
        } else {
            ok = false;
        }
    }
    if (!ok) snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool outerctl_make_frontend_url(const char *raw_url,
                                       int port,
                                       const char *socket_path,
                                       char *out,
                                       size_t out_size) {
    const char *safe_socket_path = socket_path ? socket_path : "";
    const char *safe_url = raw_url ? raw_url : "";
    if (safe_url[0]) {
        snprintf(out, out_size, "%s", safe_url);
    } else if (safe_socket_path[0]) {
        snprintf(out, out_size, "%s", safe_socket_path);
    } else if (port > 0) {
        snprintf(out, out_size, "http://127.0.0.1:%d", port);
    } else {
        return false;
    }

    const char *scheme = strstr(out, "://");
    const char *authority = scheme ? scheme + 3 : out;
    const char *slash = strchr(authority, '/');
    const char *query = strchr(authority, '?');
    const char *fragment = strchr(authority, '#');
    const char *suffix = query;
    if (!suffix || (fragment && fragment < suffix)) suffix = fragment;
    if (slash && (!suffix || slash < suffix)) return true;
    if (!suffix) {
        size_t len = strlen(out);
        if (len + 1 >= out_size) return false;
        out[len] = '/';
        out[len + 1] = '\0';
        return true;
    }

    char copy[PATH_MAX * 2];
    snprintf(copy, sizeof(copy), "%s", out);
    size_t prefix_len = (size_t)(suffix - out);
    if (prefix_len + strlen(copy + prefix_len) + 2 > out_size) return false;
    memmove(out + prefix_len + 1, copy + prefix_len, strlen(copy + prefix_len) + 1);
    out[prefix_len] = '/';
    return true;
}

static bool normalize_file_extension(const char *raw, char *out, size_t out_size) {
    if (!raw || !raw[0] || !out || out_size == 0) return false;
    while (*raw == '.') raw++;
    if (!raw[0]) return false;
    size_t offset = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && offset + 1 < out_size; p++) {
        if (*p == '/' || *p == '\\' || *p == '?' || *p == '#') return false;
        out[offset++] = (char)tolower(*p);
    }
    if (raw[offset] != '\0') return false;
    out[offset] = '\0';
    return offset > 0;
}

static bool normalize_opener_kind(const char *raw, char *out, size_t out_size) {
    if (!raw || !raw[0] || !out || out_size == 0) return false;
    size_t offset = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && offset + 1 < out_size; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_') return false;
        out[offset++] = (char)tolower(*p);
    }
    if (raw[offset] != '\0') return false;
    out[offset] = '\0';
    return offset > 0;
}

static bool make_file_opener_kind_key(const char *raw_kind, char *out, size_t out_size) {
    char normalized[128];
    if (!normalize_opener_kind(raw_kind, normalized, sizeof(normalized))) return false;
    int written = snprintf(out, out_size, "kind:%s", normalized);
    return written > 0 && (size_t)written < out_size;
}

static bool append_file_opener_url(StringBuilder *out,
                                   const char *socket_path,
                                   const char *url_template,
                                   const char *file_path) {
    const char *safe_socket_path = socket_path ? socket_path : "";
    const char *safe_template = (url_template && url_template[0]) ? url_template : "?file={file}";
    if (safe_socket_path[0]) {
        if (!sb_append(out, safe_socket_path)) return false;
        if (safe_template[0] == '?') {
            if (!sb_append(out, "/")) return false;
        } else if (safe_template[0] != '/') {
            if (!sb_append(out, "/")) return false;
        }
    }
    const char *cursor = safe_template;
    const char *placeholder = "{file}";
    size_t placeholder_len = strlen(placeholder);
    while (*cursor) {
        const char *match = strstr(cursor, placeholder);
        if (!match) {
            return sb_append(out, cursor);
        }
        if (!sb_append_n(out, cursor, (size_t)(match - cursor))) return false;
        append_url_encoded(out, file_path ? file_path : "");
        cursor = match + placeholder_len;
    }
    return true;
}

static bool outerctl_print_file_openers(sqlite3 *database,
                                        const char *backend_filter,
                                        const char *extension_filter,
                                        const char *file_path,
                                        StringBuilder *out,
                                        char *error,
                                        size_t error_size) {
    const char *sql =
        "SELECT extension, service_id, display_name, socket_path, url_template, rank "
        "FROM file_openers "
        "WHERE (?1 IS NULL OR service_id = ?1) AND (?2 IS NULL OR extension = ?2) "
        "ORDER BY extension, rank, display_name, service_id;";
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        if (backend_filter && backend_filter[0]) sqlite3_bind_text(statement, 1, backend_filter, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(statement, 1);
        if (extension_filter && extension_filter[0]) sqlite3_bind_text(statement, 2, extension_filter, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(statement, 2);
    }
    if (!ok) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        if (statement) sqlite3_finalize(statement);
        return false;
    }

    const char *headers[] = {"extension", "service_id", "display_name", "socket_path", "url_template", "rank", "url"};
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
        if (i > 0 && !sb_append(out, "\t")) ok = false;
        if (ok && !outerctl_tsv_field(out, headers[i])) ok = false;
    }
    if (ok && !sb_append(out, "\n")) ok = false;

    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(statement)) == SQLITE_ROW) {
        for (int i = 0; i < 6; i++) {
            if (i > 0 && !sb_append(out, "\t")) ok = false;
            if (ok && !outerctl_tsv_field(out, sqlite_column_text_or_empty(statement, i))) ok = false;
        }
        if (ok && !sb_append(out, "\t")) ok = false;
        if (ok && file_path && file_path[0]) {
            StringBuilder url = {0};
            ok = append_file_opener_url(&url,
                                        sqlite_column_text_or_empty(statement, 3),
                                        sqlite_column_text_or_empty(statement, 4),
                                        file_path) &&
                 outerctl_tsv_field(out, url.data ? url.data : "");
            free(url.data);
        }
        if (ok && !sb_append(out, "\n")) ok = false;
    }
    if (ok && step != SQLITE_DONE) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        ok = false;
    }
    if (!ok && !error[0]) snprintf(error, error_size, "out of memory");
    sqlite3_finalize(statement);
    return ok;
}

static int outershelld_handle_outerctl(int argc, char **argv, StringBuilder *stdout_buffer, StringBuilder *stderr_buffer) {
    if (argc < 3) {
        sb_append(stderr_buffer, "Usage: outerctl <resource> <action> [options]\n");
        return 1;
    }

    const char *resource = argv[1];
    const char *action = argv[2];
    const char *backend = NULL;
    const char *display_name = NULL;
    const char *plist_path = NULL;
    const char *systemd_unit = NULL;
    const char *log_path = NULL;
    const char *url = NULL;
    const char *frontend_id = NULL;
    const char *icon_path = NULL;
    const char *frontend_list = NULL;
    const char *socket_path = NULL;
    const char *extension = NULL;
    const char *opener_kind = NULL;
    const char *url_template = NULL;
    const char *file_path = NULL;
    int port = 0;
    int rank = 0;
    bool has_port = false;
    bool frontend_running = true;
    bool owns_plist = false;
    bool include_icons = false;

    for (int i = 3; i < argc; i++) {
        const char *arg = argv[i];
#define REQUIRE_VALUE(name, target) do { \
            if (i + 1 >= argc) { \
                sb_append(stderr_buffer, "Missing value for " name "\n"); \
                return 1; \
            } \
            target = argv[++i]; \
        } while (0)
        if (strcmp(arg, "--backend") == 0) {
            REQUIRE_VALUE("--backend", backend);
        } else if (strcmp(arg, "--name") == 0) {
            REQUIRE_VALUE("--name", display_name);
        } else if (strcmp(arg, "--plist") == 0) {
            REQUIRE_VALUE("--plist", plist_path);
        } else if (strcmp(arg, "--unit") == 0) {
            REQUIRE_VALUE("--unit", systemd_unit);
        } else if (strcmp(arg, "--path") == 0) {
            REQUIRE_VALUE("--path", log_path);
        } else if (strcmp(arg, "--url") == 0) {
            REQUIRE_VALUE("--url", url);
        } else if (strcmp(arg, "--frontend-id") == 0 || strcmp(arg, "--id") == 0) {
            REQUIRE_VALUE("--frontend-id", frontend_id);
        } else if (strcmp(arg, "--icon-path") == 0 || strcmp(arg, "--icon-file") == 0) {
            REQUIRE_VALUE("--icon-path", icon_path);
        } else if (strcmp(arg, "--list") == 0) {
            REQUIRE_VALUE("--list", frontend_list);
        } else if (strcmp(arg, "--socket-path") == 0) {
            REQUIRE_VALUE("--socket-path", socket_path);
        } else if (strcmp(arg, "--extension") == 0 || strcmp(arg, "--ext") == 0) {
            REQUIRE_VALUE("--extension", extension);
        } else if (strcmp(arg, "--kind") == 0) {
            REQUIRE_VALUE("--kind", opener_kind);
        } else if (strcmp(arg, "--url-template") == 0) {
            REQUIRE_VALUE("--url-template", url_template);
        } else if (strcmp(arg, "--file") == 0) {
            REQUIRE_VALUE("--file", file_path);
        } else if (strcmp(arg, "--port") == 0) {
            const char *raw_port = NULL;
            REQUIRE_VALUE("--port", raw_port);
            port = atoi(raw_port);
            has_port = port > 0 && port <= 65535;
            if (!has_port) {
                sb_append(stderr_buffer, "Invalid port.\n");
                return 1;
            }
        } else if (strcmp(arg, "--rank") == 0) {
            const char *raw_rank = NULL;
            REQUIRE_VALUE("--rank", raw_rank);
            char *end = NULL;
            long value = strtol(raw_rank, &end, 10);
            if (!end || *end != '\0' || value < 0 || value > INT32_MAX) {
                sb_append(stderr_buffer, "Invalid rank.\n");
                return 1;
            }
            rank = (int)value;
        } else if (strcmp(arg, "--owns-plist") == 0) {
            const char *raw = NULL;
            REQUIRE_VALUE("--owns-plist", raw);
            owns_plist = strcmp(raw, "true") == 0 || strcmp(raw, "1") == 0 || strcmp(raw, "yes") == 0;
        } else if (strcmp(arg, "--icons") == 0) {
            include_icons = true;
        } else if (strcmp(arg, "--running") == 0) {
            frontend_running = true;
        } else if (strcmp(arg, "--stopped") == 0) {
            frontend_running = false;
        } else {
            sb_append(stderr_buffer, "Unknown argument: ");
            sb_append(stderr_buffer, arg);
            sb_append(stderr_buffer, "\n");
            return 1;
        }
#undef REQUIRE_VALUE
    }

    bool is_list = strcmp(action, "list") == 0;
    if (!is_list && (!backend || !backend[0])) {
        sb_append(stderr_buffer, "Missing backend identifier.\n");
        return 1;
    }

    char error[2048] = "";
    sqlite3 *database = is_list ? open_registry_readonly(error, sizeof(error)) : open_registry_readwrite(error, sizeof(error));
    if (!database) {
        sb_append(stderr_buffer, error[0] ? error : "Failed to open registry.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }
    if (!ensure_registry_schema(database, error, sizeof(error))) {
        if (is_list) sqlite3_close(database);
        else close_registry_readwrite(database, false, error, sizeof(error));
        sb_append(stderr_buffer, error[0] ? error : "Failed to prepare registry.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }

    bool ok = true;
    bool changed = false;

    if (is_list) {
        if (strcmp(resource, "backend") == 0) {
            const char *sql = include_icons ?
                "SELECT b.service_id, COALESCE(b.display_name, '') AS display_name, COALESCE(b.icon_path, '') AS icon_path, COALESCE(s.unit_name, b.service_unit, '') AS unit_name, COALESCE(l.plist_path, '') AS unit_path, COALESCE(l.owns_plist, CASE WHEN COALESCE(s.unit_name, b.service_unit, '') != '' THEN 1 ELSE 0 END) AS owns_unit FROM backends b LEFT JOIN systemd_backends s ON s.service_id = b.service_id LEFT JOIN launchd_backends l ON l.service_id = b.service_id WHERE (?1 IS NULL OR b.service_id = ?1) ORDER BY b.service_id;" :
                "SELECT b.service_id, COALESCE(b.display_name, '') AS display_name, COALESCE(b.icon_path, '') AS icon_path, COALESCE(s.unit_name, b.service_unit, '') AS unit_name, COALESCE(l.plist_path, '') AS unit_path, COALESCE(l.owns_plist, CASE WHEN COALESCE(s.unit_name, b.service_unit, '') != '' THEN 1 ELSE 0 END) AS owns_unit FROM backends b LEFT JOIN systemd_backends s ON s.service_id = b.service_id LEFT JOIN launchd_backends l ON l.service_id = b.service_id WHERE (?1 IS NULL OR b.service_id = ?1) ORDER BY b.service_id;";
            ok = outerctl_print_query(database, sql, backend, stdout_buffer, error, sizeof(error));
        } else if (strcmp(resource, "app") == 0) {
            const char *sql = include_icons ?
                "SELECT f.frontend_id, f.url, COALESCE(f.service_id, '') AS service_id, COALESCE(f.display_name, '') AS display_name, f.port, COALESCE(f.socket_path, '') AS socket_path, COALESCE(f.icon_path, '') AS icon_path, COALESCE(fl.list, flu.list, f.list, '') AS list, COALESCE(f.running, 0) AS running FROM frontends f LEFT JOIN frontend_layouts fl ON fl.frontend_id = f.frontend_id LEFT JOIN frontend_layouts flu ON flu.url = f.url WHERE (?1 IS NULL OR f.service_id = ?1) ORDER BY f.service_id, COALESCE(fl.list, flu.list, f.list, ''), f.display_name, f.url;" :
                "SELECT f.frontend_id, f.url, COALESCE(f.service_id, '') AS service_id, COALESCE(f.display_name, '') AS display_name, f.port, COALESCE(f.socket_path, '') AS socket_path, COALESCE(f.icon_path, '') AS icon_path, COALESCE(fl.list, flu.list, f.list, '') AS list, COALESCE(f.running, 0) AS running FROM frontends f LEFT JOIN frontend_layouts fl ON fl.frontend_id = f.frontend_id LEFT JOIN frontend_layouts flu ON flu.url = f.url WHERE (?1 IS NULL OR f.service_id = ?1) ORDER BY f.service_id, COALESCE(fl.list, flu.list, f.list, ''), f.display_name, f.url;";
            ok = outerctl_print_query(database, sql, backend, stdout_buffer, error, sizeof(error));
        } else if (strcmp(resource, "log") == 0) {
            ok = outerctl_print_query(database, "SELECT path, service_id FROM log_files WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id, path;", backend, stdout_buffer, error, sizeof(error));
        } else if (strcmp(resource, "systemd") == 0) {
            ok = outerctl_print_query(database, "SELECT service_id, unit_name FROM systemd_backends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id;", backend, stdout_buffer, error, sizeof(error));
        } else if (strcmp(resource, "launchd") == 0) {
            ok = outerctl_print_query(database, "SELECT service_id, plist_path, owns_plist FROM launchd_backends WHERE (?1 IS NULL OR service_id = ?1) ORDER BY service_id;", backend, stdout_buffer, error, sizeof(error));
        } else if (strcmp(resource, "opener") == 0) {
            char normalized_extension[128] = "";
            char opener_key[160] = "";
            if (extension && extension[0] && opener_kind && opener_kind[0]) {
                snprintf(error, sizeof(error), "Specify either opener extension or kind, not both.");
                ok = false;
            } else if (extension && extension[0]) {
                if (!normalize_file_extension(extension, normalized_extension, sizeof(normalized_extension))) {
                    snprintf(error, sizeof(error), "Invalid file extension.");
                    ok = false;
                } else {
                    snprintf(opener_key, sizeof(opener_key), "%s", normalized_extension);
                }
            } else if (opener_kind && opener_kind[0]) {
                if (!make_file_opener_kind_key(opener_kind, opener_key, sizeof(opener_key))) {
                    snprintf(error, sizeof(error), "Invalid opener kind.");
                    ok = false;
                }
            }
            if (ok) {
                ok = outerctl_print_file_openers(database,
                                                 backend,
                                                 opener_key[0] ? opener_key : NULL,
                                                 file_path,
                                                 stdout_buffer,
                                                 error,
                                                 sizeof(error));
            }
        } else {
            snprintf(error, sizeof(error), "Unknown registry resource.");
            ok = false;
        }
        sqlite3_close(database);
        if (!ok) {
            sb_append(stderr_buffer, error[0] ? error : "Failed to list registry rows.");
            sb_append(stderr_buffer, "\n");
            return 1;
        }
        return 0;
    }

    ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, sizeof(error));
    if (ok && strcmp(resource, "backend") == 0) {
        if (strcmp(action, "upsert") == 0) {
            ok = bind_and_step(database,
                               "INSERT INTO backends(service_id, display_name, icon_path, service_unit) VALUES(?, ?, NULLIF(?, ''), NULL) "
                               "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon_path=excluded.icon_path;",
                               backend,
                               (display_name && display_name[0]) ? display_name : backend,
                               icon_path ? icon_path : "",
                               NULL,
                               error,
                               sizeof(error));
            changed = ok;
        } else if (strcmp(action, "remove") == 0) {
            int systemd_count = 0, launchd_count = 0, log_count = 0, frontend_count = 0, opener_count = 0;
            bool exists = false;
            ok = outerctl_backend_exists(database, backend, &exists, error, sizeof(error));
            if (ok && !exists) {
                snprintf(error, sizeof(error), "Backend not registered.");
                ok = false;
            }
            if (ok) ok = outerctl_count_rows(database, "SELECT COUNT(*) FROM systemd_backends WHERE service_id = ?;", backend, &systemd_count, error, sizeof(error));
            if (ok) ok = outerctl_count_rows(database, "SELECT COUNT(*) FROM launchd_backends WHERE service_id = ?;", backend, &launchd_count, error, sizeof(error));
            if (ok) ok = outerctl_count_rows(database, "SELECT COUNT(*) FROM log_files WHERE service_id = ?;", backend, &log_count, error, sizeof(error));
            if (ok) ok = outerctl_count_rows(database, "SELECT COUNT(*) FROM frontends WHERE service_id = ?;", backend, &frontend_count, error, sizeof(error));
            if (ok) ok = outerctl_count_rows(database, "SELECT COUNT(*) FROM file_openers WHERE service_id = ?;", backend, &opener_count, error, sizeof(error));
            if (ok && (systemd_count || launchd_count || log_count || frontend_count || opener_count)) {
                snprintf(error, sizeof(error), "Backend still has service-manager, log, app, or opener records. Clear those first.");
                ok = false;
            }
            if (ok) {
                ok = bind_and_step(database, "DELETE FROM backends WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error));
                changed = ok;
            }
        } else {
            snprintf(error, sizeof(error), "Unknown backend action.");
            ok = false;
        }
    } else if (ok) {
        ok = outerctl_require_backend(database, backend, error, sizeof(error));
    }

    if (ok && strcmp(resource, "systemd") == 0) {
        if (strcmp(action, "set") == 0) {
            if (!systemd_unit || !systemd_unit[0]) {
                snprintf(error, sizeof(error), "Missing systemd unit name.");
                ok = false;
            } else {
                ok = bind_and_step(database,
                                   "INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, 'user') "
                                   "ON CONFLICT(service_id) DO UPDATE SET unit_name=excluded.unit_name;",
                                   backend, systemd_unit, NULL, NULL, error, sizeof(error)) &&
                     bind_and_step(database, "UPDATE backends SET service_unit = ? WHERE service_id = ?;", systemd_unit, backend, NULL, NULL, error, sizeof(error));
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            ok = bind_and_step(database, "DELETE FROM systemd_backends WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error)) &&
                 bind_and_step(database, "UPDATE backends SET service_unit = NULL WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error));
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown systemd action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "launchd") == 0) {
        if (strcmp(action, "set") == 0) {
            if (!plist_path || !plist_path[0]) {
                snprintf(error, sizeof(error), "Missing launchd plist path.");
                ok = false;
            } else {
                sqlite3_stmt *statement = NULL;
                ok = sqlite3_prepare_v2(database,
                                        "INSERT INTO launchd_backends(service_id, plist_path, owns_plist) VALUES(?, ?, ?) "
                                        "ON CONFLICT(service_id) DO UPDATE SET plist_path=excluded.plist_path, owns_plist=excluded.owns_plist;",
                                        -1, &statement, NULL) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_text(statement, 1, backend, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 2, plist_path, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(statement, 3, owns_plist ? 1 : 0);
                    ok = sqlite3_step(statement) == SQLITE_DONE;
                }
                if (!ok) snprintf(error, sizeof(error), "%s", sqlite3_errmsg(database));
                if (statement) sqlite3_finalize(statement);
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            ok = bind_and_step(database, "DELETE FROM launchd_backends WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error));
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown launchd action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "log") == 0) {
        if (strcmp(action, "add") == 0) {
            if (!log_path || !log_path[0]) {
                snprintf(error, sizeof(error), "Missing log path.");
                ok = false;
            } else {
                ok = bind_and_step(database,
                                   "INSERT INTO log_files(path, service_id) VALUES(?, ?) "
                                   "ON CONFLICT(path) DO UPDATE SET service_id=excluded.service_id;",
                                   log_path, backend, NULL, NULL, error, sizeof(error));
                changed = ok;
            }
        } else if (strcmp(action, "remove") == 0) {
            if (!log_path || !log_path[0]) {
                snprintf(error, sizeof(error), "Missing log path.");
                ok = false;
            } else {
                ok = bind_and_step(database, "DELETE FROM log_files WHERE service_id = ? AND path = ?;", backend, log_path, NULL, NULL, error, sizeof(error));
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            ok = bind_and_step(database, "DELETE FROM log_files WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error));
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown log action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "app") == 0) {
        const bool has_socket = socket_path && socket_path[0];
        char stable_frontend_id[PATH_MAX * 2] = "";
        if (frontend_id && frontend_id[0]) {
            snprintf(stable_frontend_id, sizeof(stable_frontend_id), "%s", frontend_id);
        } else {
            snprintf(stable_frontend_id, sizeof(stable_frontend_id), "%s:main", backend);
        }
        if ((has_port ? 1 : 0) + (has_socket ? 1 : 0) > 1) {
            snprintf(error, sizeof(error), "Specify either --port or --socket-path, not both.");
            ok = false;
        } else if (strcmp(action, "add") == 0) {
            if ((!has_port && !has_socket && frontend_running) || !display_name || !display_name[0]) {
                snprintf(error, sizeof(error), "Missing app endpoint or display name.");
                ok = false;
            }
            char frontend_url[PATH_MAX * 2] = "";
            if (ok && (has_port || has_socket || (url && url[0])) &&
                !outerctl_make_frontend_url(url, port, socket_path, frontend_url, sizeof(frontend_url))) {
                snprintf(error, sizeof(error), "Could not build app URL.");
                ok = false;
            }
            if (ok) {
                sqlite3_stmt *statement = NULL;
                ok = sqlite3_prepare_v2(database,
                                        "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, list, running) VALUES(?, ?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), ?) "
                                        "ON CONFLICT(frontend_id) DO UPDATE SET url=excluded.url, service_id=excluded.service_id, display_name=excluded.display_name, port=excluded.port, socket_path=excluded.socket_path, icon_path=COALESCE(excluded.icon_path, frontends.icon_path), list=COALESCE(excluded.list, frontends.list), running=excluded.running;",
                                        -1, &statement, NULL) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_text(statement, 1, stable_frontend_id, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 2, frontend_url, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 3, backend, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 4, display_name, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(statement, 5, frontend_running ? port : 0);
                    sqlite3_bind_text(statement, 6, has_socket ? socket_path : "", -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 7, icon_path ? icon_path : "", -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 8, frontend_list ? frontend_list : "", -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(statement, 9, frontend_running ? 1 : 0);
                    ok = sqlite3_step(statement) == SQLITE_DONE;
                }
                if (!ok) snprintf(error, sizeof(error), "%s", sqlite3_errmsg(database));
                if (statement) sqlite3_finalize(statement);
            }
            if (ok) {
                ok = bind_and_step(database,
                                   "INSERT INTO frontend_layouts(url, list, frontend_id) VALUES(?, ?, NULLIF(?, '')) ON CONFLICT(url) DO UPDATE SET list=excluded.list, frontend_id=excluded.frontend_id;",
                                   frontend_url[0] ? frontend_url : stable_frontend_id, frontend_list ? frontend_list : "", stable_frontend_id, NULL, error, sizeof(error));
            }
            changed = ok;
        } else if (strcmp(action, "remove") == 0) {
            if (!frontend_id && !has_port && !has_socket) {
                snprintf(error, sizeof(error), "Missing app endpoint.");
                ok = false;
            } else if (frontend_id && frontend_id[0]) {
                ok = bind_and_step(database, "DELETE FROM frontends WHERE service_id = ? AND frontend_id = ?;", backend, frontend_id, NULL, NULL, error, sizeof(error));
                changed = ok;
            } else if (has_socket) {
                ok = bind_and_step(database, "DELETE FROM frontends WHERE service_id = ? AND socket_path = ?;", backend, socket_path, NULL, NULL, error, sizeof(error));
                changed = ok;
            } else {
                sqlite3_stmt *statement = NULL;
                ok = sqlite3_prepare_v2(database, "DELETE FROM frontends WHERE service_id = ? AND port = ?;", -1, &statement, NULL) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_text(statement, 1, backend, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(statement, 2, port);
                    ok = sqlite3_step(statement) == SQLITE_DONE;
                }
                if (!ok) snprintf(error, sizeof(error), "%s", sqlite3_errmsg(database));
                if (statement) sqlite3_finalize(statement);
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            ok = bind_and_step(database, "UPDATE frontends SET running = 0, url = '', port = 0 WHERE service_id = ?;", backend, NULL, NULL, NULL, error, sizeof(error));
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown app action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "opener") == 0) {
        char opener_key[160] = "";
        if (strcmp(action, "clear") != 0) {
            if (extension && extension[0] && opener_kind && opener_kind[0]) {
                snprintf(error, sizeof(error), "Specify either opener extension or kind, not both.");
                ok = false;
            } else if (extension && extension[0]) {
                if (!normalize_file_extension(extension, opener_key, sizeof(opener_key))) {
                    snprintf(error, sizeof(error), "Invalid file extension.");
                    ok = false;
                }
            } else if (opener_kind && opener_kind[0]) {
                if (!make_file_opener_kind_key(opener_kind, opener_key, sizeof(opener_key))) {
                    snprintf(error, sizeof(error), "Invalid opener kind.");
                    ok = false;
                }
            } else {
                snprintf(error, sizeof(error), "Missing opener extension or kind.");
                ok = false;
            }
        }
        if (ok && strcmp(action, "add") == 0) {
            if (!display_name || !display_name[0]) {
                snprintf(error, sizeof(error), "Missing opener display name.");
                ok = false;
            } else if (!socket_path || !socket_path[0]) {
                snprintf(error, sizeof(error), "Missing opener socket path.");
                ok = false;
            } else {
                sqlite3_stmt *statement = NULL;
                ok = sqlite3_prepare_v2(database,
                                        "INSERT INTO file_openers(extension, service_id, display_name, socket_path, url_template, rank) VALUES(?, ?, ?, ?, ?, ?) "
                                        "ON CONFLICT(extension, service_id) DO UPDATE SET display_name=excluded.display_name, socket_path=excluded.socket_path, url_template=excluded.url_template, rank=excluded.rank;",
                                        -1,
                                        &statement,
                                        NULL) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_text(statement, 1, opener_key, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 2, backend, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 3, display_name, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 4, socket_path, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 5, (url_template && url_template[0]) ? url_template : "?file={file}", -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(statement, 6, rank);
                    ok = sqlite3_step(statement) == SQLITE_DONE;
                }
                if (!ok) snprintf(error, sizeof(error), "%s", sqlite3_errmsg(database));
                if (statement) sqlite3_finalize(statement);
                changed = ok;
            }
        } else if (ok && strcmp(action, "remove") == 0) {
            ok = bind_and_step(database,
                               "DELETE FROM file_openers WHERE service_id = ? AND extension = ?;",
                               backend,
                               opener_key,
                               NULL,
                               NULL,
                               error,
                               sizeof(error));
            changed = ok;
        } else if (ok && strcmp(action, "clear") == 0) {
            ok = bind_and_step(database,
                               "DELETE FROM file_openers WHERE service_id = ?;",
                               backend,
                               NULL,
                               NULL,
                               NULL,
                               error,
                               sizeof(error));
            changed = ok;
        } else if (ok) {
            snprintf(error, sizeof(error), "Unknown opener action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "backend") != 0) {
        snprintf(error, sizeof(error), "Unknown registry resource.");
        ok = false;
    }

    if (ok) {
        ok = sqlite_exec_ok(database, "COMMIT;", error, sizeof(error));
    } else {
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
    }
    bool close_ok = close_registry_readwrite(database, ok, error, sizeof(error));
    ok = ok && close_ok;
    if (!ok) {
        sb_append(stderr_buffer, error[0] ? error : "Registry operation failed.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }
    if (changed) mark_backend_event_changed();
    return 0;
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
        close_registry_readwrite(database, false, error, error_size);
        return false;
    }

    char *icon_value = registry_icon_path_value(icon_path);
    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO backends(service_id, display_name, icon_path, service_unit) VALUES(?, ?, ?, ?) "
                           "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon_path=excluded.icon_path, service_unit=excluded.service_unit;",
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
            "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, running) VALUES(?, '', ?, ?, 0, ?, ?, 0) "
            "ON CONFLICT(frontend_id) DO UPDATE SET service_id=excluded.service_id, display_name=excluded.display_name, socket_path=excluded.socket_path, icon_path=excluded.icon_path, running=0;";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            char frontend_id[PATH_MAX * 2];
            snprintf(frontend_id, sizeof(frontend_id), "%s:main", service_id);
            sqlite3_bind_text(statement, 1, frontend_id, -1, SQLITE_TRANSIENT);
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
    free(icon_value);
    return close_registry_readwrite(database, ok, error, error_size) && ok;
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
        close_registry_readwrite_at(database, database_path, false, error, error_size);
        return false;
    }

    char *icon_value = registry_icon_path_value(icon_path);
    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) {
        ok = bind_and_step(database,
                           "INSERT INTO backends(service_id, display_name, icon_path, service_unit) VALUES(?, ?, ?, NULL) "
                           "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon_path=excluded.icon_path, service_unit=NULL;",
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
            "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, running) VALUES(?, '', ?, ?, 0, ?, ?, 0) "
            "ON CONFLICT(frontend_id) DO UPDATE SET service_id=excluded.service_id, display_name=excluded.display_name, socket_path=excluded.socket_path, icon_path=excluded.icon_path, running=0;";
        ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
        if (ok) {
            char frontend_id[PATH_MAX * 2];
            snprintf(frontend_id, sizeof(frontend_id), "%s:main", service_id);
            sqlite3_bind_text(statement, 1, frontend_id, -1, SQLITE_TRANSIENT);
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
    free(icon_value);
    return close_registry_readwrite_at(database, database_path, ok, error, error_size) && ok;
}
#endif

static bool unregister_backend_records(const char *service_id, char *error, size_t error_size) {
    sqlite3 *database = open_registry_readwrite(error, error_size);
    if (!database) return false;
    if (!ensure_registry_schema(database, error, error_size)) {
        close_registry_readwrite(database, false, error, error_size);
        return false;
    }

    bool ok = sqlite_exec_ok(database, "BEGIN IMMEDIATE TRANSACTION;", error, error_size);
    if (ok) ok = bind_and_step(database, "DELETE FROM file_openers WHERE service_id = ?;", service_id, NULL, NULL, NULL, error, error_size);
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
    return close_registry_readwrite(database, ok, error, error_size) && ok;
}

static bool uninstall_local_home_screen(char *message, size_t message_size) {
#ifdef __APPLE__
    char error[1024] = "";
    bool registry_ok = unregister_backend_records(kOuterShellServiceID, error, sizeof(error));

    const char *home = getenv("HOME");
    char plist_path[PATH_MAX] = "";
    if (home && home[0]) {
        snprintf(plist_path, sizeof(plist_path), "%s/Library/LaunchAgents/org.outershell.OuterShell.plist", home);
        unlink(plist_path);
    }
    char socket_path[PATH_MAX] = "";
    snprintf(socket_path, sizeof(socket_path), "%s", g_listen_socket_path);

    pid_t parent_pid = getpid();
    pid_t child = fork();
    if (child == 0) {
        setsid();
        usleep(300000);
        char service_target[128];
        snprintf(service_target, sizeof(service_target), "gui/%ld/org.outershell.OuterShell", (long)getuid());
        pid_t bootout_child = fork();
        if (bootout_child == 0) {
            execlp("launchctl", "launchctl", "bootout", service_target, (char *)NULL);
            _exit(127);
        }
        if (bootout_child > 0) {
            int status = 0;
            (void)waitpid(bootout_child, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                pid_t remove_child = fork();
                if (remove_child == 0) {
                    execlp("launchctl", "launchctl", "remove", "org.outershell.OuterShell", (char *)NULL);
                    _exit(127);
                }
                if (remove_child > 0) {
                    int remove_status = 0;
                    (void)waitpid(remove_child, &remove_status, 0);
                }
            }
        }
        if (socket_path[0]) {
            unlink(socket_path);
        }
        usleep(300000);
        if (kill(parent_pid, 0) == 0) {
            kill(parent_pid, SIGTERM);
            usleep(300000);
        }
        if (kill(parent_pid, 0) == 0) {
            kill(parent_pid, SIGKILL);
        }
        _exit(0);
    }

    if (!registry_ok) {
        snprintf(message,
                 message_size,
                 "Removed the Outer Shell LaunchAgent. Registry cleanup failed: %s",
                 error[0] ? error : "unknown error");
        return true;
    }
    snprintf(message, message_size, "Outer Shell LaunchAgent removed. The app will stop momentarily.");
    return true;
#else
    snprintf(message, message_size, "Local Outer Shell uninstall is only implemented for macOS.");
    return false;
#endif
}

#ifndef __APPLE__
static void cleanup_user_systemd_bundled_app(const BundledAppDefinition *app,
                                             bool remove_unit_files,
                                             bool remove_install_root) {
    if (!app || !app->unit_name || !safe_unit_name(app->unit_name)) return;

    char quoted_unit[320];
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));

    char command[768];
    snprintf(command, sizeof(command), "systemctl --user stop %s >/dev/null 2>&1 || true", quoted_unit);
    run_shell_ignored(command);

    char socket_unit[256] = "";
    char quoted_socket_unit[320] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit, sizeof(socket_unit));
        if (safe_unit_name(socket_unit)) {
            shell_quote(socket_unit, quoted_socket_unit, sizeof(quoted_socket_unit));
            snprintf(command, sizeof(command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_socket_unit);
            run_shell_ignored(command);

            /* A service already activated by the socket can outlive the socket unit. */
            snprintf(command, sizeof(command), "systemctl --user stop %s >/dev/null 2>&1 || true", quoted_unit);
            run_shell_ignored(command);
        } else {
            socket_unit[0] = '\0';
        }
    }

    snprintf(command, sizeof(command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_unit);
    run_shell_ignored(command);
    if (quoted_socket_unit[0]) {
        snprintf(command, sizeof(command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_socket_unit);
        run_shell_ignored(command);
    }

    if (remove_unit_files) {
        char unit_path[PATH_MAX];
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), app->unit_name);
        unlink(unit_path);

        char unit_wants_path[PATH_MAX];
        snprintf(unit_wants_path, sizeof(unit_wants_path), "%s/.config/systemd/user/default.target.wants/%s", home_directory(), app->unit_name);
        unlink(unit_wants_path);

        if (socket_unit[0]) {
            char socket_unit_path[PATH_MAX];
            snprintf(socket_unit_path, sizeof(socket_unit_path), "%s/.config/systemd/user/%s", home_directory(), socket_unit);
            unlink(socket_unit_path);

            char socket_wants_path[PATH_MAX];
            snprintf(socket_wants_path, sizeof(socket_wants_path), "%s/.config/systemd/user/sockets.target.wants/%s", home_directory(), socket_unit);
            unlink(socket_wants_path);
        }

        run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1 || true");
        snprintf(command, sizeof(command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_unit);
        run_shell_ignored(command);
        if (quoted_socket_unit[0]) {
            snprintf(command, sizeof(command), "systemctl --user reset-failed %s >/dev/null 2>&1 || true", quoted_socket_unit);
            run_shell_ignored(command);
        }
    }

    if (remove_install_root && safe_service_directory_name(app->install_directory_name)) {
        char install_root[PATH_MAX];
        char quoted_root[PATH_MAX + 8];
        default_user_outerwebapps_app_root(app->install_directory_name, install_root, sizeof(install_root));
        shell_quote(install_root, quoted_root, sizeof(quoted_root));
        snprintf(command, sizeof(command), "rm -rf -- %s", quoted_root);
        run_shell_ignored(command);
    }
}
#else
static void cleanup_user_systemd_bundled_app(const BundledAppDefinition *app,
                                             bool remove_unit_files,
                                             bool remove_install_root) {
    (void)app;
    (void)remove_unit_files;
    (void)remove_install_root;
}
#endif

#ifndef __APPLE__
static bool install_bundled_user_systemd_unit_from_paths(const BundledAppDefinition *app,
                                                         const char *working_directory,
                                                         const char *binary_path,
                                                         const char *bundles_dir,
                                                         const char *icon_path,
                                                         const char *log_path,
                                                         const char *outerctl_path,
                                                         char *message,
                                                         size_t message_size) {
    char error[1024] = "";
    char unit_path[PATH_MAX];
    snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), app->unit_name);
    char socket_unit_name[256] = "";
    char socket_unit_path[PATH_MAX] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit_name, sizeof(socket_unit_name));
        snprintf(socket_unit_path, sizeof(socket_unit_path), "%s/.config/systemd/user/%s", home_directory(), socket_unit_name);
    }

    cleanup_user_systemd_bundled_app(app, true, false);

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
    shell_quote(binary_path, quoted_binary, sizeof(quoted_binary));
    shell_quote(bundles_dir, quoted_bundles, sizeof(quoted_bundles));
    if (icon_path && icon_path[0]) shell_quote(icon_path, quoted_icon, sizeof(quoted_icon)); else quoted_icon[0] = '\0';
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
             "Environment=OUTERCTL_PATH=%s\n"
             "ExecStart=/bin/sh -lc %s\n"
             "Restart=on-failure\n"
             "KillMode=control-group\n"
             "\n"
             "[Install]\n"
             "WantedBy=default.target\n",
             description,
             working_directory,
             home_directory(),
             user_name,
             user_name,
             outerctl_path,
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
    if (!upsert_systemd_backend_registry(app->service_id, app->display_name, app->unit_name, "user", actual_socket_path, log_path, icon_path, error, sizeof(error))) {
        unlink(unit_path);
        if (socket_unit_path[0]) unlink(socket_unit_path);
        snprintf(message, message_size, "%s", error);
        return false;
    }

    char quoted_unit[320];
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));
    char enable_command[512];
    run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1");
    if (socket_unit_name[0]) {
        char quoted_socket_unit[320];
        shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit));
        snprintf(enable_command, sizeof(enable_command), "systemctl --user enable --now %s >/dev/null 2>&1", quoted_socket_unit);
        int status = system(enable_command);
        if (status != 0) {
            snprintf(message, message_size, "Installed %s, but failed to enable its user socket.", app->display_name);
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
            snprintf(message, message_size, "Installed %s, but failed to start its user service: %s", app->display_name, systemd_message);
            return false;
        }
    }
    return true;
}
#endif

static bool install_bundled_app_macos(const BundledAppDefinition *app,
                                      const char *scope,
                                      const char *sudo_password,
                                      bool *needs_password,
                                      char *message,
                                      size_t message_size);
#ifndef __APPLE__
static bool remove_bundled_root_support(const BundledAppDefinition *app,
                                        const char *sudo_password,
                                        bool *needs_password,
                                        char *message,
                                        size_t message_size);
#endif

static bool install_bundled_app(const BundledAppDefinition *app, const char *scope, const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
#ifdef __APPLE__
    return install_bundled_app_macos(app, scope, sudo_password, needs_password, message, message_size);
#else
    if (needs_password) *needs_password = false;
    if (!app) {
        snprintf(message, message_size, "Unknown app.");
        return false;
    }
    bool install_as_root = scope && strcmp(scope, "system") == 0;
    if (install_as_root && !app->supports_root) {
        snprintf(message, message_size, "%s does not support root installation.", app->display_name);
        return false;
    }
    if (!install_as_root && app->root_only) {
        snprintf(message, message_size, "%s can only run as root.", app->display_name);
        return false;
    }
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
        snprintf(message, message_size, "Missing %s binary for %s at %s.", app->display_name, architecture, source_binary);
        return false;
    }
    if (stat(source_bundle_arm, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(source_bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing %s content archives under %s/bundles.", app->display_name, stage_root);
        return false;
    }
    if (source_icon[0] && (stat(source_icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        snprintf(message, message_size, "Missing %s icon at %s.", app->display_name, source_icon);
        return false;
    }

    if (install_as_root) {
        if (!ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
            return false;
        }

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

        cleanup_user_systemd_bundled_app(app, true, false);

        char quoted_unit[320];
        shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));

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
                     "0600");
        }

        char quoted_system_outerwebapps_root[PATH_MAX + 8];
        shell_quote(kSystemOuterWebappsRoot, quoted_system_outerwebapps_root, sizeof(quoted_system_outerwebapps_root));
        char wrapper_contents[4096];
        snprintf(wrapper_contents, sizeof(wrapper_contents),
                 "#!/bin/sh\n"
                 "exec /usr/local/libexec/outershelld-root-tool --root-helper-outerctl \"$@\"\n");

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
                "timeout 12s systemctl --system stop %s >/dev/null 2>&1 || true\n"
                "timeout 5s systemctl --system reset-failed %s >/dev/null 2>&1 || true\n"
                "mkdir -p %s %s /var/log/outergroup %s\n"
                "chmod 0755 %s\n"
                "install -m 0755 %s %s\n"
                "install -m 0644 %s %s\n"
                "install -m 0644 %s %s\n",
                quoted_unit,
                quoted_unit,
                quoted_install_root,
                quoted_bundles_dir,
                quoted_system_outerwebapps_root,
                quoted_system_outerwebapps_root,
                quoted_binary_source,
                quoted_target_binary,
                quoted_source_bundle_arm,
                quoted_target_bundle_arm,
                quoted_source_bundle_x86,
                quoted_target_bundle_x86);
        if (quoted_socket_unit[0]) {
            fprintf(script,
                    "timeout 12s systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
                    "timeout 12s systemctl --system stop %s >/dev/null 2>&1 || true\n"
                    "timeout 5s systemctl --system reset-failed %s >/dev/null 2>&1 || true\n",
                    quoted_socket_unit,
                    quoted_unit,
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
                "chmod 0666 %s/registry.orwa.lock 2>/dev/null || true\n"
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
                quoted_system_outerwebapps_root);
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

        if (!root_helper_registry_upsert_systemd(app->service_id,
                                                 app->display_name,
                                                 app->unit_name,
                                                 actual_socket_path,
                                                 log_path,
                                                 target_icon,
                                                 sudo_password,
                                                 needs_password,
                                                 message,
                                                 message_size)) {
            return false;
        }

        if (!app->root_only) {
            char user_state_root[PATH_MAX];
            default_user_outerwebapps_app_root(app->install_directory_name, user_state_root, sizeof(user_state_root));
            if (!mkdir_p(user_state_root)) {
                snprintf(message, message_size, "Failed to create %s: %s", user_state_root, strerror(errno));
                return false;
            }
            char user_log_path[PATH_MAX];
            snprintf(user_log_path, sizeof(user_log_path), "%s/backend.log", user_state_root);
            char user_outerctl_path[PATH_MAX];
            default_user_outerctl_path(user_outerctl_path, sizeof(user_outerctl_path));
            if (!install_bundled_user_systemd_unit_from_paths(app,
                                                              install_root,
                                                              target_binary,
                                                              bundles_dir,
                                                              target_icon,
                                                              user_log_path,
                                                              user_outerctl_path,
                                                              message,
                                                              message_size)) {
                return false;
            }
        }

        snprintf(message, message_size, "Installed %s with root support.", app->display_name);
        return true;
    }

    char install_root[PATH_MAX];
    default_user_outerwebapps_app_root(app->install_directory_name, install_root, sizeof(install_root));
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
    snprintf(log_path, sizeof(log_path), "%s/backend.log", install_root);
    char unit_path[PATH_MAX];
    snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), app->unit_name);
    char socket_unit_name[256] = "";
    char socket_unit_path[PATH_MAX] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit_name, sizeof(socket_unit_name));
        snprintf(socket_unit_path, sizeof(socket_unit_path), "%s/.config/systemd/user/%s", home_directory(), socket_unit_name);
    }

    cleanup_user_systemd_bundled_app(app, true, false);

    char quoted_unit[320];
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));

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
    char outerctl_path[PATH_MAX];
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
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
             "WantedBy=default.target\n",
             description,
             install_root,
             home_directory(),
             user_name,
             user_name,
             outerctl_path,
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

#ifndef __APPLE__
static bool remove_bundled_root_support(const BundledAppDefinition *app,
                                        const char *sudo_password,
                                        bool *needs_password,
                                        char *message,
                                        size_t message_size) {
    if (needs_password) *needs_password = false;
    if (!app || !app->supports_root) {
        snprintf(message, message_size, "This app does not have root support.");
        return false;
    }
    if (!ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
        return false;
    }

    char install_root[PATH_MAX];
    snprintf(install_root, sizeof(install_root), "/opt/outergroup/%s", app->install_directory_name);
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

    char quoted_unit[320];
    char quoted_install_root[PATH_MAX + 8];
    char quoted_log_path[PATH_MAX + 8];
    char quoted_unit_path[PATH_MAX + 8];
    char quoted_socket_unit[320] = "";
    char quoted_socket_unit_path[PATH_MAX + 8] = "";
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));
    shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
    shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
    shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
    if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
    if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';

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
            "timeout 12s systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
            "%s%s%s"
            "%s%s%s"
            "rm -f -- %s %s\n"
            "rm -rf -- %s\n"
            "rm -f -- %s\n"
            "systemctl --system daemon-reload\n",
            quoted_unit,
            quoted_socket_unit[0] ? "timeout 12s systemctl --system disable --now " : "",
            quoted_socket_unit[0] ? quoted_socket_unit : "",
            quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
            quoted_socket_unit[0] ? "timeout 12s systemctl --system stop " : "",
            quoted_socket_unit[0] ? quoted_unit : "",
            quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
            quoted_unit_path,
            quoted_socket_unit_path[0] ? quoted_socket_unit_path : "''",
            quoted_install_root,
            quoted_log_path);
    fclose(script);
    chmod(script_template, 0700);
    bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
    unlink(script_template);
    if (!root_ok) return false;
    if (!root_helper_registry_remove_backend(app->service_id, sudo_password, needs_password, message, message_size)) {
        return false;
    }
    snprintf(message, message_size, "Removed root support for %s.", app->display_name);
    return true;
}
#endif

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
        snprintf(message, message_size, "This app is not available on localhost.");
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
        snprintf(message, message_size, "Missing %s backend at %s.", app->display_name, source_binary);
        return false;
    }
    if (stat(source_bundle_arm, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(source_bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing %s content archives under %s/bundles.", app->display_name, stage_root);
        return false;
    }
    if (source_icon[0] && (stat(source_icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        snprintf(message, message_size, "Missing %s icon at %s.", app->display_name, source_icon);
        return false;
    }

    char socket_path[PATH_MAX] = "";
    bundled_socket_path_for_scope(app, install_as_root ? "system" : "user", socket_path, sizeof(socket_path));

    if (install_as_root) {
        char install_root[PATH_MAX];
        snprintf(install_root, sizeof(install_root), "%s/apps/%s", kSystemOuterWebappsRoot, app->install_directory_name);
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
        default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
        char quoted_system_root[PATH_MAX + 8];
        char quoted_outerctl[PATH_MAX + 8];
        shell_quote(kSystemOuterWebappsRoot, quoted_system_root, sizeof(quoted_system_root));
        shell_quote(outerctl_path, quoted_outerctl, sizeof(quoted_outerctl));
        char wrapper_contents[4096];
        snprintf(wrapper_contents, sizeof(wrapper_contents),
                 "#!/bin/sh\n"
                 "exec env OUTERWEBAPPS_HOME=%s %s \"$@\"\n",
                 quoted_system_root,
                 quoted_outerctl);

        StringBuilder plist = {0};
        if (!make_bundled_launchd_plist(app->service_id,
                                        target_binary,
                                        bundles_dir,
                                        target_icon,
                                        socket_path,
                                        0600,
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
        default_user_outerwebapps_app_root(app->install_directory_name, user_install_root, sizeof(user_install_root));
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
                "OUTERWEBAPPS_HOME=%s %s backend upsert --backend %s --name %s --icon-path %s\n"
                "OUTERWEBAPPS_HOME=%s %s launchd set --backend %s --plist %s --owns-plist true\n"
                "OUTERWEBAPPS_HOME=%s %s app clear --backend %s\n"
                "if [ -n %s ]; then OUTERWEBAPPS_HOME=%s %s app add --backend %s --socket-path %s --name %s --icon-path %s; fi\n"
                "OUTERWEBAPPS_HOME=%s %s log clear --backend %s\n"
                "OUTERWEBAPPS_HOME=%s %s log add --backend %s --path %s\n"
                "chmod 0666 %s/registry.orwa.lock 2>/dev/null || true\n"
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
                quoted_wrapper_path,
                quoted_service_id,
                quoted_display_name,
                quoted_target_icon[0] ? quoted_target_icon : "''",
                quoted_system_root,
                quoted_wrapper_path,
                quoted_service_id,
                quoted_plist_path,
                quoted_system_root,
                quoted_wrapper_path,
                quoted_service_id,
                quoted_socket_path,
                quoted_system_root,
                quoted_wrapper_path,
                quoted_service_id,
                quoted_socket_path,
                quoted_display_name,
                quoted_target_icon[0] ? quoted_target_icon : "''",
                quoted_system_root,
                quoted_wrapper_path,
                quoted_service_id,
                quoted_system_root,
                quoted_wrapper_path,
                quoted_service_id,
                quoted_log_path,
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
    default_user_outerwebapps_app_root(app->install_directory_name, install_root, sizeof(install_root));
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
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));

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
                snprintf(install_root, sizeof(install_root), "%s/apps/%s", kSystemOuterWebappsRoot, install_name);
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
            char outerctl_path[PATH_MAX];
            char quoted_outerctl_path[PATH_MAX + 8];
            shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));
            if (install_root[0]) shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
            shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
            if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path));
            shell_quote(kSystemOuterWebappsRoot, quoted_system_root, sizeof(quoted_system_root));
            shell_quote(service_id, quoted_service_id, sizeof(quoted_service_id));
            default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
            shell_quote(outerctl_path, quoted_outerctl_path, sizeof(quoted_outerctl_path));

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
                    "OUTERWEBAPPS_HOME=%s %s app clear --backend %s >/dev/null 2>&1 || true\n"
                    "OUTERWEBAPPS_HOME=%s %s log clear --backend %s >/dev/null 2>&1 || true\n"
                    "OUTERWEBAPPS_HOME=%s %s launchd clear --backend %s >/dev/null 2>&1 || true\n"
                    "OUTERWEBAPPS_HOME=%s %s backend remove --backend %s >/dev/null 2>&1 || true\n",
                    service_id,
                    owns_plist && plist_path[0] ? quoted_plist_path : "''",
                    quoted_socket_path[0] ? quoted_socket_path : "''",
                    quoted_system_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_root,
                    quoted_outerctl_path,
                    quoted_service_id,
                    quoted_system_root,
                    quoted_outerctl_path,
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
            if (!ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
                return false;
            }

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

            char quoted_unit_path[PATH_MAX + 8];
            char quoted_socket_unit[320] = "";
            char quoted_socket_unit_path[PATH_MAX + 8] = "";
            char quoted_install_root[PATH_MAX + 8];
            char quoted_log_path[PATH_MAX + 8];
            shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
            if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
            if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';
            if (install_root[0]) shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root)); else quoted_install_root[0] = '\0';
            shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));

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
                    "timeout 12s systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
                    "%s%s%s"
                    "%s%s%s"
                    "rm -f -- %s %s\n",
                    quoted_unit,
                    quoted_socket_unit[0] ? "timeout 12s systemctl --system disable --now " : "",
                    quoted_socket_unit[0] ? quoted_socket_unit : "",
                    quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
                    quoted_socket_unit[0] ? "timeout 12s systemctl --system stop " : "",
                    quoted_socket_unit[0] ? quoted_unit : "",
                    quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
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
            if (!root_helper_registry_remove_backend(service_id, sudo_password, needs_password, message, message_size)) {
                return false;
            }
        } else {
            const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
            if (app) {
                cleanup_user_systemd_bundled_app(app, true, false);
            } else {
                char command[768];
                snprintf(command, sizeof(command), "systemctl --user disable --now %s >/dev/null 2>&1 || true", quoted_unit);
                run_shell_ignored(command);
                char unit_path[PATH_MAX];
                snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), unit_name);
                unlink(unit_path);
                run_shell_ignored("systemctl --user daemon-reload >/dev/null 2>&1 || true");
            }
        }
    }

    const BundledAppDefinition *bundled_app = bundled_app_for_service_id(service_id);
    if (!found && bundled_app) {
        cleanup_user_systemd_bundled_app(bundled_app, true, false);
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
            default_user_outerwebapps_app_root(install_name, install_root, sizeof(install_root));
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

static bool collect_python_suggestion(const char *path, PathSet *seen) {
    if (!path_is_executable_file(path)) return true;
    char canonical[PATH_MAX];
    const char *display_path = path;
    if (realpath(path, canonical)) {
        display_path = canonical;
    }
    path_set_insert(seen, display_path);
    return true;
}

static bool binary_build_string_array_from_paths(PathSet *paths, StringBuilder *out) {
    size_t fixed_size = 4 + paths->count * 8;
    if (!binary_append_zero(out, fixed_size)) return false;
    if (!binary_write_u32_at(out, 0, (uint32_t)paths->count)) return false;
    for (size_t i = 0; i < paths->count; i++) {
        if (!binary_append_string_ref_at(out, 4 + i * 8, paths->paths[i])) return false;
    }
    return true;
}

static bool collect_python_env_suggestions(const char *base, PathSet *seen) {
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
        ok = collect_python_suggestion(path, seen);
    }
    closedir(dir);
    return ok;
}

static bool collect_pyenv_suggestions(PathSet *seen) {
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
        ok = collect_python_suggestion(path, seen);
    }
    closedir(dir);
    return ok;
}

static bool collect_path_python_suggestions(PathSet *seen) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) return true;
    char copy[8192];
    snprintf(copy, sizeof(copy), "%s", path_env);
    bool ok = true;
    for (char *dir = strtok(copy, ":"); ok && dir; dir = strtok(NULL, ":")) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/python3", dir);
        ok = collect_python_suggestion(candidate, seen);
        snprintf(candidate, sizeof(candidate), "%s/python", dir);
        ok = ok && collect_python_suggestion(candidate, seen);
    }
    return ok;
}

typedef struct {
    const char *title;
    const char *value;
} BinaryChoiceSpec;

static bool build_choice_payload(const char *title, const char *value, StringBuilder *payload) {
    if (!binary_append_zero(payload, 16)) return false;
    return binary_append_string_ref_at(payload, 0, title) &&
           binary_append_string_ref_at(payload, 8, value);
}

static bool build_choices_array(const BinaryChoiceSpec *choices, size_t choice_count, StringBuilder *out) {
    BinaryPayloadList list = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < choice_count; i++) {
        StringBuilder payload = {0};
        ok = build_choice_payload(choices[i].title, choices[i].value, &payload) &&
             binary_payload_list_append(&list, &payload);
        if (!ok) free(payload.data);
    }
    if (ok) ok = binary_build_payload_array(&list, out);
    binary_payload_list_free(&list);
    return ok;
}

static bool build_field_payload(const char *key,
                                const char *label,
                                const char *default_value,
                                const char *field_type,
                                const char *placeholder,
                                PathSet *suggestions,
                                const BinaryChoiceSpec *choices,
                                size_t choice_count,
                                StringBuilder *payload) {
    StringBuilder suggestion_array = {0};
    StringBuilder choice_array = {0};
    bool ok = suggestions ? binary_build_string_array_from_paths(suggestions, &suggestion_array)
                          : build_empty_array_payload(&suggestion_array);
    ok = ok && build_choices_array(choices, choice_count, &choice_array);
    ok = ok && binary_append_zero(payload, 56) &&
         binary_append_string_ref_at(payload, 0, key) &&
         binary_append_string_ref_at(payload, 8, label) &&
         binary_append_string_ref_at(payload, 16, default_value) &&
         binary_append_string_ref_at(payload, 24, field_type) &&
         binary_append_string_ref_at(payload, 32, placeholder) &&
         binary_append_child_ref_at(payload, 40, &suggestion_array) &&
         binary_append_child_ref_at(payload, 48, &choice_array);
    free(suggestion_array.data);
    free(choice_array.data);
    return ok;
}

static bool append_field_payload(BinaryPayloadList *fields,
                                 const char *key,
                                 const char *label,
                                 const char *default_value,
                                 const char *field_type,
                                 const char *placeholder,
                                 PathSet *suggestions,
                                 const BinaryChoiceSpec *choices,
                                 size_t choice_count) {
    StringBuilder payload = {0};
    bool ok = build_field_payload(key, label, default_value, field_type, placeholder,
                                  suggestions, choices, choice_count, &payload) &&
              binary_payload_list_append(fields, &payload);
    if (!ok) free(payload.data);
    return ok;
}

static bool build_recipe_payload(const char *identifier,
                                 const char *display_name,
                                 const char *summary,
                                 BinaryPayloadList *fields,
                                 StringBuilder *payload) {
    StringBuilder field_array = {0};
    bool ok = binary_build_payload_array(fields, &field_array) &&
              binary_append_zero(payload, 32) &&
              binary_append_string_ref_at(payload, 0, identifier) &&
              binary_append_string_ref_at(payload, 8, display_name) &&
              binary_append_string_ref_at(payload, 16, summary) &&
              binary_append_child_ref_at(payload, 24, &field_array);
    free(field_array.data);
    return ok;
}

static bool append_recipe_payload(BinaryPayloadList *recipes,
                                  const char *identifier,
                                  const char *display_name,
                                  const char *summary,
                                  BinaryPayloadList *fields) {
    StringBuilder payload = {0};
    bool ok = build_recipe_payload(identifier, display_name, summary, fields, &payload) &&
              binary_payload_list_append(recipes, &payload);
    if (!ok) free(payload.data);
    return ok;
}

static void send_recipes_response(int fd) {
    PathSet seen = {0};
    bool ok = true;
    char path[PATH_MAX];
    const char *conda_dirs[] = {"miniforge3", "mambaforge", "miniconda3", "anaconda3"};
    for (size_t i = 0; ok && i < sizeof(conda_dirs) / sizeof(conda_dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/bin/python", home_directory(), conda_dirs[i]);
        ok = collect_python_suggestion(path, &seen);
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s/%s", home_directory(), conda_dirs[i]);
        ok = ok && collect_python_env_suggestions(base, &seen);
    }
    ok = ok && collect_pyenv_suggestions(&seen);
    ok = ok && collect_python_suggestion("/opt/homebrew/bin/python3", &seen);
    ok = ok && collect_python_suggestion("/usr/local/bin/python3", &seen);
    ok = ok && collect_python_suggestion("/usr/bin/python3", &seen);
    ok = ok && collect_path_python_suggestions(&seen);

    BinaryPayloadList recipes = {0};
    BinaryChoiceSpec connection_choices[] = {
        {.title = "Port", .value = "port"},
        {.title = "Unix Socket", .value = "unixSocket"},
    };
    BinaryPayloadList fields = {0};
    ok = ok &&
         append_field_payload(&fields, "command", "Command", "", "text", "bundle exec jekyll serve --host 0.0.0.0", NULL, NULL, 0) &&
         append_field_payload(&fields, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL, 0) &&
         append_field_payload(&fields, "scriptPath", "Script Path", "", "file", "~/dev/run-service.sh", NULL, NULL, 0) &&
         append_field_payload(&fields, "port", "Port", "", "text", "4000", NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "", "text", "My Service", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "", "text", "my-service", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "command-port", "Run a command, use a hardcoded port number",
                               "Create a script that runs a command you choose and registers a frontend on a hardcoded port number.",
                               &fields);
    binary_payload_list_free(&fields);
    fields = (BinaryPayloadList){0};
    ok = ok &&
         append_field_payload(&fields, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL, 0) &&
         append_field_payload(&fields, "scriptPath", "Script Path", "", "file", "~/dev/run-service.sh", NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "", "text", "My Service", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "", "text", "my-service", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "custom", "Blank Script",
                               "Create a minimal script that shows how to use OUTERCTL_PATH yourself.",
                               &fields);
    binary_payload_list_free(&fields);
    fields = (BinaryPayloadList){0};
    ok = ok &&
         append_field_payload(&fields, "python", "Python", "/usr/bin/python3", "file", "/usr/bin/python3", &seen, NULL, 0) &&
         append_field_payload(&fields, "workdir", "Working Dir", "~/dev", "directory", "~/dev", NULL, NULL, 0) &&
         append_field_payload(&fields, "scriptPath", "Script Path", "", "file", "~/dev/run-jupyter.py", NULL, NULL, 0) &&
         append_field_payload(&fields, "frontendTransport", "Connection", "port", "choice", "", NULL, connection_choices, 2) &&
         append_field_payload(&fields, "port", "Port", "", "text", "Auto", NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "Jupyter Lab", "text", "Jupyter Lab", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "jupyter", "text", "jupyter", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "jupyter", "Jupyter Lab",
                               "Create a script that launches Jupyter Lab and finds its browser URL with jupyter server list.",
                               &fields);
    binary_payload_list_free(&fields);
    fields = (BinaryPayloadList){0};
    ok = ok &&
         append_field_payload(&fields, "projectDir", "Project Dir", "~", "directory", "~/dev/my-project", NULL, NULL, 0) &&
         append_field_payload(&fields, "scriptPath", "Script Path", "", "file", "~/dev/my-project/run-jupyter.py", NULL, NULL, 0) &&
         append_field_payload(&fields, "frontendTransport", "Connection", "port", "choice", "", NULL, connection_choices, 2) &&
         append_field_payload(&fields, "port", "Port", "", "text", "Auto", NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "Jupyter Lab", "text", "Jupyter Lab", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "jupyter-uv", "text", "jupyter-uv", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "jupyter-uv", "Jupyter Lab (uv or .venv)",
                               "Create a script that launches Jupyter Lab from .venv and finds its browser URL with jupyter server list.",
                               &fields);
    binary_payload_list_free(&fields);
    fields = (BinaryPayloadList){0};
    ok = ok &&
         append_field_payload(&fields, "executablePath", "Executable", "", "file", "~/path/to/executable.sh", NULL, NULL, 0) &&
         append_field_payload(&fields, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "", "text", "My Service", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "", "text", "my-service", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "existing-executable", "Use Existing Executable",
                               "Choose a script or executable you already keep in your own folders.",
                               &fields);
    binary_payload_list_free(&fields);

    StringBuilder suggestion_array = {0};
    StringBuilder recipe_array = {0};
    ok = ok && binary_build_string_array_from_paths(&seen, &suggestion_array) &&
         binary_build_payload_array(&recipes, &recipe_array);
    StringBuilder builder = {0};
    ok = ok && binary_append_zero(&builder, 16) &&
         binary_append_child_ref_at(&builder, 0, &suggestion_array) &&
         binary_append_child_ref_at(&builder, 8, &recipe_array);
    free(suggestion_array.data);
    free(recipe_array.data);
    binary_payload_list_free(&recipes);
    if (!ok) {
        free(builder.data);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }
    send_binary_response(fd, 200, &builder);
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
        send_action_response(fd, 400, false, "Missing displayName or command.");
        return;
    }
    if (!display_name[0] || !command[0]) {
        if (!recipe[0]) {
            send_action_response(fd, 400, false, "Display name and command are required.");
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
        send_action_response(fd, 400, false, "Could not construct a safe launchd label.");
        return;
    }
#else
    snprintf(unit_name, sizeof(unit_name), "%s.service", unit_stem);
    if (!safe_unit_name(unit_name)) {
        send_action_response(fd, 400, false, "Could not construct a safe systemd unit name.");
        return;
    }
#endif

    char backend_dir[PATH_MAX];
    char log_path[PATH_MAX];
    char unit_path[PATH_MAX];
    char initial_frontend_id[PATH_MAX * 2] = "";
    char initial_frontend_url[PATH_MAX * 2] = "";
    char initial_frontend_socket_path[PATH_MAX] = "";
    char initial_frontend_list[PATH_MAX] = "";
    int initial_frontend_port = 0;
    default_user_outerwebapps_app_root(service_id, backend_dir, sizeof(backend_dir));
#ifdef __APPLE__
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), service_id);
    snprintf(unit_path, sizeof(unit_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), unit_name);
#else
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
                send_action_response(fd, 400, false, "Script Path is required.");
                return;
            }
        }

        if (strcmp(recipe, "command-port") == 0) {
            char port[32] = "";
            query_value(query, "command", command, sizeof(command));
            query_value(query, "port", port, sizeof(port));
            if (!command[0] || !valid_port_text(port)) {
                send_action_response(fd, 400, false, "Command and a valid port are required.");
                return;
            }
            snprintf(initial_frontend_id, sizeof(initial_frontend_id), "%s:main", service_id);
            snprintf(initial_frontend_url, sizeof(initial_frontend_url), "127.0.0.1:%s/", port);
            initial_frontend_port = atoi(port);
            StringBuilder script = {0};
            if (!make_fixed_port_script(service_id, display_name, port, command, &script)) {
                free(script.data);
                send_action_response(fd, 500, false, "Failed to generate script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_response(fd, 500, false, error);
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
                send_action_response(fd, 500, false, "Failed to generate script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_response(fd, 500, false, error);
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
            bool use_unix_socket = strcmp(transport, "unixSocket") == 0;
            if (!use_unix_socket && port[0] && !valid_port_text(port)) {
                send_action_response(fd, 400, false, "Port must be empty or a valid TCP port.");
                return;
            }
            snprintf(initial_frontend_id, sizeof(initial_frontend_id), "%s:main", service_id);
            snprintf(initial_frontend_url, sizeof(initial_frontend_url), "/lab");
            snprintf(initial_frontend_list, sizeof(initial_frontend_list), "Jupyter");
            if (use_unix_socket) {
                runtime_socket_path(service_id, initial_frontend_socket_path, sizeof(initial_frontend_socket_path));
            } else if (port[0]) {
                initial_frontend_port = atoi(port);
                snprintf(initial_frontend_url, sizeof(initial_frontend_url), "127.0.0.1:%s/lab", port);
            }
            StringBuilder script = {0};
            if (!make_jupyter_script(service_id, display_name, python, use_unix_socket ? "" : port,
                                     use_unix_socket,
                                     strcmp(recipe, "jupyter-uv") == 0,
                                     &script)) {
                free(script.data);
                send_action_response(fd, 500, false, "Failed to generate Jupyter script.");
                return;
            }
            if (!write_text_file(script_path, script.data, error, sizeof(error))) {
                free(script.data);
                send_action_response(fd, 500, false, error);
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
                send_action_response(fd, 400, false, "Executable is required.");
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
            send_action_response(fd, 400, false, "Unknown recipe.");
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
            send_action_response(fd, 500, false, error[0] ? error : "Failed to generate script.");
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
    char outerctl_path[PATH_MAX];
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
    char unit_contents[12000];
    snprintf(unit_contents, sizeof(unit_contents),
             "[Unit]\n"
             "Description=%s\n"
             "After=network.target\n"
             "\n"
             "[Service]\n"
             "Type=simple\n"
             "WorkingDirectory=%s\n"
             "Environment=OUTERCTL_PATH=%s\n"
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
             outerctl_path,
             quoted_command,
             log_path,
             log_path);

    if (!mkdir_p(backend_dir)) {
        snprintf(error, sizeof(error), "Failed to create %s: %s", backend_dir, strerror(errno));
        send_action_response(fd, 500, false, error);
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
            send_action_response(fd, 500, false, error);
            return;
        }
    }
    char launch_agents_dir[PATH_MAX];
    snprintf(launch_agents_dir, sizeof(launch_agents_dir), "%s/Library/LaunchAgents", home_directory());
    if (!mkdir_p(launch_agents_dir)) {
        snprintf(error, sizeof(error), "Failed to create %s: %s", launch_agents_dir, strerror(errno));
        send_action_response(fd, 500, false, error);
        return;
    }
    StringBuilder plist = {0};
    if (!make_launchd_plist(unit_name, command, working_directory, outerctl_path, log_path, &plist)) {
        free(plist.data);
        send_action_response(fd, 500, false, "Failed to generate LaunchAgent plist.");
        return;
    }
    if (!write_text_file(unit_path, plist.data, error, sizeof(error))) {
        free(plist.data);
        send_action_response(fd, 500, false, error);
        return;
    }
    free(plist.data);
#else
    if (!write_text_file(unit_path, unit_contents, error, sizeof(error))) {
        send_action_response(fd, 500, false, error);
        return;
    }
#endif
    if (!register_created_backend(service_id, display_name,
#ifdef __APPLE__
                                  unit_path,
#else
                                  unit_name,
#endif
                                  log_path,
                                  initial_frontend_id,
                                  initial_frontend_url,
                                  initial_frontend_port,
                                  initial_frontend_socket_path,
                                  initial_frontend_list,
                                  error,
                                  sizeof(error))) {
        unlink(unit_path);
        send_action_response(fd, 500, false, error);
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
        send_action_response(fd, 500, false, response);
        return;
    }

    char response[512];
    snprintf(response, sizeof(response), "Created %s.", display_name);
    log_event("Created and started backend %s.", service_id);
    mark_backend_event_changed();
    send_action_response(fd, 200, true, response);
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
            send_log_response(fd, "", "", "", false, 0, 0, "missing serviceID or path");
            return;
        }
        char error[512] = "";
        bool found = resolve_log_path_any(service_id, log_index, raw_path, sizeof(raw_path), error, sizeof(error));
        if (!found) {
            send_log_response(fd, service_id, "", "", false, 0, 0, "no registered log file for this backend");
            return;
        }
    }

    char path[PATH_MAX];
    expand_tilde_path(raw_path, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        char message[PATH_MAX + 80];
        snprintf(message, sizeof(message), "failed to stat log file: %s", strerror(errno));
        send_log_response(fd, service_id, raw_path, "", false, 0, 0, message);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        send_log_response(fd, service_id, raw_path, "", false, 0, 0, "log path is not a regular file");
        return;
    }

    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t bytes_to_read = file_size < (uint64_t)requested_bytes ? file_size : (uint64_t)requested_bytes;
    uint64_t start_offset = file_size > bytes_to_read ? file_size - bytes_to_read : 0;
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_log_response(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
        return;
    }
    if (start_offset > 0 && lseek(file_fd, (off_t)start_offset, SEEK_SET) < 0) {
        close(file_fd);
        send_log_response(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
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
            send_log_response(fd, service_id, raw_path, "", false, file_size, (double)st.st_mtime, strerror(errno));
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    buffer[offset] = '\0';
    send_log_response(fd, service_id, raw_path, buffer, start_offset > 0, file_size, (double)st.st_mtime, "");
    free(buffer);
}

static void send_outer_descriptor(int fd) {
    const char *plugin_json = "{\"backendsAPIPath\":\"/api/backends\",\"logsAPIPath\":\"/api/logs\",\"controlAPIPath\":\"/api/control\",\"createAPIPath\":\"/api/create\",\"recipesAPIPath\":\"/api/recipes\",\"filePickerAPIPath\":\"/api/file-picker\"}";
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

static bool api_request_is_complete(const char *request, size_t length, size_t *complete_length) {
    *complete_length = 0;
    if (length < 4) return false;
    uint32_t message_length = read_uint32_le((const unsigned char *)request);
    if (message_length > READ_BUFFER_SIZE - 4) {
        *complete_length = READ_BUFFER_SIZE + 1;
        return true;
    }
    if (length < 4u + message_length) return false;
    *complete_length = 4u + message_length;
    return true;
}

static bool api_read_string_ref(const unsigned char *message,
                                size_t message_length,
                                size_t ref_offset,
                                char **out) {
    *out = NULL;
    if (ref_offset + 8 > message_length) return false;
    uint32_t offset = read_uint32_le(message + ref_offset);
    uint32_t length = read_uint32_le(message + ref_offset + 4);
    if (offset > message_length || length > message_length - offset) return false;
    char *value = malloc((size_t)length + 1);
    if (!value) return false;
    if (length) memcpy(value, message + offset, length);
    value[length] = '\0';
    *out = value;
    return true;
}

enum {
    OUTERSHELLD_API_OUTERCTL_INVOKE = 1,
    OUTERSHELLD_API_OUTERCTL_INVOKE_RESPONSE = 2,
    OUTERSHELLD_API_FILE_OPENERS_QUERY = 3,
    OUTERSHELLD_API_FILE_OPENERS_RESPONSE = 4,
    OUTERSHELLD_API_FILE_OPENERS_ROW_SIZE = 40
};

static bool api_append_string_ref32(StringBuilder *rows, StringBuilder *variable, const char *text) {
    const char *safe_text = text ? text : "";
    size_t offset = variable->length;
    size_t length = strlen(safe_text);
    if (offset > UINT32_MAX || length > UINT32_MAX) return false;
    unsigned char ref[8];
    write_uint32_le(ref, (uint32_t)offset);
    write_uint32_le(ref + 4, (uint32_t)length);
    return sb_append_n(rows, (const char *)ref, sizeof(ref)) &&
           sb_append_n(variable, safe_text, length);
}

static void api_patch_string_refs32(char *data, size_t data_length, uint32_t variable_offset) {
    for (size_t offset = 0; offset + 8 <= data_length; offset += 8) {
        uint32_t relative = read_uint32_le((const unsigned char *)data + offset);
        write_uint32_le((unsigned char *)data + offset, relative + variable_offset);
    }
}

static bool api_send_frame(int fd, StringBuilder *message) {
    if (!message || message->length > UINT32_MAX) return false;
    unsigned char prefix[4];
    write_uint32_le(prefix, (uint32_t)message->length);
    return queue_all(fd, prefix, sizeof(prefix)) &&
           queue_all(fd, message->data, message->length);
}

#ifndef __APPLE__
static int connect_unix_stream(const char *socket_path, char *error, size_t error_size) {
    if (!socket_path || !socket_path[0]) {
        snprintf(error, error_size, "socket path is empty");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        snprintf(error, error_size, "socket path is too long: %s", socket_path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to create socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_size, "Failed to connect to root helper socket %s: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool root_helper_outerctl(int argc,
                                 char **argv,
                                 const char *sudo_password,
                                 bool *needs_password,
                                 char *message,
                                 size_t message_size) {
    if (needs_password) *needs_password = false;
    if (message_size > 0) message[0] = '\0';
    if (argc < 3 || !argv) {
        snprintf(message, message_size, "Invalid root helper registry request.");
        return false;
    }

    StringBuilder command = {0};
    bool ok = sb_append(&command, "/usr/local/libexec/outershelld-root-tool --root-helper-outerctl");
    for (int i = 1; ok && i < argc; i++) {
        char quoted[PATH_MAX * 2];
        shell_quote(argv[i] ? argv[i] : "", quoted, sizeof(quoted));
        ok = sb_append(&command, " ") && sb_append(&command, quoted);
    }
    if (!ok) {
        free(command.data);
        snprintf(message, message_size, "Failed to build root registry command.");
        return false;
    }

    int exit_status = -1;
    ok = run_sudo_shell(command.data, sudo_password, message, message_size, &exit_status);
    free(command.data);
    if (!ok && sudo_failure_needs_password(message, exit_status)) {
        if (needs_password) *needs_password = true;
        snprintf(message, message_size, "Administrator password required.");
    } else if (!ok && !message[0]) {
        snprintf(message, message_size, "Root registry operation failed.");
    }
    return ok;
}

static bool root_helper_registry_upsert_systemd(const char *service_id,
                                                const char *display_name,
                                                const char *unit_name,
                                                const char *socket_path,
                                                const char *log_path,
                                                const char *icon_path,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size) {
    char *backend_argv[] = {"outerctl", "backend", "upsert", "--backend", (char *)service_id, "--name", (char *)display_name, "--icon-path", (char *)(icon_path ? icon_path : ""), NULL};
    if (!root_helper_outerctl(9, backend_argv, sudo_password, needs_password, message, message_size)) return false;

    char *systemd_argv[] = {"outerctl", "systemd", "set", "--backend", (char *)service_id, "--unit", (char *)unit_name, NULL};
    if (!root_helper_outerctl(7, systemd_argv, sudo_password, needs_password, message, message_size)) return false;

    char *app_clear_argv[] = {"outerctl", "app", "clear", "--backend", (char *)service_id, NULL};
    if (!root_helper_outerctl(5, app_clear_argv, sudo_password, needs_password, message, message_size)) return false;

    if (socket_path && socket_path[0]) {
        char *app_add_argv[] = {"outerctl", "app", "add", "--backend", (char *)service_id, "--socket-path", (char *)socket_path, "--name", (char *)display_name, "--icon-path", (char *)(icon_path ? icon_path : ""), NULL};
        if (!root_helper_outerctl(11, app_add_argv, sudo_password, needs_password, message, message_size)) return false;
    }

    char *log_clear_argv[] = {"outerctl", "log", "clear", "--backend", (char *)service_id, NULL};
    if (!root_helper_outerctl(5, log_clear_argv, sudo_password, needs_password, message, message_size)) return false;

    char *log_add_argv[] = {"outerctl", "log", "add", "--backend", (char *)service_id, "--path", (char *)log_path, NULL};
    return root_helper_outerctl(7, log_add_argv, sudo_password, needs_password, message, message_size);
}

static bool root_helper_registry_remove_backend(const char *service_id,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size) {
    char *app_clear_argv[] = {"outerctl", "app", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, app_clear_argv, sudo_password, needs_password, message, message_size);
    char *log_clear_argv[] = {"outerctl", "log", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, log_clear_argv, sudo_password, needs_password, message, message_size);
    char *opener_clear_argv[] = {"outerctl", "opener", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, opener_clear_argv, sudo_password, needs_password, message, message_size);
    char *systemd_clear_argv[] = {"outerctl", "systemd", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, systemd_clear_argv, sudo_password, needs_password, message, message_size);
    char *backend_remove_argv[] = {"outerctl", "backend", "remove", "--backend", (char *)service_id, NULL};
    if (root_helper_outerctl(5, backend_remove_argv, sudo_password, needs_password, message, message_size)) return true;
    return contains_case_insensitive(message, "Backend not registered");
}

static bool root_helper_registry_clear_frontends(const char *service_id,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size) {
    char *app_clear_argv[] = {"outerctl", "app", "clear", "--backend", (char *)service_id, NULL};
    return root_helper_outerctl(5, app_clear_argv, sudo_password, needs_password, message, message_size);
}
#endif

static void api_send_outerctl_response(int fd, int status, StringBuilder *stdout_buffer, StringBuilder *stderr_buffer) {
    StringBuilder message = {0};
    bool ok = binary_append_zero(&message, 22) &&
              binary_write_u16_at(&message, 0, OUTERSHELLD_API_OUTERCTL_INVOKE_RESPONSE) &&
              binary_write_u32_at(&message, 2, (uint32_t)status) &&
              binary_append_data_ref_at(&message, 6, stdout_buffer->data, stdout_buffer->length) &&
              binary_append_data_ref_at(&message, 14, stderr_buffer->data, stderr_buffer->length);
    if (!ok || message.length > UINT32_MAX) {
        free(message.data);
        return;
    }
    api_send_frame(fd, &message);
    free(message.data);
}

static void api_send_file_openers_response(int fd,
                                           uint32_t status,
                                           const char *error,
                                           StringBuilder *rows,
                                           StringBuilder *variable,
                                           uint32_t row_count) {
    StringBuilder message = {0};
    size_t rows_length = rows ? rows->length : 0;
    size_t variable_length = variable ? variable->length : 0;
    bool ok = rows_length <= UINT32_MAX &&
              variable_length <= UINT32_MAX &&
              18 + rows_length <= UINT32_MAX &&
              binary_append_zero(&message, 18);
    if (ok && rows_length > 0) {
        uint32_t variable_offset = (uint32_t)(18 + rows_length);
        api_patch_string_refs32(rows->data, rows->length, variable_offset);
        ok = sb_append_n(&message, rows->data, rows->length);
    }
    if (ok && variable_length > 0) {
        ok = sb_append_n(&message, variable->data, variable->length);
    }
    if (ok) {
        ok = binary_write_u16_at(&message, 0, OUTERSHELLD_API_FILE_OPENERS_RESPONSE) &&
             binary_write_u32_at(&message, 2, status) &&
             binary_append_string_ref_at(&message, 6, error ? error : "") &&
             binary_write_u32_at(&message, 14, row_count);
    }
    if (ok) api_send_frame(fd, &message);
    free(message.data);
}

static bool unix_socket_path_accessible_to_current_user(const char *socket_path) {
    if (!socket_path || !socket_path[0]) return false;
    struct stat st;
    if (stat(socket_path, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;
    return access(socket_path, R_OK | W_OK) == 0;
}

static bool api_append_file_openers_from_database(sqlite3 *database,
                                                  const char *extension,
                                                  const char *file_path,
                                                  bool require_socket_access,
                                                  StringBuilder *rows,
                                                  StringBuilder *variable,
                                                  uint32_t *row_count,
                                                  char *error,
                                                  size_t error_size) {
    const char *sql =
        "SELECT extension, service_id, display_name, socket_path, url_template "
        "FROM file_openers "
        "WHERE extension = ?1 "
        "ORDER BY rank, display_name, service_id;";
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, extension, -1, SQLITE_TRANSIENT);
    } else {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    }

    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *socket_path = sqlite_column_text_or_empty(statement, 3);
        if (require_socket_access && !unix_socket_path_accessible_to_current_user(socket_path)) {
            continue;
        }
        StringBuilder url = {0};
        ok = append_file_opener_url(&url,
                                    socket_path,
                                    sqlite_column_text_or_empty(statement, 4),
                                    file_path ? file_path : "") &&
             api_append_string_ref32(rows, variable, sqlite_column_text_or_empty(statement, 0)) &&
             api_append_string_ref32(rows, variable, sqlite_column_text_or_empty(statement, 1)) &&
             api_append_string_ref32(rows, variable, sqlite_column_text_or_empty(statement, 2)) &&
             api_append_string_ref32(rows, variable, socket_path) &&
             api_append_string_ref32(rows, variable, url.data ? url.data : "");
        free(url.data);
        if (ok && row_count) *row_count += 1;
    }
    if (ok && step != SQLITE_DONE) {
        snprintf(error, error_size, "%s", sqlite3_errmsg(database));
        ok = false;
    }
    if (!ok && !error[0]) snprintf(error, error_size, "out of memory");
    if (statement) sqlite3_finalize(statement);
    return ok;
}

static bool api_query_file_openers(const char *extension_filter,
                                   const char *detected_kind,
                                   const char *file_path,
                                   StringBuilder *rows,
                                   StringBuilder *variable,
                                   uint32_t *row_count,
                                   char *error,
                                   size_t error_size) {
    if (row_count) *row_count = 0;
    char extension[128] = "";
    char kind_key[160] = "";
    if (extension_filter && extension_filter[0] &&
        !normalize_file_extension(extension_filter, extension, sizeof(extension))) {
        snprintf(error, error_size, "Invalid file extension.");
        return false;
    }
    if (detected_kind && detected_kind[0] &&
        !make_file_opener_kind_key(detected_kind, kind_key, sizeof(kind_key))) {
        snprintf(error, error_size, "Invalid opener kind.");
        return false;
    }
    if (!extension[0] && !kind_key[0]) {
        snprintf(error, error_size, "Missing file opener query.");
        return false;
    }

    sqlite3 *database = open_registry_readonly(error, error_size);
    if (!database) return false;
    bool ok = true;
    if (extension[0]) {
        ok = api_append_file_openers_from_database(database,
                                                  extension,
                                                  file_path,
                                                  false,
                                                  rows,
                                                  variable,
                                                  row_count,
                                                  error,
                                                  error_size);
    }
    if (ok && kind_key[0]) {
        ok = api_append_file_openers_from_database(database,
                                                  kind_key,
                                                  file_path,
                                                  false,
                                                  rows,
                                                  variable,
                                                  row_count,
                                                  error,
                                                  error_size);
    }
    sqlite3_close(database);

    if (ok && g_system_registry_database_path[0] && registry_storage_exists_at(g_system_registry_database_path)) {
        char system_error[512] = "";
        sqlite3 *system_database = open_system_registry_readonly(system_error, sizeof(system_error));
        if (system_database) {
            if (extension[0]) {
                ok = api_append_file_openers_from_database(system_database,
                                                          extension,
                                                          file_path,
                                                          true,
                                                          rows,
                                                          variable,
                                                          row_count,
                                                          error,
                                                          error_size);
            }
            if (ok && kind_key[0]) {
                ok = api_append_file_openers_from_database(system_database,
                                                          kind_key,
                                                          file_path,
                                                          true,
                                                          rows,
                                                          variable,
                                                          row_count,
                                                          error,
                                                          error_size);
            }
            sqlite3_close(system_database);
        }
    }
    return ok;
}

static bool process_api_file_openers_request(ReactorClient *client, const unsigned char *message, size_t message_length) {
    char *extension = NULL;
    char *detected_kind = NULL;
    char *file_path = NULL;
    char error[512] = "";
    StringBuilder rows = {0};
    StringBuilder variable = {0};
    uint32_t row_count = 0;
    bool ok = message_length >= 18 &&
              api_read_string_ref(message, message_length, 2, &extension) &&
              api_read_string_ref(message, message_length, 10, &file_path);
    if (ok) {
        if (message_length >= 26) {
            ok = api_read_string_ref(message, message_length, 18, &detected_kind);
        } else {
            detected_kind = strdup("");
            ok = detected_kind != NULL;
        }
    }
    ok = ok && api_query_file_openers(extension, detected_kind, file_path, &rows, &variable, &row_count, error, sizeof(error));
    if (!ok && !error[0]) snprintf(error, sizeof(error), "Invalid file openers request.");
    api_send_file_openers_response(client->fd, ok ? 0u : 1u, error, &rows, &variable, ok ? row_count : 0);
    free(extension);
    free(detected_kind);
    free(file_path);
    free(rows.data);
    free(variable.data);
    return false;
}

static bool process_api_outerctl_request(ReactorClient *client, const unsigned char *message, size_t message_length) {
    StringBuilder stdout_buffer = {0};
    StringBuilder stderr_buffer = {0};
    int status = 1;

    if (message_length < 14) {
        sb_append(&stderr_buffer, "Unsupported API message.\n");
        api_send_outerctl_response(client->fd, status, &stdout_buffer, &stderr_buffer);
        free(stdout_buffer.data);
        free(stderr_buffer.data);
        return false;
    }

    uint32_t argc_u32 = read_uint32_le(message + 2);
    if (argc_u32 > 256 || 14u + (uint64_t)argc_u32 * 8u > message_length) {
        sb_append(&stderr_buffer, "Invalid outerctl argv payload.\n");
        api_send_outerctl_response(client->fd, status, &stdout_buffer, &stderr_buffer);
        free(stdout_buffer.data);
        free(stderr_buffer.data);
        return false;
    }

    int argc = (int)argc_u32;
    char *registry_path = NULL;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    bool ok = argv != NULL && api_read_string_ref(message, message_length, 6, &registry_path);
    for (int i = 0; ok && i < argc; i++) {
        ok = api_read_string_ref(message, message_length, 14u + (size_t)i * 8u, &argv[i]);
    }
    if (!ok) {
        sb_append(&stderr_buffer, "Invalid outerctl argv string reference.\n");
    } else {
        char saved_registry_path[PATH_MAX];
        snprintf(saved_registry_path, sizeof(saved_registry_path), "%s", g_registry_database_path);
        if (registry_path && registry_path[0]) {
            snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s", registry_path);
        }
        status = outershelld_handle_outerctl(argc, argv, &stdout_buffer, &stderr_buffer);
        snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s", saved_registry_path);
    }
    api_send_outerctl_response(client->fd, status, &stdout_buffer, &stderr_buffer);
    if (argv) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
    }
    free(registry_path);
    free(stdout_buffer.data);
    free(stderr_buffer.data);
    return false;
}

static bool process_api_client_request(ReactorClient *client, char *request, size_t n) {
    if (n < 6) return false;
    const unsigned char *message = (const unsigned char *)request + 4;
    size_t message_length = n - 4;
    uint16_t message_type = read_uint16_le(message);
    if (message_type == OUTERSHELLD_API_OUTERCTL_INVOKE) {
        return process_api_outerctl_request(client, message, message_length);
    }
    if (message_type == OUTERSHELLD_API_FILE_OPENERS_QUERY) {
        return process_api_file_openers_request(client, message, message_length);
    }

    StringBuilder stdout_buffer = {0};
    StringBuilder stderr_buffer = {0};
    sb_append(&stderr_buffer, "Unsupported API message.\n");
    api_send_outerctl_response(client->fd, 1, &stdout_buffer, &stderr_buffer);
    free(stdout_buffer.data);
    free(stderr_buffer.data);
    return false;
}

static bool prepare_events_response_or_wait(ReactorClient *client, const char *query) {
    char since_backends_raw[64] = "";
    char since_log_raw[64] = "";
    char service_id[PATH_MAX] = "";
    char log_index_raw[64] = "";
    query_value(query, "sinceBackends", since_backends_raw, sizeof(since_backends_raw));
    query_value(query, "sinceLog", since_log_raw, sizeof(since_log_raw));
    query_value(query, "serviceID", service_id, sizeof(service_id));
    query_value(query, "logIndex", log_index_raw, sizeof(log_index_raw));
    int log_index = log_index_raw[0] ? atoi(log_index_raw) : 0;
    if (log_index < 0) log_index = 0;

    uint64_t since_backends = parse_u64_or_zero(since_backends_raw);
    uint64_t since_log = parse_u64_or_zero(since_log_raw);
    uint64_t backends_version = current_backends_event_version();
    uint64_t log_version = current_log_event_version(service_id, log_index);
    bool backends_changed = since_backends == 0 || backends_version != since_backends;
    bool log_changed = service_id[0] && (since_log == 0 || log_version != since_log);
    if (backends_changed || log_changed) {
        send_events_response(client->fd, backends_changed, log_changed, false, backends_version, log_version);
        return false;
    }

    client->waiting_for_events = true;
    client->event_deadline_ms = monotonic_milliseconds() + 25000;
    client->event_since_backends = since_backends;
    client->event_since_log = since_log;
    snprintf(client->event_log_service_id, sizeof(client->event_log_service_id), "%s", service_id);
    client->event_log_index = log_index;
    return true;
}

static bool process_client_request(ReactorClient *client, char *request, size_t n) {
    int fd = client->fd;
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
        return false;
    }
    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "HEAD") != 0 && strcasecmp(method, "POST") != 0) {
        send_text_response(fd, 400, "unsupported method\n");
        return false;
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
    } else if (strcmp(target, "/api/file-picker") == 0) {
        send_file_picker_response(fd, query);
    } else if (strcmp(target, "/api/events") == 0) {
        return prepare_events_response_or_wait(client, query);
    } else {
        send_text_response(fd, 404, "not found\n");
    }
    return false;
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
    return fd;
}

static int systemd_activated_listener_named(const char *wanted_name, bool *activation_flag) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    const char *listen_fdnames = getenv("LISTEN_FDNAMES");
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
    int selected = -1;
    if (!wanted_name || !wanted_name[0] || !listen_fdnames || !listen_fdnames[0]) {
        selected = 3;
    } else {
        const char *name = listen_fdnames;
        for (long i = 0; i < fds; i++) {
            const char *separator = strchr(name, ':');
            size_t length = separator ? (size_t)(separator - name) : strlen(name);
            if (strlen(wanted_name) == length && strncmp(name, wanted_name, length) == 0) {
                selected = 3 + (int)i;
                break;
            }
            if (!separator) break;
            name = separator + 1;
        }
    }
    if (selected >= 0 && activation_flag) {
        *activation_flag = true;
    }
    return selected;
}

static void clear_systemd_activation_environment(void) {
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
}

static bool socket_activation_enabled(void) {
    return g_systemd_socket_activation || g_api_systemd_socket_activation || g_launchd_socket_activation;
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

static void add_reactor_client(ReactorClient *clients, size_t *client_count, int client_fd, bool is_api) {
    if (*client_count >= MAX_REACTOR_CLIENTS) {
        close(client_fd);
        return;
    }
    set_fd_nonblocking(client_fd, true);
    ReactorClient *client = &clients[(*client_count)++];
    memset(client, 0, sizeof(*client));
    client->fd = client_fd;
    client->is_api = is_api;
#ifndef __APPLE__
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) == 0) {
        client->peer_uid = credentials.uid;
        client->has_peer_uid = true;
    }
#else
    uid_t peer_uid = (uid_t)-1;
    gid_t peer_gid = (gid_t)-1;
    if (getpeereid(client_fd, &peer_uid, &peer_gid) == 0) {
        client->peer_uid = peer_uid;
        client->has_peer_uid = true;
    }
#endif
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
            bool complete = client->is_api
                ? api_request_is_complete(client->request, client->length, complete_length)
                : request_is_complete(client->request, client->length, complete_length);
            if (complete) {
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

static void accept_ready_clients(int listener, ReactorClient *clients, size_t *client_count, bool is_api) {
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
        add_reactor_client(clients, client_count, client, is_api);
    }
}

static bool event_client_ready(ReactorClient *client, bool *timed_out,
                               uint64_t *backends_version, uint64_t *log_version) {
    *timed_out = monotonic_milliseconds() >= client->event_deadline_ms;
    *backends_version = current_backends_event_version();
    *log_version = current_log_event_version(client->event_log_service_id, client->event_log_index);
    bool backends_changed = *backends_version != client->event_since_backends;
    bool log_changed = client->event_log_service_id[0] && *log_version != client->event_since_log;
    return *timed_out || backends_changed || log_changed;
}

static void flush_ready_event_clients(ReactorClient *clients, size_t *client_count) {
    for (size_t i = *client_count; i > 0; i--) {
        size_t index = i - 1;
        ReactorClient *client = &clients[index];
        if (!client->waiting_for_events) continue;
        bool timed_out = false;
        uint64_t backends_version = 0;
        uint64_t log_version = 0;
        if (!event_client_ready(client, &timed_out, &backends_version, &log_version)) continue;
        send_events_response(client->fd,
                             backends_version != client->event_since_backends,
                             client->event_log_service_id[0] && log_version != client->event_since_log,
                             timed_out,
                             backends_version,
                             log_version);
        close_reactor_client(clients, client_count, index);
    }
}

static void run_reactor(int listener, int api_listener) {
    ReactorClient *clients = calloc(MAX_REACTOR_CLIENTS, sizeof(ReactorClient));
    if (!clients) {
        fprintf(stderr, "failed to allocate reactor clients\n");
        return;
    }
    size_t client_count = 0;

    set_fd_nonblocking(listener, true);
    if (api_listener >= 0) set_fd_nonblocking(api_listener, true);
    while (!g_shutdown_requested) {
        flush_ready_event_clients(clients, &client_count);
        struct pollfd poll_fds[MAX_REACTOR_CLIENTS + 2];
        size_t polled_client_count = client_count;
        poll_fds[0] = (struct pollfd){.fd = listener, .events = POLLIN, .revents = 0};
        size_t client_poll_offset = 1;
        if (api_listener >= 0) {
            poll_fds[1] = (struct pollfd){.fd = api_listener, .events = POLLIN, .revents = 0};
            client_poll_offset = 2;
        }
        for (size_t i = 0; i < polled_client_count; i++) {
            poll_fds[i + client_poll_offset] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN, .revents = 0};
        }

        int timeout_ms = 1000;
        if (socket_activation_enabled() && !g_stay_alive_when_socket_idle && polled_client_count == 0) {
            timeout_ms = 60000;
        }

        int poll_result = poll(poll_fds, (nfds_t)(polled_client_count + client_poll_offset), timeout_ms);
        if (poll_result == 0) {
            if (socket_activation_enabled() && !g_stay_alive_when_socket_idle && client_count == 0) {
                break;
            }
        } else if (poll_result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        } else {
            if (poll_fds[0].revents & POLLIN) {
                accept_ready_clients(listener, clients, &client_count, false);
            } else if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                break;
            }
            if (api_listener >= 0) {
                if (poll_fds[1].revents & POLLIN) {
                    accept_ready_clients(api_listener, clients, &client_count, true);
                } else if (poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    break;
                }
            }

            for (size_t i = polled_client_count; i > 0; i--) {
                size_t index = i - 1;
                short revents = poll_fds[index + client_poll_offset].revents;
                if (revents == 0) continue;

                if (clients[index].waiting_for_events) {
                    if (revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                        close_reactor_client(clients, &client_count, index);
                    }
                    continue;
                }

                if (revents & POLLIN) {
                    size_t complete_length = 0;
                    bool should_close = false;
                    bool complete = read_reactor_client(&clients[index], &complete_length, &should_close);
                    if (complete) {
                        set_fd_nonblocking(clients[index].fd, false);
                        if (complete_length > READ_BUFFER_SIZE) {
                            send_text_response(clients[index].fd, 400, "request too large\n");
                            close_reactor_client(clients, &client_count, index);
                        } else {
                            bool keep_open = clients[index].is_api
                                ? process_api_client_request(&clients[index],
                                                             clients[index].request,
                                                             complete_length)
                                : process_client_request(&clients[index],
                                                         clients[index].request,
                                                         complete_length);
                            if (!keep_open) {
                                close_reactor_client(clients, &client_count, index);
                            }
                        }
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

static void run_api_reactor(int api_listener) {
    ReactorClient *clients = calloc(MAX_REACTOR_CLIENTS, sizeof(ReactorClient));
    if (!clients) {
        fprintf(stderr, "failed to allocate API reactor clients\n");
        return;
    }
    size_t client_count = 0;
    set_fd_nonblocking(api_listener, true);
    while (!g_shutdown_requested) {
        struct pollfd poll_fds[MAX_REACTOR_CLIENTS + 1];
        size_t polled_client_count = client_count;
        poll_fds[0] = (struct pollfd){.fd = api_listener, .events = POLLIN, .revents = 0};
        for (size_t i = 0; i < polled_client_count; i++) {
            poll_fds[i + 1] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN, .revents = 0};
        }

        int timeout_ms = socket_activation_enabled() && !g_stay_alive_when_socket_idle && polled_client_count == 0 ? 60000 : 1000;
        int poll_result = poll(poll_fds, (nfds_t)(polled_client_count + 1), timeout_ms);
        if (poll_result == 0) {
            if (socket_activation_enabled() && !g_stay_alive_when_socket_idle && client_count == 0) break;
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (poll_fds[0].revents & POLLIN) {
            accept_ready_clients(api_listener, clients, &client_count, true);
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
                        close_reactor_client(clients, &client_count, index);
                    } else {
                        (void)process_api_client_request(&clients[index],
                                                         clients[index].request,
                                                         complete_length);
                        close_reactor_client(clients, &client_count, index);
                    }
                } else if (should_close) {
                    close_reactor_client(clients, &client_count, index);
                }
            } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_reactor_client(clients, &client_count, index);
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
    for (size_t i = 0; i < client_count; i++) close(clients[i].fd);
    free(clients);
}

#ifndef __APPLE__
static bool root_helper_peer_allowed(int client_fd) {
    if (g_root_helper_owner_uid == (uid_t)-1) return false;
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return false;
    }
    return credentials.uid == 0;
}

static void run_root_helper_api_loop(int api_listener) {
    if (api_listener < 0) return;
    set_fd_nonblocking(api_listener, true);
    while (!g_shutdown_requested) {
        struct pollfd pfd = {.fd = api_listener, .events = POLLIN, .revents = 0};
        int poll_result = poll(&pfd, 1, socket_activation_enabled() ? 60000 : 1000);
        if (poll_result == 0) {
            if (socket_activation_enabled()) break;
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(api_listener, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (!root_helper_peer_allowed(client_fd)) {
            close(client_fd);
            continue;
        }

        char request[READ_BUFFER_SIZE];
        unsigned char length_bytes[4];
        bool ok = read_exact_with_timeout(client_fd, length_bytes, sizeof(length_bytes), 5000);
        uint32_t message_length = ok ? read_uint32_le(length_bytes) : 0;
        if (ok && message_length <= READ_BUFFER_SIZE - 4) {
            memcpy(request, length_bytes, sizeof(length_bytes));
            ok = read_exact_with_timeout(client_fd, request + 4, message_length, 5000);
        } else {
            ok = false;
        }
        if (ok) {
            ReactorClient client = {0};
            client.fd = client_fd;
            client.is_api = true;
            (void)process_api_client_request(&client, request, (size_t)message_length + 4);
        }
        close(client_fd);
    }
}
#endif

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--api-socket-path PATH | --no-api-socket] [--http-only | --broker-only] [--launchd-socket-name NAME] [--bundles-dir DIR] [--bundled-apps-dir DIR] [--app-base-url URL] [--public-base-url URL] [--database PATH] [--system-database PATH] [--stay-alive] [--root-helper --root-helper-owner-uid UID]\n", program);
}

int OuterShellBackendMain(int argc, char **argv) {
    int port = DEFAULT_PORT;
    bool use_port = true;
    bool use_api_socket = true;
    bool http_enabled = true;
#ifndef __APPLE__
    bool root_helper_mode = false;
#endif
    char socket_path[PATH_MAX] = "";
    char api_socket_path[PATH_MAX] = "";
    char launchd_socket_name[128] = "Listener";
    const char *bundles_dir = "bundles";
    const char *app_base_url = getenv("OUTER_SHELL_APP_BASE_URL");
    const char *public_base_url = getenv("OUTER_SHELL_PUBLIC_BASE_URL");
    if (app_base_url && app_base_url[0]) {
        snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", app_base_url);
    }
    if (public_base_url && public_base_url[0]) {
        snprintf(g_home_screen_public_base_url, sizeof(g_home_screen_public_base_url), "%s", public_base_url);
    }
    default_registry_database_path(g_registry_database_path, sizeof(g_registry_database_path));
    default_system_registry_database_path(g_system_registry_database_path, sizeof(g_system_registry_database_path));
    default_api_socket_path(api_socket_path, sizeof(api_socket_path));

#ifndef __APPLE__
    if (argc >= 2 && strcmp(argv[1], "--root-helper-outerctl") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "root helper outerctl mode requires root.\n");
            return 2;
        }
        snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s", g_system_registry_database_path);
        StringBuilder stdout_buffer = {0};
        StringBuilder stderr_buffer = {0};
        int status = outershelld_handle_outerctl(argc - 1, argv + 1, &stdout_buffer, &stderr_buffer);
        if (stdout_buffer.data && stdout_buffer.length > 0) {
            fwrite(stdout_buffer.data, 1, stdout_buffer.length, stdout);
        }
        if (stderr_buffer.data && stderr_buffer.length > 0) {
            fwrite(stderr_buffer.data, 1, stderr_buffer.length, stderr);
        }
        free(stdout_buffer.data);
        free(stderr_buffer.data);
        return status;
    }
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], socket_path, sizeof(socket_path));
            use_port = false;
        } else if (strcmp(argv[i], "--api-socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], api_socket_path, sizeof(api_socket_path));
            use_api_socket = true;
        } else if (strcmp(argv[i], "--no-api-socket") == 0) {
            use_api_socket = false;
            api_socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--http-only") == 0) {
            http_enabled = true;
            use_api_socket = false;
            api_socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--broker-only") == 0) {
            http_enabled = false;
            use_api_socket = true;
            use_port = false;
        } else if (strcmp(argv[i], "--launchd-socket-name") == 0 && i + 1 < argc) {
            snprintf(launchd_socket_name, sizeof(launchd_socket_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            bundles_dir = argv[++i];
        } else if (strcmp(argv[i], "--bundled-apps-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_bundled_apps_directory, sizeof(g_bundled_apps_directory));
        } else if (strcmp(argv[i], "--app-base-url") == 0 && i + 1 < argc) {
            snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--public-base-url") == 0 && i + 1 < argc) {
            snprintf(g_home_screen_public_base_url, sizeof(g_home_screen_public_base_url), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--database") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_registry_database_path, sizeof(g_registry_database_path));
        } else if (strcmp(argv[i], "--system-database") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_system_registry_database_path, sizeof(g_system_registry_database_path));
        } else if (strcmp(argv[i], "--stay-alive") == 0) {
            g_stay_alive_when_socket_idle = true;
        } else if (strcmp(argv[i], "--root-helper") == 0) {
#ifdef __APPLE__
            usage(argv[0]);
            return 2;
#else
            root_helper_mode = true;
            use_port = false;
#endif
        } else if (strcmp(argv[i], "--root-helper-owner-uid") == 0 && i + 1 < argc) {
#ifdef __APPLE__
            usage(argv[0]);
            return 2;
#else
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0') {
                usage(argv[0]);
                return 2;
            }
            g_root_helper_owner_uid = (uid_t)value;
#endif
        } else {
            usage(argv[0]);
            return 2;
        }
    }

#ifndef __APPLE__
    if (root_helper_mode) {
        if (g_root_helper_owner_uid == (uid_t)-1 || geteuid() != 0) {
            fprintf(stderr, "root helper mode requires root and --root-helper-owner-uid.\n");
            return 2;
        }
        snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s", g_system_registry_database_path);
        int api_listener = systemd_activated_listener_named("api", &g_api_systemd_socket_activation);
        clear_systemd_activation_environment();
        if (api_listener < 0) {
            api_listener = create_unix_listener(api_socket_path);
        }
        if (api_socket_path[0]) {
            snprintf(g_api_socket_path, sizeof(g_api_socket_path), "%s", api_socket_path);
        }
        if (api_listener < 0) return 1;
        g_api_listener_fd = api_listener;
        fprintf(stderr, "outershelld root helper API listening on %s\n", api_socket_path[0] ? api_socket_path : "(socket activated)");
        fprintf(stderr, "System registry database: %s\n", g_registry_database_path);
        run_root_helper_api_loop(api_listener);
        close(api_listener);
        g_api_listener_fd = -1;
        if (g_api_socket_path[0] && !g_api_systemd_socket_activation) {
            unlink(g_api_socket_path);
        }
        return 0;
    }
#endif

    snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
             "%s/BackendsContent.bundle.macos-arm.aar", bundles_dir);
    snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
             "%s/BackendsContent.bundle.macos-x86.aar", bundles_dir);

    migrate_user_outerwebapps_state();

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!http_enabled) {
        int api_listener = use_api_socket ? systemd_activated_listener_named("api", &g_api_systemd_socket_activation) : -1;
        clear_systemd_activation_environment();
        if (!use_api_socket) {
            fprintf(stderr, "broker-only mode requires an API socket\n");
            return 2;
        }
        if (api_listener < 0) {
            api_listener = create_unix_listener(api_socket_path);
        }
        if (api_socket_path[0]) {
            snprintf(g_api_socket_path, sizeof(g_api_socket_path), "%s", api_socket_path);
        }
        if (api_listener < 0) return 1;
        g_api_listener_fd = api_listener;
        fprintf(stderr, "outershelld API listening on %s\n", api_socket_path[0] ? api_socket_path : "(socket activated)");
        fprintf(stderr, "Registry database: %s\n", g_registry_database_path);
        if (g_system_registry_database_path[0]) {
            fprintf(stderr, "System registry database: %s\n", g_system_registry_database_path);
        }
        run_api_reactor(api_listener);
        close(api_listener);
        g_api_listener_fd = -1;
        if (g_api_socket_path[0] && !g_api_systemd_socket_activation) {
            unlink(g_api_socket_path);
        }
        return 0;
    }

    int listener = !use_port ? systemd_activated_listener_named("http", &g_systemd_socket_activation) : -1;
    int api_listener = use_api_socket ? systemd_activated_listener_named("api", &g_api_systemd_socket_activation) : -1;
    clear_systemd_activation_environment();
    if (listener < 0 && !use_port) {
        listener = launchd_activated_listener(launchd_socket_name);
    }
    if (listener < 0) {
        listener = use_port ? create_tcp_listener(port) : create_unix_listener(socket_path);
    }
    if (!use_port && socket_path[0]) {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    }
    if (listener < 0) return 1;
    if (use_api_socket && api_listener < 0) {
        api_listener = create_unix_listener(api_socket_path);
    }
    if (use_api_socket && api_socket_path[0]) {
        snprintf(g_api_socket_path, sizeof(g_api_socket_path), "%s", api_socket_path);
    }
    if (use_api_socket && api_listener < 0) {
        close(listener);
        return 1;
    }
    g_listener_fd = listener;
    g_api_listener_fd = api_listener;
    if (use_port) {
        fprintf(stderr, "outershelld HTTP listening on http://127.0.0.1:%d/\n", port);
    } else {
        fprintf(stderr, "outershelld HTTP listening on %s/\n", socket_path);
    }
    if (use_api_socket) {
        fprintf(stderr, "outershelld API listening on %s\n", api_socket_path[0] ? api_socket_path : "(socket activated)");
    }
    fprintf(stderr, "Registry database: %s\n", g_registry_database_path);
    if (g_system_registry_database_path[0]) {
        fprintf(stderr, "System registry database: %s\n", g_system_registry_database_path);
    }
    char resolved_bundled_apps_root[PATH_MAX];
    bundled_apps_root(resolved_bundled_apps_root, sizeof(resolved_bundled_apps_root));
    fprintf(stderr, "App payloads directory: %s\n", resolved_bundled_apps_root);
    if (g_bundled_apps_base_url[0]) {
        fprintf(stderr, "App payloads base URL: %s\n", g_bundled_apps_base_url);
    }
    if (g_home_screen_public_base_url[0]) {
        fprintf(stderr, "Outer Shell public base URL: %s\n", g_home_screen_public_base_url);
    }

    run_reactor(listener, api_listener);

    close(listener);
    if (api_listener >= 0) close(api_listener);
    g_listener_fd = -1;
    g_api_listener_fd = -1;
    if (!use_port && g_listen_socket_path[0] && !g_systemd_socket_activation && !g_launchd_socket_activation) {
        unlink(g_listen_socket_path);
    }
    if (use_api_socket && g_api_socket_path[0] && !g_api_systemd_socket_activation) {
        unlink(g_api_socket_path);
    }
    return 0;
}

#ifndef OUTER_SHELL_BACKEND_LIBRARY
int main(int argc, char **argv) {
    return OuterShellBackendMain(argc, argv);
}
#endif
