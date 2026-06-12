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
#define MAX_REACTOR_CLIENTS 128
#define CLIENT_IDLE_TIMEOUT_MS 10000

static const char *kBundleUrlPath = "/bundles/OuterShell";
static const char *kBundleFilePathMacosArm = "bundles/OuterShell.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/OuterShell.bundle.macos-x86.aar";

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_bundled_apps_directory[PATH_MAX] = "";
static char g_bundled_apps_base_url[2048] = "";
static char g_home_screen_public_base_url[2048] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static char g_http_proxy_api_socket_path[PATH_MAX] = "";
static bool g_systemd_socket_activation = false;
static bool g_launchd_socket_activation = false;
static bool g_stay_alive_when_socket_idle = false;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_listener_fd = -1;

typedef struct {
    int fd;
    bool is_api;
    uid_t peer_uid;
    bool has_peer_uid;
    char request[READ_BUFFER_SIZE];
    size_t length;
    int64_t last_activity_ms;
    bool waiting_for_api_response;
    int api_response_fd;
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
    const char *archive_name;
} BundledAppDefinition;

static const BundledAppDefinition kBundledApps[] = {
    {
        .service_id = "dev.outergroup.Top",
        .display_name = "Top",
        .stage_directory_name = "Top",
        .binary_name = "TopBackend",
        .bundle_prefix = "TopContent",
        .icon_name = "app-icon.png",
        .source_name = "TopBackend.c",
        .archive_name = "Top.tar.gz"
    },
    {
        .service_id = "dev.outergroup.Files",
        .display_name = "Files",
        .stage_directory_name = "Files",
        .binary_name = "FilesBackend",
        .bundle_prefix = "FilesContent",
        .icon_name = "app-icon.png",
        .source_name = "FilesBackend.c",
        .archive_name = "Files.tar.gz"
    },
    {
        .service_id = "dev.outergroup.Plaintext",
        .display_name = "Plaintext",
        .stage_directory_name = "Plaintext",
        .binary_name = "PlaintextBackend",
        .bundle_prefix = "PlaintextContent",
        .icon_name = NULL,
        .source_name = "PlaintextBackend.c",
        .archive_name = "Plaintext.tar.gz"
    },
    {
        .service_id = "dev.outergroup.NetworkInspector",
        .display_name = "Network Inspector",
        .stage_directory_name = "NetworkInspector",
        .binary_name = "NetworkInspectorBackend",
        .bundle_prefix = "NetworkInspectorContent",
        .icon_name = "app-icon.png",
        .source_name = "NetworkInspectorBackend.c",
        .archive_name = "NetworkInspector.tar.gz"
    },
    {
        .service_id = "dev.outergroup.Firehose",
        .display_name = "Firehose",
        .stage_directory_name = "Firehose",
        .binary_name = "TraceBackend",
        .bundle_prefix = "TraceContent",
        .icon_name = "app-icon.png",
        .source_name = "TraceBackend.c",
        .archive_name = "Firehose.tar.gz"
    },
    {
        .service_id = "dev.outergroup.Profile",
        .display_name = "Profile",
        .stage_directory_name = "Profile",
        .binary_name = "ProfileBackend",
        .bundle_prefix = "ProfileContent",
        .icon_name = "app-icon.png",
        .source_name = "ProfileBackend.c",
        .archive_name = "Profile.tar.gz"
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
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
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

static void send_outer_descriptor(int fd) {
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

    send_response(fd, 200, "OK", "application/vnd.outerframe", payload, total_len);
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
            if (strcmp(kBundledApps[i].service_id, "dev.outergroup.Top") != 0) return NULL;
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

    char archive_url[2048];
    join_url_path(archive_url, sizeof(archive_url), g_bundled_apps_base_url, app->archive_name);
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
    snprintf(archive_path, sizeof(archive_path), "%s/%s.tar.gz", cache_root, app->stage_directory_name);
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

static bool fetch_home_screen_available_version(const char *heartbeat, char *out, size_t out_size, char *message, size_t message_size) {
    if (out && out_size > 0) out[0] = '\0';
    if (!g_home_screen_public_base_url[0]) {
        snprintf(message, message_size, "No Outer Shell update URL is configured.");
        return false;
    }
    char base_url[2048];
    snprintf(base_url, sizeof(base_url), "%s", g_home_screen_public_base_url);
    size_t len = strlen(base_url);
    while (len > 0 && base_url[len - 1] == '/') base_url[--len] = '\0';
    StringBuilder url = {0};
    bool ok = sb_append(&url, base_url) &&
              sb_append(&url, "/latest/version.txt?heartbeat=") &&
              (append_url_encoded(&url, heartbeat && heartbeat[0] ? heartbeat : "manual"), true);
    if (!ok) {
        free(url.data);
        snprintf(message, message_size, "Failed to build update URL.");
        return false;
    }
    char download_error[512] = "";
    bool fetched = outer_shell_fetch_url_text(url.data, out, out_size, download_error, sizeof(download_error));
    free(url.data);
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
    char base_url[2048];
    snprintf(base_url, sizeof(base_url), "%s", g_home_screen_public_base_url);
    size_t len = strlen(base_url);
    while (len > 0 && base_url[len - 1] == '/') base_url[--len] = '\0';

    char cache_root[PATH_MAX];
    home_screen_install_cache_root(cache_root, sizeof(cache_root));
    if (!mkdir_p(cache_root)) {
        snprintf(message, message_size, "Failed to create install cache at %s: %s", cache_root, strerror(errno));
        return false;
    }

    snprintf(script_path, script_path_size, "%s/install.sh", cache_root);
    char script_url[4096];
    snprintf(script_url, sizeof(script_url), "%s/latest/install.sh?heartbeat=manual", base_url);
    char error[512] = "";
    if (!outer_shell_download_url_to_file(script_url, script_path, error, sizeof(error))) {
        snprintf(message, message_size, "Failed to download Outer Shell install script: %s", error);
        return false;
    }
    chmod(script_path, 0755);

    const char *archive_name = "outer-shell-macos.tar.gz";
#ifndef __APPLE__
    char architecture[64];
    if (!remote_machine_architecture(architecture, sizeof(architecture))) {
        snprintf(message, message_size, "Unsupported machine architecture.");
        return false;
    }
    char linux_archive_name[128];
    snprintf(linux_archive_name, sizeof(linux_archive_name), "outer-shell-%s.tar.gz", architecture);
    archive_name = linux_archive_name;
#endif
    snprintf(archive_path, archive_path_size, "%s/%s", cache_root, archive_name);
    char archive_url[4096];
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
            if (!fetch_home_screen_available_version("manual", version, sizeof(version), error, error_size)) {
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

static uint16_t ui_route_for_http_request(const char *method, const char *target) {
    if (!target || !target[0]) return OUTERSHELLD_UI_ROUTE_NONE;
    if (strcasecmp(method, "POST") == 0) {
        if (strcmp(target, "/api/control") == 0) return OUTERSHELLD_UI_ROUTE_CONTROL;
        if (strcmp(target, "/api/create") == 0) return OUTERSHELLD_UI_ROUTE_CREATE;
        return OUTERSHELLD_UI_ROUTE_NONE;
    }
    if (strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0) {
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

    if (route == OUTERSHELLD_UI_ROUTE_EVENTS) {
        set_fd_nonblocking(api_fd, true);
        client->waiting_for_api_response = true;
        client->api_response_fd = api_fd;
        client->length = 0;
        client->request[0] = '\0';
        return true;
    }

    StringBuilder response = {0};
    ok = api_read_frame_from_fd(api_fd, &response, error, sizeof(error));
    close(api_fd);
    if (!ok) {
        free(response.data);
        char message[768];
        snprintf(message, sizeof(message), "outershelld API failed: %s\n", error);
        send_text_response(client_fd, 500, message);
        return false;
    }

    (void)send_ui_api_response_message_as_http(client_fd, response.data, response.length);
    free(response.data);
    return false;
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

    uint16_t ui_route = ui_route_for_http_request(method, target);
    if (ui_route != OUTERSHELLD_UI_ROUTE_NONE) {
        return proxy_ui_request_to_api(client, ui_route, query, body, body_length);
    }

    if (strcasecmp(method, "POST") == 0) {
        send_text_response(fd, 404, "not found\n");
    } else if (is_navigator_route(target)) {
        send_outer_descriptor(fd);
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
            send_bundle_file(fd, bundle_arm_path());
        } else if (strcmp(target, bundle_path_macos_x86) == 0) {
            send_bundle_file(fd, bundle_x86_path());
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
    client->api_response_fd = -1;
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
        if (client->length >= sizeof(client->request) - 1) {
            *complete_length = READ_BUFFER_SIZE + 1;
            return true;
        }

        ssize_t got = read(fd,
                           client->request + client->length,
                           sizeof(client->request) - client->length - 1);
        if (got > 0) {
            client->length += (size_t)got;
            client->request[client->length] = '\0';
            client->last_activity_ms = monotonic_milliseconds();
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
                            if (complete_length <= READ_BUFFER_SIZE && complete_length >= 4) {
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
                        if (complete_length > READ_BUFFER_SIZE) {
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

static void outer_shell_backend_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--api-socket-path PATH] [--launchd-socket-name NAME] [--bundles-dir DIR] [--stay-alive]\n", program);
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
    if (geteuid() == 0) {
        snprintf(out, out_size, "/run/outershelld-api");
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_size, "%s/outershelld-api", runtime);
        return;
    }
    snprintf(out, out_size, "/run/user/%d/outershelld-api", (int)getuid());
#endif
}

static void initialize_runtime_paths(char *api_socket_path, size_t api_socket_path_size) {
    default_api_socket_path(api_socket_path, api_socket_path_size);
    const char *api_socket_env = getenv("OUTERSHELLD_API_SOCKET");
    if (api_socket_env && api_socket_env[0]) {
        expand_tilde_path(api_socket_env, api_socket_path, api_socket_path_size);
    }
}

int OuterShellBackendMain(int argc, char **argv) {
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
