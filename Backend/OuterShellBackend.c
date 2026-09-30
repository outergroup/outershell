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

#include "OuterShellAPI.h"
#include "OuterShellBuffer.h"
#include "OuterShellDownloader.h"
#include "OuterShellPlatform.h"
#include "../Resources/OuterShellPaths.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
extern int launch_activate_socket(const char *name, int **fds, size_t *cnt);
#endif

static int64_t api_monotonic_milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


static bool api_read_exact_with_timeout(int fd, void *buffer, size_t length, int timeout_ms) {
    unsigned char *bytes = buffer;
    size_t offset = 0;
    int64_t deadline = api_monotonic_milliseconds() + timeout_ms;
    while (offset < length) {
        ssize_t got = read(fd, bytes + offset, length - offset);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (api_monotonic_milliseconds() >= deadline) return false;
            usleep(1000);
            continue;
        }
        return false;
    }
    return true;
}


static bool api_read_frame_from_fd(int fd, StringBuilder *message, char *error, size_t error_size) {
    unsigned char length_bytes[4];
    if (!api_read_exact_with_timeout(fd, length_bytes, sizeof(length_bytes), 30000)) {
        snprintf(error, error_size, "Timed out reading API response.");
        return false;
    }
    uint32_t message_length = read_uint32_le(length_bytes);
    if (message_length > OUTERSHELL_API_MAX_FRAME_SIZE) {
        snprintf(error, error_size, "API response is too large.");
        return false;
    }
    if (!sb_reserve(message, message_length)) {
        snprintf(error, error_size, "Out of memory.");
        return false;
    }
    message->length = message_length;
    message->data[message_length] = '\0';
    if (!api_read_exact_with_timeout(fd, message->data, message_length, 30000)) {
        snprintf(error, error_size, "Timed out reading API response body.");
        return false;
    }
    return true;
}


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
        snprintf(error, error_size, "Failed to connect to socket %s: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}



#define DEFAULT_PORT 7354
#define READ_BUFFER_SIZE 65536
#define MAX_HTTP_REQUEST_SIZE (16u * 1024u * 1024u)
#define MAX_REACTOR_CLIENTS 128
#define CLIENT_IDLE_TIMEOUT_MS 10000
#define TRANSFER_IDLE_TIMEOUT_MS 120000

static const char *kBundleUrlPath = "/bundles/OuterShell";
static const char *kBundleFilePathMacosArm = "bundles/OuterShell.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/OuterShell.bundle.macos-x86.aar";

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_bundled_apps_directory[PATH_MAX] = "";
static char g_native_app_template_directory[PATH_MAX] = "";
static char g_web_root_directory[PATH_MAX] = "";
static char g_bundled_apps_base_url[2048] = "";
static char g_home_screen_public_base_url[2048] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static char g_http_proxy_api_socket_path[PATH_MAX] = "";
static char g_container_transfers_directory[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static bool g_launchd_socket_activation = false;
static bool g_stay_alive_when_socket_idle = false;
static time_t g_backend_start_time = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;

typedef struct {
    int fd;
    bool is_api;
    uid_t peer_uid;
    bool has_peer_uid;
    char *request;
    size_t request_capacity;
    size_t length;
    int64_t last_activity_ms;
    bool waiting_for_api_response;
    int api_response_fd;
    bool streaming_container_upload;
    int container_upload_fd;
    uint64_t container_upload_offset;
    uint64_t container_upload_received;
    uint64_t container_upload_length;
} ReactorClient;

static const char *bundle_arm_path(void) {
    return g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
}

static const char *bundle_x86_path(void) {
    return g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
}

static void bundle_url_path(char *out, size_t out_size) {
    struct stat arm_st;
    struct stat x86_st;
    if (stat(bundle_arm_path(), &arm_st) == 0 &&
        stat(bundle_x86_path(), &x86_st) == 0 &&
        arm_st.st_size >= 0 &&
        x86_st.st_size >= 0) {
        snprintf(out,
                 out_size,
                 "%s-%lld-%lld-%lld-%lld",
                 kBundleUrlPath,
                 (long long)arm_st.st_mtime,
                 (long long)arm_st.st_size,
                 (long long)x86_st.st_mtime,
                 (long long)x86_st.st_size);
        return;
    }
    snprintf(out, out_size, "%s", kBundleUrlPath);
}

typedef struct {
    const char *service_id;
    const char *display_name;
    const char *stage_directory_name;
    const char *binary_name;
    const char *bundle_prefix;
    const char *icon_name;
    const char *source_name;
    bool supports_macos;
} BundledAppDefinition;

static const BundledAppDefinition kBundledApps[] = {
    {
        .service_id = "org.outershell.Top",
        .display_name = "Top",
        .stage_directory_name = "Top",
        .binary_name = "TopBackend",
        .bundle_prefix = "TopContent",
        .icon_name = "app-icon.png",
        .source_name = "TopBackend.c",
        .supports_macos = true
    },
    {
        .service_id = "org.outershell.Files",
        .display_name = "Files",
        .stage_directory_name = "Files",
        .binary_name = "FilesBackend",
        .bundle_prefix = "FilesContent",
        .icon_name = "app-icon.png",
        .source_name = "FilesBackend.c",
        .supports_macos = true
    },
    {
        .service_id = "org.outershell.Plaintext",
        .display_name = "Plaintext",
        .stage_directory_name = "Plaintext",
        .binary_name = "PlaintextBackend",
        .bundle_prefix = "PlaintextContent",
        .icon_name = "app-icon.png",
        .source_name = "PlaintextBackend.c",
        .supports_macos = true
    },
    {
        .service_id = "org.outershell.Firehose",
        .display_name = "Firehose",
        .stage_directory_name = "Firehose",
        .binary_name = "FirehoseBackend",
        .bundle_prefix = "FirehoseContent",
        .icon_name = "app-icon.png",
        .source_name = NULL,
        .supports_macos = false
    },
    {
        .service_id = "org.outershell.Profile",
        .display_name = "Profile",
        .stage_directory_name = "Profile",
        .binary_name = "ProfileBackend",
        .bundle_prefix = "ProfileContent",
        .icon_name = "app-icon.png",
        .source_name = "ProfileBackend.c",
        .supports_macos = false
    }
};

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
    if (g_listener_fd >= 0) {
        close((int)g_listener_fd);
    }
}

void OuterShellBackendRequestShutdown(void) {
    handle_shutdown_signal(SIGTERM);
}

static const char *http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 409: return "Conflict";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Error";
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
                              "Vary: Outerframe-Accept\r\n"
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
    send_response(fd, status, http_status_text(status), "text/plain; charset=utf-8", message, strlen(message));
}

static void send_container_upload_response(int fd, int status, uint64_t offset) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n"
                              "Upload-Offset: %llu\r\n"
                              "Upload-Protocol: outershell-resumable-v1\r\n"
                              "\r\n",
                              status,
                              http_status_text(status),
                              (unsigned long long)offset);
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        queue_all(fd, header, (size_t)header_len);
    }
}

static void send_cached_response_header(int fd,
                                        int status,
                                        const char *content_type,
                                        size_t content_length,
                                        const char *etag,
                                        const char *last_modified,
                                        bool vary_outerframe_accept) {
    char header[1024];
    char last_modified_header[128] = "";
    char vary_header[64] = "";
    if (last_modified && last_modified[0]) {
        snprintf(last_modified_header, sizeof(last_modified_header),
                 "Last-Modified: %s\r\n", last_modified);
    }
    if (vary_outerframe_accept) {
        snprintf(vary_header, sizeof(vary_header), "Vary: Outerframe-Accept\r\n");
    }
    int header_len;
    if (status == 304) {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 304 Not Modified\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: public, max-age=0, must-revalidate\r\n"
                              "ETag: %s\r\n"
                              "%s"
                              "%s"
                              "\r\n",
                              etag, last_modified_header, vary_header);
    } else {
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: public, max-age=0, must-revalidate\r\n"
                              "ETag: %s\r\n"
                              "%s"
                              "%s"
                              "\r\n",
                              content_type, content_length, etag,
                              last_modified_header, vary_header);
    }
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        queue_all(fd, header, (size_t)header_len);
    }
}

static void send_cached_memory_response(int fd,
                                        const char *request,
                                        size_t header_length,
                                        const char *content_type,
                                        const void *body,
                                        size_t body_length,
                                        const time_t *last_modified,
                                        bool send_body,
                                        bool vary_outerframe_accept);

static void send_outer_descriptor(int fd,
                                  const char *request,
                                  size_t header_length,
                                  bool send_body) {
    const char *plugin_json = "{\"backendsAPIPath\":\"/api/backends\",\"logsAPIPath\":\"/api/logs\",\"controlAPIPath\":\"/api/control\",\"createAPIPath\":\"/api/create\",\"recipesAPIPath\":\"/api/recipes\",\"filePickerAPIPath\":\"/api/file-picker\"}";
    char bundle_path[PATH_MAX];
    bundle_url_path(bundle_path, sizeof(bundle_path));
    size_t path_len = strlen(bundle_path);
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
    memcpy(payload + header_len, bundle_path, path_len);
    memcpy(payload + data_offset, plugin_json, plugin_len);

    // The descriptor is generated in memory; a backend restart is a conservative
    // Last-Modified boundary, while its ETag tracks the exact payload.
    send_cached_memory_response(fd, request, header_length,
                                "application/vnd.outerframe", payload, total_len,
                                &g_backend_start_time,
                                send_body, true);
    free(payload);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;
    if (!dst || dst_size == 0) return;
    for (size_t i = 0; src && src[i] && out + 1 < dst_size; i++) {
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
    if (!query || !name) return false;
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

static void shell_quote(const char *value, char *out, size_t out_size) {
    if (out_size == 0) return;
    size_t pos = 0;
    if (pos + 1 < out_size) out[pos++] = '\'';
    for (const char *p = value ? value : ""; *p && pos + 5 < out_size; p++) {
        if (*p == '\'') {
            memcpy(out + pos, "'\\''", 4);
            pos += 4;
        } else {
            out[pos++] = *p;
        }
    }
    if (pos + 1 < out_size) out[pos++] = '\'';
    out[pos < out_size ? pos : out_size - 1] = '\0';
}

static void append_path_component(char *out, size_t out_size, const char *base, const char *component) {
    if (!out || out_size == 0) return;
    if (!base || !base[0]) {
        snprintf(out, out_size, "%s", component ? component : "");
        return;
    }
    if (!component || !component[0]) {
        snprintf(out, out_size, "%s", base);
        return;
    }
    size_t length = strlen(base);
    snprintf(out, out_size, "%s%s%s", base, length > 0 && base[length - 1] == '/' ? "" : "/", component);
}

static void join_url_path(char *out, size_t out_size, const char *base_url, const char *path) {
    if (!out || out_size == 0) return;
    if (!base_url || !base_url[0] || !path || !path[0]) {
        out[0] = '\0';
        return;
    }
    size_t len = strlen(base_url);
    snprintf(out, out_size, "%s%s%s", base_url, len > 0 && base_url[len - 1] == '/' ? "" : "/", path);
}

static const BundledAppDefinition *bundled_app_for_service_id(const char *service_id) {
    if (!service_id) return NULL;
    for (size_t i = 0; i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        if (strcmp(kBundledApps[i].service_id, service_id) == 0) {
#ifdef __APPLE__
            if (!kBundledApps[i].supports_macos) return NULL;
#endif
            return &kBundledApps[i];
        }
    }
    return NULL;
}

static bool directory_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool current_executable_path(char *out, size_t out_size) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)out_size;
    if (_NSGetExecutablePath(out, &size) != 0) return false;
    char resolved[PATH_MAX];
    if (realpath(out, resolved)) snprintf(out, out_size, "%s", resolved);
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

static bool remote_machine_uses_musl(void) {
    FILE *pipe = popen("ldd --version 2>&1", "r");
    if (pipe) {
        char line[256];
        bool saw_output = false;
        bool found_musl = false;
        while (fgets(line, sizeof(line), pipe)) {
            saw_output = true;
            if (strstr(line, "musl") || strstr(line, "Musl") || strstr(line, "MUSL")) {
                found_musl = true;
            }
        }
        pclose(pipe);
        if (saw_output) return found_musl;
    }

    struct utsname names;
    if (uname(&names) != 0) return false;
    char loader[PATH_MAX];
    snprintf(loader, sizeof(loader), "/lib/ld-musl-%s.so.1", names.machine);
    return access(loader, F_OK) == 0;
}

static const char *remote_linux_binary_directory(void) {
    return remote_machine_uses_musl() ? "RemoteLinuxBinariesMusl" : "RemoteLinuxBinaries";
}
#endif

static bool current_bundled_app_archive_platform(const BundledAppDefinition *app, char *out, size_t out_size) {
    if (!app || !out || out_size == 0) return false;
#ifdef __APPLE__
    if (!app->supports_macos) return false;
#if defined(__aarch64__) || defined(__arm64__)
    snprintf(out, out_size, "macos-arm64");
    return true;
#elif defined(__x86_64__)
    snprintf(out, out_size, "macos-x86_64");
    return true;
#else
    return false;
#endif
#else
    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) return false;
    if (strcmp(architecture, "aarch64") != 0 && strcmp(architecture, "x86_64") != 0) return false;
    snprintf(out, out_size, "linux-%s%s", architecture, remote_machine_uses_musl() ? "-musl" : "");
    return true;
#endif
}

static bool bundled_app_archive_relative_path(const BundledAppDefinition *app, char *out, size_t out_size) {
    char platform[64];
    if (!current_bundled_app_archive_platform(app, platform, sizeof(platform))) return false;
    snprintf(out, out_size, "%s/%s.tar.gz", app->stage_directory_name, platform);
    return true;
}

static bool bundled_app_archive_cache_path(const BundledAppDefinition *app, const char *cache_root, char *out, size_t out_size) {
    char platform[64];
    if (!current_bundled_app_archive_platform(app, platform, sizeof(platform))) return false;
    snprintf(out, out_size, "%s/%s-%s.tar.gz", cache_root, app->stage_directory_name, platform);
    return true;
}

static void bundled_apps_root(char *out, size_t out_size) {
    if (g_bundled_apps_directory[0]) {
        snprintf(out, out_size, "%s", g_bundled_apps_directory);
        return;
    }
    const char *env_root = getenv("OUTERSHELL_BUNDLED_APPS_DIR");
    if (env_root && env_root[0]) {
        expand_tilde_path(env_root, out, out_size);
        return;
    }
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
            }
        }
    }
    char cwd[PATH_MAX] = "";
    if (getcwd(cwd, sizeof(cwd))) {
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
#ifdef __APPLE__
    char app_bundle[PATH_MAX];
    snprintf(app_bundle, sizeof(app_bundle), "%s/%s.app", stage_root, app->stage_directory_name);
    char app_binary[PATH_MAX];
    snprintf(app_binary, sizeof(app_binary), "%s/Contents/MacOS/%s", app_bundle, app->binary_name);
    char app_bundle_arm[PATH_MAX];
    snprintf(app_bundle_arm, sizeof(app_bundle_arm), "%s/Contents/Resources/bundles/%s.bundle.macos-arm.aar", app_bundle, app->bundle_prefix);
    char app_bundle_x86[PATH_MAX];
    snprintf(app_bundle_x86, sizeof(app_bundle_x86), "%s/Contents/Resources/bundles/%s.bundle.macos-x86.aar", app_bundle, app->bundle_prefix);
    bool has_app_bundle = stat(app_binary, &st) == 0 && S_ISREG(st.st_mode) &&
                          stat(app_bundle_arm, &st) == 0 && S_ISREG(st.st_mode) &&
                          stat(app_bundle_x86, &st) == 0 && S_ISREG(st.st_mode);
    if (has_app_bundle && app->icon_name && app->icon_name[0]) {
        char app_icon[PATH_MAX];
        snprintf(app_icon, sizeof(app_icon), "%s/Contents/Resources/%s", app_bundle, app->icon_name);
        has_app_bundle = stat(app_icon, &st) == 0 && S_ISREG(st.st_mode);
    }
    if (has_app_bundle) return true;
#endif
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
    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) return false;
    char linux_binary[PATH_MAX];
    snprintf(linux_binary, sizeof(linux_binary), "%s/%s/%s/%s", stage_root, remote_linux_binary_directory(), architecture, app->binary_name);
    return stat(linux_binary, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static void bundled_app_download_cache_root(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/Caches/outershell/outer-shell/bundled-apps", home_directory());
#else
    if (geteuid() == 0) {
        snprintf(out, out_size, "/var/cache/outershell/outer-shell/bundled-apps");
        return;
    }
    const char *cache_home = getenv("XDG_CACHE_HOME");
    if (cache_home && cache_home[0]) {
        snprintf(out, out_size, "%s/outershell/outer-shell/bundled-apps", cache_home);
    } else {
        snprintf(out, out_size, "%s/.cache/outershell/outer-shell/bundled-apps", home_directory());
    }
#endif
}

static bool stage_bundled_app(const BundledAppDefinition *app, char *out_stage_root, size_t out_stage_root_size, char *message, size_t message_size) {
    bundled_app_stage_root(app, out_stage_root, out_stage_root_size);
    if (bundled_app_stage_has_expected_files(app, out_stage_root)) return true;

    char archive_relative_path[PATH_MAX];
    if (!bundled_app_archive_relative_path(app, archive_relative_path, sizeof(archive_relative_path))) {
        snprintf(message, message_size, "%s is not available for this platform.", app->display_name);
        return false;
    }

    char archive_url[2048];
    join_url_path(archive_url, sizeof(archive_url), g_bundled_apps_base_url, archive_relative_path);
    if (!archive_url[0]) {
        snprintf(message, message_size, "No app download URL is configured for %s.", app->display_name);
        return false;
    }

    char cache_root[PATH_MAX];
    bundled_app_download_cache_root(cache_root, sizeof(cache_root));
    if (!mkdir_p(cache_root)) {
        snprintf(message, message_size, "Failed to create app download cache at %s: %s", cache_root, strerror(errno));
        return false;
    }

    char archive_path[PATH_MAX];
    if (!bundled_app_archive_cache_path(app, cache_root, archive_path, sizeof(archive_path))) {
        snprintf(message, message_size, "%s is not available for this platform.", app->display_name);
        return false;
    }
    char download_error[1024] = "";
    if (!outer_shell_download_url_to_file(archive_url, archive_path, download_error, sizeof(download_error))) {
        snprintf(message, message_size, "Failed to download %s from %s: %s", app->display_name, archive_url, download_error);
        return false;
    }

    char quoted_archive_path[PATH_MAX + 8];
    char quoted_cache_root[PATH_MAX + 8];
    shell_quote(archive_path, quoted_archive_path, sizeof(quoted_archive_path));
    shell_quote(cache_root, quoted_cache_root, sizeof(quoted_cache_root));
    char command[4096];
    snprintf(command, sizeof(command), "tar -xzf %s -C %s", quoted_archive_path, quoted_cache_root);
    if (system(command) != 0) {
        snprintf(message, message_size, "Failed to extract %s.", app->display_name);
        return false;
    }

    snprintf(out_stage_root, out_stage_root_size, "%s/%s", cache_root, app->stage_directory_name);
    if (!bundled_app_stage_has_expected_files(app, out_stage_root)) {
        snprintf(message, message_size, "Downloaded %s, but its payload is incomplete.", app->display_name);
        return false;
    }
    return true;
}

static bool build_home_screen_update_url(const char *path, const char *heartbeat, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!g_home_screen_public_base_url[0]) {
        return false;
    }
    char base_url[2048];
    snprintf(base_url, sizeof(base_url), "%s", g_home_screen_public_base_url);
    size_t len = strlen(base_url);
    while (len > 0 && base_url[len - 1] == '/') base_url[--len] = '\0';
    const char *trimmed_path = path && path[0] == '/' ? path + 1 : (path ? path : "");
    StringBuilder url = {0};
    bool ok = sb_append(&url, base_url) &&
              sb_append(&url, "/") &&
              sb_append(&url, trimmed_path) &&
              sb_append(&url, "?") &&
              outer_shell_append_update_query(&url,
                                              heartbeat,
                                              NULL,
                                              getenv("OUTER_SHELL_SERVICE_MANAGER"));
    if (ok) snprintf(out, out_size, "%s", url.data ? url.data : "");
    free(url.data);
    return ok && out[0] != '\0';
}

static bool fetch_home_screen_available_version(const char *heartbeat, char *out, size_t out_size, char *message, size_t message_size) {
    if (out && out_size > 0) out[0] = '\0';
    char url[4096];
    if (!build_home_screen_update_url("latest/version.txt", heartbeat, url, sizeof(url))) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    char download_error[512] = "";
    bool fetched = outer_shell_fetch_url_text(url, out, out_size, download_error, sizeof(download_error));
    if (!fetched) {
        snprintf(message, message_size, "Could not fetch Outer Shell version: %s", download_error);
        return false;
    }
    while (*out && isspace((unsigned char)out[strlen(out) - 1])) out[strlen(out) - 1] = '\0';
    while (*out && isspace((unsigned char)*out)) memmove(out, out + 1, strlen(out));
    if (!out[0]) {
        snprintf(message, message_size, "Outer Shell version file was empty.");
        return false;
    }
    return true;
}

static void home_screen_install_cache_root(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/Caches/outershell/outer-shell/install", home_directory());
#else
    if (geteuid() == 0) {
        snprintf(out, out_size, "/var/cache/outershell/outer-shell/install");
        return;
    }
    const char *cache_home = getenv("XDG_CACHE_HOME");
    if (cache_home && cache_home[0]) {
        snprintf(out, out_size, "%s/outershell/outer-shell/install", cache_home);
    } else {
        snprintf(out, out_size, "%s/.cache/outershell/outer-shell/install", home_directory());
    }
#endif
}

static bool stage_home_screen_installer(char *script_path, size_t script_path_size, char *archive_path, size_t archive_path_size, char *message, size_t message_size) {
    if (!g_home_screen_public_base_url[0]) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    char cache_root[PATH_MAX];
    home_screen_install_cache_root(cache_root, sizeof(cache_root));
    if (!mkdir_p(cache_root)) {
        snprintf(message, message_size, "Failed to create install cache at %s: %s", cache_root, strerror(errno));
        return false;
    }

    snprintf(script_path, script_path_size, "%s/install.sh", cache_root);
    char script_url[4096];
    if (!build_home_screen_update_url("latest/install.sh", "extra", script_url, sizeof(script_url))) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    char error[512] = "";
    if (!outer_shell_download_url_to_file(script_url, script_path, error, sizeof(error))) {
        snprintf(message, message_size, "Failed to download Outer Shell install script: %s", error);
        return false;
    }
    chmod(script_path, 0755);

#ifdef __APPLE__
#if defined(__x86_64__)
    const char *archive_name = "outer-shell-macos-x86_64.zip";
#else
    const char *archive_name = "outer-shell-macos-arm64.zip";
#endif
#else
    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) {
        snprintf(message, message_size, "Unsupported machine architecture.");
        return false;
    }
    char linux_archive_name[128];
    snprintf(linux_archive_name, sizeof(linux_archive_name), "outer-shell-linux-%s%s.tar.gz", architecture, remote_machine_uses_musl() ? "-musl" : "");
    const char *archive_name = linux_archive_name;
#endif
    snprintf(archive_path, archive_path_size, "%s/%s", cache_root, archive_name);
    char archive_url[4096];
    char base_url[2048];
    snprintf(base_url, sizeof(base_url), "%s", g_home_screen_public_base_url);
    size_t len = strlen(base_url);
    while (len > 0 && base_url[len - 1] == '/') base_url[--len] = '\0';
    snprintf(archive_url, sizeof(archive_url), "%s/latest/%s", base_url, archive_name);
    if (!outer_shell_download_url_to_file(archive_url, archive_path, error, sizeof(error))) {
        snprintf(message, message_size, "Failed to download Outer Shell archive: %s", error);
        return false;
    }
    return true;
}

static bool operation_installs_bundled_app(const char *operation) {
    return strcmp(operation, "run") == 0 ||
           strcmp(operation, "install") == 0 ||
           strcmp(operation, "runRoot") == 0 ||
           strcmp(operation, "installRoot") == 0 ||
           strcmp(operation, "runUser") == 0 ||
           strcmp(operation, "installUser") == 0 ||
           strcmp(operation, "addRootSupport") == 0;
}

static bool append_form_separator_if_needed(StringBuilder *builder) {
    return builder->length == 0 || sb_append(builder, "&");
}

static bool augment_control_request_body(const char *query,
                                         const char *body,
                                         StringBuilder *owned_body,
                                         const char **out_body,
                                         size_t *out_body_length,
                                         char *error,
                                         size_t error_size) {
    *out_body = body ? body : "";
    *out_body_length = body ? strlen(body) : 0;
    char service_id[PATH_MAX] = "";
    char operation[64] = "";
    if (!query_value_any(query, body, "serviceID", service_id, sizeof(service_id)) ||
        !query_value_any(query, body, "operation", operation, sizeof(operation))) {
        return true;
    }

    if (operation_installs_bundled_app(operation)) {
        const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
        if (!app) return true;
        char stage_root[PATH_MAX] = "";
        if (!stage_bundled_app(app, stage_root, sizeof(stage_root), error, error_size)) {
            return false;
        }
        if (!sb_append(owned_body, body ? body : "") ||
            !append_form_separator_if_needed(owned_body) ||
            !sb_append(owned_body, "bundledStageRoot=")) {
            snprintf(error, error_size, "Out of memory.");
            return false;
        }
        append_url_encoded(owned_body, stage_root);
        *out_body = owned_body->data;
        *out_body_length = owned_body->length;
        return true;
    }

    if (strcmp(service_id, "org.outershell.OuterShell") == 0) {
        if (strcmp(operation, "checkUpdate") == 0 || strcmp(operation, "checkOuterShellUpdate") == 0) {
            char version[128] = "";
            if (!fetch_home_screen_available_version("extra", version, sizeof(version), error, error_size)) {
                return false;
            }
            if (!sb_append(owned_body, body ? body : "") ||
                !append_form_separator_if_needed(owned_body) ||
                !sb_append(owned_body, "availableVersion=")) {
                snprintf(error, error_size, "Out of memory.");
                return false;
            }
            append_url_encoded(owned_body, version);
            *out_body = owned_body->data;
            *out_body_length = owned_body->length;
            return true;
        }
        if (strcmp(operation, "update") == 0 || strcmp(operation, "updateOuterShell") == 0 ||
            strcmp(operation, "uninstall") == 0 || strcmp(operation, "uninstallOuterShell") == 0) {
            char script_path[PATH_MAX] = "";
            char archive_path[PATH_MAX] = "";
            if (!stage_home_screen_installer(script_path, sizeof(script_path), archive_path, sizeof(archive_path), error, error_size)) {
                return false;
            }
            if (!sb_append(owned_body, body ? body : "") ||
                !append_form_separator_if_needed(owned_body) ||
                !sb_append(owned_body, "installerScriptPath=")) {
                snprintf(error, error_size, "Out of memory.");
                return false;
            }
            append_url_encoded(owned_body, script_path);
            if (!sb_append(owned_body, "&installerArchivePath=")) {
                snprintf(error, error_size, "Out of memory.");
                return false;
            }
            append_url_encoded(owned_body, archive_path);
            *out_body = owned_body->data;
            *out_body_length = owned_body->length;
            return true;
        }
    }

    return true;
}

static const char *find_http_line_end(const char *cursor, const char *end) {
    while (cursor < end) {
        if (*cursor == '\0' ||
            (cursor + 1 < end && cursor[0] == '\r' && cursor[1] == '\n')) {
            return cursor;
        }
        cursor++;
    }
    return end;
}

static bool request_header_value(const char *request,
                                 size_t header_length,
                                 const char *header_name,
                                 const char **out_value,
                                 size_t *out_value_length) {
    const char *cursor = request;
    const char *end = request + header_length;
    const char *line_end = find_http_line_end(cursor, end);
    if (line_end >= end || *line_end == '\0') return false;
    cursor = line_end + 2;

    size_t header_name_length = strlen(header_name);
    while (cursor < end && *cursor != '\0') {
        line_end = find_http_line_end(cursor, end);
        if (line_end == cursor) break;
        const char *colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon &&
            (size_t)(colon - cursor) == header_name_length &&
            strncasecmp(cursor, header_name, header_name_length) == 0) {
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            const char *value_end = line_end;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;
            *out_value = value;
            *out_value_length = (size_t)(value_end - value);
            return true;
        }
        if (line_end >= end || *line_end == '\0') break;
        cursor = line_end + 2;
    }
    return false;
}

static long bundle_mtime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#else
    return st->st_mtim.tv_nsec;
#endif
}

static long bundle_ctime_nanoseconds(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_ctimespec.tv_nsec;
#else
    return st->st_ctim.tv_nsec;
#endif
}

static void bundle_etag(const struct stat *st, char *out, size_t out_size) {
    snprintf(out, out_size, "W/\"%llx-%lx-%llx-%lx-%llx\"",
             (unsigned long long)st->st_mtime,
             (unsigned long)bundle_mtime_nanoseconds(st),
             (unsigned long long)st->st_ctime,
             (unsigned long)bundle_ctime_nanoseconds(st),
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

    static const char *formats[] = {
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

static bool cached_response_is_not_modified(const char *request,
                                            size_t header_length,
                                            const char *etag,
                                            const time_t *last_modified) {
    const char *condition = NULL;
    size_t condition_length = 0;
    bool has_if_none_match = request_header_value(request, header_length,
                                                   "If-None-Match",
                                                   &condition, &condition_length);
    if (has_if_none_match) {
        return if_none_match_matches(condition, condition_length, etag);
    }
    if (last_modified &&
        request_header_value(request, header_length, "If-Modified-Since",
                             &condition, &condition_length)) {
        time_t modified_since;
        return parse_http_date(condition, condition_length, &modified_since) &&
               *last_modified <= modified_since;
    }
    return false;
}

static void memory_response_etag(const void *body, size_t body_length, char *out, size_t out_size) {
    const unsigned char *bytes = body;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < body_length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, out_size, "W/\"%016llx-%zx\"",
             (unsigned long long)hash, body_length);
}

static void send_cached_memory_response(int fd,
                                        const char *request,
                                        size_t header_length,
                                        const char *content_type,
                                        const void *body,
                                        size_t body_length,
                                        const time_t *last_modified,
                                        bool send_body,
                                        bool vary_outerframe_accept) {
    char etag[96];
    char last_modified_text[64] = "";
    memory_response_etag(body, body_length, etag, sizeof(etag));
    if (last_modified) {
        format_http_date(*last_modified, last_modified_text, sizeof(last_modified_text));
    }
    if (cached_response_is_not_modified(request, header_length, etag, last_modified)) {
        send_cached_response_header(fd, 304, content_type, 0, etag, last_modified_text,
                                    vary_outerframe_accept);
        return;
    }
    send_cached_response_header(fd, 200, content_type, body_length, etag, last_modified_text,
                                vary_outerframe_accept);
    if (send_body && body_length > 0) queue_all(fd, body, body_length);
}

static void send_cached_file(int fd,
                             const char *path,
                             const char *content_type,
                             const char *request,
                             size_t header_length,
                             bool send_body,
                             bool vary_outerframe_accept) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_text_response(fd, 404, "not found\n");
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_text_response(fd, 500, "failed to stat file\n");
        return;
    }
    char etag[96];
    char last_modified[64];
    bundle_etag(&st, etag, sizeof(etag));
    format_http_date(st.st_mtime, last_modified, sizeof(last_modified));

    if (cached_response_is_not_modified(request, header_length, etag, &st.st_mtime)) {
        close(file_fd);
        send_cached_response_header(fd, 304, content_type, 0, etag, last_modified,
                                    vary_outerframe_accept);
        return;
    }

    size_t size = (size_t)st.st_size;
    if (!send_body) {
        close(file_fd);
        send_cached_response_header(fd, 200, content_type, size, etag, last_modified,
                                    vary_outerframe_accept);
        return;
    }
    unsigned char *data = malloc(size > 0 ? size : 1);
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
            send_text_response(fd, 500, "failed to read file\n");
            return;
        }
        if (got == 0) break;
        offset += (size_t)got;
    }
    close(file_fd);
    send_cached_response_header(fd, 200, content_type, offset, etag, last_modified,
                                vary_outerframe_accept);
    if (offset > 0) queue_all(fd, data, offset);
    free(data);
}

static void send_web_file(int fd,
                          const char *filename,
                          const char *content_type,
                          const char *request,
                          size_t header_length,
                          bool send_body,
                          bool vary_outerframe_accept) {
    if (!g_web_root_directory[0]) {
        send_text_response(fd, 404, "Outer Shell web frontend is not installed.\n");
        return;
    }
    if (!filename || !filename[0] || strchr(filename, '/') || strstr(filename, "..")) {
        send_text_response(fd, 404, "not found\n");
        return;
    }

    char path[PATH_MAX];
    append_path_component(path, sizeof(path), g_web_root_directory, filename);
    send_cached_file(fd, path, content_type, request, header_length, send_body,
                     vary_outerframe_accept);
}

static void send_web_app_icon(int fd,
                              const char *request,
                              size_t header_length,
                              bool send_body) {
    if (!g_web_root_directory[0]) {
        send_text_response(fd, 404, "Outer Shell web frontend is not installed.\n");
        return;
    }

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/../app-icon.png", g_web_root_directory);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        send_text_response(fd, 404, "not found\n");
        return;
    }
    send_cached_file(fd, path, "image/png", request, header_length, send_body, false);
}

static bool archive_append_u16(StringBuilder *archive, uint16_t value) {
    unsigned char bytes[2];
    write_uint16_le(bytes, value);
    return sb_append_n(archive, (const char *)bytes, sizeof(bytes));
}

static bool archive_append_u32(StringBuilder *archive, uint32_t value) {
    unsigned char bytes[4];
    write_uint32_le(bytes, value);
    return sb_append_n(archive, (const char *)bytes, sizeof(bytes));
}

static bool archive_append_u64(StringBuilder *archive, uint64_t value) {
    unsigned char bytes[8];
    write_uint64_le(bytes, value);
    return sb_append_n(archive, (const char *)bytes, sizeof(bytes));
}

static bool archive_append_template_file(StringBuilder *archive,
                                         const char *root,
                                         const char *relative_path,
                                         const struct stat *st) {
    size_t relative_length = strlen(relative_path);
    if (relative_length == 0 || relative_length > UINT16_MAX) {
        return false;
    }
    if (st->st_size < 0) {
        return false;
    }

    char full_path[PATH_MAX];
    int full_length = snprintf(full_path, sizeof(full_path), "%s/%s", root, relative_path);
    if (full_length < 0 || (size_t)full_length >= sizeof(full_path)) {
        return false;
    }

    int file_fd = open(full_path, O_RDONLY);
    if (file_fd < 0) {
        return false;
    }

    bool ok = archive_append_u16(archive, (uint16_t)relative_length) &&
              archive_append_u32(archive, (uint32_t)(st->st_mode & 0777)) &&
              archive_append_u64(archive, (uint64_t)st->st_size) &&
              sb_append_n(archive, relative_path, relative_length);

    char buffer[32768];
    off_t remaining = st->st_size;
    while (ok && remaining > 0) {
        ssize_t got = read(file_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (got == 0) {
            ok = false;
            break;
        }
        ok = sb_append_n(archive, buffer, (size_t)got);
        remaining -= got;
    }

    close(file_fd);
    return ok;
}

static bool archive_append_template_directory(StringBuilder *archive,
                                              const char *root,
                                              const char *relative_directory) {
    char full_directory[PATH_MAX];
    if (relative_directory[0]) {
        int full_length = snprintf(full_directory, sizeof(full_directory), "%s/%s", root, relative_directory);
        if (full_length < 0 || (size_t)full_length >= sizeof(full_directory)) {
            return false;
        }
    } else {
        snprintf(full_directory, sizeof(full_directory), "%s", root);
    }

    DIR *dir = opendir(full_directory);
    if (!dir) {
        return false;
    }

    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strcmp(name, ".DS_Store") == 0 || strncmp(name, "._", 2) == 0) {
            continue;
        }

        char child_relative[PATH_MAX];
        int child_length;
        if (relative_directory[0]) {
            child_length = snprintf(child_relative, sizeof(child_relative), "%s/%s", relative_directory, name);
        } else {
            child_length = snprintf(child_relative, sizeof(child_relative), "%s", name);
        }
        if (child_length < 0 || (size_t)child_length >= sizeof(child_relative)) {
            ok = false;
            break;
        }

        char child_full[PATH_MAX];
        int full_length = snprintf(child_full, sizeof(child_full), "%s/%s", root, child_relative);
        if (full_length < 0 || (size_t)full_length >= sizeof(child_full)) {
            ok = false;
            break;
        }

        struct stat st;
        if (lstat(child_full, &st) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = archive_append_template_directory(archive, root, child_relative);
        } else if (S_ISREG(st.st_mode)) {
            ok = archive_append_template_file(archive, root, child_relative, &st);
        }
    }

    closedir(dir);
    return ok;
}

static void send_native_app_template_archive(int fd) {
    if (!g_native_app_template_directory[0]) {
        send_text_response(fd, 404, "native app template directory is not configured\n");
        return;
    }

    struct stat st;
    if (stat(g_native_app_template_directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char message[PATH_MAX + 96];
        snprintf(message, sizeof(message), "native app template directory not found at %s\n", g_native_app_template_directory);
        send_text_response(fd, 404, message);
        return;
    }

    StringBuilder archive = {0};
    bool ok = sb_append_n(&archive, "OSNTPL1", 7) &&
              sb_append_n(&archive, "\0", 1) &&
              archive_append_template_directory(&archive, g_native_app_template_directory, "") &&
              archive_append_u16(&archive, 0);
    if (!ok) {
        free(archive.data);
        send_text_response(fd, 500, "failed to build native app template archive\n");
        return;
    }

    send_response(fd,
                  200,
                  "OK",
                  "application/vnd.outershell.native-app-template",
                  archive.data,
                  archive.length);
    free(archive.data);
}

static bool native_app_value_has_no_controls(const char *value, size_t maximum_length) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length > maximum_length) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static bool native_app_value_uses_characters(const char *value,
                                             const char *characters,
                                             size_t maximum_length) {
    if (!native_app_value_has_no_controls(value, maximum_length)) return false;
    return strspn(value, characters) == strlen(value);
}

static bool native_app_scheme_is_valid(const char *value) {
    if (!native_app_value_has_no_controls(value, 128)) return false;
    if (!(isalpha((unsigned char)value[0]) || value[0] == '_')) return false;
    for (const unsigned char *p = (const unsigned char *)value + 1; *p; p++) {
        if (!(isalnum(*p) || *p == '_')) return false;
    }
    return true;
}

typedef struct {
    char *form;
    const unsigned char *icon_png;
    size_t icon_png_length;
} NativeAppProjectRequest;

static bool parse_native_app_project_request(const char *body,
                                             size_t body_length,
                                             NativeAppProjectRequest *request) {
    memset(request, 0, sizeof(*request));
    const unsigned char *bytes = (const unsigned char *)body;
    size_t form_length = body_length;
    if (body_length >= 16 && memcmp(bytes, "OSNREQ1\0", 8) == 0) {
        form_length = read_uint32_le(bytes + 8);
        size_t icon_length = read_uint32_le(bytes + 12);
        if (form_length > 16 * 1024 || icon_length > 4 * 1024 * 1024 ||
            form_length > body_length - 16 || icon_length != body_length - 16 - form_length) {
            return false;
        }
        request->icon_png = bytes + 16 + form_length;
        request->icon_png_length = icon_length;
        bytes += 16;
    } else if (memchr(body, '\0', body_length) != NULL) {
        return false;
    }
    request->form = malloc(form_length + 1);
    if (!request->form) return false;
    memcpy(request->form, bytes, form_length);
    request->form[form_length] = '\0';
    return true;
}

static bool write_native_app_file(const char *path, const void *bytes, size_t length) {
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) return false;
    const unsigned char *cursor = bytes;
    size_t remaining = length;
    bool ok = true;
    while (remaining > 0) {
        ssize_t written = write(output, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (written == 0) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    if (close(output) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

static bool native_app_icon_is_valid(const unsigned char *png, size_t length) {
    static const unsigned char signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (!png || length < 33 || length > 4 * 1024 * 1024 ||
        memcmp(png, signature, sizeof(signature)) != 0 ||
        memcmp(png + 12, "IHDR", 4) != 0) {
        return false;
    }
    uint32_t width = ((uint32_t)png[16] << 24) | ((uint32_t)png[17] << 16) |
                     ((uint32_t)png[18] << 8) | png[19];
    uint32_t height = ((uint32_t)png[20] << 24) | ((uint32_t)png[21] << 16) |
                      ((uint32_t)png[22] << 8) | png[23];
    return width == 1024 && height == 1024;
}

static bool remove_native_app_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int child_length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (child_length < 0 || (size_t)child_length >= sizeof(child) ||
            !remove_native_app_tree(child)) {
            ok = false;
        }
    }
    closedir(directory);
    return rmdir(path) == 0 && ok;
}

static bool make_native_app_temp_directory(char *path, size_t path_size) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    int length = snprintf(path, path_size, "%s%soutershell-native-project.XXXXXX",
                          base, base[strlen(base) - 1] == '/' ? "" : "/");
    return length > 0 && (size_t)length < path_size && mkdtemp(path) != NULL;
}

static void send_native_app_project_creation(int fd,
                                             const char *query,
                                             const char *body,
                                             size_t body_length) {
    NativeAppProjectRequest request;
    if (!parse_native_app_project_request(body, body_length, &request)) {
        send_text_response(fd, 400, "invalid native app project request\n");
        return;
    }

    char name[256], app_id[256], scheme[160], folder[160], socket_name[256];
    char source_root[PATH_MAX] = "~/outerframe-apps";
    char targets[64], macos_language[32], backend_language[32], isolation[32];
    if (!query_value_any(query, request.form, "name", name, sizeof(name)) ||
        !query_value_any(query, request.form, "appID", app_id, sizeof(app_id)) ||
        !query_value_any(query, request.form, "scheme", scheme, sizeof(scheme)) ||
        !query_value_any(query, request.form, "folder", folder, sizeof(folder)) ||
        !query_value_any(query, request.form, "socket", socket_name, sizeof(socket_name)) ||
        !query_value_any(query, request.form, "targets", targets, sizeof(targets)) ||
        !query_value_any(query, request.form, "macOSLanguage", macos_language, sizeof(macos_language)) ||
        !query_value_any(query, request.form, "backendLanguage", backend_language, sizeof(backend_language)) ||
        !query_value_any(query, request.form, "isolation", isolation, sizeof(isolation))) {
        free(request.form);
        send_text_response(fd, 400, "missing native app project configuration\n");
        return;
    }
    (void)query_value_any(query, request.form, "sourceRoot", source_root, sizeof(source_root));
    free(request.form);

    static const char *component_characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
    if (!native_app_value_has_no_controls(name, 200) || strpbrk(name, "\"\\$`") != NULL ||
        !native_app_value_uses_characters(app_id, component_characters, 200) ||
        !native_app_value_has_no_controls(source_root, sizeof(source_root) - 1) ||
        (source_root[0] != '/' && strcmp(source_root, "~") != 0 && strncmp(source_root, "~/", 2) != 0) ||
        strpbrk(source_root, "\"\\$`") != NULL ||
        !native_app_value_uses_characters(folder, component_characters, 128) ||
        strcmp(folder, ".") == 0 || strcmp(folder, "..") == 0 ||
        !native_app_value_uses_characters(socket_name, component_characters, 200) ||
        !native_app_scheme_is_valid(scheme) ||
        (strcmp(targets, "html") != 0 && strcmp(targets, "macos") != 0 && strcmp(targets, "html macos") != 0) ||
        (strcmp(macos_language, "swift") != 0 && strcmp(macos_language, "objc") != 0) ||
        (strcmp(backend_language, "go") != 0 && strcmp(backend_language, "c") != 0) ||
        (strcmp(isolation, "container") != 0 && strcmp(isolation, "host") != 0)) {
        send_text_response(fd, 400, "invalid native app project configuration\n");
        return;
    }

    if (request.icon_png_length > 0 && !native_app_icon_is_valid(request.icon_png, request.icon_png_length)) {
        send_text_response(fd, 400, "invalid native app icon\n");
        return;
    }

    if (!g_native_app_template_directory[0]) {
        send_text_response(fd, 503, "native app template directory is not configured\n");
        return;
    }
    char helper[PATH_MAX];
    snprintf(helper, sizeof(helper), "%s/create-project.py", g_native_app_template_directory);
    struct stat helper_stat;
    if (stat(helper, &helper_stat) != 0 || !S_ISREG(helper_stat.st_mode)) {
        send_text_response(fd, 503, "native app project creator is unavailable\n");
        return;
    }

    char temp_root[PATH_MAX];
    if (!make_native_app_temp_directory(temp_root, sizeof(temp_root))) {
        send_text_response(fd, 500, "failed to create native app project workspace\n");
        return;
    }
    char builder_output[PATH_MAX];
    char icon_path[PATH_MAX];
    snprintf(builder_output, sizeof(builder_output), "%s/response", temp_root);
    snprintf(icon_path, sizeof(icon_path), "%s/app-icon.png", temp_root);
    if (request.icon_png_length > 0 &&
        !write_native_app_file(icon_path, request.icon_png, request.icon_png_length)) {
        (void)remove_native_app_tree(temp_root);
        send_text_response(fd, 500, "failed to stage native app icon\n");
        return;
    }

    char quoted_helper[PATH_MAX * 4 + 8];
    char quoted_builder_output[PATH_MAX * 4 + 8];
    char quoted_icon_path[PATH_MAX * 4 + 8];
    char quoted_source_root[PATH_MAX * 4 + 8];
    char quoted_name[sizeof(name) * 4 + 8];
    char quoted_app_id[sizeof(app_id) * 4 + 8];
    char quoted_scheme[sizeof(scheme) * 4 + 8];
    char quoted_folder[sizeof(folder) * 4 + 8];
    char quoted_socket[sizeof(socket_name) * 4 + 8];
    char quoted_targets[sizeof(targets) * 4 + 8];
    shell_quote(helper, quoted_helper, sizeof(quoted_helper));
    shell_quote(builder_output, quoted_builder_output, sizeof(quoted_builder_output));
    shell_quote(icon_path, quoted_icon_path, sizeof(quoted_icon_path));
    shell_quote(source_root, quoted_source_root, sizeof(quoted_source_root));
    shell_quote(name, quoted_name, sizeof(quoted_name));
    shell_quote(app_id, quoted_app_id, sizeof(quoted_app_id));
    shell_quote(scheme, quoted_scheme, sizeof(quoted_scheme));
    shell_quote(folder, quoted_folder, sizeof(quoted_folder));
    shell_quote(socket_name, quoted_socket, sizeof(quoted_socket));
    shell_quote(targets, quoted_targets, sizeof(quoted_targets));

    char icon_option[PATH_MAX * 4 + 32] = "";
    if (request.icon_png_length > 0) {
        snprintf(icon_option, sizeof(icon_option), " --icon %s", quoted_icon_path);
    }
    char command[PATH_MAX * 16 + 8192];
    int command_length = snprintf(command, sizeof(command),
                                  "python3 %s --name %s --app-id %s --scheme %s --source-root %s --folder %s "
                                  "--socket %s --targets %s --macos-language %s "
                                  "--backend-language %s --isolation %s --builder-output %s%s 2>&1",
                                  quoted_helper, quoted_name, quoted_app_id, quoted_scheme,
                                  quoted_source_root, quoted_folder, quoted_socket, quoted_targets, macos_language,
                                  backend_language, isolation, quoted_builder_output, icon_option);
    if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
        (void)remove_native_app_tree(temp_root);
        send_text_response(fd, 500, "native app project command is too long\n");
        return;
    }

    FILE *pipe = popen(command, "r");
    if (!pipe) {
        (void)remove_native_app_tree(temp_root);
        send_text_response(fd, 500, "failed to start native app project creation\n");
        return;
    }
    StringBuilder output = {0};
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), pipe)) {
        if (output.length < 128 * 1024) {
            size_t available = 128 * 1024 - output.length;
            size_t length = strlen(chunk);
            (void)sb_append_n(&output, chunk, length < available ? length : available);
        }
    }
    int wait_status = pclose(pipe);
    int exit_status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    if (!output.data || output.length == 0) {
        (void)sb_append(&output, exit_status == 0
            ? "native app project installed\n"
            : "native app project creation failed\n");
    }
    int http_status = exit_status == 0 ? 200 : (exit_status == 17 ? 409 : (exit_status == 2 ? 400 : 500));
    if (http_status == 200) {
        StringBuilder archive = {0};
        bool archive_ok = sb_append_n(&archive, "OSNTPL1", 7) &&
                          sb_append_n(&archive, "\0", 1) &&
                          archive_append_template_directory(&archive, builder_output, "") &&
                          archive_append_u16(&archive, 0);
        if (archive_ok) {
            send_response(fd, 200, "OK", "application/vnd.outershell.native-app-project",
                          archive.data, archive.length);
        } else {
            send_text_response(fd, 500, "failed to package native app project response\n");
        }
        free(archive.data);
    } else {
        const char *reason = http_status == 409 ? "Conflict" :
                             (http_status == 400 ? "Bad Request" : "Internal Server Error");
        send_response(fd, http_status, reason, "text/plain; charset=utf-8", output.data, output.length);
    }
    free(output.data);
    (void)remove_native_app_tree(temp_root);
}

static void dispatch_native_app_project_creation(int fd,
                                                 const char *query,
                                                 const char *body,
                                                 size_t body_length) {
    pid_t worker = fork();
    if (worker < 0) {
        send_text_response(fd, 500, "failed to start native app project worker\n");
        return;
    }
    if (worker != 0) return;

    long descriptor_limit = sysconf(_SC_OPEN_MAX);
    if (descriptor_limit < 0 || descriptor_limit > 65536) descriptor_limit = 65536;
    for (int candidate = 3; candidate < descriptor_limit; candidate++) {
        if (candidate != fd) close(candidate);
    }
    send_native_app_project_creation(fd, query, body, body_length);
    close(fd);
    _exit(0);
}

static bool is_navigator_route(const char *target) {
    return strcmp(target, "/") == 0 ||
           strcmp(target, "/apps") == 0 ||
           strcmp(target, "/backends") == 0 ||
           strcmp(target, "/new") == 0 ||
           strcmp(target, "/backends.outer") == 0;
}

static bool request_accepts_outerframe(const char *request, size_t header_length) {
    const char *header_name = "Outerframe-Accept:";
    size_t header_name_length = strlen(header_name);
    const char *cursor = request;
    const char *end = request + header_length;
    while (cursor < end) {
        const char *line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) line_end = end;
        if ((size_t)(line_end - cursor) >= header_name_length &&
            strncasecmp(cursor, header_name, header_name_length) == 0) {
            const char *value = cursor + header_name_length;
            while (value < line_end && isspace((unsigned char)*value)) value++;
            size_t value_length = (size_t)(line_end - value);
            const char *media_type = "application/vnd.outerframe";
            size_t media_type_length = strlen(media_type);
            for (size_t i = 0; i + media_type_length <= value_length; i++) {
                if (strncasecmp(value + i, media_type, media_type_length) == 0) return true;
            }
            return false;
        }
        if (line_end >= end) break;
        cursor = line_end + 2;
    }
    return false;
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

static bool parsed_uint64_header(const char *request,
                                 size_t header_length,
                                 const char *name,
                                 uint64_t *value) {
    char header[READ_BUFFER_SIZE];
    if (header_length >= sizeof(header)) return false;
    memcpy(header, request, header_length);
    header[header_length] = '\0';

    char needle[128];
    int needle_length = snprintf(needle, sizeof(needle), "\r\n%s:", name);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return false;
    char *line = strcasestr(header, needle);
    if (!line) return false;
    char *text = line + needle_length;
    while (*text == ' ' || *text == '\t') text++;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\r' && *end != '\n' && *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool header_value_contains(const char *request,
                                  size_t header_length,
                                  const char *name,
                                  const char *value) {
    char header[READ_BUFFER_SIZE];
    if (header_length >= sizeof(header)) return false;
    memcpy(header, request, header_length);
    header[header_length] = '\0';

    char needle[128];
    int needle_length = snprintf(needle, sizeof(needle), "\r\n%s:", name);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return false;
    char *line = strcasestr(header, needle);
    if (!line) return false;
    char *line_end = strstr(line + needle_length, "\r\n");
    if (!line_end) line_end = header + header_length;
    char saved = *line_end;
    *line_end = '\0';
    bool contains = strcasestr(line + needle_length, value) != NULL;
    *line_end = saved;
    return contains;
}

static const char *container_transfer_id_from_target(const char *target) {
    static const char prefix[] = "/api/container-transfers/";
    if (!target || strncmp(target, prefix, sizeof(prefix) - 1) != 0) return NULL;
    const char *identifier = target + sizeof(prefix) - 1;
    if (strlen(identifier) != 36) return NULL;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (identifier[i] != '-') return NULL;
        } else if (!isxdigit((unsigned char)identifier[i])) {
            return NULL;
        }
    }
    return identifier;
}

static int open_container_upload(const char *transfer_id, int flags, struct stat *status) {
    if (!g_container_transfers_directory[0] || !transfer_id) return -1;
    int root_fd = open(g_container_transfers_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) return -1;
    int transfer_fd = openat(root_fd, transfer_id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root_fd);
    if (transfer_fd < 0) return -1;
    int upload_fd = openat(transfer_fd,
                           "upload.outershell-container",
                           flags | O_CLOEXEC | O_NOFOLLOW);
    close(transfer_fd);
    if (upload_fd < 0) return -1;
    if (fstat(upload_fd, status) != 0 || !S_ISREG(status->st_mode)) {
        close(upload_fd);
        return -1;
    }
    return upload_fd;
}

static bool write_container_upload_bytes(ReactorClient *client,
                                         const char *bytes,
                                         size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t wrote = write(client->container_upload_fd, bytes + offset, length - offset);
        if (wrote > 0) {
            offset += (size_t)wrote;
            client->container_upload_received += (uint64_t)wrote;
            continue;
        }
        if (wrote < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

typedef enum {
    CONTAINER_UPLOAD_NOT_HANDLED = 0,
    CONTAINER_UPLOAD_STREAMING = 1,
    CONTAINER_UPLOAD_FINISHED = 2
} ContainerUploadDispatch;

static ContainerUploadDispatch begin_container_upload_if_ready(ReactorClient *client,
                                                                bool *should_close) {
    const char *separator = strstr(client->request, "\r\n\r\n");
    if (!separator) return CONTAINER_UPLOAD_NOT_HANDLED;
    size_t header_length = (size_t)(separator + 4 - client->request);
    char method[16] = "";
    char target[1024] = "";
    char version[16] = "";
    if (sscanf(client->request, "%15s %1023s %15s", method, target, version) != 3 ||
        strcasecmp(method, "PATCH") != 0) {
        return CONTAINER_UPLOAD_NOT_HANDLED;
    }
    const char *transfer_id = container_transfer_id_from_target(target);
    if (!transfer_id) return CONTAINER_UPLOAD_NOT_HANDLED;

    uint64_t upload_offset = 0;
    uint64_t content_length = 0;
    if (!parsed_uint64_header(client->request, header_length, "Upload-Offset", &upload_offset) ||
        !parsed_uint64_header(client->request, header_length, "Content-Length", &content_length)) {
        send_text_response(client->fd, 400, "Upload-Offset and Content-Length are required.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }

    struct stat status;
    int upload_fd = open_container_upload(transfer_id, O_WRONLY, &status);
    if (upload_fd < 0) {
        send_text_response(client->fd, 404, "The container transfer does not exist.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    if (flock(upload_fd, LOCK_EX | LOCK_NB) != 0) {
        close(upload_fd);
        send_text_response(client->fd, 409, "Another request is writing this container transfer.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    uint64_t current_offset = (uint64_t)status.st_size;
    if (current_offset != upload_offset) {
        send_container_upload_response(client->fd, 409, current_offset);
        close(upload_fd);
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    if (lseek(upload_fd, 0, SEEK_END) < 0) {
        close(upload_fd);
        send_text_response(client->fd, 500, "Could not seek the container transfer.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }

    client->streaming_container_upload = true;
    client->container_upload_fd = upload_fd;
    client->container_upload_offset = upload_offset;
    client->container_upload_received = 0;
    client->container_upload_length = content_length;
    if (header_value_contains(client->request,
                              header_length,
                              "Expect",
                              "100-continue")) {
        static const char continue_response[] = "HTTP/1.1 100 Continue\r\n\r\n";
        queue_all(client->fd, continue_response, sizeof(continue_response) - 1);
    }
    size_t available_body = client->length > header_length ? client->length - header_length : 0;
    if ((uint64_t)available_body > content_length) {
        send_text_response(client->fd, 400, "The container upload body is larger than Content-Length.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    if (available_body > 0 &&
        !write_container_upload_bytes(client, client->request + header_length, available_body)) {
        send_text_response(client->fd, 500, "Could not write the container transfer.\n");
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    client->length = 0;
    client->request[0] = '\0';
    if (client->container_upload_received == client->container_upload_length) {
        uint64_t next_offset = client->container_upload_offset + client->container_upload_received;
        send_container_upload_response(client->fd, 204, next_offset);
        *should_close = true;
        return CONTAINER_UPLOAD_FINISHED;
    }
    return CONTAINER_UPLOAD_STREAMING;
}

static bool send_container_upload_offset(int fd, const char *target) {
    const char *transfer_id = container_transfer_id_from_target(target);
    if (!transfer_id) return false;
    struct stat status;
    int upload_fd = open_container_upload(transfer_id, O_RDONLY, &status);
    if (upload_fd < 0) {
        send_text_response(fd, 404, "The container transfer does not exist.\n");
        return true;
    }
    close(upload_fd);
    send_container_upload_response(fd, 200, (uint64_t)status.st_size);
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
    if (content_length > MAX_HTTP_REQUEST_SIZE ||
        header_length > MAX_HTTP_REQUEST_SIZE - content_length) {
        *complete_length = MAX_HTTP_REQUEST_SIZE + 1;
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
    if (message_length > OUTERSHELL_API_MAX_FRAME_SIZE) {
        *complete_length = OUTERSHELL_API_MAX_FRAME_SIZE + 5u;
        return true;
    }
    if (length < 4u + message_length) return false;
    *complete_length = 4u + message_length;
    return true;
}

static uint16_t ui_route_for_http_request(const char *method, const char *target) {
    if (!target || !target[0]) return OUTERSHELLD_UI_ROUTE_NONE;
    if (strcasecmp(method, "POST") == 0) {
        if (strcmp(target, "/api/layout") == 0) return OUTERSHELLD_UI_ROUTE_LAYOUT_WRITE;
        if (strcmp(target, "/api/control") == 0) return OUTERSHELLD_UI_ROUTE_CONTROL;
        if (strcmp(target, "/api/create") == 0) return OUTERSHELLD_UI_ROUTE_CREATE;
        if (strcmp(target, "/api/icon-observation") == 0) return OUTERSHELLD_UI_ROUTE_ICON_OBSERVATION;
        if (strcmp(target, "/api/safe-spaces") == 0) return OUTERSHELLD_UI_ROUTE_SAFE_SPACES;
        if (strcmp(target, "/api/safe-space-icon-observation") == 0) {
            return OUTERSHELLD_UI_ROUTE_SAFE_SPACE_ICON_OBSERVATION;
        }
        return OUTERSHELLD_UI_ROUTE_NONE;
    }
    if (strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0) {
        if (strcmp(target, "/api/container-snapshot") == 0) return OUTERSHELLD_UI_ROUTE_CONTAINER_SNAPSHOT;
        if (strcmp(target, "/api/layout") == 0) return OUTERSHELLD_UI_ROUTE_LAYOUT_READ;
        if (strcmp(target, "/api/backends") == 0) return OUTERSHELLD_UI_ROUTE_BACKENDS;
        if (strcmp(target, "/api/logs") == 0) return OUTERSHELLD_UI_ROUTE_LOGS;
        if (strcmp(target, "/api/recipes") == 0) return OUTERSHELLD_UI_ROUTE_RECIPES;
        if (strcmp(target, "/api/file-picker") == 0) return OUTERSHELLD_UI_ROUTE_FILE_PICKER;
        if (strcmp(target, "/api/events") == 0) return OUTERSHELLD_UI_ROUTE_EVENTS;
    }
    return OUTERSHELLD_UI_ROUTE_NONE;
}

static bool send_ui_api_response_message_as_http(int client_fd, const char *response_data, size_t response_length) {
    const unsigned char *message = (const unsigned char *)response_data;
    size_t message_length = response_length;
    const unsigned char *payload = NULL;
    size_t payload_length = 0;
    char *api_error = NULL;
    bool ok = message_length >= 24 &&
              read_uint16_le(message) == OUTERSHELLD_API_UI_RESPONSE &&
              api_read_string_ref(message, message_length, 8, &api_error) &&
              api_read_data_ref(message, message_length, 16, &payload, &payload_length);
    uint32_t status = ok ? read_uint32_le(message + 2) : 500u;
    uint16_t content_kind = ok ? read_uint16_le(message + 6) : UI_API_CONTENT_TEXT;
    if (!ok) {
        char text[768];
        snprintf(text, sizeof(text), "outershelld API request failed: %s\n", api_error && api_error[0] ? api_error : "invalid response");
        send_text_response(client_fd, 500, text);
        free(api_error);
        return false;
    }

    send_response(client_fd,
                  (int)status,
                  http_status_text((int)status),
                  content_kind == UI_API_CONTENT_TEXT ? "text/plain; charset=utf-8" : "application/octet-stream",
                  payload,
                  payload_length);
    free(api_error);
    return true;
}

static bool proxy_ui_request_to_api(ReactorClient *client,
                                    uint16_t route,
                                    const char *query,
                                    const char *body,
                                    size_t body_length) {
    int client_fd = client->fd;
    StringBuilder owned_body = {0};
    if (route == OUTERSHELLD_UI_ROUTE_CONTROL) {
        char stage_error[1024] = "";
        const char *augmented_body = body ? body : "";
        size_t augmented_body_length = body_length;
        if (!augment_control_request_body(query,
                                          body ? body : "",
                                          &owned_body,
                                          &augmented_body,
                                          &augmented_body_length,
                                          stage_error,
                                          sizeof(stage_error))) {
            free(owned_body.data);
            send_text_response(client_fd, 500, stage_error[0] ? stage_error : "failed to stage control request\n");
            return false;
        }
        body = augmented_body;
        body_length = augmented_body_length;
    }

    char error[512] = "";
    int api_fd = connect_unix_stream(g_http_proxy_api_socket_path, error, sizeof(error));
    if (api_fd < 0) {
        free(owned_body.data);
        char response[768];
        snprintf(response, sizeof(response), "outershelld API unavailable: %s\n", error);
        send_text_response(client_fd, 500, response);
        return false;
    }

    StringBuilder request = {0};
    bool ok = body_length <= UINT32_MAX &&
              binary_append_zero(&request, 24) &&
              binary_write_u16_at(&request, 0, OUTERSHELLD_API_UI_REQUEST) &&
              binary_write_u16_at(&request, 2, route) &&
              binary_write_u32_at(&request, 4, 0) &&
              binary_append_string_ref_at(&request, 8, query ? query : "") &&
              binary_append_data_ref_at(&request, 16, body ? body : "", body_length);
    if (!ok || !api_send_frame(api_fd, &request)) {
        free(request.data);
        free(owned_body.data);
        close(api_fd);
        send_text_response(client_fd, 500, "failed to send request to outershelld API\n");
        return false;
    }
    free(request.data);
    free(owned_body.data);

    set_fd_nonblocking(api_fd, true);
    client->waiting_for_api_response = true;
    client->api_response_fd = api_fd;
    client->length = 0;
    client->request[0] = '\0';
    return true;
}

static bool process_http_client_request(ReactorClient *client, char *request, size_t n) {
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
    if (body_length > content_length) body_length = content_length;
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

    if (strcasecmp(method, "HEAD") == 0 && send_container_upload_offset(fd, target)) {
        return false;
    }

    uint16_t ui_route = ui_route_for_http_request(method, target);
    if (ui_route != OUTERSHELLD_UI_ROUTE_NONE) {
        return proxy_ui_request_to_api(client, ui_route, query, body, body_length);
    }

    if (strcasecmp(method, "POST") == 0 && strcmp(target, "/api/native-app-projects") == 0) {
        dispatch_native_app_project_creation(fd, query, body, body_length);
    } else if (strcasecmp(method, "POST") == 0) {
        send_text_response(fd, 404, "not found\n");
    } else if (strcmp(target, "/api/native-app-template") == 0) {
        send_native_app_template_archive(fd);
    } else if (is_navigator_route(target)) {
        if (strcmp(target, "/backends.outer") == 0 || request_accepts_outerframe(request, header_length)) {
            send_outer_descriptor(fd, request, header_length,
                                  strcasecmp(method, "HEAD") != 0);
        } else {
            send_web_file(fd, "index.html", "text/html; charset=utf-8",
                          request, header_length, strcasecmp(method, "HEAD") != 0, true);
        }
    } else if (strcmp(target, "/web/style.css") == 0) {
        send_web_file(fd, "style.css", "text/css; charset=utf-8",
                      request, header_length, strcasecmp(method, "HEAD") != 0, false);
    } else if (strcmp(target, "/web/app.js") == 0) {
        send_web_file(fd, "app.js", "text/javascript; charset=utf-8",
                      request, header_length, strcasecmp(method, "HEAD") != 0, false);
    } else if (strcmp(target, "/web/favicon.png") == 0) {
        send_web_app_icon(fd, request, header_length, strcasecmp(method, "HEAD") != 0);
    } else {
        char bundle_path[PATH_MAX];
        char bundle_path_macos_arm[PATH_MAX];
        char bundle_path_macos_x86[PATH_MAX];
        bundle_url_path(bundle_path, sizeof(bundle_path));
        snprintf(bundle_path_macos_arm, sizeof(bundle_path_macos_arm), "%s/macos-arm", bundle_path);
        snprintf(bundle_path_macos_x86, sizeof(bundle_path_macos_x86), "%s/macos-x86", bundle_path);
        if (strcmp(target, bundle_path) == 0) {
            send_text_response(fd, 200, "macos-arm\nmacos-x86\n");
        } else if (strcmp(target, bundle_path_macos_arm) == 0) {
            send_cached_file(fd, bundle_arm_path(), "application/octet-stream",
                             request, header_length, strcasecmp(method, "HEAD") != 0, false);
        } else if (strcmp(target, bundle_path_macos_x86) == 0) {
            send_cached_file(fd, bundle_x86_path(), "application/octet-stream",
                             request, header_length, strcasecmp(method, "HEAD") != 0, false);
        } else {
            send_text_response(fd, 404, "not found\n");
        }
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
    if (clients[index].api_response_fd >= 0) {
        close(clients[index].api_response_fd);
    }
    if (clients[index].container_upload_fd >= 0) {
        close(clients[index].container_upload_fd);
    }
    free(clients[index].request);
    clients[index].request = NULL;
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
    client->request_capacity = READ_BUFFER_SIZE;
    client->request = malloc(client->request_capacity);
    if (!client->request) {
        close(client_fd);
        (*client_count)--;
        return;
    }
    client->fd = client_fd;
    client->api_response_fd = -1;
    client->container_upload_fd = -1;
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

static bool read_reactor_client_from_fd(ReactorClient *client,
                                        int fd,
                                        bool parse_api_frame,
                                        size_t *complete_length,
                                        bool *should_close) {
    *complete_length = 0;
    *should_close = false;

    for (;;) {
        if (!parse_api_frame && client->streaming_container_upload) {
            char buffer[READ_BUFFER_SIZE];
            uint64_t remaining = client->container_upload_length - client->container_upload_received;
            size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            ssize_t got = read(fd, buffer, wanted);
            if (got > 0) {
                if (!write_container_upload_bytes(client, buffer, (size_t)got)) {
                    send_text_response(client->fd, 500, "Could not write the container transfer.\n");
                    *should_close = true;
                    return false;
                }
                client->last_activity_ms = monotonic_milliseconds();
                if (client->container_upload_received == client->container_upload_length) {
                    uint64_t next_offset = client->container_upload_offset +
                        client->container_upload_received;
                    send_container_upload_response(client->fd, 204, next_offset);
                    *should_close = true;
                    return false;
                }
                continue;
            }
            if (got == 0) {
                *should_close = true;
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            *should_close = true;
            return false;
        }

        size_t maximum_size = parse_api_frame
            ? OUTERSHELL_API_MAX_FRAME_SIZE + 4u
            : MAX_HTTP_REQUEST_SIZE;
        if (client->length >= client->request_capacity - 1) {
            size_t maximum_capacity = maximum_size + 1;
            if (client->request_capacity >= maximum_capacity) {
                *complete_length = maximum_size + 1;
                return true;
            }
            size_t new_capacity = client->request_capacity <= maximum_capacity / 2
                ? client->request_capacity * 2
                : maximum_capacity;
            char *grown = realloc(client->request, new_capacity);
            if (!grown) {
                *should_close = true;
                return false;
            }
            client->request = grown;
            client->request_capacity = new_capacity;
        }

        ssize_t got = read(fd,
                           client->request + client->length,
                           client->request_capacity - client->length - 1);
        if (got > 0) {
            client->length += (size_t)got;
            client->request[client->length] = '\0';
            client->last_activity_ms = monotonic_milliseconds();
            if (!parse_api_frame) {
                ContainerUploadDispatch upload = begin_container_upload_if_ready(client, should_close);
                if (upload == CONTAINER_UPLOAD_FINISHED) return false;
                if (upload == CONTAINER_UPLOAD_STREAMING) continue;
            }
            bool complete = parse_api_frame
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

static bool read_reactor_client(ReactorClient *client,
                                size_t *complete_length,
                                bool *should_close) {
    return read_reactor_client_from_fd(client, client->fd, client->is_api, complete_length, should_close);
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

static void run_http_reactor(int listener) {
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
            int poll_fd = clients[i].waiting_for_api_response ? clients[i].api_response_fd : clients[i].fd;
            poll_fds[i + 1] = (struct pollfd){.fd = poll_fd, .events = POLLIN, .revents = 0};
        }

        int timeout_ms = 1000;
        if (socket_activation_enabled() && !g_stay_alive_when_socket_idle && polled_client_count == 0) {
            timeout_ms = 60000;
        }

        int poll_result = poll(poll_fds, (nfds_t)(polled_client_count + 1), timeout_ms);
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

            for (size_t i = polled_client_count; i > 0; i--) {
                size_t index = i - 1;
                short revents = poll_fds[index + 1].revents;
                if (revents == 0) continue;

                if (clients[index].waiting_for_api_response) {
                    if (revents & POLLIN) {
                        size_t complete_length = 0;
                        bool should_close = false;
                        bool complete = read_reactor_client_from_fd(&clients[index],
                                                                    clients[index].api_response_fd,
                                                                    true,
                                                                    &complete_length,
                                                                    &should_close);
                        if (complete) {
                            if (complete_length <= OUTERSHELL_API_MAX_FRAME_SIZE + 4u &&
                                complete_length >= 4) {
                                (void)send_ui_api_response_message_as_http(clients[index].fd,
                                                                           clients[index].request + 4,
                                                                           complete_length - 4);
                            } else {
                                send_text_response(clients[index].fd, 500, "outershelld API response is too large\n");
                            }
                            close_reactor_client(clients, &client_count, index);
                        } else if (should_close) {
                            close_reactor_client(clients, &client_count, index);
                        }
                    } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
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
                        if (complete_length > MAX_HTTP_REQUEST_SIZE) {
                            send_text_response(clients[index].fd, 400, "request too large\n");
                            close_reactor_client(clients, &client_count, index);
                        } else {
                            bool keep_open = process_http_client_request(&clients[index],
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
            if (clients[index].waiting_for_api_response) continue;
            int64_t idle_timeout = clients[index].streaming_container_upload
                ? TRANSFER_IDLE_TIMEOUT_MS
                : CLIENT_IDLE_TIMEOUT_MS;
            if (now - clients[index].last_activity_ms > idle_timeout) {
                close_reactor_client(clients, &client_count, index);
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }

    for (size_t i = 0; i < client_count; i++) {
        close(clients[i].fd);
        if (clients[i].api_response_fd >= 0) close(clients[i].api_response_fd);
        free(clients[i].request);
    }
    free(clients);
}

static void outer_shell_backend_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--api-socket-path PATH] [--container-transfers-dir DIR] [--launchd-socket-name NAME] [--bundles-dir DIR] [--web-root DIR] [--native-app-template-dir DIR] [--stay-alive]\n", program);
}

static void initialize_runtime_paths(char *api_socket_path, size_t api_socket_path_size) {
    outer_shell_default_api_socket_path(api_socket_path, api_socket_path_size);
}

int OuterShellBackendMain(int argc, char **argv) {
    g_backend_start_time = time(NULL);
    int port = DEFAULT_PORT;
    bool use_port = true;
    char socket_path[PATH_MAX] = "";
    char api_socket_path[PATH_MAX] = "";
    char launchd_socket_name[128] = "Listener";
    const char *bundles_dir = "bundles";

    initialize_runtime_paths(api_socket_path, sizeof(api_socket_path));
    const char *app_base_url = getenv("OUTER_SHELL_APP_BASE_URL");
    const char *public_base_url = getenv("OUTER_SHELL_PUBLIC_BASE_URL");
    if (app_base_url && app_base_url[0]) {
        snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", app_base_url);
    }
    if (public_base_url && public_base_url[0]) {
        snprintf(g_home_screen_public_base_url, sizeof(g_home_screen_public_base_url), "%s", public_base_url);
    }

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
        } else if (strcmp(argv[i], "--container-transfers-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i],
                              g_container_transfers_directory,
                              sizeof(g_container_transfers_directory));
        } else if (strcmp(argv[i], "--launchd-socket-name") == 0 && i + 1 < argc) {
            snprintf(launchd_socket_name, sizeof(launchd_socket_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            bundles_dir = argv[++i];
        } else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_web_root_directory, sizeof(g_web_root_directory));
        } else if (strcmp(argv[i], "--native-app-template-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_native_app_template_directory, sizeof(g_native_app_template_directory));
        } else if (strcmp(argv[i], "--bundled-apps-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_bundled_apps_directory, sizeof(g_bundled_apps_directory));
        } else if (strcmp(argv[i], "--app-base-url") == 0 && i + 1 < argc) {
            snprintf(g_bundled_apps_base_url, sizeof(g_bundled_apps_base_url), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--public-base-url") == 0 && i + 1 < argc) {
            snprintf(g_home_screen_public_base_url, sizeof(g_home_screen_public_base_url), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--database") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--system-database") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--stay-alive") == 0) {
            g_stay_alive_when_socket_idle = true;
        } else {
            outer_shell_backend_usage(argv[0]);
            return 2;
        }
    }

    if (!api_socket_path[0]) {
        fprintf(stderr, "OuterShellBackend requires an outershelld API socket path.\n");
        return 2;
    }
    snprintf(g_http_proxy_api_socket_path, sizeof(g_http_proxy_api_socket_path), "%s", api_socket_path);
    if (!g_container_transfers_directory[0]) {
        const char *configured_transfers = getenv("OUTER_SHELL_CONTAINER_TRANSFERS_DIR");
        if (configured_transfers && configured_transfers[0]) {
            expand_tilde_path(configured_transfers,
                              g_container_transfers_directory,
                              sizeof(g_container_transfers_directory));
        }
    }

    snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
             "%s/OuterShell.bundle.macos-arm.aar", bundles_dir);
    snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
             "%s/OuterShell.bundle.macos-x86.aar", bundles_dir);

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

    int listener = !use_port ? systemd_activated_listener_named("http", &g_systemd_socket_activation) : -1;
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
    g_listener_fd = listener;
    if (use_port) {
        fprintf(stderr, "OuterShellBackend HTTP listening on http://127.0.0.1:%d/\n", port);
    } else {
        fprintf(stderr, "OuterShellBackend HTTP listening on %s/\n", socket_path);
    }
    fprintf(stderr, "outershelld API socket: %s\n", g_http_proxy_api_socket_path);

    run_http_reactor(listener);

    close(listener);
    g_listener_fd = -1;
    if (!use_port && g_listen_socket_path[0] && !g_systemd_socket_activation && !g_launchd_socket_activation) {
        unlink(g_listen_socket_path);
    }
    return 0;
}

#if !defined(OUTER_SHELL_BACKEND_LIBRARY) || defined(OUTER_SHELL_BACKEND_STANDALONE)
int main(int argc, char **argv) {
    return OuterShellBackendMain(argc, argv);
}
#endif
