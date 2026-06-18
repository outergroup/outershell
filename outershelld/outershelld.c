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
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef OUTER_SHELL_BACKEND_LIBRARY
#include <sqlite3.h>
#endif
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
#ifndef __APPLE__
#include <dlfcn.h>
#endif

#include "../Backend/OuterShellAPI.h"
#include "../Backend/OuterShellBuffer.h"
#include "../Backend/OuterShellPlatform.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
extern int launch_activate_socket(const char *name, int **fds, size_t *cnt);
#endif

typedef struct {
    StringBuilder *items;
    size_t count;
    size_t capacity;
} BinaryPayloadList;

static uint64_t read_uint64_le(const unsigned char *src) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | (uint64_t)src[i];
    }
    return value;
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


static void ui_api_response_free(UiApiResponse *response) {
    if (!response) return;
    free(response->body.data);
    memset(response, 0, sizeof(*response));
}


static void ui_api_set_text_response(UiApiResponse *response, int status, const char *message) {
    if (!response) return;
    free(response->body.data);
    memset(&response->body, 0, sizeof(response->body));
    response->status = status;
    response->content_kind = UI_API_CONTENT_TEXT;
    (void)sb_append_n(&response->body, message ? message : "", strlen(message ? message : ""));
}


static void api_send_ui_response(int fd, const UiApiResponse *response) {
    StringBuilder message = {0};
    const void *body = response && response->body.data ? response->body.data : "";
    size_t body_length = response ? response->body.length : 0;
    bool ok = body_length <= UINT32_MAX &&
              binary_append_zero(&message, 24) &&
              binary_write_u16_at(&message, 0, OUTERSHELLD_API_UI_RESPONSE) &&
              binary_write_u32_at(&message, 2, (uint32_t)(response ? response->status : 500)) &&
              binary_write_u16_at(&message, 6, (uint16_t)(response ? response->content_kind : UI_API_CONTENT_TEXT)) &&
              binary_append_string_ref_at(&message, 8, "") &&
              binary_append_data_ref_at(&message, 16, body, body_length);
    if (ok) api_send_frame(fd, &message);
    free(message.data);
}


static void run_shell_ignored(const char *command) {
    int result = system(command);
    (void)result;
}


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


static bool path_has_directory_prefix(const char *path, const char *directory) {
    if (!path || !directory || !path[0] || !directory[0]) return false;
    size_t length = strlen(directory);
    while (length > 1 && directory[length - 1] == '/') length--;
    return strncmp(path, directory, length) == 0 &&
           (path[length] == '/' || path[length] == '\0');
}


static void current_user_runtime_directory(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    size_t required_length = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (required_length > 0) {
        char temp_dir[PATH_MAX];
        if (required_length < sizeof(temp_dir) &&
            confstr(_CS_DARWIN_USER_TEMP_DIR, temp_dir, required_length) > 0 &&
            temp_dir[0]) {
            size_t length = strlen(temp_dir);
            while (length > 1 && temp_dir[length - 1] == '/') temp_dir[--length] = '\0';
            snprintf(out, out_size, "%s", temp_dir);
            return;
        }
    }
    snprintf(out, out_size, "/tmp");
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s", runtime_dir);
        return;
    }
    snprintf(out, out_size, "/run/user/%d", (int)getuid());
#endif
}


static void system_runtime_directory(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    snprintf(out, out_size, "/var/run");
#else
    snprintf(out, out_size, "/run");
#endif
}


static void outerloop_http_unix_allowlist_path(bool system_scope, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    if (system_scope) {
        snprintf(out, out_size, "/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow");
    } else {
        snprintf(out, out_size, "%s/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow", home_directory());
    }
#else
    if (system_scope) {
        snprintf(out, out_size, "/etc/outerloop/http-unix.allow");
    } else {
        const char *config_home = getenv("XDG_CONFIG_HOME");
        if (config_home && config_home[0]) {
            snprintf(out, out_size, "%s/outerloop/http-unix.allow", config_home);
        } else {
            snprintf(out, out_size, "%s/.config/outerloop/http-unix.allow", home_directory());
        }
    }
#endif
}


static void outerloop_http_unix_allowlist_entry(const char *socket_path,
                                                bool system_scope,
                                                char *out,
                                                size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!socket_path || !socket_path[0]) return;

    char runtime_dir[PATH_MAX];
    if (system_scope) {
        system_runtime_directory(runtime_dir, sizeof(runtime_dir));
    } else {
        current_user_runtime_directory(runtime_dir, sizeof(runtime_dir));
    }
    size_t runtime_length = strlen(runtime_dir);
    while (runtime_length > 1 && runtime_dir[runtime_length - 1] == '/') runtime_dir[--runtime_length] = '\0';
    if (path_has_directory_prefix(socket_path, runtime_dir)) {
        const char *suffix = socket_path + runtime_length;
        if (*suffix == '/') suffix++;
        snprintf(out, out_size, "%s/%s", system_scope ? "%T" : "%t", suffix);
        return;
    }
    snprintf(out, out_size, "%s", socket_path);
}


static bool text_contains_exact_line(const char *text, const char *line) {
    if (!text || !line || !line[0]) return false;
    size_t line_length = strlen(line);
    const char *cursor = text;
    while (*cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t current_length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        while (current_length > 0 && cursor[current_length - 1] == '\r') current_length--;
        if (current_length == line_length && strncmp(cursor, line, line_length) == 0) return true;
        if (!line_end) break;
        cursor = line_end + 1;
    }
    return false;
}


static bool append_outerloop_http_unix_allowlist_entry(const char *socket_path,
                                                       bool system_scope,
                                                       char *error,
                                                       size_t error_size) {
    if (!socket_path || !socket_path[0]) return true;
    char allowlist_path[PATH_MAX];
    char entry[PATH_MAX + 16];
    outerloop_http_unix_allowlist_path(system_scope, allowlist_path, sizeof(allowlist_path));
    outerloop_http_unix_allowlist_entry(socket_path, system_scope, entry, sizeof(entry));
    if (!entry[0]) return true;

    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", allowlist_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(directory)) {
            snprintf(error, error_size, "failed to create %s: %s", directory, strerror(errno));
            return false;
        }
        chmod(directory, system_scope ? 0755 : 0700);
    }

    struct stat st;
    if (lstat(allowlist_path, &st) == 0 && !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "%s is not a regular file", allowlist_path);
        return false;
    }
    int fd = open(allowlist_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        snprintf(error, error_size, "failed to open %s: %s", allowlist_path, strerror(errno));
        return false;
    }
    bool ok = true;
    (void)flock(fd, LOCK_EX);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "%s is not a regular file", allowlist_path);
        ok = false;
    } else {
        uid_t expected_uid = system_scope ? 0 : geteuid();
        if (st.st_uid != expected_uid) {
            snprintf(error, error_size, "%s is owned by uid %d, expected uid %d", allowlist_path, (int)st.st_uid, (int)expected_uid);
            ok = false;
        }
    }
    if (ok) {
        (void)fchmod(fd, 0644);
        size_t size = st.st_size > 0 ? (size_t)st.st_size : 0;
        char *contents = malloc(size + 1);
        if (!contents) {
            snprintf(error, error_size, "Out of memory.");
            ok = false;
        } else {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                snprintf(error, error_size, "failed to read %s: %s", allowlist_path, strerror(errno));
                ok = false;
            }
            size_t offset = 0;
            while (ok && offset < size) {
                ssize_t got = read(fd, contents + offset, size - offset);
                if (got < 0) {
                    if (errno == EINTR) continue;
                    snprintf(error, error_size, "failed to read %s: %s", allowlist_path, strerror(errno));
                    ok = false;
                    break;
                }
                if (got == 0) break;
                offset += (size_t)got;
            }
            contents[offset] = '\0';
            if (ok && !text_contains_exact_line(contents, entry)) {
                if (lseek(fd, 0, SEEK_END) < 0) {
                    snprintf(error, error_size, "failed to append %s: %s", allowlist_path, strerror(errno));
                    ok = false;
                }
                if (ok && offset > 0 && contents[offset - 1] != '\n') {
                    ok = queue_all(fd, "\n", 1);
                }
                if (ok) ok = queue_all(fd, entry, strlen(entry)) && queue_all(fd, "\n", 1);
                if (!ok) snprintf(error, error_size, "failed to append %s: %s", allowlist_path, strerror(errno));
            }
            free(contents);
        }
        (void)flock(fd, LOCK_UN);
    }
    close(fd);
    return ok;
}


static bool append_outerloop_http_unix_allowlist_entry_for_current_scope(const char *socket_path,
                                                                         char *error,
                                                                         size_t error_size) {
    return append_outerloop_http_unix_allowlist_entry(socket_path, geteuid() == 0, error, error_size);
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

#ifndef __APPLE__
static void systemd_exec_quote_arg(const char *value, bool preserve_specifiers, char *out, size_t out_size) {
    size_t offset = 0;
    if (out_size == 0) return;
    out[offset++] = '"';
    for (const char *p = value ? value : ""; *p && offset + 5 < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[offset++] = '\\';
            out[offset++] = (char)c;
        } else if (c == '%' && !preserve_specifiers) {
            out[offset++] = '%';
            out[offset++] = '%';
        } else if (c == '\n') {
            out[offset++] = '\\';
            out[offset++] = 'n';
        } else if (c == '\r') {
            out[offset++] = '\\';
            out[offset++] = 'r';
        } else if (c == '\t') {
            out[offset++] = '\\';
            out[offset++] = 't';
        } else {
            out[offset++] = (char)c;
        }
    }
    if (offset + 1 < out_size) {
        out[offset++] = '"';
    }
    out[offset] = '\0';
}

static void bundled_systemd_exec_start(const char *binary_path,
                                       const char *service_id,
                                       const char *socket_path,
                                       const char *bundles_dir,
                                       const char *icon_path,
                                       char *out,
                                       size_t out_size) {
    char quoted_binary[PATH_MAX * 2 + 16];
    char quoted_service_id[1024];
    char quoted_socket_path[PATH_MAX * 2 + 32];
    char quoted_bundles[PATH_MAX * 2 + 16];
    char quoted_icon[PATH_MAX * 2 + 16];
    systemd_exec_quote_arg(binary_path, false, quoted_binary, sizeof(quoted_binary));
    systemd_exec_quote_arg(service_id, false, quoted_service_id, sizeof(quoted_service_id));
    if (socket_path && socket_path[0]) {
        systemd_exec_quote_arg(socket_path, true, quoted_socket_path, sizeof(quoted_socket_path));
    } else {
        quoted_socket_path[0] = '\0';
    }
    systemd_exec_quote_arg(bundles_dir, false, quoted_bundles, sizeof(quoted_bundles));
    if (icon_path && icon_path[0]) {
        systemd_exec_quote_arg(icon_path, false, quoted_icon, sizeof(quoted_icon));
    } else {
        quoted_icon[0] = '\0';
    }

    if (quoted_icon[0]) {
        if (quoted_socket_path[0]) {
            snprintf(out, out_size,
                     "%s --label %s --socket-path %s --bundles-dir %s --icon-file %s",
                     quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles, quoted_icon);
        } else {
            snprintf(out, out_size,
                     "%s --label %s --bundles-dir %s --icon-file %s",
                     quoted_binary, quoted_service_id, quoted_bundles, quoted_icon);
        }
    } else {
        if (quoted_socket_path[0]) {
            snprintf(out, out_size,
                     "%s --label %s --socket-path %s --bundles-dir %s",
                     quoted_binary, quoted_service_id, quoted_socket_path, quoted_bundles);
        } else {
            snprintf(out, out_size,
                     "%s --label %s --bundles-dir %s",
                     quoted_binary, quoted_service_id, quoted_bundles);
        }
    }
}
#endif


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


#ifndef __APPLE__
static uint64_t string_state_token(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!text) return hash;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}
#endif


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


static void append_path_component(char *out, size_t out_size, const char *base, const char *component) {
    snprintf(out, out_size, "%s/%s", base && base[0] ? base : "", component && component[0] ? component : "");
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


#define DEFAULT_PORT 7354
#define READ_BUFFER_SIZE 65536
#define DEFAULT_LOG_BYTES 131072
#define MAX_LOG_BYTES 1048576
#define MAX_REACTOR_CLIENTS 128
#define CLIENT_IDLE_TIMEOUT_MS 10000
#define SYSTEMD_STATUS_CACHE_TTL_MS 1500
#define MAX_SYSTEMD_STATUS_ENTRIES 512

static const char *kOuterShellServiceID = "org.outershell.OuterShell";
static const char *kMigrationServiceID = "org.outershell.OuterShellMigration";
#ifdef __APPLE__
static const char *kSystemOuterShellRoot = "/Library/Application Support/outershell";
#else
static const char *kSystemOuterShellRoot = "/var/lib/outershell";
#endif

static char g_registry_database_path[PATH_MAX] = "";
static char g_system_registry_database_path[PATH_MAX] = "";
static char g_bundled_apps_directory[PATH_MAX] = "";
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
    bool waiting_for_api_response;
    int api_response_fd;
    bool waiting_for_events;
    bool event_response_is_api;
    int64_t event_deadline_ms;
    uint64_t event_since_backends;
    uint64_t event_since_log;
    char event_log_service_id[PATH_MAX];
    char event_log_path[PATH_MAX];
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
static pthread_mutex_t g_systemd_status_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_backend_event_sequence = 1;
static pthread_mutex_t g_backend_event_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char *content_type;
    const char *url_template;
    int rank;
} BundledAppOpenerDefinition;

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
    bool supports_macos;
    const char *version;
    const BundledAppOpenerDefinition *openers;
    size_t opener_count;
} BundledAppDefinition;

typedef struct {
    const char *old_text;
    const char *new_text;
} TextReplacement;

#ifndef OUTER_SHELL_BACKEND_LIBRARY
static bool ensure_registry_schema(sqlite3 *database, char *error, size_t error_size);
static bool export_registry_binary_from_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size);
static bool import_registry_binary_into_sqlite(sqlite3 *database, const char *sqlite_path, char *error, size_t error_size);
#endif
static bool registry_binary_output_path(const char *sqlite_path, char *out, size_t out_size);
static int registry_binary_lock(const char *registry_path, int operation, char *error, size_t error_size);
static bool registry_storage_exists_at(const char *database_path);
#ifndef OUTER_SHELL_BACKEND_LIBRARY
static void rewrite_files_in_directory_replacing_text(const char *directory,
                                                      const TextReplacement *replacements,
                                                      size_t replacement_count,
                                                      bool recursive);
#endif
static void mark_backend_event_changed(void);

static const BundledAppOpenerDefinition kPlaintextOpeners[] = {
    {
        .content_type = "public.text",
        .url_template = "?file={file}",
        .rank = 0
    }
};

static const BundledAppDefinition kBundledApps[] = {
    {
        .service_id = "org.outershell.Top",
        .display_name = "Top",
        .unit_name = "org.outershell.Top.service",
        .stage_directory_name = "Top",
        .install_directory_name = "org.outershell.Top",
        .binary_name = "TopBackend",
        .bundle_prefix = "TopContent",
        .icon_symbol_name = "chart.bar.xaxis",
        .icon_name = "app-icon.png",
        .source_name = "TopBackend.c",
        .socket_name = "org.outershell.Top",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .supports_macos = true,
        .version = "1"
    },
    {
        .service_id = "org.outershell.Files",
        .display_name = "Files",
        .unit_name = "org.outershell.Files.service",
        .stage_directory_name = "Files",
        .install_directory_name = "org.outershell.Files",
        .binary_name = "FilesBackend",
        .bundle_prefix = "FilesContent",
        .icon_symbol_name = "folder",
        .icon_name = "app-icon.png",
        .source_name = "FilesBackend.c",
        .socket_name = "org.outershell.Files",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .supports_macos = false,
        .version = "1"
    },
    {
        .service_id = "org.outershell.Plaintext",
        .display_name = "Plaintext",
        .unit_name = "org.outershell.Plaintext.service",
        .stage_directory_name = "Plaintext",
        .install_directory_name = "org.outershell.Plaintext",
        .binary_name = "PlaintextBackend",
        .bundle_prefix = "PlaintextContent",
        .icon_symbol_name = "doc.plaintext",
        .icon_name = NULL,
        .source_name = "PlaintextBackend.c",
        .socket_name = "org.outershell.Plaintext",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .supports_macos = false,
        .version = "1",
        .openers = kPlaintextOpeners,
        .opener_count = sizeof(kPlaintextOpeners) / sizeof(kPlaintextOpeners[0])
    },
    {
        .service_id = "org.outershell.NetworkInspector",
        .display_name = "Network Inspector",
        .unit_name = "org.outershell.NetworkInspector.service",
        .stage_directory_name = "NetworkInspector",
        .install_directory_name = "org.outershell.NetworkInspector",
        .binary_name = "NetworkInspectorBackend",
        .bundle_prefix = "NetworkInspectorContent",
        .icon_symbol_name = "network",
        .icon_name = "app-icon.png",
        .source_name = "NetworkInspectorBackend.c",
        .socket_name = "org.outershell.NetworkInspector",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .supports_macos = false,
        .version = "1"
    },
    {
        .service_id = "org.outershell.Firehose",
        .display_name = "Firehose",
        .unit_name = "org.outershell.Firehose.service",
        .stage_directory_name = "Firehose",
        .install_directory_name = "org.outershell.Firehose",
        .binary_name = "FirehoseBackend",
        .bundle_prefix = "FirehoseContent",
        .icon_symbol_name = "text.line.last.and.arrowtriangle.forward",
        .icon_name = "app-icon.png",
        .source_name = NULL,
        .socket_name = "org.outershell.Firehose",
        .socket_activated = true,
        .supports_root = true,
        .root_only = true,
        .supports_macos = false,
        .version = "1"
    },
    {
        .service_id = "org.outershell.Profile",
        .display_name = "Profile",
        .unit_name = "org.outershell.Profile.service",
        .stage_directory_name = "Profile",
        .install_directory_name = "org.outershell.Profile",
        .binary_name = "ProfileBackend",
        .bundle_prefix = "ProfileContent",
        .icon_symbol_name = "flame",
        .icon_name = "app-icon.png",
        .source_name = "ProfileBackend.c",
        .socket_name = "org.outershell.Profile",
        .socket_activated = true,
        .supports_root = true,
        .root_only = false,
        .supports_macos = false,
        .version = "1"
    }
};

static bool is_home_screen_service_id(const char *service_id) {
    return service_id && strcmp(service_id, kOuterShellServiceID) == 0;
}

typedef void (*OuterShelldMenuBarVisibilityCallback)(int enabled);
typedef int (*OuterShelldMenuBarVisibilityGetter)(void);
typedef void (*OuterShelldBackendEventChangedCallback)(void);
static OuterShelldMenuBarVisibilityCallback g_menu_bar_visibility_callback = NULL;
static OuterShelldMenuBarVisibilityGetter g_menu_bar_visibility_getter = NULL;
static OuterShelldBackendEventChangedCallback g_backend_event_changed_callback = NULL;

void OuterShelldSetMenuBarVisibilityCallbacks(OuterShelldMenuBarVisibilityCallback callback,
                                              OuterShelldMenuBarVisibilityGetter getter) {
    g_menu_bar_visibility_callback = callback;
    g_menu_bar_visibility_getter = getter;
}

void OuterShelldSetBackendEventChangedCallback(OuterShelldBackendEventChangedCallback callback) {
    g_backend_event_changed_callback = callback;
}

void OuterShelldMarkBackendEventChanged(void) {
    mark_backend_event_changed();
}

static bool set_agent_menu_bar_visibility(bool enabled) {
    if (!g_menu_bar_visibility_callback) return false;
    g_menu_bar_visibility_callback(enabled ? 1 : 0);
    return true;
}

static bool get_agent_menu_bar_visibility(void) {
    if (!g_menu_bar_visibility_getter) return true;
    return g_menu_bar_visibility_getter() != 0;
}

static UiApiResponse *g_captured_ui_response = NULL;

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

void OuterShelldRequestShutdown(void) {
    handle_shutdown_signal(SIGTERM);
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
    if (g_captured_ui_response) {
        g_captured_ui_response->status = status;
        g_captured_ui_response->content_kind =
            content_type && strncmp(content_type, "text/", 5) == 0
                ? UI_API_CONTENT_TEXT
                : UI_API_CONTENT_BINARY;
        if (body && body_len > 0 &&
            !sb_append_n(&g_captured_ui_response->body, (const char *)body, body_len)) {
            free(g_captured_ui_response->body.data);
            memset(&g_captured_ui_response->body, 0, sizeof(g_captured_ui_response->body));
            g_captured_ui_response->status = 500;
            g_captured_ui_response->content_kind = UI_API_CONTENT_TEXT;
            (void)sb_append_n(&g_captured_ui_response->body, "out of memory\n", strlen("out of memory\n"));
        }
        return;
    }

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

static void send_binary_response(int fd, int status, StringBuilder *builder) {
    send_response(fd, status, http_status_text(status), "application/octet-stream", builder->data, builder->length);
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

static bool direct_root_session_uses_system_scope(void) {
#ifdef __APPLE__
    return false;
#else
    return geteuid() == 0;
#endif
}

static void default_user_outershell_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    const char *override_root = getenv("OUTERSHELL_HOME");
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
    snprintf(out, out_size, "%s/Library/Application Support/outershell", home_directory());
#else
    if (direct_root_session_uses_system_scope()) {
        snprintf(out, out_size, "%s", kSystemOuterShellRoot);
        return;
    }
    const char *state_home = getenv("XDG_STATE_HOME");
    if (state_home && state_home[0]) {
        snprintf(out, out_size, "%s/outershell", state_home);
    } else {
        snprintf(out, out_size, "%s/.local/state/outershell", home_directory());
    }
#endif
}

static bool registry_database_path_for_passwd(const struct passwd *pw, char *out, size_t out_size) {
    if (!pw || !pw->pw_dir || !pw->pw_dir[0] || !out || out_size == 0) return false;
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/Application Support/outershell/registry.orwa", pw->pw_dir);
#else
    if (pw->pw_uid == 0) {
        snprintf(out, out_size, "%s/registry.orwa", kSystemOuterShellRoot);
    } else {
        snprintf(out, out_size, "%s/.local/state/outershell/registry.orwa", pw->pw_dir);
    }
#endif
    return out[0] != '\0' && strlen(out) < out_size;
}

static bool requester_registry_database_path(const char *requester_user,
                                             char *out,
                                             size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (geteuid() != 0) return false;

    if (requester_user && requester_user[0]) {
        struct passwd *pw = getpwnam(requester_user);
        if (!pw || pw->pw_uid == 0) return false;
        char path[PATH_MAX];
        if (!registry_database_path_for_passwd(pw, path, sizeof(path))) return false;
        if (strcmp(path, g_registry_database_path) == 0) return false;
        if (!registry_storage_exists_at(path)) return false;
        snprintf(out, out_size, "%s", path);
        return out[0] != '\0';
    }
    return false;
}

static void default_user_outershell_cache_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/Caches/outershell", home_directory());
#else
    if (direct_root_session_uses_system_scope()) {
        snprintf(out, out_size, "/var/cache/outershell");
        return;
    }
    const char *cache_home = getenv("XDG_CACHE_HOME");
    if (cache_home && cache_home[0]) {
        snprintf(out, out_size, "%s/outershell", cache_home);
    } else {
        snprintf(out, out_size, "%s/.cache/outershell", home_directory());
    }
#endif
}

static void default_outer_shell_cache_root(char *out, size_t out_size) {
    char root[PATH_MAX];
    default_user_outershell_cache_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outer-shell", root);
}

static void default_bundled_app_cache_root(char *out, size_t out_size) {
    char root[PATH_MAX];
    default_outer_shell_cache_root(root, sizeof(root));
    snprintf(out, out_size, "%s/bundled-apps", root);
}

static void trim_trailing_path_separators(char *path) {
    if (!path) return;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        path[--length] = '\0';
    }
}

static bool path_text_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    char normalized_a[PATH_MAX];
    char normalized_b[PATH_MAX];
    snprintf(normalized_a, sizeof(normalized_a), "%s", a);
    snprintf(normalized_b, sizeof(normalized_b), "%s", b);
    trim_trailing_path_separators(normalized_a);
    trim_trailing_path_separators(normalized_b);
    return strcmp(normalized_a, normalized_b) == 0;
}

static bool bundled_app_stage_root_is_download_cache(const BundledAppDefinition *app, const char *stage_root) {
    if (!app || !app->stage_directory_name || !app->stage_directory_name[0] || !stage_root || !stage_root[0]) return false;
    char expanded_stage_root[PATH_MAX];
    char cache_root[PATH_MAX];
    char expected_stage_root[PATH_MAX];
    expand_tilde_path(stage_root, expanded_stage_root, sizeof(expanded_stage_root));
    default_bundled_app_cache_root(cache_root, sizeof(cache_root));
    snprintf(expected_stage_root, sizeof(expected_stage_root), "%s/%s", cache_root, app->stage_directory_name);
    return path_text_equal(expanded_stage_root, expected_stage_root);
}

static void cleanup_bundled_app_cache(const BundledAppDefinition *app) {
    if (!app || !app->stage_directory_name || !app->stage_directory_name[0]) return;
    char cache_root[PATH_MAX];
    char app_root[PATH_MAX];
    char legacy_archive_path[PATH_MAX];
    char linux_aarch64_archive_path[PATH_MAX];
    char linux_x86_64_archive_path[PATH_MAX];
    char macos_arm64_archive_path[PATH_MAX];
    char macos_x86_64_archive_path[PATH_MAX];
    char outer_shell_cache_root[PATH_MAX];
    char outershell_cache_root[PATH_MAX];
    default_bundled_app_cache_root(cache_root, sizeof(cache_root));
    default_outer_shell_cache_root(outer_shell_cache_root, sizeof(outer_shell_cache_root));
    default_user_outershell_cache_root(outershell_cache_root, sizeof(outershell_cache_root));
    snprintf(app_root, sizeof(app_root), "%s/%s", cache_root, app->stage_directory_name);
    snprintf(legacy_archive_path, sizeof(legacy_archive_path), "%s/%s.tar.gz", cache_root, app->stage_directory_name);
    snprintf(linux_aarch64_archive_path, sizeof(linux_aarch64_archive_path), "%s/%s-linux-aarch64.tar.gz", cache_root, app->stage_directory_name);
    snprintf(linux_x86_64_archive_path, sizeof(linux_x86_64_archive_path), "%s/%s-linux-x86_64.tar.gz", cache_root, app->stage_directory_name);
    snprintf(macos_arm64_archive_path, sizeof(macos_arm64_archive_path), "%s/%s-macos-arm64.tar.gz", cache_root, app->stage_directory_name);
    snprintf(macos_x86_64_archive_path, sizeof(macos_x86_64_archive_path), "%s/%s-macos-x86_64.tar.gz", cache_root, app->stage_directory_name);

    char quoted_app_root[PATH_MAX + 8];
    char quoted_legacy_archive_path[PATH_MAX + 8];
    char quoted_linux_aarch64_archive_path[PATH_MAX + 8];
    char quoted_linux_x86_64_archive_path[PATH_MAX + 8];
    char quoted_macos_arm64_archive_path[PATH_MAX + 8];
    char quoted_macos_x86_64_archive_path[PATH_MAX + 8];
    char quoted_cache_root[PATH_MAX + 8];
    char quoted_outer_shell_cache_root[PATH_MAX + 8];
    char quoted_outershell_cache_root[PATH_MAX + 8];
    shell_quote(app_root, quoted_app_root, sizeof(quoted_app_root));
    shell_quote(legacy_archive_path, quoted_legacy_archive_path, sizeof(quoted_legacy_archive_path));
    shell_quote(linux_aarch64_archive_path, quoted_linux_aarch64_archive_path, sizeof(quoted_linux_aarch64_archive_path));
    shell_quote(linux_x86_64_archive_path, quoted_linux_x86_64_archive_path, sizeof(quoted_linux_x86_64_archive_path));
    shell_quote(macos_arm64_archive_path, quoted_macos_arm64_archive_path, sizeof(quoted_macos_arm64_archive_path));
    shell_quote(macos_x86_64_archive_path, quoted_macos_x86_64_archive_path, sizeof(quoted_macos_x86_64_archive_path));
    shell_quote(cache_root, quoted_cache_root, sizeof(quoted_cache_root));
    shell_quote(outer_shell_cache_root, quoted_outer_shell_cache_root, sizeof(quoted_outer_shell_cache_root));
    shell_quote(outershell_cache_root, quoted_outershell_cache_root, sizeof(quoted_outershell_cache_root));

    char command[PATH_MAX * 9 + 128];
    snprintf(command,
             sizeof(command),
             "rm -rf -- %s; rm -f -- %s %s %s %s %s; rmdir -- %s %s %s >/dev/null 2>&1 || true",
             quoted_app_root,
             quoted_legacy_archive_path,
             quoted_linux_aarch64_archive_path,
             quoted_linux_x86_64_archive_path,
             quoted_macos_arm64_archive_path,
             quoted_macos_x86_64_archive_path,
             quoted_cache_root,
             quoted_outer_shell_cache_root,
             quoted_outershell_cache_root);
    run_shell_ignored(command);
}

static void cleanup_bundled_app_cache_if_stage_root_is_download_cache(const BundledAppDefinition *app, const char *stage_root) {
    if (!bundled_app_stage_root_is_download_cache(app, stage_root)) return;
    cleanup_bundled_app_cache(app);
    log_event("Removed staged bundled app cache for %s.", app->service_id);
}

static void default_user_outerctl_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/bin/outerctl", root);
}

static void default_outershell_install_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outer-shell", root);
}

#ifdef __APPLE__
static void default_user_outershelld_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outershelld/outershelld", root);
}
#endif

static void default_system_outershelld_install_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s/outershelld", kSystemOuterShellRoot);
}

static void default_system_outershelld_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_system_outershelld_install_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outershelld", root);
}

static void default_system_outerctl_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s/bin/outerctl", kSystemOuterShellRoot);
}

#ifndef __APPLE__
static void default_outershelld_install_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/outershelld", root);
}

static void system_binary_users_dir(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s/system-binary-users", kSystemOuterShellRoot);
}

static void system_binary_user_marker_path(uid_t uid, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char dir[PATH_MAX];
    system_binary_users_dir(dir, sizeof(dir));
    snprintf(out, out_size, "%s/uid-%ld", dir, (long)uid);
}

static void system_binary_root_apps_marker_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    char dir[PATH_MAX];
    system_binary_users_dir(dir, sizeof(dir));
    snprintf(out, out_size, "%s/root-apps", dir);
}
#endif

static void default_user_home_screen_install_root(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
#ifdef __APPLE__
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/apps/org.outershell.OuterShell", root);
#else
    default_outershell_install_root(out, out_size);
#endif
}

static void default_user_outershell_app_root(const char *install_name, char *out, size_t out_size) {
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/apps/%s", root, install_name && install_name[0] ? install_name : "");
}

static void default_user_outershell_apps_root(char *out, size_t out_size) {
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/apps", root);
}

#ifndef OUTER_SHELL_BACKEND_LIBRARY
static void legacy_user_registry_database_path(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/registry.sqlite3", home_directory());
#else
    snprintf(out, out_size, "%s/.outeragent/registry.sqlite3", home_directory());
#endif
}
#endif

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

static void legacy_outer_shell_outerctl_path(char *out, size_t out_size) {
#ifdef __APPLE__
    snprintf(out, out_size, "%s/Library/dev.outergroup.OuterLoop/outerctl", home_directory());
#else
    snprintf(out, out_size, "%s/.outerloop/outer-shell/bin/outerctl", home_directory());
#endif
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
static bool read_exact_with_timeout(int fd, void *buffer, size_t length, int timeout_ms);
static bool run_sudo_shell(const char *command, const char *password, char *output, size_t output_size, int *exit_status);
static bool process_api_client_request(ReactorClient *client, char *request, size_t n);
static bool prepare_events_response_or_wait(ReactorClient *client, const char *query);
static bool event_client_ready(ReactorClient *client, bool *timed_out, uint64_t *backends_version, uint64_t *log_version);
static uint64_t current_backends_event_version(void);
static uint64_t current_log_path_event_version(const char *raw_path);
static uint64_t current_log_event_version(const char *service_id, int log_index);
static void send_events_response(int fd,
                                 bool backends_changed,
                                 bool log_changed,
                                 bool timed_out,
                                 uint64_t backends_version,
                                 uint64_t log_version);
static bool ensure_root_helper_installed(const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool root_helper_outerctl(int argc,
                                 char **argv,
                                 const char *sudo_password,
                                 bool *needs_password,
                                 char *message,
                                 size_t message_size);
static bool root_helper_registry_upsert_bundled_openers(const BundledAppDefinition *app,
                                                        const char *socket_path,
                                                        const char *sudo_password,
                                                        bool *needs_password,
                                                        char *message,
                                                        size_t message_size);
#ifndef __APPLE__
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
static bool remove_bundled_root_support(const BundledAppDefinition *app,
                                        const char *sudo_password,
                                        bool *needs_password,
                                        char *message,
                                        size_t message_size);
#else
static bool root_helper_registry_upsert_launchd(const char *service_id,
                                                const char *display_name,
                                                const char *plist_path,
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
            if (!kBundledApps[i].supports_macos) {
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
    return app->supports_macos;
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
    if (direct_root_session_uses_system_scope()) {
        snprintf(out, out_size, "/run/%s", app->socket_name);
        return;
    }
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
    if (direct_root_session_uses_system_scope()) {
        snprintf(out, out_size, "/run/%s", name);
        return;
    }
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        snprintf(out, out_size, "%s/%s", runtime_dir, name);
    } else {
        snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), name);
    }
#endif
}

static void unlink_advertised_home_screen_socket(void) {
    char socket_path[PATH_MAX] = "";
    if (g_listen_socket_path[0]) {
        snprintf(socket_path, sizeof(socket_path), "%s", g_listen_socket_path);
    } else {
        runtime_socket_path(kOuterShellServiceID, socket_path, sizeof(socket_path));
    }
    if (socket_path[0]) {
        unlink(socket_path);
    }
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

#ifdef __APPLE__
static bool macos_root_tool_source_path(const char *executable, char *out, size_t out_size) {
    char user_outershelld[PATH_MAX];
    default_user_outershelld_path(user_outershelld, sizeof(user_outershelld));
    struct stat st;
    if (stat(user_outershelld, &st) == 0 && S_ISREG(st.st_mode) && access(user_outershelld, X_OK) == 0) {
        snprintf(out, out_size, "%s", user_outershelld);
        return out[0] != '\0';
    }

    char directory[PATH_MAX];
    if (!parent_directory(executable, directory, sizeof(directory))) return false;

    for (int depth = 0; depth < 5; depth++) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/outershelld", directory);
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && access(candidate, X_OK) == 0) {
            snprintf(out, out_size, "%s", candidate);
            return out[0] != '\0';
        }
        char parent[PATH_MAX];
        if (!parent_directory(directory, parent, sizeof(parent)) || strcmp(parent, directory) == 0) break;
        snprintf(directory, sizeof(directory), "%s", parent);
    }

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
    const char *outershell_home = getenv("OUTERSHELL_HOME");
    if (outershell_home && outershell_home[0]) {
        char install_root[PATH_MAX];
        append_path_component(install_root, sizeof(install_root), outershell_home, "outer-shell");
        append_path_component(out, out_size, install_root, "bundled-apps");
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

#ifdef __APPLE__
static void bundled_app_macos_app_bundle_path(const BundledAppDefinition *app,
                                              const char *root,
                                              char *out,
                                              size_t out_size) {
    snprintf(out, out_size, "%s/%s.app", root ? root : "", app && app->stage_directory_name ? app->stage_directory_name : "");
}

static void bundled_app_macos_app_binary_path(const BundledAppDefinition *app,
                                              const char *app_bundle,
                                              char *out,
                                              size_t out_size) {
    snprintf(out, out_size, "%s/Contents/MacOS/%s", app_bundle ? app_bundle : "", app && app->binary_name ? app->binary_name : "");
}

static void bundled_app_macos_app_bundles_dir(const char *app_bundle, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/Contents/Resources/bundles", app_bundle ? app_bundle : "");
}

static void bundled_app_macos_app_icon_path(const BundledAppDefinition *app,
                                            const char *app_bundle,
                                            char *out,
                                            size_t out_size) {
    if (app && app->icon_name && app->icon_name[0]) {
        snprintf(out, out_size, "%s/Contents/Resources/%s", app_bundle ? app_bundle : "", app->icon_name);
    } else if (out_size > 0) {
        out[0] = '\0';
    }
}

static bool bundled_app_macos_app_has_expected_files(const BundledAppDefinition *app, const char *app_bundle) {
    if (!app || !app_bundle || !app_bundle[0]) return false;
    struct stat st;

    char app_binary[PATH_MAX];
    bundled_app_macos_app_binary_path(app, app_bundle, app_binary, sizeof(app_binary));
    if (stat(app_binary, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    char bundles_dir[PATH_MAX];
    bundled_app_macos_app_bundles_dir(app_bundle, bundles_dir, sizeof(bundles_dir));
    char bundle_arm[PATH_MAX];
    snprintf(bundle_arm, sizeof(bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
    if (stat(bundle_arm, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    char bundle_x86[PATH_MAX];
    snprintf(bundle_x86, sizeof(bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
    if (stat(bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    if (app->icon_name && app->icon_name[0]) {
        char icon_path[PATH_MAX];
        bundled_app_macos_app_icon_path(app, app_bundle, icon_path, sizeof(icon_path));
        if (stat(icon_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    }
    return true;
}
#endif

static bool bundled_app_stage_has_expected_files(const BundledAppDefinition *app, const char *stage_root) {
    if (!app || !stage_root || !stage_root[0]) return false;
    struct stat st;

#ifdef __APPLE__
    char app_bundle[PATH_MAX];
    bundled_app_macos_app_bundle_path(app, stage_root, app_bundle, sizeof(app_bundle));
    if (bundled_app_macos_app_has_expected_files(app, app_bundle)) return true;
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
    snprintf(linux_binary, sizeof(linux_binary), "%s/RemoteLinuxBinaries/%s/%s", stage_root, architecture, app->binary_name);
    return stat(linux_binary, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static bool resolve_bundled_app_stage_root(const BundledAppDefinition *app,
                                           const char *requested_stage_root,
                                           char *out_stage_root,
                                           size_t out_stage_root_size,
                                           char *message,
                                           size_t message_size) {
    if (requested_stage_root && requested_stage_root[0]) {
        expand_tilde_path(requested_stage_root, out_stage_root, out_stage_root_size);
        if (bundled_app_stage_has_expected_files(app, out_stage_root)) {
            return true;
        }
        snprintf(message, message_size, "Staged %s payload is incomplete at %s.", app->display_name, out_stage_root);
        return false;
    }

    bundled_app_stage_root(app, out_stage_root, out_stage_root_size);
    if (bundled_app_stage_has_expected_files(app, out_stage_root)) {
        return true;
    }

    snprintf(message,
             message_size,
             "Missing %s payload at %s. OuterShellBackend must stage the app before requesting installation.",
             app->display_name,
             out_stage_root);
    return false;
}

static void default_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERSHELL_REGISTRY");
    if (!env_path || !env_path[0]) {
        env_path = getenv("BACKENDS_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
    char root[PATH_MAX];
    default_user_outershell_root(root, sizeof(root));
    snprintf(out, out_size, "%s/registry.orwa", root);
}

static void default_system_registry_database_path(char *out, size_t out_size) {
    const char *env_path = getenv("OUTERSHELL_SYSTEM_REGISTRY");
    if (!env_path || !env_path[0]) {
        env_path = getenv("BACKENDS_SYSTEM_REGISTRY_DB");
    }
    if (env_path && env_path[0]) {
        expand_tilde_path(env_path, out, out_size);
        return;
    }
    snprintf(out, out_size, "%s/registry.orwa", kSystemOuterShellRoot);
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
    if (direct_root_session_uses_system_scope()) {
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

#ifndef __APPLE__
static void root_helper_socket_path_for_uid(uid_t uid, char *out, size_t out_size) {
    snprintf(out, out_size, "/run/outershelld-root-helper-%ld.sock", (long)uid);
}

static void root_helper_unit_name_for_uid(uid_t uid, const char *suffix, char *out, size_t out_size) {
    snprintf(out, out_size, "outershelld-root-helper-%ld.%s", (long)uid, suffix && suffix[0] ? suffix : "service");
}
#endif

#ifndef OUTER_SHELL_BACKEND_LIBRARY
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

static void archive_migrated_sqlite_registry(const char *sqlite_path) {
    if (!sqlite_path || !sqlite_path[0]) return;
    char migrated_path[PATH_MAX];
    int written = snprintf(migrated_path, sizeof(migrated_path), "%s.migrated", sqlite_path);
    if (written < 0 || (size_t)written >= sizeof(migrated_path)) {
        log_event("Could not archive migrated registry %s: path is too long.", sqlite_path);
        return;
    }
    (void)unlink(migrated_path);
    if (rename(sqlite_path, migrated_path) != 0 && errno != ENOENT) {
        log_event("Could not archive migrated registry %s: %s", sqlite_path, strerror(errno));
    }
}

static bool migrate_sqlite_registry_to_binary_if_needed(const char *sqlite_path, const char *binary_path, char *error, size_t error_size) {
    struct stat binary_stat;
    if (stat(binary_path, &binary_stat) == 0 && S_ISREG(binary_stat.st_mode)) {
        struct stat sqlite_stat;
        if (stat(sqlite_path, &sqlite_stat) == 0 && S_ISREG(sqlite_stat.st_mode)) {
            archive_migrated_sqlite_registry(sqlite_path);
        }
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
        archive_migrated_sqlite_registry(sqlite_path);
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

static sqlite3 *open_registry_readwrite_at(const char *path, char *error, size_t error_size) {
    return open_registry_memory_at(path, true, error, error_size);
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
            ? "INSERT OR REPLACE INTO frontends_new(frontend_id, url, service_id, display_name, port, socket_path, icon, icon_path, list) "
              "SELECT frontend_id, COALESCE(url, ''), service_id, COALESCE(NULLIF(display_name, ''), name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, icon_path, list FROM frontends;"
            : "INSERT OR REPLACE INTO frontends_new(frontend_id, url, service_id, display_name, port, socket_path, icon, icon_path, list) "
              "SELECT frontend_id, COALESCE(url, ''), service_id, COALESCE(display_name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, icon_path, list FROM frontends;";
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
                            "list TEXT"
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
    char *old_frontends_sql = sqlite3_mprintf("SELECT COALESCE(NULLIF(service_id, ''), 'app') || ':' || rowid, url, service_id, %s, COALESCE(port, 0), COALESCE(socket_path, ''), icon, CASE WHEN icon IS NOT NULL AND substr(icon, 1, 5) != 'data:' THEN icon ELSE NULL END, list FROM frontends;",
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
                                    "SELECT service_id, COALESCE(display_name, ''), service_unit FROM backends;",
                                    "INSERT OR REPLACE INTO backends(service_id, display_name, service_unit) VALUES (?, ?, ?);",
                                    error, error_size);
    if (ok) ok = copy_registry_rows(old_database, database,
                                    old_frontends_sql,
                                    "INSERT OR REPLACE INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon, icon_path, list) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
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
    ok = close_registry_readwrite_at(database, new_path, ok, error, error_size) && ok;
    if (ok) {
        archive_migrated_sqlite_registry(old_path);
    }
    return ok;
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

static void migrate_user_outershell_state(void) {
    char old_registry[PATH_MAX];
    char old_apps_root[PATH_MAX];
    char new_apps_root[PATH_MAX];
    char old_outerctl[PATH_MAX];
    char old_outer_shell_outerctl[PATH_MAX];
    char new_outerctl[PATH_MAX];
    legacy_user_registry_database_path(old_registry, sizeof(old_registry));
    legacy_user_apps_root(old_apps_root, sizeof(old_apps_root));
    default_user_outershell_apps_root(new_apps_root, sizeof(new_apps_root));
    legacy_user_outerctl_path(old_outerctl, sizeof(old_outerctl));
    legacy_outer_shell_outerctl_path(old_outer_shell_outerctl, sizeof(old_outer_shell_outerctl));
    default_user_outerctl_path(new_outerctl, sizeof(new_outerctl));
    char new_root[PATH_MAX];
    default_user_outershell_root(new_root, sizeof(new_root));
    (void)mkdir_p(new_root);
    (void)mkdir_p(new_apps_root);

    TextReplacement replacements[] = {
        {old_outer_shell_outerctl, new_outerctl},
        {old_outerctl, new_outerctl},
        {old_apps_root, new_apps_root},
        {"outeragent.log", "backend.log"},
        {"OUTERAGENT_ROOT", "OUTERSHELL_HOME"},
        {"/var/lib/outergroup/outeragent", kSystemOuterShellRoot}
    };

    char error[1024] = "";
    char binary_registry[PATH_MAX] = "";
    bool new_binary_registry_exists = registry_binary_output_path(g_registry_database_path,
                                                                  binary_registry,
                                                                  sizeof(binary_registry)) &&
                                      access(binary_registry, F_OK) == 0;
    if (access(old_registry, R_OK) == 0) {
        if (!new_binary_registry_exists) {
            if (merge_registry_database(old_registry,
                                        g_registry_database_path,
                                        replacements,
                                        sizeof(replacements) / sizeof(replacements[0]),
                                        error,
                                        sizeof(error))) {
                log_event("Migrated user outershell registry from %s to %s.", old_registry, g_registry_database_path);
            } else {
                log_event("Failed to migrate user registry from %s: %s", old_registry, error);
            }
        } else {
            archive_migrated_sqlite_registry(old_registry);
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
#else
static bool migrate_sqlite_registry_to_binary_if_needed(const char *sqlite_path, const char *binary_path, char *error, size_t error_size) {
    (void)sqlite_path;
    (void)binary_path;
    (void)error;
    (void)error_size;
    return true;
}

static void migrate_user_outershell_state(void) {
}
#endif

enum {
    ORWA_TABLE_BACKENDS = 0,
    ORWA_TABLE_FRONTENDS = 1,
    ORWA_TABLE_FRONTEND_LAYOUTS = 2,
    ORWA_TABLE_LOG_FILES = 3,
    ORWA_TABLE_CONTENT_TYPES = 4,
    ORWA_TABLE_FILE_OPENERS = 5,
    ORWA_TABLE_COUNT = 6,
    ORWA_LEGACY_FOUR_TABLE_COUNT = 4,
    ORWA_LEGACY_THREE_TABLE_COUNT = 3,
    ORWA_TABLE_DESCRIPTOR_SIZE = 20,
    ORWA_HEADER_SIZE = 8 + ORWA_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_FOUR_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_FOUR_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_THREE_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_THREE_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_BACKENDS_ROW_SIZE = 68,
    ORWA_LEGACY_BACKENDS_ROW_SIZE = 84,
    ORWA_LEGACY_FRONTENDS_ROW_SIZE = 97,
    ORWA_FRONTENDS_ROW_SIZE = 113,
    ORWA_FRONTENDS_ROW_SIZE_WITH_FLAGS = 117,
    ORWA_FRONTEND_LAYOUTS_ROW_SIZE = 32,
    ORWA_LOG_FILES_ROW_SIZE = 32,
    ORWA_CONTENT_TYPES_ROW_SIZE = 96,
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

#ifndef OUTER_SHELL_BACKEND_LIBRARY
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
#endif

static bool registry_binary_output_path(const char *sqlite_path, char *out, size_t out_size) {
    if (!sqlite_path || !sqlite_path[0]) return false;
    const char *basename = strrchr(sqlite_path, '/');
    basename = basename ? basename + 1 : sqlite_path;
    if (strcmp(basename, "registry.orwa") == 0) {
        int written = snprintf(out, out_size, "%s", sqlite_path);
        return written >= 0 && (size_t)written < out_size;
    }
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

static bool registry_legacy_sqlite_path(const char *registry_path, char *out, size_t out_size) {
    if (!registry_path || !registry_path[0]) return false;
    const char *basename = strrchr(registry_path, '/');
    basename = basename ? basename + 1 : registry_path;
    if (strcmp(basename, "registry.orwa") != 0) {
        int written = snprintf(out, out_size, "%s", registry_path);
        return written >= 0 && (size_t)written < out_size;
    }
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", registry_path);
    char *slash = strrchr(directory, '/');
    if (!slash) {
        return snprintf(out, out_size, "registry.sqlite3") > 0;
    }
    if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int written = snprintf(out, out_size, "%s/registry.sqlite3", directory);
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

#ifndef OUTER_SHELL_BACKEND_LIBRARY
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
    uint32_t flags = sqlite3_column_int(statement, 4) != 0 ? 1u : 0u;
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 0)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 1)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 2)) &&
           registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 3)) &&
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
    return registry_binary_append_string_ref(pool, variable_region, rows, sqlite_column_text_or_empty(statement, 7));
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
#endif

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

#ifndef OUTER_SHELL_BACKEND_LIBRARY
static bool registry_binary_step(sqlite3 *database, sqlite3_stmt *statement, char *error, size_t error_size) {
    int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) return true;
    snprintf(error, error_size, "%s", sqlite3_errmsg(database));
    return false;
}

static bool registry_binary_import_backend(sqlite3 *database,
                                           const char *service_id,
                                           const char *display_name,
                                           const char *unit_name,
                                           const char *unit_path,
                                           bool owns_unit,
                                           char *error,
                                           size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO backends(service_id, display_name, service_unit) VALUES(?, ?, NULLIF(?, '')) "
                                 "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, service_unit=excluded.service_unit;",
                                 -1,
                                 &statement,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, service_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, unit_name, -1, SQLITE_TRANSIENT);
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
                                            char *error,
                                            size_t error_size) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(database,
                                 "INSERT INTO frontends(frontend_id, url, service_id, display_name, port, socket_path, icon_path, list) VALUES(?, ?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, '')) "
                                 "ON CONFLICT(frontend_id) DO UPDATE SET url=excluded.url, service_id=excluded.service_id, display_name=excluded.display_name, port=excluded.port, socket_path=excluded.socket_path, icon_path=excluded.icon_path, list=excluded.list;",
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
                                     i == ORWA_TABLE_CONTENT_TYPES ? ORWA_CONTENT_TYPES_ROW_SIZE :
                                     i == ORWA_TABLE_FILE_OPENERS ? ORWA_FILE_OPENERS_ROW_SIZE :
                                     ORWA_LOG_FILES_ROW_SIZE;
        if ((i == ORWA_TABLE_BACKENDS
                ? (descriptors[i].row_size != ORWA_BACKENDS_ROW_SIZE && descriptors[i].row_size != ORWA_LEGACY_BACKENDS_ROW_SIZE)
                : i == ORWA_TABLE_FRONTENDS
                ? (descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE &&
                   descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE_WITH_FLAGS &&
                   descriptors[i].row_size != ORWA_LEGACY_FRONTENDS_ROW_SIZE)
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
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_BACKENDS].offset + row * descriptors[ORWA_TABLE_BACKENDS].row_size;
        char *service_id = NULL, *display_name = NULL, *unit_name = NULL, *unit_path = NULL;
        ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes), read_uint64_le(row_bytes + 8), &service_id, error, error_size) &&
             registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 16), read_uint64_le(row_bytes + 24), &display_name, error, error_size);
        if (ok && descriptors[ORWA_TABLE_BACKENDS].row_size == ORWA_LEGACY_BACKENDS_ROW_SIZE) {
            ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 48), read_uint64_le(row_bytes + 56), &unit_name, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 64), read_uint64_le(row_bytes + 72), &unit_path, error, error_size);
        } else if (ok) {
            ok = registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 32), read_uint64_le(row_bytes + 40), &unit_name, error, error_size) &&
                 registry_binary_read_string(bytes, file_size, variable_offset, read_uint64_le(row_bytes + 48), read_uint64_le(row_bytes + 56), &unit_path, error, error_size);
        }
        if (ok) {
            size_t flags_offset = descriptors[ORWA_TABLE_BACKENDS].row_size == ORWA_LEGACY_BACKENDS_ROW_SIZE ? 80 : 64;
            bool owns_unit = (read_uint32_le(row_bytes + flags_offset) & 1u) != 0;
            ok = registry_binary_import_backend(database, service_id, display_name, unit_name, unit_path, owns_unit, error, error_size);
        }
        free(service_id);
        free(display_name);
        free(unit_name);
        free(unit_path);
    }

    for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FRONTENDS].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FRONTENDS].offset + row * descriptors[ORWA_TABLE_FRONTENDS].row_size;
        char *url = NULL, *service_id = NULL, *display_name = NULL, *icon_path = NULL, *list = NULL, *socket_path = NULL, *frontend_id = NULL;
        uint32_t port = 0;
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
            ok = registry_binary_import_frontend(database, frontend_id, url, service_id, display_name, port, socket_path, icon_path, list, error, error_size);
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
        {.row_count = 0, .row_size = ORWA_CONTENT_TYPES_ROW_SIZE},
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
        char *sql = sqlite3_mprintf("SELECT b.service_id, COALESCE(b.display_name, ''), %s, %s, %s FROM backends b %s %s ORDER BY b.service_id;",
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
                                              5,
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
                                          "SELECT url, COALESCE(service_id, ''), COALESCE(display_name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), COALESCE(icon_path, ''), COALESCE(list, ''), COALESCE(frontend_id, '') FROM frontends ORDER BY service_id, COALESCE(list, ''), display_name, url;",
                                          8,
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
#endif

typedef struct {
    char *service_id;
    char *display_name;
    char *unit_name;
    char *unit_path;
    bool owns_unit;
} RegistryBackendRecord;

typedef struct {
    char *frontend_id;
    char *url;
    char *service_id;
    char *display_name;
    int port;
    char *socket_path;
    char *icon_path;
    char *list;
} RegistryFrontendRecord;

typedef struct {
    char *url;
    char *list;
} RegistryFrontendLayoutRecord;

typedef struct {
    char *path;
    char *service_id;
} RegistryLogFileRecord;

typedef struct {
    char *identifier;
    char *display_name;
    char *conforms_to;
    char *extensions;
    char *filenames;
    char *mime_types;
} RegistryContentTypeRecord;

typedef struct {
    char *extension;
    char *service_id;
    char *display_name;
    char *socket_path;
    char *url_template;
    int rank;
} RegistryFileOpenerRecord;

typedef struct {
    RegistryBackendRecord *backends;
    size_t backend_count;
    size_t backend_capacity;
    RegistryFrontendRecord *frontends;
    size_t frontend_count;
    size_t frontend_capacity;
    RegistryFrontendLayoutRecord *layouts;
    size_t layout_count;
    size_t layout_capacity;
    RegistryLogFileRecord *logs;
    size_t log_count;
    size_t log_capacity;
    RegistryContentTypeRecord *content_types;
    size_t content_type_count;
    size_t content_type_capacity;
    RegistryFileOpenerRecord *openers;
    size_t opener_count;
    size_t opener_capacity;
    char binary_path[PATH_MAX];
    int lock_fd;
} RegistryStore;

static char *registry_strdup(const char *value) {
    char *copy = strdup(value ? value : "");
    return copy;
}

static bool registry_assign_string(char **slot, const char *value) {
    char *copy = registry_strdup(value);
    if (!copy) return false;
    free(*slot);
    *slot = copy;
    return true;
}

static void registry_store_free(RegistryStore *store) {
    if (!store) return;
    for (size_t i = 0; i < store->backend_count; i++) {
        free(store->backends[i].service_id);
        free(store->backends[i].display_name);
        free(store->backends[i].unit_name);
        free(store->backends[i].unit_path);
    }
    free(store->backends);
    for (size_t i = 0; i < store->frontend_count; i++) {
        free(store->frontends[i].frontend_id);
        free(store->frontends[i].url);
        free(store->frontends[i].service_id);
        free(store->frontends[i].display_name);
        free(store->frontends[i].socket_path);
        free(store->frontends[i].icon_path);
        free(store->frontends[i].list);
    }
    free(store->frontends);
    for (size_t i = 0; i < store->layout_count; i++) {
        free(store->layouts[i].url);
        free(store->layouts[i].list);
    }
    free(store->layouts);
    for (size_t i = 0; i < store->log_count; i++) {
        free(store->logs[i].path);
        free(store->logs[i].service_id);
    }
    free(store->logs);
    for (size_t i = 0; i < store->content_type_count; i++) {
        free(store->content_types[i].identifier);
        free(store->content_types[i].display_name);
        free(store->content_types[i].conforms_to);
        free(store->content_types[i].extensions);
        free(store->content_types[i].filenames);
        free(store->content_types[i].mime_types);
    }
    free(store->content_types);
    for (size_t i = 0; i < store->opener_count; i++) {
        free(store->openers[i].extension);
        free(store->openers[i].service_id);
        free(store->openers[i].display_name);
        free(store->openers[i].socket_path);
        free(store->openers[i].url_template);
    }
    free(store->openers);
    if (store->lock_fd >= 0) {
        flock(store->lock_fd, LOCK_UN);
        close(store->lock_fd);
    }
    memset(store, 0, sizeof(*store));
    store->lock_fd = -1;
}

#define REGISTRY_ENSURE_CAPACITY(store, field, type) \
    do { \
        if ((store)->field##_count == (store)->field##_capacity) { \
            size_t new_capacity = (store)->field##_capacity ? (store)->field##_capacity * 2 : 16; \
            type *new_items = realloc((store)->field##s, new_capacity * sizeof(type)); \
            if (!new_items) return false; \
            (store)->field##s = new_items; \
            (store)->field##_capacity = new_capacity; \
        } \
    } while (0)

static RegistryBackendRecord *registry_store_find_backend(RegistryStore *store, const char *service_id) {
    for (size_t i = 0; i < store->backend_count; i++) {
        if (strcmp(store->backends[i].service_id, service_id ? service_id : "") == 0) return &store->backends[i];
    }
    return NULL;
}

static const RegistryBackendRecord *registry_store_find_backend_const(const RegistryStore *store, const char *service_id) {
    for (size_t i = 0; i < store->backend_count; i++) {
        if (strcmp(store->backends[i].service_id, service_id ? service_id : "") == 0) return &store->backends[i];
    }
    return NULL;
}

static RegistryFrontendRecord *registry_store_find_frontend(RegistryStore *store, const char *frontend_id) {
    for (size_t i = 0; i < store->frontend_count; i++) {
        if (strcmp(store->frontends[i].frontend_id, frontend_id ? frontend_id : "") == 0) return &store->frontends[i];
    }
    return NULL;
}

static RegistryFrontendLayoutRecord *registry_store_find_layout(RegistryStore *store, const char *url) {
    for (size_t i = 0; i < store->layout_count; i++) {
        if (strcmp(store->layouts[i].url, url ? url : "") == 0) return &store->layouts[i];
    }
    return NULL;
}

static const RegistryFrontendLayoutRecord *registry_store_find_layout_const(const RegistryStore *store, const char *url) {
    for (size_t i = 0; i < store->layout_count; i++) {
        if (strcmp(store->layouts[i].url, url ? url : "") == 0) return &store->layouts[i];
    }
    return NULL;
}

static RegistryLogFileRecord *registry_store_find_log_by_path(RegistryStore *store, const char *path) {
    for (size_t i = 0; i < store->log_count; i++) {
        if (strcmp(store->logs[i].path, path ? path : "") == 0) return &store->logs[i];
    }
    return NULL;
}

static RegistryContentTypeRecord *registry_store_find_content_type(RegistryStore *store, const char *identifier) {
    for (size_t i = 0; i < store->content_type_count; i++) {
        if (strcmp(store->content_types[i].identifier, identifier ? identifier : "") == 0) return &store->content_types[i];
    }
    return NULL;
}

static const RegistryContentTypeRecord *registry_store_find_content_type_const(const RegistryStore *store, const char *identifier) {
    for (size_t i = 0; i < store->content_type_count; i++) {
        if (strcmp(store->content_types[i].identifier, identifier ? identifier : "") == 0) return &store->content_types[i];
    }
    return NULL;
}

static RegistryFileOpenerRecord *registry_store_find_opener(RegistryStore *store, const char *extension, const char *service_id) {
    for (size_t i = 0; i < store->opener_count; i++) {
        if (strcmp(store->openers[i].extension, extension ? extension : "") == 0 &&
            strcmp(store->openers[i].service_id, service_id ? service_id : "") == 0) {
            return &store->openers[i];
        }
    }
    return NULL;
}

static bool registry_store_upsert_backend(RegistryStore *store,
                                          const char *service_id,
                                          const char *display_name,
                                          const char *unit_name,
                                          const char *unit_path,
                                          bool owns_unit) {
    RegistryBackendRecord *record = registry_store_find_backend(store, service_id);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, backend, RegistryBackendRecord);
        record = &store->backends[store->backend_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->service_id, service_id)) return false;
    }
    if (!registry_assign_string(&record->display_name, display_name) ||
        !registry_assign_string(&record->unit_name, unit_name) ||
        !registry_assign_string(&record->unit_path, unit_path)) {
        return false;
    }
    record->owns_unit = owns_unit;
    return true;
}

static bool registry_store_upsert_frontend(RegistryStore *store,
                                           const char *frontend_id,
                                           const char *url,
                                           const char *service_id,
                                           const char *display_name,
                                           int port,
                                           const char *socket_path,
                                           const char *icon_path,
                                           const char *list,
                                           bool preserve_empty_icon_and_list) {
    RegistryFrontendRecord *record = registry_store_find_frontend(store, frontend_id);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, frontend, RegistryFrontendRecord);
        record = &store->frontends[store->frontend_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->frontend_id, frontend_id)) return false;
    }
    if (!registry_assign_string(&record->url, url) ||
        !registry_assign_string(&record->service_id, service_id) ||
        !registry_assign_string(&record->display_name, display_name) ||
        !registry_assign_string(&record->socket_path, socket_path)) {
        return false;
    }
    record->port = port > 0 ? port : 0;
    if (!preserve_empty_icon_and_list || (icon_path && icon_path[0])) {
        if (!registry_assign_string(&record->icon_path, icon_path)) return false;
    } else if (!record->icon_path && !registry_assign_string(&record->icon_path, "")) {
        return false;
    }
    if (!preserve_empty_icon_and_list || (list && list[0])) {
        if (!registry_assign_string(&record->list, list)) return false;
    } else if (!record->list && !registry_assign_string(&record->list, "")) {
        return false;
    }
    return true;
}

static bool registry_store_upsert_layout(RegistryStore *store, const char *url, const char *list) {
    RegistryFrontendLayoutRecord *record = registry_store_find_layout(store, url);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, layout, RegistryFrontendLayoutRecord);
        record = &store->layouts[store->layout_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->url, url)) return false;
    }
    return registry_assign_string(&record->list, list);
}

static bool registry_store_upsert_log(RegistryStore *store, const char *path, const char *service_id) {
    RegistryLogFileRecord *record = registry_store_find_log_by_path(store, path);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, log, RegistryLogFileRecord);
        record = &store->logs[store->log_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->path, path)) return false;
    }
    return registry_assign_string(&record->service_id, service_id);
}

static bool registry_store_upsert_content_type(RegistryStore *store,
                                               const char *identifier,
                                               const char *display_name,
                                               const char *conforms_to,
                                               const char *extensions,
                                               const char *filenames,
                                               const char *mime_types) {
    RegistryContentTypeRecord *record = registry_store_find_content_type(store, identifier);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, content_type, RegistryContentTypeRecord);
        record = &store->content_types[store->content_type_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->identifier, identifier)) return false;
    }
    return registry_assign_string(&record->display_name, display_name) &&
           registry_assign_string(&record->conforms_to, conforms_to) &&
           registry_assign_string(&record->extensions, extensions) &&
           registry_assign_string(&record->filenames, filenames) &&
           registry_assign_string(&record->mime_types, mime_types);
}

static bool registry_store_upsert_opener(RegistryStore *store,
                                         const char *extension,
                                         const char *service_id,
                                         const char *display_name,
                                         const char *socket_path,
                                         const char *url_template,
                                         int rank) {
    RegistryFileOpenerRecord *record = registry_store_find_opener(store, extension, service_id);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, opener, RegistryFileOpenerRecord);
        record = &store->openers[store->opener_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->extension, extension) ||
            !registry_assign_string(&record->service_id, service_id)) {
            return false;
        }
    }
    if (!registry_assign_string(&record->display_name, display_name) ||
        !registry_assign_string(&record->socket_path, socket_path) ||
        !registry_assign_string(&record->url_template, (url_template && url_template[0]) ? url_template : "?file={file}")) {
        return false;
    }
    record->rank = rank;
    return true;
}

static void registry_store_remove_frontend_at(RegistryStore *store, size_t index) {
    if (index >= store->frontend_count) return;
    RegistryFrontendRecord *frontend = &store->frontends[index];
    for (size_t i = store->layout_count; i > 0; i--) {
        RegistryFrontendLayoutRecord *layout = &store->layouts[i - 1];
        bool matches_frontend_id = frontend->frontend_id && frontend->frontend_id[0] &&
                                   strcmp(layout->url ? layout->url : "", frontend->frontend_id) == 0;
        bool matches_url = frontend->url && frontend->url[0] &&
                           strcmp(layout->url ? layout->url : "", frontend->url) == 0;
        if (!matches_frontend_id && !matches_url) continue;
        free(layout->url);
        free(layout->list);
        memmove(layout, layout + 1, (store->layout_count - i) * sizeof(*layout));
        store->layout_count--;
    }
    RegistryFrontendRecord *record = &store->frontends[index];
    free(record->frontend_id);
    free(record->url);
    free(record->service_id);
    free(record->display_name);
    free(record->socket_path);
    free(record->icon_path);
    free(record->list);
    memmove(record, record + 1, (store->frontend_count - index - 1) * sizeof(*record));
    store->frontend_count--;
}

static void registry_store_remove_log_at(RegistryStore *store, size_t index) {
    if (index >= store->log_count) return;
    RegistryLogFileRecord *record = &store->logs[index];
    free(record->path);
    free(record->service_id);
    memmove(record, record + 1, (store->log_count - index - 1) * sizeof(*record));
    store->log_count--;
}

static void registry_store_remove_content_type_at(RegistryStore *store, size_t index) {
    if (index >= store->content_type_count) return;
    RegistryContentTypeRecord *record = &store->content_types[index];
    free(record->identifier);
    free(record->display_name);
    free(record->conforms_to);
    free(record->extensions);
    free(record->filenames);
    free(record->mime_types);
    memmove(record, record + 1, (store->content_type_count - index - 1) * sizeof(*record));
    store->content_type_count--;
}

static void registry_store_remove_opener_at(RegistryStore *store, size_t index) {
    if (index >= store->opener_count) return;
    RegistryFileOpenerRecord *record = &store->openers[index];
    free(record->extension);
    free(record->service_id);
    free(record->display_name);
    free(record->socket_path);
    free(record->url_template);
    memmove(record, record + 1, (store->opener_count - index - 1) * sizeof(*record));
    store->opener_count--;
}

static bool registry_store_remove_backend(RegistryStore *store, const char *service_id) {
    for (size_t i = 0; i < store->backend_count; i++) {
        if (strcmp(store->backends[i].service_id, service_id ? service_id : "") != 0) continue;
        RegistryBackendRecord *record = &store->backends[i];
        free(record->service_id);
        free(record->display_name);
        free(record->unit_name);
        free(record->unit_path);
        memmove(record, record + 1, (store->backend_count - i - 1) * sizeof(*record));
        store->backend_count--;
        return true;
    }
    return false;
}

static void registry_store_clear_backend_frontends(RegistryStore *store, const char *service_id) {
    for (size_t i = store->frontend_count; i > 0; i--) {
        if (strcmp(store->frontends[i - 1].service_id, service_id ? service_id : "") == 0) {
            registry_store_remove_frontend_at(store, i - 1);
        }
    }
}

static void registry_store_clear_backend_logs(RegistryStore *store, const char *service_id) {
    for (size_t i = store->log_count; i > 0; i--) {
        if (strcmp(store->logs[i - 1].service_id, service_id ? service_id : "") == 0) {
            registry_store_remove_log_at(store, i - 1);
        }
    }
}

static void registry_store_clear_backend_openers(RegistryStore *store, const char *service_id) {
    for (size_t i = store->opener_count; i > 0; i--) {
        if (strcmp(store->openers[i - 1].service_id, service_id ? service_id : "") == 0) {
            registry_store_remove_opener_at(store, i - 1);
        }
    }
}

static void registry_store_clear_content_types(RegistryStore *store) {
    while (store->content_type_count > 0) {
        registry_store_remove_content_type_at(store, store->content_type_count - 1);
    }
}

static bool registry_store_read_string(const unsigned char *bytes,
                                       size_t file_size,
                                       uint64_t variable_offset,
                                       const unsigned char *row_bytes,
                                       size_t offset,
                                       char **out,
                                       char *error,
                                       size_t error_size) {
    return registry_binary_read_string(bytes,
                                       file_size,
                                       variable_offset,
                                       read_uint64_le(row_bytes + offset),
                                       read_uint64_le(row_bytes + offset + 8),
                                       out,
                                       error,
                                       error_size);
}

static bool registry_store_load_orwa_file(RegistryStore *store, const char *path, char *error, size_t error_size) {
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
    bool ok = true;
    if (file_size < ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ||
        memcmp(file_data, "ORWA", 4) != 0 ||
        read_uint32_le(bytes + 4) != 1) {
        snprintf(error, error_size, "Registry binary has an unsupported header.");
        ok = false;
    }

    RegistryBinaryTableDescriptor descriptors[ORWA_TABLE_COUNT] = {0};
    size_t table_count = 0;
    uint64_t variable_offset = 0;
    if (ok) {
        uint64_t first_table_offset = read_uint64_le(bytes + 8);
        table_count = first_table_offset == ORWA_HEADER_SIZE ? ORWA_TABLE_COUNT :
                      first_table_offset == ORWA_LEGACY_FOUR_TABLE_HEADER_SIZE ? ORWA_LEGACY_FOUR_TABLE_COUNT :
                      first_table_offset == ORWA_LEGACY_THREE_TABLE_HEADER_SIZE ? ORWA_LEGACY_THREE_TABLE_COUNT :
                      0;
        if (table_count == 0 || file_size < 8 + table_count * ORWA_TABLE_DESCRIPTOR_SIZE) {
            snprintf(error, error_size, "Registry binary has an unsupported table layout.");
            ok = false;
        }
    }
    for (size_t i = 0; ok && i < table_count; i++) {
        size_t descriptor_offset = 8 + i * ORWA_TABLE_DESCRIPTOR_SIZE;
        descriptors[i].offset = read_uint64_le(bytes + descriptor_offset);
        descriptors[i].row_count = read_uint64_le(bytes + descriptor_offset + 8);
        descriptors[i].row_size = read_uint32_le(bytes + descriptor_offset + 16);
        uint32_t expected_row_size = i == ORWA_TABLE_BACKENDS ? ORWA_BACKENDS_ROW_SIZE :
                                     i == ORWA_TABLE_FRONTENDS ? descriptors[i].row_size :
                                     i == ORWA_TABLE_FRONTEND_LAYOUTS && table_count != ORWA_LEGACY_THREE_TABLE_COUNT ? ORWA_FRONTEND_LAYOUTS_ROW_SIZE :
                                     i == ORWA_TABLE_CONTENT_TYPES ? ORWA_CONTENT_TYPES_ROW_SIZE :
                                     i == ORWA_TABLE_FILE_OPENERS ? ORWA_FILE_OPENERS_ROW_SIZE :
                                     ORWA_LOG_FILES_ROW_SIZE;
        if ((i == ORWA_TABLE_BACKENDS
                ? (descriptors[i].row_size != ORWA_BACKENDS_ROW_SIZE && descriptors[i].row_size != ORWA_LEGACY_BACKENDS_ROW_SIZE)
                : i == ORWA_TABLE_FRONTENDS
                ? (descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE &&
                   descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE_WITH_FLAGS &&
                   descriptors[i].row_size != ORWA_LEGACY_FRONTENDS_ROW_SIZE)
                : descriptors[i].row_size != expected_row_size) ||
            descriptors[i].offset > (uint64_t)file_size ||
            descriptors[i].row_count > UINT64_MAX / descriptors[i].row_size ||
            descriptors[i].row_count * descriptors[i].row_size > (uint64_t)file_size - descriptors[i].offset) {
            snprintf(error, error_size, "Registry binary table descriptor is invalid.");
            ok = false;
            break;
        }
        uint64_t table_end = descriptors[i].offset + descriptors[i].row_count * descriptors[i].row_size;
        if (table_end > variable_offset) variable_offset = table_end;
    }

    for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_BACKENDS].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_BACKENDS].offset + row * descriptors[ORWA_TABLE_BACKENDS].row_size;
        char *service_id = NULL, *display_name = NULL, *unit_name = NULL, *unit_path = NULL;
        ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &service_id, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &display_name, error, error_size);
        if (ok && descriptors[ORWA_TABLE_BACKENDS].row_size == ORWA_LEGACY_BACKENDS_ROW_SIZE) {
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &unit_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 64, &unit_path, error, error_size);
        } else if (ok) {
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &unit_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &unit_path, error, error_size);
        }
        if (ok) {
            size_t flags_offset = descriptors[ORWA_TABLE_BACKENDS].row_size == ORWA_LEGACY_BACKENDS_ROW_SIZE ? 80 : 64;
            ok = registry_store_upsert_backend(store,
                                               service_id,
                                               display_name,
                                               unit_name,
                                               unit_path,
                                               (read_uint32_le(row_bytes + flags_offset) & 1u) != 0);
            if (!ok) snprintf(error, error_size, "Out of memory.");
        }
        free(service_id);
        free(display_name);
        free(unit_name);
        free(unit_path);
    }

    for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FRONTENDS].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FRONTENDS].offset + row * descriptors[ORWA_TABLE_FRONTENDS].row_size;
        char *url = NULL, *service_id = NULL, *display_name = NULL, *icon_path = NULL, *list = NULL, *socket_path = NULL, *frontend_id = NULL;
        uint32_t port = 0;
        ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &url, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &service_id, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &display_name, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &icon_path, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 64, &list, error, error_size);
        if (ok) {
            uint8_t endpoint_kind = row_bytes[80];
            if (endpoint_kind == 1u) {
                port = read_uint32_le(row_bytes + 81);
                socket_path = registry_strdup("");
            } else if (endpoint_kind == 2u) {
                ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 81, &socket_path, error, error_size);
            } else if (endpoint_kind == 0u) {
                socket_path = registry_strdup("");
            } else {
                snprintf(error, error_size, "Registry binary frontend endpoint kind is unsupported.");
                ok = false;
            }
            if (ok && !socket_path) {
                snprintf(error, error_size, "Out of memory.");
                ok = false;
            }
        }
        if (ok && descriptors[ORWA_TABLE_FRONTENDS].row_size >= ORWA_FRONTENDS_ROW_SIZE) {
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 97, &frontend_id, error, error_size);
        } else if (ok) {
            size_t needed = strlen(service_id ? service_id : "") + strlen(url ? url : "") + 2;
            frontend_id = malloc(needed);
            if (frontend_id) snprintf(frontend_id, needed, "%s:%s", service_id ? service_id : "app", url && url[0] ? url : "main");
            else {
                snprintf(error, error_size, "Out of memory.");
                ok = false;
            }
        }
        if (ok) {
            ok = registry_store_upsert_frontend(store, frontend_id, url, service_id, display_name, (int)port, socket_path, icon_path, list, false);
            if (!ok) snprintf(error, error_size, "Out of memory.");
        }
        if (ok && table_count == ORWA_LEGACY_THREE_TABLE_COUNT) {
            ok = registry_store_upsert_layout(store, url, list);
            if (!ok) snprintf(error, error_size, "Out of memory.");
        }
        free(url);
        free(service_id);
        free(display_name);
        free(icon_path);
        free(list);
        free(socket_path);
        free(frontend_id);
    }

    if (ok && table_count != ORWA_LEGACY_THREE_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FRONTEND_LAYOUTS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FRONTEND_LAYOUTS].offset + row * ORWA_FRONTEND_LAYOUTS_ROW_SIZE;
            char *url = NULL, *list = NULL;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &url, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &list, error, error_size);
            if (ok) {
                ok = registry_store_upsert_layout(store, url, list);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(url);
            free(list);
        }
    }

    size_t log_table_index = table_count == ORWA_LEGACY_THREE_TABLE_COUNT ? 2 : ORWA_TABLE_LOG_FILES;
    for (uint64_t row = 0; ok && row < descriptors[log_table_index].row_count; row++) {
        const unsigned char *row_bytes = bytes + descriptors[log_table_index].offset + row * ORWA_LOG_FILES_ROW_SIZE;
        char *path_value = NULL, *service_id = NULL;
        ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &path_value, error, error_size) &&
             registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &service_id, error, error_size);
        if (ok) {
            ok = registry_store_upsert_log(store, path_value, service_id);
            if (!ok) snprintf(error, error_size, "Out of memory.");
        }
        free(path_value);
        free(service_id);
    }

    if (ok && table_count == ORWA_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_CONTENT_TYPES].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_CONTENT_TYPES].offset + row * ORWA_CONTENT_TYPES_ROW_SIZE;
            char *identifier = NULL, *display_name = NULL, *conforms_to = NULL, *extensions = NULL, *filenames = NULL, *mime_types = NULL;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &identifier, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &display_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &conforms_to, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &extensions, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 64, &filenames, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 80, &mime_types, error, error_size);
            if (ok) {
                ok = registry_store_upsert_content_type(store,
                                                        identifier,
                                                        display_name,
                                                        conforms_to,
                                                        extensions,
                                                        filenames,
                                                        mime_types);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(identifier);
            free(display_name);
            free(conforms_to);
            free(extensions);
            free(filenames);
            free(mime_types);
        }
    }

    if (ok && table_count == ORWA_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FILE_OPENERS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FILE_OPENERS].offset + row * ORWA_FILE_OPENERS_ROW_SIZE;
            char *extension = NULL, *service_id = NULL, *display_name = NULL, *socket_path = NULL, *url_template = NULL;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &extension, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &service_id, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &display_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &socket_path, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 64, &url_template, error, error_size);
            if (ok) {
                ok = registry_store_upsert_opener(store,
                                                  extension,
                                                  service_id,
                                                  display_name,
                                                  socket_path,
                                                  url_template,
                                                  (int)read_uint32_le(row_bytes + 80));
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(extension);
            free(service_id);
            free(display_name);
            free(socket_path);
            free(url_template);
        }
    }

    free(file_data);
    return ok;
}

static bool registry_store_write_orwa_file(RegistryStore *store, const char *path, char *error, size_t error_size) {
    RegistryBinaryTableDescriptor descriptors[ORWA_TABLE_COUNT] = {
        {.row_count = store->backend_count, .row_size = ORWA_BACKENDS_ROW_SIZE},
        {.row_count = store->frontend_count, .row_size = ORWA_FRONTENDS_ROW_SIZE},
        {.row_count = store->layout_count, .row_size = ORWA_FRONTEND_LAYOUTS_ROW_SIZE},
        {.row_count = store->log_count, .row_size = ORWA_LOG_FILES_ROW_SIZE},
        {.row_count = store->content_type_count, .row_size = ORWA_CONTENT_TYPES_ROW_SIZE},
        {.row_count = store->opener_count, .row_size = ORWA_FILE_OPENERS_ROW_SIZE},
    };
    uint64_t offset = ORWA_HEADER_SIZE;
    for (size_t i = 0; i < ORWA_TABLE_COUNT; i++) {
        descriptors[i].offset = offset;
        offset += descriptors[i].row_count * descriptors[i].row_size;
    }
    uint64_t variable_region_offset = offset;
    StringBuilder rows = {0};
    StringBuilder variable_region = {0};
    StringBuilder file = {0};
    RegistryBinaryStringPool pool = {.variable_base_offset = variable_region_offset};
    bool ok = true;

    for (size_t i = 0; ok && i < store->backend_count; i++) {
        RegistryBackendRecord *record = &store->backends[i];
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, record->service_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->display_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->unit_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->unit_path) &&
             binary_append_u32(&rows, record->owns_unit ? 1u : 0u);
    }
    for (size_t i = 0; ok && i < store->frontend_count; i++) {
        RegistryFrontendRecord *record = &store->frontends[i];
        unsigned char endpoint_kind = record->socket_path && record->socket_path[0] ? 2u : (record->port > 0 ? 1u : 0u);
        unsigned char zero_padding[12] = {0};
        unsigned char empty_payload[16] = {0};
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, record->url) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->service_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->display_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->icon_path) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->list) &&
             sb_append_n(&rows, (const char *)&endpoint_kind, sizeof(endpoint_kind));
        if (ok && endpoint_kind == 2u) {
            ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, record->socket_path);
        } else if (ok && endpoint_kind == 1u) {
            ok = binary_append_u32(&rows, (uint32_t)record->port) &&
                 sb_append_n(&rows, (const char *)zero_padding, sizeof(zero_padding));
        } else if (ok) {
            ok = sb_append_n(&rows, (const char *)empty_payload, sizeof(empty_payload));
        }
        if (ok) ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, record->frontend_id);
    }
    for (size_t i = 0; ok && i < store->layout_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->layouts[i].url) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->layouts[i].list);
    }
    for (size_t i = 0; ok && i < store->log_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->logs[i].path) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->logs[i].service_id);
    }
    for (size_t i = 0; ok && i < store->content_type_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].identifier) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].display_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].conforms_to) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].extensions) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].filenames) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].mime_types);
    }
    for (size_t i = 0; ok && i < store->opener_count; i++) {
        unsigned char padding[4] = {0};
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].extension) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].service_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].display_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].socket_path) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].url_template) &&
             binary_append_u32(&rows, (uint32_t)store->openers[i].rank) &&
             sb_append_n(&rows, (const char *)padding, sizeof(padding));
    }
    if (ok && rows.length != variable_region_offset - ORWA_HEADER_SIZE) {
        snprintf(error, error_size, "Registry binary row length mismatch.");
        ok = false;
    }
    if (ok) ok = sb_append_n(&file, "ORWA", 4) && binary_append_u32(&file, 1);
    for (size_t i = 0; ok && i < ORWA_TABLE_COUNT; i++) {
        ok = binary_append_u64(&file, descriptors[i].offset) &&
             binary_append_u64(&file, descriptors[i].row_count) &&
             binary_append_u32(&file, descriptors[i].row_size);
    }
    if (ok) {
        ok = sb_append_n(&file, rows.data ? rows.data : "", rows.length) &&
             sb_append_n(&file, variable_region.data ? variable_region.data : "", variable_region.length);
    }
    if (ok) ok = registry_binary_write_file(path, file.data, file.length, error, error_size);

    registry_binary_string_pool_free(&pool);
    free(rows.data);
    free(variable_region.data);
    free(file.data);
    if (!ok && !error[0]) snprintf(error, error_size, "Out of memory while writing registry.");
    return ok;
}

static bool registry_store_open_at(RegistryStore *store, const char *database_path, bool writable, char *error, size_t error_size) {
    memset(store, 0, sizeof(*store));
    store->lock_fd = -1;
    if (!registry_binary_output_path(database_path, store->binary_path, sizeof(store->binary_path))) {
        snprintf(error, error_size, "Registry path is too long.");
        return false;
    }
    if (writable && !ensure_parent_directory(store->binary_path, error, error_size)) return false;
    char sqlite_path[PATH_MAX];
    if (!registry_legacy_sqlite_path(database_path, sqlite_path, sizeof(sqlite_path))) {
        snprintf(error, error_size, "Registry path is too long.");
        return false;
    }
    if (!migrate_sqlite_registry_to_binary_if_needed(sqlite_path, store->binary_path, error, error_size)) return false;
    if (writable) {
        store->lock_fd = registry_binary_lock(store->binary_path, LOCK_EX, error, error_size);
        if (store->lock_fd < 0) return false;
        if (g_registry_write_lock_fd >= 0) {
            snprintf(error, error_size, "registry writer lock is already held");
            registry_store_free(store);
            return false;
        }
        g_registry_write_lock_fd = store->lock_fd;
        snprintf(g_registry_write_lock_path, sizeof(g_registry_write_lock_path), "%s", store->binary_path);
    }
    bool ok = registry_store_load_orwa_file(store, store->binary_path, error, error_size);
    if (!ok) {
        if (g_registry_write_lock_fd == store->lock_fd) {
            g_registry_write_lock_fd = -1;
            g_registry_write_lock_path[0] = '\0';
        }
        registry_store_free(store);
    }
    return ok;
}

static bool registry_store_close(RegistryStore *store, bool commit, char *error, size_t error_size) {
    bool ok = true;
    if (commit) ok = registry_store_write_orwa_file(store, store->binary_path, error, error_size);
    if (g_registry_write_lock_fd == store->lock_fd) {
        g_registry_write_lock_fd = -1;
        g_registry_write_lock_path[0] = '\0';
    }
    registry_store_free(store);
    return ok;
}

static bool registry_store_open_user_readonly(RegistryStore *store, char *error, size_t error_size) {
    return registry_store_open_at(store, g_registry_database_path, false, error, error_size);
}

static bool registry_store_open_system_readonly(RegistryStore *store, char *error, size_t error_size) {
    return registry_store_open_at(store, g_system_registry_database_path, false, error, error_size);
}

static bool registry_store_open_user_readwrite(RegistryStore *store, char *error, size_t error_size) {
    return registry_store_open_at(store, g_registry_database_path, true, error, error_size);
}

static bool registry_store_only_contains_backend(const RegistryStore *store, const char *service_id) {
    if (!store || !service_id || !service_id[0]) return false;
    for (size_t i = 0; i < store->backend_count; i++) {
        if (strcmp(store->backends[i].service_id, service_id) != 0) return false;
    }
    for (size_t i = 0; i < store->frontend_count; i++) {
        if (strcmp(store->frontends[i].service_id, service_id) != 0) return false;
    }
    for (size_t i = 0; i < store->log_count; i++) {
        if (strcmp(store->logs[i].service_id, service_id) != 0) return false;
    }
    for (size_t i = 0; i < store->opener_count; i++) {
        if (strcmp(store->openers[i].service_id, service_id) != 0) return false;
    }
    for (size_t i = 0; i < store->layout_count; i++) {
        const char *url = store->layouts[i].url ? store->layouts[i].url : "";
        bool belongs_to_backend = false;
        for (size_t j = 0; j < store->frontend_count; j++) {
            const RegistryFrontendRecord *frontend = &store->frontends[j];
            if (strcmp(frontend->service_id, service_id) != 0) continue;
            const char *frontend_id = frontend->frontend_id && frontend->frontend_id[0] ? frontend->frontend_id : frontend->url;
            if (strcmp(url, frontend_id ? frontend_id : "") == 0 ||
                strcmp(url, frontend->url ? frontend->url : "") == 0) {
                belongs_to_backend = true;
                break;
            }
        }
        if (!belongs_to_backend) return false;
    }
    return store->content_type_count == 0;
}

static size_t collect_systemd_status_scope_via_systemctl(const char *scope,
                                                         SystemdStatusEntry *entries,
                                                         size_t capacity) {
    const char *scope_argument = (scope && strcmp(scope, "system") == 0) ? "--system" : "--user";
    char command[256];
    snprintf(command,
             sizeof(command),
             "timeout 2s systemctl %s list-units --all --type=service --type=socket --no-legend --no-pager --plain 2>/dev/null",
             scope_argument);

    FILE *pipe = popen(command, "r");
    if (!pipe) return 0;

    size_t count = 0;
    char line[2048];
    while (count < capacity && fgets(line, sizeof(line), pipe)) {
        char unit_name[256] = "";
        char load_state[32] = "";
        char active_state[32] = "";
        if (sscanf(line, "%255s %31s %31s", unit_name, load_state, active_state) != 3) {
            continue;
        }
        if (!safe_unit_name(unit_name)) {
            continue;
        }
        SystemdStatusEntry *entry = &entries[count++];
        snprintf(entry->unit_name, sizeof(entry->unit_name), "%s", unit_name);
        snprintf(entry->scope, sizeof(entry->scope), "%s", scope && scope[0] ? scope : "user");
        snprintf(entry->active_state, sizeof(entry->active_state), "%s", active_state);
    }
    pclose(pipe);
    return count;
}

static void replace_systemd_status_cache(const SystemdStatusEntry *entries, size_t count) {
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    if (count > MAX_SYSTEMD_STATUS_ENTRIES) count = MAX_SYSTEMD_STATUS_ENTRIES;
    if (entries && count > 0) {
        memcpy(g_systemd_status_cache.entries, entries, count * sizeof(SystemdStatusEntry));
    }
    g_systemd_status_cache.count = count;
    g_systemd_status_cache.refreshed_at_ms = monotonic_milliseconds();
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);
}

static void invalidate_systemd_status_cache(void) {
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    g_systemd_status_cache.refreshed_at_ms = 0;
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);
}

#ifndef __APPLE__
static void replace_systemd_status_scope_cache(const char *scope, const SystemdStatusEntry *entries, size_t count) {
    const char *normalized_scope = (scope && strcmp(scope, "system") == 0) ? "system" : "user";
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    size_t write_index = 0;
    for (size_t i = 0; i < g_systemd_status_cache.count; i++) {
        if (strcmp(g_systemd_status_cache.entries[i].scope, normalized_scope) == 0) {
            continue;
        }
        if (write_index != i) {
            g_systemd_status_cache.entries[write_index] = g_systemd_status_cache.entries[i];
        }
        write_index++;
    }
    size_t available = MAX_SYSTEMD_STATUS_ENTRIES - write_index;
    if (count > available) count = available;
    if (entries && count > 0) {
        memcpy(g_systemd_status_cache.entries + write_index, entries, count * sizeof(SystemdStatusEntry));
    }
    g_systemd_status_cache.count = write_index + count;
    g_systemd_status_cache.refreshed_at_ms = monotonic_milliseconds();
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);
}

typedef struct OuterSdBus OuterSdBus;
typedef struct OuterSdBusMessage OuterSdBusMessage;
typedef struct OuterSdBusSlot OuterSdBusSlot;

typedef struct {
    const char *name;
    const char *message;
    int _need_free;
} OuterSdBusError;

typedef int (*OuterSdBusMessageHandler)(OuterSdBusMessage *message, void *userdata, OuterSdBusError *ret_error);

typedef struct {
    void *handle;
    int (*open_user)(OuterSdBus **bus);
    int (*open_system)(OuterSdBus **bus);
    int (*call_method)(OuterSdBus *bus,
                       const char *destination,
                       const char *path,
                       const char *interface,
                       const char *member,
                       OuterSdBusError *ret_error,
                       OuterSdBusMessage **reply,
                       const char *types,
                       ...);
    int (*add_match)(OuterSdBus *bus,
                     OuterSdBusSlot **slot,
                     const char *match,
                     OuterSdBusMessageHandler callback,
                     void *userdata);
    int (*message_enter_container)(OuterSdBusMessage *message, char type, const char *contents);
    int (*message_exit_container)(OuterSdBusMessage *message);
    int (*message_read)(OuterSdBusMessage *message, const char *types, ...);
    OuterSdBusMessage *(*message_unref)(OuterSdBusMessage *message);
    void (*error_free)(OuterSdBusError *error);
    int (*process)(OuterSdBus *bus, OuterSdBusMessage **message);
    int (*wait)(OuterSdBus *bus, uint64_t timeout_usec);
    OuterSdBus *(*unref)(OuterSdBus *bus);
} SystemdBusApi;

typedef struct {
    const char *scope;
    OuterSdBus *bus;
    bool dirty;
    bool active;
} SystemdBusWatch;

static SystemdBusApi g_systemd_bus_api = {0};
static pthread_mutex_t g_systemd_status_watcher_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_systemd_status_watcher_started = false;
static bool g_systemd_status_event_cache_failed = false;

static bool load_systemd_bus_api(SystemdBusApi *api) {
    if (api->handle) return true;
    void *handle = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!handle) return false;
    api->handle = handle;
#define LOAD_SYSTEMD_BUS_SYMBOL(field, symbol) do { \
        *(void **)(&api->field) = dlsym(handle, symbol); \
        if (!api->field) { \
            dlclose(handle); \
            memset(api, 0, sizeof(*api)); \
            return false; \
        } \
    } while (0)
    LOAD_SYSTEMD_BUS_SYMBOL(open_user, "sd_bus_open_user");
    LOAD_SYSTEMD_BUS_SYMBOL(open_system, "sd_bus_open_system");
    LOAD_SYSTEMD_BUS_SYMBOL(call_method, "sd_bus_call_method");
    LOAD_SYSTEMD_BUS_SYMBOL(add_match, "sd_bus_add_match");
    LOAD_SYSTEMD_BUS_SYMBOL(message_enter_container, "sd_bus_message_enter_container");
    LOAD_SYSTEMD_BUS_SYMBOL(message_exit_container, "sd_bus_message_exit_container");
    LOAD_SYSTEMD_BUS_SYMBOL(message_read, "sd_bus_message_read");
    LOAD_SYSTEMD_BUS_SYMBOL(message_unref, "sd_bus_message_unref");
    LOAD_SYSTEMD_BUS_SYMBOL(error_free, "sd_bus_error_free");
    LOAD_SYSTEMD_BUS_SYMBOL(process, "sd_bus_process");
    LOAD_SYSTEMD_BUS_SYMBOL(wait, "sd_bus_wait");
    LOAD_SYSTEMD_BUS_SYMBOL(unref, "sd_bus_unref");
#undef LOAD_SYSTEMD_BUS_SYMBOL
    return true;
}

static void set_systemd_status_event_cache_failed(bool failed) {
    pthread_mutex_lock(&g_systemd_status_watcher_mutex);
    g_systemd_status_event_cache_failed = failed;
    pthread_mutex_unlock(&g_systemd_status_watcher_mutex);
}

static bool should_skip_systemd_status_polling(void) {
    bool skip = false;
    pthread_mutex_lock(&g_systemd_status_watcher_mutex);
    skip = g_systemd_status_watcher_started && !g_systemd_status_event_cache_failed;
    pthread_mutex_unlock(&g_systemd_status_watcher_mutex);
    return skip;
}

static int systemd_bus_signal_handler(OuterSdBusMessage *message, void *userdata, OuterSdBusError *ret_error) {
    (void)message;
    (void)ret_error;
    SystemdBusWatch *watch = userdata;
    if (watch) watch->dirty = true;
    return 0;
}

static bool subscribe_systemd_bus_watch(SystemdBusWatch *watch) {
    if (!watch || !watch->bus) return false;
    const char *manager_match =
        "type='signal',sender='org.freedesktop.systemd1',"
        "path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.systemd1.Manager'";
    const char *unit_properties_match =
        "type='signal',sender='org.freedesktop.systemd1',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "path_namespace='/org/freedesktop/systemd1/unit'";
    if (g_systemd_bus_api.add_match(watch->bus, NULL, manager_match, systemd_bus_signal_handler, watch) < 0) {
        return false;
    }
    if (g_systemd_bus_api.add_match(watch->bus, NULL, unit_properties_match, systemd_bus_signal_handler, watch) < 0) {
        return false;
    }

    OuterSdBusError error = {0};
    OuterSdBusMessage *reply = NULL;
    int result = g_systemd_bus_api.call_method(watch->bus,
                                               "org.freedesktop.systemd1",
                                               "/org/freedesktop/systemd1",
                                               "org.freedesktop.systemd1.Manager",
                                               "Subscribe",
                                               &error,
                                               &reply,
                                               "");
    if (reply) g_systemd_bus_api.message_unref(reply);
    g_systemd_bus_api.error_free(&error);
    return result >= 0;
}

static bool collect_systemd_status_scope_via_dbus(SystemdBusWatch *watch,
                                                  SystemdStatusEntry *entries,
                                                  size_t capacity,
                                                  size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!watch || !watch->bus || !entries) return false;

    OuterSdBusError error = {0};
    OuterSdBusMessage *reply = NULL;
    int result = g_systemd_bus_api.call_method(watch->bus,
                                               "org.freedesktop.systemd1",
                                               "/org/freedesktop/systemd1",
                                               "org.freedesktop.systemd1.Manager",
                                               "ListUnits",
                                               &error,
                                               &reply,
                                               "");
    if (result < 0 || !reply) {
        if (reply) g_systemd_bus_api.message_unref(reply);
        g_systemd_bus_api.error_free(&error);
        return false;
    }

    size_t count = 0;
    result = g_systemd_bus_api.message_enter_container(reply, 'a', "(ssssssouso)");
    while (result > 0) {
        result = g_systemd_bus_api.message_enter_container(reply, 'r', "ssssssouso");
        if (result <= 0) break;

        const char *unit_name = NULL;
        const char *description = NULL;
        const char *load_state = NULL;
        const char *active_state = NULL;
        const char *sub_state = NULL;
        const char *following = NULL;
        const char *object_path = NULL;
        uint32_t job_id = 0;
        const char *job_type = NULL;
        const char *job_path = NULL;
        int read_result = g_systemd_bus_api.message_read(reply,
                                                         "ssssssouso",
                                                         &unit_name,
                                                         &description,
                                                         &load_state,
                                                         &active_state,
                                                         &sub_state,
                                                         &following,
                                                         &object_path,
                                                         &job_id,
                                                         &job_type,
                                                         &job_path);
        (void)description;
        (void)load_state;
        (void)sub_state;
        (void)following;
        (void)object_path;
        (void)job_id;
        (void)job_type;
        (void)job_path;
        g_systemd_bus_api.message_exit_container(reply);
        if (read_result < 0) continue;
        if (!safe_unit_name(unit_name) || !active_state || !active_state[0]) continue;
        if (count >= capacity) continue;
        SystemdStatusEntry *entry = &entries[count++];
        snprintf(entry->unit_name, sizeof(entry->unit_name), "%s", unit_name);
        snprintf(entry->scope, sizeof(entry->scope), "%s", watch->scope);
        snprintf(entry->active_state, sizeof(entry->active_state), "%s", active_state);
    }
    g_systemd_bus_api.message_exit_container(reply);
    g_systemd_bus_api.message_unref(reply);
    g_systemd_bus_api.error_free(&error);
    if (out_count) *out_count = count;
    return true;
}

static bool refresh_systemd_bus_watch(SystemdBusWatch *watch, bool notify) {
    SystemdStatusEntry entries[MAX_SYSTEMD_STATUS_ENTRIES];
    size_t count = 0;
    if (!collect_systemd_status_scope_via_dbus(watch, entries, MAX_SYSTEMD_STATUS_ENTRIES, &count)) {
        return false;
    }
    replace_systemd_status_scope_cache(watch->scope, entries, count);
    if (notify) mark_backend_event_changed();
    return true;
}

static bool open_systemd_bus_watch(SystemdBusWatch *watch, const char *scope) {
    memset(watch, 0, sizeof(*watch));
    watch->scope = scope;
    int result = strcmp(scope, "system") == 0
        ? g_systemd_bus_api.open_system(&watch->bus)
        : g_systemd_bus_api.open_user(&watch->bus);
    if (result < 0 || !watch->bus) return false;
    if (!subscribe_systemd_bus_watch(watch)) {
        g_systemd_bus_api.unref(watch->bus);
        watch->bus = NULL;
        return false;
    }
    watch->active = refresh_systemd_bus_watch(watch, false);
    watch->dirty = false;
    if (!watch->active) {
        g_systemd_bus_api.unref(watch->bus);
        watch->bus = NULL;
    }
    return watch->active;
}

static void process_systemd_bus_watch(SystemdBusWatch *watch) {
    if (!watch || !watch->active || !watch->bus) return;
    while (!g_shutdown_requested) {
        int result = g_systemd_bus_api.process(watch->bus, NULL);
        if (result <= 0) break;
    }
    if (watch->dirty) {
        watch->dirty = false;
        if (!refresh_systemd_bus_watch(watch, true)) {
            watch->active = false;
            g_systemd_bus_api.unref(watch->bus);
            watch->bus = NULL;
            mark_backend_event_changed();
        }
    }
}

static void *systemd_status_watcher_main(void *context) {
    (void)context;
    if (!load_systemd_bus_api(&g_systemd_bus_api)) {
        set_systemd_status_event_cache_failed(true);
        return NULL;
    }

    SystemdBusWatch user_watch;
    SystemdBusWatch system_watch;
    bool user_active = open_systemd_bus_watch(&user_watch, "user");
    bool system_active = open_systemd_bus_watch(&system_watch, "system");
    bool any_active = user_active || system_active;
    set_systemd_status_event_cache_failed(!any_active);
    if (!any_active) return NULL;
    mark_backend_event_changed();

    while (!g_shutdown_requested && (user_watch.active || system_watch.active)) {
        process_systemd_bus_watch(&user_watch);
        process_systemd_bus_watch(&system_watch);
        if (user_watch.active && user_watch.bus) {
            g_systemd_bus_api.wait(user_watch.bus, 500000);
        } else if (system_watch.active && system_watch.bus) {
            g_systemd_bus_api.wait(system_watch.bus, 500000);
        } else {
            usleep(500000);
        }
    }

    if (user_watch.bus) g_systemd_bus_api.unref(user_watch.bus);
    if (system_watch.bus) g_systemd_bus_api.unref(system_watch.bus);
    set_systemd_status_event_cache_failed(true);
    return NULL;
}

static void start_systemd_status_watcher(void) {
    pthread_mutex_lock(&g_systemd_status_watcher_mutex);
    if (g_systemd_status_watcher_started) {
        pthread_mutex_unlock(&g_systemd_status_watcher_mutex);
        return;
    }
    g_systemd_status_watcher_started = true;
    g_systemd_status_event_cache_failed = false;
    pthread_mutex_unlock(&g_systemd_status_watcher_mutex);

    pthread_t thread;
    int result = pthread_create(&thread, NULL, systemd_status_watcher_main, NULL);
    if (result == 0) {
        pthread_detach(thread);
    } else {
        set_systemd_status_event_cache_failed(true);
    }
}
#endif

static void refresh_systemd_status_cache_if_needed(void) {
#ifndef __APPLE__
    if (should_skip_systemd_status_polling()) return;
#endif
    int64_t now = monotonic_milliseconds();
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    if (g_systemd_status_cache.refreshed_at_ms > 0 &&
        now - g_systemd_status_cache.refreshed_at_ms < SYSTEMD_STATUS_CACHE_TTL_MS) {
        pthread_mutex_unlock(&g_systemd_status_cache_mutex);
        return;
    }
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);

    SystemdStatusEntry entries[MAX_SYSTEMD_STATUS_ENTRIES];
    size_t count = direct_root_session_uses_system_scope()
        ? 0
        : collect_systemd_status_scope_via_systemctl("user", entries, MAX_SYSTEMD_STATUS_ENTRIES);
#ifndef __APPLE__
    count += collect_systemd_status_scope_via_systemctl("system",
                                                        entries + count,
                                                        MAX_SYSTEMD_STATUS_ENTRIES - count);
#endif
    replace_systemd_status_cache(entries, count);
}

static bool cached_systemd_active_state(const char *unit_name, const char *scope, char *out, size_t out_size) {
    if (out_size > 0) out[0] = '\0';
    if (!safe_unit_name(unit_name) || !out || out_size == 0) return false;
    const char *normalized_scope = (scope && strcmp(scope, "system") == 0) ? "system" : "user";
    refresh_systemd_status_cache_if_needed();
    bool found = false;
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    for (size_t i = 0; i < g_systemd_status_cache.count; i++) {
        SystemdStatusEntry *entry = &g_systemd_status_cache.entries[i];
        if (strcmp(entry->scope, normalized_scope) == 0 &&
            strcmp(entry->unit_name, unit_name) == 0) {
            snprintf(out, out_size, "%s", entry->active_state);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);
    return found;
}

static void systemd_status(const char *unit_name, const char *scope, char *out, size_t out_size) {
    if (!safe_unit_name(unit_name)) {
        snprintf(out, out_size, "unknown");
        return;
    }
    char active_state[32] = "";
    bool has_active_state = cached_systemd_active_state(unit_name, scope, active_state, sizeof(active_state));
    if (has_active_state && strcmp(active_state, "active") == 0) {
        snprintf(out, out_size, "running");
    } else if (!has_active_state ||
               strcmp(active_state, "inactive") == 0 ||
               strcmp(active_state, "failed") == 0) {
        for (size_t i = 0; i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
            if (!kBundledApps[i].socket_activated || strcmp(kBundledApps[i].unit_name, unit_name) != 0) {
                continue;
            }
            char socket_unit[256];
            systemd_socket_unit_name(unit_name, socket_unit, sizeof(socket_unit));
            if (safe_unit_name(socket_unit)) {
                char socket_active_state[32] = "";
                if (cached_systemd_active_state(socket_unit, scope, socket_active_state, sizeof(socket_active_state)) &&
                    strcmp(socket_active_state, "active") == 0) {
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

static uint64_t systemd_status_state_token(void) {
    uint64_t token = 1;
#ifndef __APPLE__
    refresh_systemd_status_cache_if_needed();
    pthread_mutex_lock(&g_systemd_status_cache_mutex);
    for (size_t i = 0; i < g_systemd_status_cache.count; i++) {
        SystemdStatusEntry *entry = &g_systemd_status_cache.entries[i];
        token = mix_u64(token, string_state_token(entry->unit_name));
        token = mix_u64(token, string_state_token(entry->scope));
        token = mix_u64(token, string_state_token(entry->active_state));
    }
    pthread_mutex_unlock(&g_systemd_status_cache_mutex);
#endif
    return token;
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
        snprintf(out, out_size, "available");
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
        char command[2048];
        snprintf(command,
                 sizeof(command),
                 "launchctl kill TERM %s 2>&1 || true; "
                 "i=0; "
                 "while [ $i -lt 20 ]; do "
                 "  launchctl print %s 2>/dev/null | grep -q 'state = running' || exit 0; "
                 "  sleep 0.1; "
                 "  i=$((i + 1)); "
                 "done; "
                 "launchctl kill KILL %s 2>&1 || true; "
                 "i=0; "
                 "while [ $i -lt 20 ]; do "
                 "  launchctl print %s 2>/dev/null | grep -q 'state = running' || exit 0; "
                 "  sleep 0.1; "
                 "  i=$((i + 1)); "
                 "done; "
                 "echo 'Launchd service is still running.'; "
                 "exit 1",
                 quoted_target,
                 quoted_target,
                 quoted_target,
                 quoted_target);
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

    char command[4096];
    if (strcmp(operation, "stop") == 0) {
        snprintf(command,
                 sizeof(command),
                 "launchctl kill TERM %s 2>&1 || true; "
                 "i=0; "
                 "while [ $i -lt 20 ]; do "
                 "  launchctl print %s 2>/dev/null | grep -q 'state = running' || exit 0; "
                 "  sleep 0.1; "
                 "  i=$((i + 1)); "
                 "done; "
                 "launchctl kill KILL %s 2>&1 || true; "
                 "i=0; "
                 "while [ $i -lt 20 ]; do "
                 "  launchctl print %s 2>/dev/null | grep -q 'state = running' || exit 0; "
                 "  sleep 0.1; "
                 "  i=$((i + 1)); "
                 "done; "
                 "echo 'Launchd service is still running.'; "
                 "exit 1",
                 quoted_target,
                 quoted_target,
                 quoted_target,
                 quoted_target);
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

static bool run_systemd_operation_with_options(const char *unit_name,
                                               const char *scope,
                                               const char *operation,
                                               const char *sudo_password,
                                               bool *needs_password,
                                               char *message,
                                               size_t message_size,
                                               bool no_block) {
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
    snprintf(command,
             sizeof(command),
             "systemctl %s%s %s %s 2>&1",
             scope_argument,
             no_block ? " --no-block" : "",
             operation,
             quoted_unit);

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

static bool run_systemd_operation(const char *unit_name,
                                  const char *scope,
                                  const char *operation,
                                  const char *sudo_password,
                                  bool *needs_password,
                                  char *message,
                                  size_t message_size) {
    return run_systemd_operation_with_options(unit_name,
                                              scope,
                                              operation,
                                              sudo_password,
                                              needs_password,
                                              message,
                                              message_size,
                                              false);
}

static bool lookup_systemd_backend(const RegistryStore *store,
                                   const char *service_id,
                                   char *unit_name,
                                   size_t unit_name_size,
                                   char *scope,
                                   size_t scope_size,
                                   const char *default_scope) {
    const RegistryBackendRecord *record = registry_store_find_backend_const(store, service_id);
    if (!record || !record->unit_name || !record->unit_name[0]) return false;
#ifdef __APPLE__
    if (record->unit_path && record->unit_path[0]) return false;
#endif
    snprintf(unit_name, unit_name_size, "%s", record->unit_name);
    snprintf(scope, scope_size, "%s", default_scope && default_scope[0] ? default_scope : "user");
    return true;
}

static bool lookup_systemd_backend_any(const char *service_id,
                                       char *unit_name,
                                       size_t unit_name_size,
                                       char *scope,
                                       size_t scope_size) {
    char error[512] = "";
    RegistryStore store;
    if (registry_store_open_user_readonly(&store, error, sizeof(error))) {
        bool found = lookup_systemd_backend(&store, service_id, unit_name, unit_name_size, scope, scope_size, "user");
        registry_store_free(&store);
        if (found) return true;
    }

    if (registry_store_open_system_readonly(&store, error, sizeof(error))) {
        bool found = lookup_systemd_backend(&store, service_id, unit_name, unit_name_size, scope, scope_size, "system");
        registry_store_free(&store);
        if (found) return true;
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
        RegistryStore store;
        if (registry_store_open_user_readonly(&store, error, sizeof(error))) {
            bool found = lookup_systemd_backend(&store, service_id, unit_name, unit_name_size, scope, scope_size, "user");
            registry_store_free(&store);
            if (found) return true;
        }
    }

    if (!wants_user) {
        RegistryStore store;
        if (registry_store_open_system_readonly(&store, error, sizeof(error))) {
            bool found = lookup_systemd_backend(&store, service_id, unit_name, unit_name_size, scope, scope_size, "system");
            registry_store_free(&store);
            if (found) return true;
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

    RegistryStore store;
    if (!registry_store_open_system_readonly(&store, error, error_size)) return false;
    if (exists) *exists = registry_store_find_backend_const(&store, service_id) != NULL;
    registry_store_free(&store);
    return true;
}
#endif

#ifdef __APPLE__
static bool lookup_launchd_backend(const RegistryStore *store,
                                   const char *service_id,
                                   char *plist_path,
                                   size_t plist_path_size,
                                   int *owns_plist) {
    const RegistryBackendRecord *record = registry_store_find_backend_const(store, service_id);
    if (!record || !record->unit_path || !record->unit_path[0]) return false;
    snprintf(plist_path, plist_path_size, "%s", record->unit_path);
    if (owns_plist) *owns_plist = record->owns_unit ? 1 : 0;
    return true;
}

static bool lookup_launchd_backend_any(const char *service_id,
                                       char *plist_path,
                                       size_t plist_path_size,
                                       int *owns_plist) {
    char error[512] = "";
    RegistryStore store;
    if (registry_store_open_user_readonly(&store, error, sizeof(error))) {
        bool found = lookup_launchd_backend(&store, service_id, plist_path, plist_path_size, owns_plist);
        registry_store_free(&store);
        if (found) return true;
    }
    if (registry_store_open_system_readonly(&store, error, sizeof(error))) {
        bool found = lookup_launchd_backend(&store, service_id, plist_path, plist_path_size, owns_plist);
        registry_store_free(&store);
        if (found) return true;
    }
    return false;
}

static bool lookup_launchd_backend_any_for_scope(const char *service_id,
                                                 const char *requested_scope,
                                                 char *plist_path,
                                                 size_t plist_path_size,
                                                 int *owns_plist) {
    bool wants_system = requested_scope && strcmp(requested_scope, "system") == 0;
    bool wants_user = requested_scope && strcmp(requested_scope, "user") == 0;
    char error[512] = "";

    if (!wants_system) {
        RegistryStore store;
        if (registry_store_open_user_readonly(&store, error, sizeof(error))) {
            bool found = lookup_launchd_backend(&store, service_id, plist_path, plist_path_size, owns_plist);
            registry_store_free(&store);
            if (found) return true;
        }
    }

    if (!wants_user) {
        RegistryStore store;
        if (registry_store_open_system_readonly(&store, error, sizeof(error))) {
            bool found = lookup_launchd_backend(&store, service_id, plist_path, plist_path_size, owns_plist);
            registry_store_free(&store);
            if (found) return true;
        }
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
#define BACKEND_FLAG_MENU_BAR_VISIBILITY_ENABLED 0x200u
#define LOG_FILE_FLAG_READABLE 0x01u
#define FILE_PICKER_FLAG_IS_DIRECTORY 0x01u
#define ACTION_FLAG_OK 0x01u
#define ACTION_FLAG_NEEDS_PASSWORD 0x02u

static bool root_outershell_migration_pending(void);
static bool installed_home_screen_version(char *out, size_t out_size);
static bool accept_home_screen_available_version(const char *provided_version, char *out, size_t out_size, char *message, size_t message_size);
static void mark_update_check_completed(void);
static int compare_versions(const char *installed, const char *available);
static bool home_screen_update_available(char *available, size_t available_size);
static bool run_home_screen_install_script(const char *subcommand,
                                           const char *script_path,
                                           const char *archive_path,
                                           bool remove_user_state,
                                           char *message,
                                           size_t message_size);
static bool uninstall_local_home_screen(const char *sudo_password,
                                        bool *needs_password,
                                        bool remove_user_state,
                                        char *message,
                                        size_t message_size);

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

static bool lookup_frontend_layout(const RegistryStore *store,
                                   const char *frontend_id,
                                   const char *url,
                                   char *list,
                                   size_t list_size,
                                   bool *found) {
    if (found) *found = false;
    if (list && list_size) list[0] = '\0';
    if (!store) return true;
    const RegistryFrontendLayoutRecord *record = NULL;
    if (frontend_id && frontend_id[0]) record = registry_store_find_layout_const(store, frontend_id);
    if (!record && url && url[0]) record = registry_store_find_layout_const(store, url);
    if (record) {
        if (found) *found = true;
        snprintf(list, list_size, "%s", record->list ? record->list : "");
    }
    if (!record && (!frontend_id || !frontend_id[0]) && url && url[0]) {
        size_t length = strlen(url);
        if (length > 1 && url[length - 1] == '/') {
            char without_trailing_slash[PATH_MAX];
            if (length < sizeof(without_trailing_slash)) {
                memcpy(without_trailing_slash, url, length - 1);
                without_trailing_slash[length - 1] = '\0';
                record = registry_store_find_layout_const(store, without_trailing_slash);
                if (record) {
                    if (found) *found = true;
                    snprintf(list, list_size, "%s", record->list ? record->list : "");
                }
            }
        }
    }
    return true;
}

static bool build_frontends_array_payload(const RegistryStore *database,
                                          const RegistryStore *layout_database,
                                          const char *service_id,
                                          bool is_running,
                                          StringBuilder *out) {
    BinaryPayloadList list = {0};
    bool ok = true;
    for (size_t i = 0; ok && database && i < database->frontend_count; i++) {
        const RegistryFrontendRecord *record = &database->frontends[i];
        if (strcmp(record->service_id ? record->service_id : "", service_id ? service_id : "") != 0) continue;
        StringBuilder payload = {0};
        const char *url = record->url ? record->url : "";
        const char *frontend_id = record->frontend_id ? record->frontend_id : "";
        const char *suggested_list = record->list ? record->list : "";
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
        ok = build_frontend_payload(record->display_name,
                                    frontend_id,
                                    url,
                                    record->port,
                                    record->socket_path,
                                    record->icon_path,
                                    has_layout ? layout_list : suggested_list,
                                    is_running,
                                    &payload) &&
             binary_payload_list_append(&list, &payload);
        if (!ok) free(payload.data);
    }
    if (ok) ok = binary_build_payload_array(&list, out);
    binary_payload_list_free(&list);
    return ok;
}

static bool build_log_files_array_payload(const RegistryStore *database, const char *service_id, StringBuilder *out) {
    BinaryPayloadList list = {0};
    bool ok = true;
    int index = 0;
    for (size_t i = 0; ok && database && i < database->log_count; i++) {
        const RegistryLogFileRecord *record = &database->logs[i];
        if (strcmp(record->service_id ? record->service_id : "", service_id ? service_id : "") != 0) continue;
        StringBuilder payload = {0};
        ok = build_log_file_payload(service_id, record->path, index, &payload) &&
             binary_payload_list_append(&list, &payload);
        if (!ok) free(payload.data);
        index++;
    }
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

static bool append_registered_backend_payloads(const RegistryStore *database,
                                               const RegistryStore *layout_database,
                                               BinaryPayloadList *payloads,
                                               bool *bundled_installed,
                                               size_t bundled_installed_count,
                                               const char *registry_scope) {
    bool ok = true;
    for (size_t i = 0; ok && database && i < database->backend_count; i++) {
        const RegistryBackendRecord *record = &database->backends[i];
        const char *service_id = record->service_id ? record->service_id : "";
        const char *display_name = record->display_name ? record->display_name : "";
        const char *service_unit = record->unit_name ? record->unit_name : "";
        const char *plist_path = record->unit_path ? record->unit_path : "";
        int owns_plist = record->owns_unit ? 1 : 0;
        char effective_service_scope[32];
        snprintf(effective_service_scope, sizeof(effective_service_scope), "%s", registry_scope && registry_scope[0] ? registry_scope : "user");
#ifndef __APPLE__
        if (registry_scope && strcmp(registry_scope, "system") == 0) {
            snprintf(effective_service_scope, sizeof(effective_service_scope), "system");
        }
#endif
        bool is_self = is_home_screen_service_id(service_id);
        const BundledAppDefinition *bundled_app = bundled_app_for_service_id(service_id);

        char status[32] = "unknown";
        bool has_launchd_unit = plist_path[0];
        bool has_systemd_unit = service_unit[0];
#ifdef __APPLE__
        has_systemd_unit = false;
#endif
        if (bundled_app && (has_launchd_unit || has_systemd_unit)) {
            size_t bundled_index = (size_t)(bundled_app - kBundledApps);
            if (bundled_index < bundled_installed_count) bundled_installed[bundled_index] = true;
        }
#ifdef __APPLE__
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
            if (get_agent_menu_bar_visibility()) flags |= BACKEND_FLAG_MENU_BAR_VISIBILITY_ENABLED;
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
        bool service_is_running = strcmp(status, "running") == 0;
        ok = build_frontends_array_payload(database, layout_database ? layout_database : database, service_id, service_is_running, &frontends) &&
             build_log_files_array_payload(database, service_id, &logs) &&
             build_backend_payload(service_id, display_name, service_unit, service_unit_path,
                                   effective_service_scope, status, flags, "",
                                   plist_path, &frontends, &logs, &payload) &&
             binary_payload_list_append(payloads, &payload);
        free(frontends.data);
        free(logs.data);
        if (!ok) free(payload.data);
    }
    return ok;
}

static bool append_root_migration_backend_payload(BinaryPayloadList *payloads) {
    if (!root_outershell_migration_pending()) return true;
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
                                    direct_root_session_uses_system_scope() ? "system" : "user",
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

static bool file_contains_any_legacy_outershell_text(const char *path) {
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

static bool directory_contains_legacy_outershell_text(const char *directory, bool recursive) {
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
            found = recursive && directory_contains_legacy_outershell_text(path, true);
        } else if (S_ISREG(st.st_mode)) {
            found = file_contains_any_legacy_outershell_text(path);
        }
    }
    closedir(dir);
    return found;
}

static bool root_outershell_migration_pending(void) {
#ifdef __APPLE__
    struct stat st;
    return stat("/Library/dev.outergroup.OuterLoop/registry.sqlite3", &st) == 0 ||
           stat("/Library/dev.outergroup.OuterLoop", &st) == 0 ||
           directory_contains_legacy_outershell_text("/Library/LaunchDaemons", false);
#else
    struct stat st;
    return stat("/var/lib/outergroup/outeragent/registry.sqlite3", &st) == 0 ||
           stat("/var/lib/outergroup/outeragent", &st) == 0 ||
           directory_contains_legacy_outershell_text("/opt/outergroup", true) ||
           directory_contains_legacy_outershell_text("/etc/systemd/system", false);
#endif
}

static void send_backends_response(int fd) {
    BinaryPayloadList payloads = {0};
    char user_error[512] = "";
    char system_error[512] = "";
    RegistryStore user_database;
    RegistryStore system_database;
    bool have_user_database = registry_store_open_user_readonly(&user_database, user_error, sizeof(user_error));
    bool user_registry_is_system_registry = strcmp(g_registry_database_path, g_system_registry_database_path) == 0;
    bool have_system_database = user_registry_is_system_registry
        ? false
        : registry_store_open_system_readonly(&system_database, system_error, sizeof(system_error));

    bool ok = true;
    const char *error = "";
    if (!have_user_database && !have_system_database) {
        error = user_error[0] ? user_error : system_error;
    }
    bool bundled_installed[sizeof(kBundledApps) / sizeof(kBundledApps[0])] = {0};
    const char *primary_registry_scope = direct_root_session_uses_system_scope() ? "system" : "user";
    if (have_user_database) {
        ok = ok && append_registered_backend_payloads(&user_database, &user_database, &payloads, bundled_installed,
                                                       sizeof(bundled_installed) / sizeof(bundled_installed[0]),
                                                       primary_registry_scope);
    }
    if (have_system_database) {
        ok = ok && append_registered_backend_payloads(&system_database, have_user_database ? &user_database : NULL, &payloads, bundled_installed,
                                                       sizeof(bundled_installed) / sizeof(bundled_installed[0]),
                                                       "system");
    }
    ok = ok && append_root_migration_backend_payload(&payloads);

    for (size_t i = 0; ok && i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        if (!bundled_app_is_available_on_platform(&kBundledApps[i])) continue;
        if (bundled_installed[i]) continue;
        ok = ok && append_bundled_backend_placeholder_payload(&payloads, &kBundledApps[i]);
    }
    if (have_user_database) registry_store_free(&user_database);
    if (have_system_database) registry_store_free(&system_database);

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

static bool resolve_log_path(const RegistryStore *database, const char *service_id, int log_index,
                             char *path, size_t path_size) {
    int index = 0;
    for (size_t i = 0; database && i < database->log_count; i++) {
        const RegistryLogFileRecord *record = &database->logs[i];
        if (strcmp(record->service_id ? record->service_id : "", service_id ? service_id : "") != 0) continue;
        if (index == (log_index < 0 ? 0 : log_index)) {
            snprintf(path, path_size, "%s", record->path ? record->path : "");
            return true;
        }
        index++;
    }
    return false;
}

static bool resolve_log_path_any(const char *service_id, int log_index, char *path, size_t path_size, char *error, size_t error_size) {
    RegistryStore database;
    if (registry_store_open_user_readonly(&database, error, error_size)) {
        bool found = resolve_log_path(&database, service_id, log_index, path, path_size);
        registry_store_free(&database);
        if (found) return true;
    }

    if (registry_store_open_system_readonly(&database, error, error_size)) {
        bool found = resolve_log_path(&database, service_id, log_index, path, path_size);
        registry_store_free(&database);
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
    pthread_mutex_lock(&g_backend_event_mutex);
    uint64_t version = g_backend_event_sequence;
    pthread_mutex_unlock(&g_backend_event_mutex);
    version = mix_u64(version, registry_file_state_token(g_registry_database_path));
    version = mix_u64(version, registry_file_state_token(g_system_registry_database_path));
    version = mix_u64(version, systemd_status_state_token());
    return version ? version : 1;
}

static uint64_t current_log_event_version(const char *service_id, int log_index) {
    if (!service_id || !service_id[0]) return 0;
    char error[512] = "";
    char raw_path[PATH_MAX] = "";
    if (!resolve_log_path_any(service_id, log_index, raw_path, sizeof(raw_path), error, sizeof(error))) {
        return mix_u64(current_backends_event_version(), 0);
    }
    return current_log_path_event_version(raw_path);
}

static uint64_t current_log_path_event_version(const char *raw_path) {
    if (!raw_path || !raw_path[0]) return 0;
    char path[PATH_MAX];
    expand_tilde_path(raw_path, path, sizeof(path));
    return mix_u64(current_backends_event_version(), file_state_token(path));
}

static void mark_backend_event_changed(void) {
    pthread_mutex_lock(&g_backend_event_mutex);
    g_backend_event_sequence++;
    if (g_backend_event_sequence == 0) g_backend_event_sequence = 1;
    pthread_mutex_unlock(&g_backend_event_mutex);
    if (g_backend_event_changed_callback) {
        g_backend_event_changed_callback();
    }
}

static bool try_connect_tcp_port(int port) {
    if (port <= 0 || port > 65535) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    bool ok = false;
    if (set_fd_nonblocking(fd, true)) {
        int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (result == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
            if (poll(&pfd, 1, 250) > 0 && (pfd.revents & POLLOUT)) {
                int socket_error = 0;
                socklen_t socket_error_size = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 &&
                    socket_error == 0) {
                    ok = true;
                }
            }
        }
    }
    close(fd);
    return ok;
}

static bool try_connect_unix_socket(const char *socket_path) {
    if (!socket_path || !socket_path[0] ||
        strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    bool ok = false;
    if (set_fd_nonblocking(fd, true)) {
        int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (result == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
            if (poll(&pfd, 1, 250) > 0 && (pfd.revents & POLLOUT)) {
                int socket_error = 0;
                socklen_t socket_error_size = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 &&
                    socket_error == 0) {
                    ok = true;
                }
            }
        }
    }
    close(fd);
    return ok;
}

static bool frontend_endpoint_is_ready(int port, const char *socket_path, bool system_scope) {
    if (socket_path && socket_path[0]) {
        if (system_scope) {
            return true;
        }
        return try_connect_unix_socket(socket_path);
    }
    if (port > 0) {
        return try_connect_tcp_port(port);
    }
    return false;
}

static bool any_frontend_endpoint_ready(const RegistryStore *database, const char *service_id, bool system_scope, bool *has_endpoint) {
    *has_endpoint = false;
    bool ready = false;
    for (size_t i = 0; database && i < database->frontend_count; i++) {
        const RegistryFrontendRecord *record = &database->frontends[i];
        if (strcmp(record->service_id ? record->service_id : "", service_id ? service_id : "") != 0) continue;
        int port = record->port;
        const char *socket_path = record->socket_path ? record->socket_path : "";
        if (port <= 0 && (!socket_path || !socket_path[0])) {
            continue;
        }
        *has_endpoint = true;
        if (frontend_endpoint_is_ready(port, socket_path, system_scope)) {
            ready = true;
            break;
        }
    }
    return ready;
}

static bool wait_for_frontend_endpoint_ready(const char *service_id,
                                             const char *scope,
                                             char *message,
                                             size_t message_size) {
    const bool system_scope = scope && strcmp(scope, "system") == 0;
    const int64_t deadline = monotonic_milliseconds() + 15000;
    int sleep_us = 100000;
    bool saw_endpoint = false;

    while (monotonic_milliseconds() < deadline) {
        char error[512] = "";
        RegistryStore database;
        bool have_database = system_scope
            ? registry_store_open_system_readonly(&database, error, sizeof(error))
            : registry_store_open_user_readonly(&database, error, sizeof(error));
        if (have_database) {
            bool has_endpoint = false;
            bool ready = any_frontend_endpoint_ready(&database, service_id, system_scope, &has_endpoint);
            registry_store_free(&database);
            if (has_endpoint) saw_endpoint = true;
            if (ready) {
                snprintf(message, message_size, "Started and frontend endpoint is ready.");
                return true;
            }
        }

        usleep((useconds_t)sleep_us);
        if (sleep_us < 500000) sleep_us += 100000;
    }

    snprintf(message,
             message_size,
             saw_endpoint
                 ? "Started, but timed out waiting for the frontend endpoint."
                 : "Started, but no frontend endpoint is registered yet.");
    return false;
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

static bool install_bundled_app(const BundledAppDefinition *app,
                                const char *scope,
                                const char *stage_root,
                                const char *sudo_password,
                                bool *needs_password,
                                char *message,
                                size_t message_size);
#ifdef __APPLE__
static bool install_bundled_app_user_launchagent_for_system_payload(const BundledAppDefinition *app,
                                                                    char *message,
                                                                    size_t message_size);
#endif
static bool uninstall_backend(const char *service_id, const char *sudo_password, bool *needs_password, char *message, size_t message_size);
static bool run_root_outershell_migration(const char *sudo_password, bool *needs_password, char *message, size_t message_size);

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
    RegistryStore database;
    if (!registry_store_open_at(&database, database_path, false, error, error_size)) return false;
    bool use_frontend_id = frontend_id && frontend_id[0];
    for (size_t i = 0; i < database.frontend_count; i++) {
        RegistryFrontendRecord *record = &database.frontends[i];
        if (strcmp(record->service_id ? record->service_id : "", service_id ? service_id : "") != 0) continue;
        const char *key = use_frontend_id ? record->frontend_id : record->url;
        const char *wanted = use_frontend_id ? frontend_id : frontend_url;
        if (strcmp(key ? key : "", wanted ? wanted : "") == 0) {
            if (found) *found = true;
            break;
        }
    }
    registry_store_free(&database);
    return true;
}

static bool update_frontend_layout_in_user_registry(const char *frontend_id,
                                                    const char *frontend_url,
                                                    const char *list_name,
                                                    char *error,
                                                    size_t error_size) {
    RegistryStore database;
    if (!registry_store_open_user_readwrite(&database, error, error_size)) return false;
    bool ok = registry_store_upsert_layout(&database,
                                           (frontend_url && frontend_url[0]) ? frontend_url : (frontend_id ? frontend_id : ""),
                                           list_name ? list_name : "");
    if (!ok) snprintf(error, error_size, "Out of memory.");
    return registry_store_close(&database, ok, error, error_size) && ok;
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

static void send_control_response(int fd, const char *query, const char *body) {
    char service_id[PATH_MAX] = "";
    char operation[32] = "";
    char requested_scope[32] = "";
    char sudo_password[PATH_MAX] = "";
    char bundled_stage_root[PATH_MAX] = "";
    char available_version[128] = "";
    char installer_script_path[PATH_MAX] = "";
    char installer_archive_path[PATH_MAX] = "";
    if (!query_value_any(query, body, "serviceID", service_id, sizeof(service_id)) ||
        !query_value_any(query, body, "operation", operation, sizeof(operation))) {
        send_action_response(fd, 400, false, "Missing serviceID or operation.");
        return;
    }
    query_value_any(query, body, "scope", requested_scope, sizeof(requested_scope));
    query_value_any(query, body, "sudoPassword", sudo_password, sizeof(sudo_password));
    query_value_any(query, body, "bundledStageRoot", bundled_stage_root, sizeof(bundled_stage_root));
    query_value_any(query, body, "availableVersion", available_version, sizeof(available_version));
    query_value_any(query, body, "installerScriptPath", installer_script_path, sizeof(installer_script_path));
    query_value_any(query, body, "installerArchivePath", installer_archive_path, sizeof(installer_archive_path));
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
        bool ok = run_root_outershell_migration(sudo_password, &needs_password, message, sizeof(message));
        log_event("%s root outershell migration: %s", ok ? "Completed" : "Failed", message);
        if (ok) mark_backend_event_changed();
        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
        return;
    }

    if (is_home_screen_service_id(service_id)) {
        char message[4096] = "";
        if (strcmp(operation, "showMenuBarWhenRunning") == 0 ||
            strcmp(operation, "hideMenuBarWhenRunning") == 0) {
            bool enabled = strcmp(operation, "showMenuBarWhenRunning") == 0;
            if (set_agent_menu_bar_visibility(enabled)) {
                mark_backend_event_changed();
                snprintf(message,
                         sizeof(message),
                         enabled ? "Outer Shell will show in the menu bar when backends are running."
                                 : "Outer Shell will not show in the menu bar when backends are running.");
                send_action_response(fd, 200, true, message);
                return;
            }
            send_action_response(fd, 400, false, "Menu bar visibility is only available on macOS.");
            return;
        }
        if (strcmp(operation, "checkUpdate") == 0 || strcmp(operation, "checkOuterShellUpdate") == 0) {
            char installed[128] = "";
            char latest[128] = "";
            installed_home_screen_version(installed, sizeof(installed));
            bool ok = accept_home_screen_available_version(available_version, latest, sizeof(latest), message, sizeof(message));
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
            bool remove_user_state = false;
            if (strcmp(installer_command, "uninstall") == 0) {
                RegistryStore database;
                char registry_error[1024] = "";
                if (registry_store_open_user_readonly(&database, registry_error, sizeof(registry_error))) {
                    remove_user_state = registry_store_only_contains_backend(&database, service_id);
                    registry_store_free(&database);
                } else {
                    log_event("Could not inspect registry before Outer Shell uninstall: %s",
                              registry_error[0] ? registry_error : "unknown error");
                }
            }
            bool ok = false;
#ifdef __APPLE__
            bool needs_password = false;
            if (strcmp(installer_command, "uninstall") == 0) {
                ok = uninstall_local_home_screen(sudo_password,
                                                 &needs_password,
                                                 remove_user_state,
                                                 message,
                                                 sizeof(message));
                log_event("%s Outer Shell %s: %s", ok ? "Completed" : "Failed", installer_command, message);
                if (ok) mark_backend_event_changed();
                send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
                return;
            } else {
#else
            if (strcmp(installer_command, "uninstall") == 0) {
                unlink_advertised_home_screen_socket();
            }
#endif
                ok = run_home_screen_install_script(installer_command,
                                                    installer_script_path,
                                                    installer_archive_path,
                                                    remove_user_state,
                                                    message,
                                                    sizeof(message));
#ifdef __APPLE__
            }
#endif
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
        const char *scope = direct_root_session_uses_system_scope()
            ? "system"
            : ((strcmp(operation, "runRoot") == 0 || strcmp(operation, "installRoot") == 0) ? "system" : "user");
        if (strcmp(scope, "system") == 0 && !app->supports_root) {
            send_action_response(fd, 400, false, "This app does not support running as root.");
            return;
        }
        if (strcmp(scope, "user") == 0 && app->root_only) {
            send_action_response(fd, 400, false, "This app can only run as root.");
            return;
        }
        bool ok = install_bundled_app(app, scope, bundled_stage_root, sudo_password, &needs_password, message, sizeof(message));
#ifdef __APPLE__
        if (ok && strcmp(scope, "system") == 0 && !app->root_only) {
            char user_message[4096] = "";
            bool user_ok = install_bundled_app_user_launchagent_for_system_payload(app, user_message, sizeof(user_message));
            log_event("%s user LaunchAgent for app %s after root install: %s",
                      user_ok ? "Installed" : "Failed to install",
                      app->service_id,
                      user_message);
            if (user_ok) {
                snprintf(message, sizeof(message), "Installed %s for user and root.", app->display_name);
            } else {
                snprintf(message, sizeof(message), "Installed %s as root, but failed to install its user LaunchAgent: %s",
                         app->display_name,
                         user_message);
                ok = false;
            }
        }
#endif
	        log_event("%s app %s as %s: %s",
	                  ok ? "Installed" : "Failed to install",
	                  app->service_id,
	                  scope,
	                  message);
	        if (ok) {
	            cleanup_bundled_app_cache_if_stage_root_is_download_cache(app, bundled_stage_root);
	            mark_backend_event_changed();
	        }
	        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
	        return;
	    }

    if (strcmp(operation, "addRootSupport") == 0 || strcmp(operation, "removeRootSupport") == 0) {
        const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
        if (!app || !app->supports_root) {
            send_action_response(fd, 404, false, "This app does not support root support.");
            return;
        }
        if (direct_root_session_uses_system_scope()) {
            send_action_response(fd, 400, false, "Root support is implicit when connected as root.");
            return;
        }
        char message[4096] = "";
        bool needs_password = false;
        bool ok = false;
        if (strcmp(operation, "addRootSupport") == 0) {
            ok = install_bundled_app(app, "system", bundled_stage_root, sudo_password, &needs_password, message, sizeof(message));
#ifdef __APPLE__
            if (ok && !app->root_only) {
                char user_message[4096] = "";
                bool user_ok = install_bundled_app_user_launchagent_for_system_payload(app, user_message, sizeof(user_message));
                log_event("%s user LaunchAgent for app %s after adding root support: %s",
                          user_ok ? "Installed" : "Failed to install",
                          app->service_id,
                          user_message);
                if (user_ok) {
                    snprintf(message, sizeof(message), "Added root support for %s.", app->display_name);
                } else {
                    snprintf(message, sizeof(message), "Added root support for %s, but failed to update its user LaunchAgent: %s",
                             app->display_name,
                             user_message);
                    ok = false;
                }
            }
#endif
        } else {
#ifndef __APPLE__
            if (!app->root_only) {
                char user_unit[256] = "";
                char user_scope[32] = "user";
                RegistryStore database;
                bool has_user_install = false;
                if (registry_store_open_user_readonly(&database, message, sizeof(message))) {
                    has_user_install = lookup_systemd_backend(&database, service_id, user_unit, sizeof(user_unit), user_scope, sizeof(user_scope), "user");
                    registry_store_free(&database);
                }
                if (has_user_install) {
                    ok = install_bundled_app(app, "user", bundled_stage_root, NULL, NULL, message, sizeof(message));
                    if (!ok) {
                        log_event("Failed to restore user install before removing root support for %s: %s", service_id, message);
                        send_action_response(fd, 500, false, message);
                        return;
                    }
                }
            }
#endif
            ok = remove_bundled_root_support(app, sudo_password, &needs_password, message, sizeof(message));
        }
	        log_event("%s root support for app %s: %s",
	                  ok ? (strcmp(operation, "addRootSupport") == 0 ? "Added" : "Removed") : "Failed to change",
	                  app->service_id,
	                  message);
	        if (ok) {
	            cleanup_bundled_app_cache_if_stage_root_is_download_cache(app, bundled_stage_root);
	            mark_backend_event_changed();
	        }
	        send_action_response_ex(fd, ok ? 200 : (needs_password ? 401 : 500), ok, message, needs_password);
	        return;
	    }

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
    if (lookup_launchd_backend_any_for_scope(service_id, requested_scope, plist_path, sizeof(plist_path), &owns_plist)) {
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
        if (ok && strcmp(operation, "start") == 0) {
            char wait_message[1024] = "";
            bool system_scope = strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0;
            if (wait_for_frontend_endpoint_ready(service_id,
                                                 system_scope ? "system" : "user",
                                                 wait_message,
                                                 sizeof(wait_message))) {
                snprintf(message, sizeof(message), "%s", wait_message);
            } else {
                ok = false;
                snprintf(message, sizeof(message), "%s", wait_message);
            }
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
            ok = run_systemd_operation_with_options(unit_name,
                                                    scope,
                                                    "stop",
                                                    sudo_password,
                                                    &needs_password,
                                                    message,
                                                    sizeof(message),
                                                    true);
            if (ok && strcmp(message, "ok") == 0) {
                snprintf(message, sizeof(message), "Stop requested.");
            }
        } else {
            ok = run_systemd_operation(socket_unit,
                                       scope,
                                       "start",
                                       sudo_password,
                                       &needs_password,
                                       message,
                                       sizeof(message));
            if (!ok && !needs_password) {
                ignored_needs_password = false;
                ignored_message[0] = '\0';
                ok = run_systemd_operation(unit_name,
                                           scope,
                                           "start",
                                           sudo_password,
                                           &ignored_needs_password,
                                           ignored_message,
                                           sizeof(ignored_message));
                if (ok) {
                    snprintf(message, sizeof(message), "%s", ignored_message);
                } else if (ignored_needs_password) {
                    needs_password = true;
                    snprintf(message, sizeof(message), "%s", ignored_message);
                }
            }
        }
    } else {
        ok = run_systemd_operation(unit_name, scope, operation, sudo_password, &needs_password, message, sizeof(message));
    }
    if (ok) {
        invalidate_systemd_status_cache();
    }
    if (ok && strcmp(operation, "start") == 0) {
        char wait_message[1024] = "";
        if (wait_for_frontend_endpoint_ready(service_id, scope, wait_message, sizeof(wait_message))) {
            snprintf(message, sizeof(message), "%s", wait_message);
        } else {
            ok = false;
            snprintf(message, sizeof(message), "%s", wait_message);
        }
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

#ifndef OUTER_SHELL_BACKEND_LIBRARY
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
#endif

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

static bool accept_home_screen_available_version(const char *provided_version, char *out, size_t out_size, char *message, size_t message_size) {
    if (out && out_size > 0) out[0] = '\0';
    if (!provided_version || !provided_version[0]) {
        snprintf(message, message_size, "OuterShellBackend did not provide an available version.");
        return false;
    }
    snprintf(out, out_size, "%s", provided_version);
    trim_whitespace_in_place(out);
    if (!out[0]) {
        snprintf(message, message_size, "Outer Shell version file was empty.");
        return false;
    }
    return true;
}

static bool home_screen_update_available(char *available, size_t available_size) {
    if (!update_check_due()) return false;
    (void)available;
    (void)available_size;
    return false;
}

static bool run_home_screen_install_script(const char *subcommand,
                                           const char *script_path,
                                           const char *archive_path,
                                           bool remove_user_state,
                                           char *message,
                                           size_t message_size) {
    if (!script_path || !script_path[0]) {
        snprintf(message, message_size, "OuterShellBackend did not provide an installer script.");
        return false;
    }
    if (!archive_path || !archive_path[0]) {
        snprintf(message, message_size, "OuterShellBackend did not provide an installer archive.");
        return false;
    }

    struct stat st;
    if (stat(script_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Installer script is missing at %s.", script_path);
        return false;
    }
    if (stat(archive_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Installer archive is missing at %s.", archive_path);
        return false;
    }

    char quoted_script_path[PATH_MAX + 8];
    char quoted_archive_path[PATH_MAX + 8];
    char quoted_subcommand[64];
    shell_quote(script_path, quoted_script_path, sizeof(quoted_script_path));
    shell_quote(archive_path, quoted_archive_path, sizeof(quoted_archive_path));
    shell_quote(subcommand && subcommand[0] ? subcommand : "install", quoted_subcommand, sizeof(quoted_subcommand));
    char command[8192];
    snprintf(command,
             sizeof(command),
             "OUTERSHELL_INSTALL_ARCHIVE=%s OUTERSHELL_UNINSTALL_REMOVE_USER_STATE=%s sh %s %s",
             quoted_archive_path,
             remove_user_state ? "1" : "0",
             quoted_script_path,
             quoted_subcommand);

#ifdef __linux__
    if (strcmp(subcommand, "update") == 0 || strcmp(subcommand, "uninstall") == 0) {
        char quoted_command[18000];
        shell_quote(command, quoted_command, sizeof(quoted_command));
        const char *scope = (geteuid() == 0) ? "--system" : "--user";
        const char *unit_suffix = (strcmp(subcommand, "uninstall") == 0) ? "uninstall" : "update";
        char launch_command[20000];
        snprintf(launch_command,
                 sizeof(launch_command),
                 "systemd-run %s --unit=org.outershell.OuterShell-installer-%s --collect /bin/sh -c %s",
                 scope,
                 unit_suffix,
                 quoted_command);

        FILE *pipe = popen(launch_command, "r");
        if (!pipe) {
            snprintf(message, message_size, "Failed to start Outer Shell %s: %s", subcommand, strerror(errno));
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
            if (!message[0]) {
                snprintf(message, message_size, "Outer Shell %s started.", subcommand);
            }
            return true;
        }
        if (!message[0]) {
            snprintf(message, message_size, "Failed to start Outer Shell %s.", subcommand);
        }
        return false;
    }
#endif

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

    if (geteuid() == 0) {
        int output_pipe[2] = {-1, -1};
        if (pipe(output_pipe) != 0) {
            snprintf(output, output_size, "Failed to create command pipe: %s", strerror(errno));
            return false;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(output_pipe[0]);
            close(output_pipe[1]);
            snprintf(output, output_size, "Failed to fork command: %s", strerror(errno));
            return false;
        }
        if (pid == 0) {
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(output_pipe[0]);
            close(output_pipe[1]);
            execlp("sh", "sh", "-c", command, (char *)NULL);
            _exit(127);
        }
        close(output_pipe[1]);

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

    char quoted_executable[PATH_MAX + 8];
    char quoted_system_root[PATH_MAX + 8];
#ifdef __APPLE__
    char helper_source[PATH_MAX];
    if (!macos_root_tool_source_path(executable, helper_source, sizeof(helper_source))) {
        snprintf(message, message_size, "Could not resolve standalone outershelld helper path.");
        return false;
    }
    shell_quote(helper_source, quoted_executable, sizeof(quoted_executable));
#else
    if (strstr(executable, " (deleted)") != NULL || access(executable, X_OK) != 0) {
        char installed_executable[PATH_MAX];
        char installed_root[PATH_MAX];
        default_outershelld_install_root(installed_root, sizeof(installed_root));
        snprintf(installed_executable, sizeof(installed_executable), "%s/outershelld", installed_root);
        if (access(installed_executable, X_OK) == 0) {
            snprintf(executable, sizeof(executable), "%s", installed_executable);
        }
    }
    shell_quote(executable, quoted_executable, sizeof(quoted_executable));
#endif
    shell_quote(kSystemOuterShellRoot, quoted_system_root, sizeof(quoted_system_root));
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
#ifndef __APPLE__
    char user_outerctl[PATH_MAX];
    default_user_outerctl_path(user_outerctl, sizeof(user_outerctl));
    if (access(user_outerctl, X_OK) != 0) {
        snprintf(message, message_size, "Could not find user outerctl at %s.", user_outerctl);
        fclose(script);
        unlink(script_template);
        return false;
    }

    char user_install_root[PATH_MAX];
    default_outershell_install_root(user_install_root, sizeof(user_install_root));
    char user_daemon_root[PATH_MAX];
    default_outershelld_install_root(user_daemon_root, sizeof(user_daemon_root));
    char user_outershelld[PATH_MAX];
    snprintf(user_outershelld, sizeof(user_outershelld), "%s/outershelld", user_daemon_root);
    char user_version[PATH_MAX];
    snprintf(user_version, sizeof(user_version), "%s/version", user_daemon_root);
    char user_outerctl_parent[PATH_MAX];
    if (!parent_directory(user_outerctl, user_outerctl_parent, sizeof(user_outerctl_parent)) ||
        !mkdir_p(user_install_root) ||
        !mkdir_p(user_daemon_root) ||
        !mkdir_p(user_outerctl_parent)) {
        snprintf(message, message_size, "Could not prepare user Outer Shell executable paths.");
        fclose(script);
        unlink(script_template);
        return false;
    }

    char system_install_root[PATH_MAX];
    char system_outershelld[PATH_MAX];
    char system_outerctl[PATH_MAX];
    char system_outerctl_parent[PATH_MAX];
    char system_version[PATH_MAX];
    char system_users_dir[PATH_MAX];
    char system_user_marker[PATH_MAX];
    default_system_outershelld_install_root(system_install_root, sizeof(system_install_root));
    default_system_outershelld_path(system_outershelld, sizeof(system_outershelld));
    default_system_outerctl_path(system_outerctl, sizeof(system_outerctl));
    system_binary_users_dir(system_users_dir, sizeof(system_users_dir));
    system_binary_user_marker_path(owner_uid, system_user_marker, sizeof(system_user_marker));
    snprintf(system_version, sizeof(system_version), "%s/version", system_install_root);
    if (!parent_directory(system_outerctl, system_outerctl_parent, sizeof(system_outerctl_parent))) {
        snprintf(message, message_size, "Could not resolve system outerctl directory.");
        fclose(script);
        unlink(script_template);
        return false;
    }

    char service_name[256];
    char socket_name[256];
    root_helper_unit_name_for_uid(owner_uid, "service", service_name, sizeof(service_name));
    root_helper_unit_name_for_uid(owner_uid, "socket", socket_name, sizeof(socket_name));

    char quoted_service_name[320];
    char quoted_socket_name[320];
    char quoted_user_outershelld[PATH_MAX + 8];
    char quoted_user_version[PATH_MAX + 8];
    char quoted_user_outerctl[PATH_MAX + 8];
    char quoted_system_install_root[PATH_MAX + 8];
    char quoted_system_outerctl_parent[PATH_MAX + 8];
    char quoted_system_outershelld[PATH_MAX + 8];
    char quoted_system_outerctl[PATH_MAX + 8];
    char quoted_system_version[PATH_MAX + 8];
    char quoted_system_users_dir[PATH_MAX + 8];
    char quoted_system_user_marker[PATH_MAX + 8];
    shell_quote(service_name, quoted_service_name, sizeof(quoted_service_name));
    shell_quote(socket_name, quoted_socket_name, sizeof(quoted_socket_name));
    shell_quote(user_outershelld, quoted_user_outershelld, sizeof(quoted_user_outershelld));
    shell_quote(user_version, quoted_user_version, sizeof(quoted_user_version));
    shell_quote(user_outerctl, quoted_user_outerctl, sizeof(quoted_user_outerctl));
    shell_quote(system_install_root, quoted_system_install_root, sizeof(quoted_system_install_root));
    shell_quote(system_outerctl_parent, quoted_system_outerctl_parent, sizeof(quoted_system_outerctl_parent));
    shell_quote(system_outershelld, quoted_system_outershelld, sizeof(quoted_system_outershelld));
    shell_quote(system_outerctl, quoted_system_outerctl, sizeof(quoted_system_outerctl));
    shell_quote(system_version, quoted_system_version, sizeof(quoted_system_version));
    shell_quote(system_users_dir, quoted_system_users_dir, sizeof(quoted_system_users_dir));
    shell_quote(system_user_marker, quoted_system_user_marker, sizeof(quoted_system_user_marker));
    (void)owner_name;

    fprintf(script,
            "set -eu\n"
            "systemctl --system disable --now outerloop-rootd.service >/dev/null 2>&1 || true\n"
            "rm -f /etc/systemd/system/outerloop-rootd.service\n"
            "systemctl --system daemon-reload >/dev/null 2>&1 || true\n"
            "systemctl --system reset-failed outerloop-rootd.service >/dev/null 2>&1 || true\n"
            "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
            "systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
            "rm -f /etc/systemd/system/%s /etc/systemd/system/%s\n"
            "systemctl --system daemon-reload >/dev/null 2>&1 || true\n"
            "systemctl --system stop outershelld.service >/dev/null 2>&1 || true\n"
            "mkdir -p %s %s %s %s /etc/systemd/system /var/log/outergroup\n"
            "chmod 1777 %s\n"
            "touch %s\n"
            "chown %ld:%ld %s 2>/dev/null || true\n"
            "rm -f /usr/local/libexec/outershelld-root-helper\n"
            "if [ \"$(readlink -f %s 2>/dev/null || printf '%%s' %s)\" != \"$(readlink -f %s 2>/dev/null || printf '%%s' %s)\" ]; then install -m 0755 %s %s; fi\n"
            "if [ \"$(readlink -f %s 2>/dev/null || printf '%%s' %s)\" != \"$(readlink -f %s 2>/dev/null || printf '%%s' %s)\" ]; then install -m 0755 %s %s; fi\n"
            "cat > /etc/systemd/system/outershelld.socket <<'__OUTERSHELLD_SOCKET__'\n"
            "[Unit]\n"
            "Description=Outer Shell API Socket\n"
            "\n"
            "[Socket]\n"
            "ListenStream=/run/outershelld-api\n"
            "FileDescriptorName=api\n"
            "SocketMode=0600\n"
            "Service=outershelld.service\n"
            "\n"
            "[Install]\n"
            "WantedBy=sockets.target\n"
            "__OUTERSHELLD_SOCKET__\n"
            "cat > /etc/systemd/system/outershelld.service <<'__OUTERSHELLD_SERVICE__'\n"
            "[Unit]\n"
            "Description=Outer Shell daemon\n"
            "\n"
            "[Service]\n"
            "Environment=OUTERSHELL_HOME=/var/lib/outershell\n"
            "Environment=OUTER_SHELL_PUBLIC_BASE_URL=%s\n"
            "ExecStart=/var/lib/outershell/outershelld/outershelld\n"
            "Restart=no\n"
            "StandardOutput=append:/var/log/outergroup/outershelld.log\n"
            "StandardError=append:/var/log/outergroup/outershelld.log\n"
            "\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n"
            "__OUTERSHELLD_SERVICE__\n"
            "if [ -f %s ]; then cp %s %s; else printf '%%s\\n' 'root-support' > %s; fi\n"
            "chmod 0644 %s\n"
            "touch /var/log/outergroup/outershelld.log\n"
            "chmod 0644 /var/log/outergroup/outershelld.log\n"
            "rm -f %s %s\n"
            "ln -s %s %s\n"
            "ln -s %s %s\n"
            "systemctl --system daemon-reload\n"
            "systemctl --system enable outershelld.socket >/dev/null 2>&1\n"
            "systemctl --system start outershelld.socket\n",
            quoted_service_name,
            quoted_socket_name,
            service_name,
            socket_name,
            quoted_system_install_root,
            quoted_system_outerctl_parent,
            quoted_system_root,
            quoted_system_users_dir,
            quoted_system_users_dir,
            quoted_system_user_marker,
            (long)owner_uid,
            (long)owner_uid,
            quoted_system_user_marker,
            quoted_executable,
            quoted_system_outershelld,
            quoted_system_outershelld,
            quoted_system_outershelld,
            quoted_executable,
            quoted_system_outershelld,
            quoted_user_outerctl,
            quoted_system_outerctl,
            quoted_system_outerctl,
            quoted_system_outerctl,
            quoted_user_outerctl,
            quoted_system_outerctl,
            g_home_screen_public_base_url,
            quoted_user_version,
            quoted_user_version,
            quoted_system_version,
            quoted_system_version,
            quoted_system_version,
            quoted_user_outershelld,
            quoted_user_outerctl,
            quoted_system_outershelld,
            quoted_user_outershelld,
            quoted_system_outerctl,
            quoted_user_outerctl);
#else
    (void)owner_uid;
    (void)owner_name;
    char helper_parent[PATH_MAX];
    if (!parent_directory(helper_source, helper_parent, sizeof(helper_parent))) {
        snprintf(message, message_size, "Could not resolve standalone outershelld helper directory.");
        fclose(script);
        unlink(script_template);
        return false;
    }
    char user_outerctl_source[PATH_MAX];
    default_user_outerctl_path(user_outerctl_source, sizeof(user_outerctl_source));
    if (access(user_outerctl_source, X_OK) != 0) {
        snprintf(message, message_size, "Could not find user outerctl payload at %s.", user_outerctl_source);
        fclose(script);
        unlink(script_template);
        return false;
    }
    char user_outerctl[PATH_MAX];
    default_user_outerctl_path(user_outerctl, sizeof(user_outerctl));
    char user_outerctl_parent[PATH_MAX];
    if (!parent_directory(user_outerctl, user_outerctl_parent, sizeof(user_outerctl_parent))) {
        snprintf(message, message_size, "Could not resolve user outerctl directory.");
        fclose(script);
        unlink(script_template);
        return false;
    }
    char system_install_root[PATH_MAX];
    char system_outershelld[PATH_MAX];
    char system_outerctl[PATH_MAX];
    char system_outerctl_parent[PATH_MAX];
    default_system_outershelld_install_root(system_install_root, sizeof(system_install_root));
    default_system_outershelld_path(system_outershelld, sizeof(system_outershelld));
    default_system_outerctl_path(system_outerctl, sizeof(system_outerctl));
    if (!parent_directory(system_outerctl, system_outerctl_parent, sizeof(system_outerctl_parent))) {
        snprintf(message, message_size, "Could not resolve system outerctl directory.");
        fclose(script);
        unlink(script_template);
        return false;
    }

    char quoted_helper_parent[PATH_MAX + 8];
    char quoted_user_outerctl_source[PATH_MAX + 8];
    char quoted_user_outerctl[PATH_MAX + 8];
    char quoted_user_outerctl_parent[PATH_MAX + 8];
    char quoted_system_install_root[PATH_MAX + 8];
    char quoted_system_outershelld[PATH_MAX + 8];
    char quoted_system_outerctl[PATH_MAX + 8];
    char quoted_system_outerctl_parent[PATH_MAX + 8];
    shell_quote(helper_parent, quoted_helper_parent, sizeof(quoted_helper_parent));
    shell_quote(user_outerctl_source, quoted_user_outerctl_source, sizeof(quoted_user_outerctl_source));
    shell_quote(user_outerctl, quoted_user_outerctl, sizeof(quoted_user_outerctl));
    shell_quote(user_outerctl_parent, quoted_user_outerctl_parent, sizeof(quoted_user_outerctl_parent));
    shell_quote(system_install_root, quoted_system_install_root, sizeof(quoted_system_install_root));
    shell_quote(system_outershelld, quoted_system_outershelld, sizeof(quoted_system_outershelld));
    shell_quote(system_outerctl, quoted_system_outerctl, sizeof(quoted_system_outerctl));
    shell_quote(system_outerctl_parent, quoted_system_outerctl_parent, sizeof(quoted_system_outerctl_parent));

    fprintf(script,
            "set -eu\n"
            "mkdir -p /usr/local/libexec %s %s %s %s %s\n"
            "rm -f /usr/local/libexec/outershelld-root-helper\n"
            "if [ ! -e %s ] || ! cmp -s %s %s; then install -m 0755 %s %s; fi\n"
            "chmod 0755 %s\n"
            "if [ ! -e %s ] || ! cmp -s %s %s; then install -m 0755 %s %s; fi\n"
            "chmod 0755 %s\n"
            "rm -f /usr/local/libexec/outershelld-root-tool\n"
            "ln -s %s /usr/local/libexec/outershelld-root-tool\n"
            "rm -f %s %s\n"
            "ln -s %s %s\n"
            "ln -s %s %s\n",
            quoted_system_root,
            quoted_system_install_root,
            quoted_helper_parent,
            quoted_system_outerctl_parent,
            quoted_user_outerctl_parent,
            quoted_system_outershelld,
            quoted_executable,
            quoted_system_outershelld,
            quoted_executable,
            quoted_system_outershelld,
            quoted_system_outershelld,
            quoted_system_outerctl,
            quoted_user_outerctl_source,
            quoted_system_outerctl,
            quoted_user_outerctl_source,
            quoted_system_outerctl,
            quoted_system_outerctl,
            quoted_system_outershelld,
            quoted_executable,
            quoted_user_outerctl,
            quoted_system_outershelld,
            quoted_executable,
            quoted_system_outerctl,
            quoted_user_outerctl);
#endif
    fclose(script);
    chmod(script_template, 0700);

    bool ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
    unlink(script_template);
    if (!ok) return false;

    snprintf(message, message_size, "Root Outer Shell support installed.");
    return true;
}

#ifndef __APPLE__
static void write_system_binary_cleanup_shell(FILE *script) {
    char system_root[PATH_MAX];
    char system_install_root[PATH_MAX];
    char system_outershelld[PATH_MAX];
    char system_outerctl[PATH_MAX];
    char system_version[PATH_MAX];
    char system_users_dir[PATH_MAX];
    default_system_outershelld_install_root(system_install_root, sizeof(system_install_root));
    default_system_outershelld_path(system_outershelld, sizeof(system_outershelld));
    default_system_outerctl_path(system_outerctl, sizeof(system_outerctl));
    system_binary_users_dir(system_users_dir, sizeof(system_users_dir));
    snprintf(system_root, sizeof(system_root), "%s", kSystemOuterShellRoot);
    snprintf(system_version, sizeof(system_version), "%s/version", system_install_root);

    char quoted_system_root[PATH_MAX + 8];
    char quoted_system_install_root[PATH_MAX + 8];
    char quoted_system_outershelld[PATH_MAX + 8];
    char quoted_system_outerctl[PATH_MAX + 8];
    char quoted_system_version[PATH_MAX + 8];
    char quoted_system_users_dir[PATH_MAX + 8];
    shell_quote(system_root, quoted_system_root, sizeof(quoted_system_root));
    shell_quote(system_install_root, quoted_system_install_root, sizeof(quoted_system_install_root));
    shell_quote(system_outershelld, quoted_system_outershelld, sizeof(quoted_system_outershelld));
    shell_quote(system_outerctl, quoted_system_outerctl, sizeof(quoted_system_outerctl));
    shell_quote(system_version, quoted_system_version, sizeof(quoted_system_version));
    shell_quote(system_users_dir, quoted_system_users_dir, sizeof(quoted_system_users_dir));

    fprintf(script,
            "system_binary_users_dir=%s\n"
            "system_outershell_home=%s\n"
            "system_daemon_root=%s\n"
            "system_outershelld_path=%s\n"
            "system_outerctl_path=%s\n"
            "system_version_path=%s\n"
            "system_binary_users_empty() {\n"
            "  if [ ! -d \"$system_binary_users_dir\" ]; then return 0; fi\n"
            "  for marker in \"$system_binary_users_dir\"/*; do\n"
            "    [ -e \"$marker\" ] || continue\n"
            "    name=$(basename \"$marker\")\n"
            "    case \"$name\" in\n"
            "      root-apps)\n"
            "        [ \"$(stat -c %%u \"$marker\" 2>/dev/null || echo invalid)\" = \"0\" ] && return 1\n"
            "        ;;\n"
            "      uid-*)\n"
            "        uid=${name#uid-}\n"
            "        case \"$uid\" in *[!0-9]*|'') continue ;; esac\n"
            "        [ \"$(stat -c %%u \"$marker\" 2>/dev/null || echo invalid)\" = \"$uid\" ] && return 1\n"
            "        ;;\n"
            "    esac\n"
            "  done\n"
            "  return 0\n"
            "}\n"
            "remove_system_binaries_if_unused() {\n"
            "  system_binary_users_empty || return 0\n"
            "  systemctl --system disable --now outershelld.socket outershelld.service >/dev/null 2>&1 || true\n"
            "  rm -f /etc/systemd/system/outershelld.service /etc/systemd/system/outershelld.socket /run/outershelld-api\n"
            "  systemctl --system daemon-reload >/dev/null 2>&1 || true\n"
            "  rm -f \"$system_outershelld_path\" \"$system_outerctl_path\" \"$system_version_path\"\n"
            "  rm -f /var/log/outergroup/outershelld.log /var/log/outergroup/org.outershell.OuterShell.log\n"
            "  if ! find \"$system_outershell_home/apps\" -mindepth 1 -print -quit 2>/dev/null | grep -q . &&\n"
            "     ! find /opt/outergroup -mindepth 1 -print -quit 2>/dev/null | grep -q .; then\n"
            "    rm -f \"$system_outershell_home/registry.orwa\" \"$system_outershell_home/registry.orwa.lock\"\n"
            "    rmdir \"$system_outershell_home/apps\" /opt/outergroup >/dev/null 2>&1 || true\n"
            "  fi\n"
            "  rmdir \"$system_daemon_root\" \"$system_outershell_home/bin\" \"$system_binary_users_dir\" \"$system_outershell_home\" /var/log/outergroup >/dev/null 2>&1 || true\n"
            "}\n",
            quoted_system_users_dir,
            quoted_system_root,
            quoted_system_install_root,
            quoted_system_outershelld,
            quoted_system_outerctl,
            quoted_system_version);
}

static void write_root_apps_marker_cleanup_shell(FILE *script) {
    char root_apps_marker[PATH_MAX];
    system_binary_root_apps_marker_path(root_apps_marker, sizeof(root_apps_marker));
    char quoted_root_apps_marker[PATH_MAX + 8];
    shell_quote(root_apps_marker, quoted_root_apps_marker, sizeof(quoted_root_apps_marker));
    fprintf(script,
            "if ! find /opt/outergroup -mindepth 1 -maxdepth 1 -type d -print -quit 2>/dev/null | grep -q .; then\n"
            "  rm -f %s\n"
            "fi\n",
            quoted_root_apps_marker);
    write_system_binary_cleanup_shell(script);
    fprintf(script, "remove_system_binaries_if_unused\n");
}
#endif

static bool run_root_outershell_migration(const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;
    char old_outerctl[PATH_MAX];
    char old_outer_shell_outerctl[PATH_MAX];
    char new_outerctl[PATH_MAX];
    char old_user_apps[PATH_MAX];
    char new_user_apps[PATH_MAX];
    legacy_user_outerctl_path(old_outerctl, sizeof(old_outerctl));
    legacy_outer_shell_outerctl_path(old_outer_shell_outerctl, sizeof(old_outer_shell_outerctl));
    default_user_outerctl_path(new_outerctl, sizeof(new_outerctl));
    legacy_user_apps_root(old_user_apps, sizeof(old_user_apps));
    default_user_outershell_apps_root(new_user_apps, sizeof(new_user_apps));

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
    snprintf(new_system_apps_root, sizeof(new_system_apps_root), "%s/apps", kSystemOuterShellRoot);

    char quoted_old_outerctl[PATH_MAX + 8];
    char quoted_old_outer_shell_outerctl[PATH_MAX + 8];
    char quoted_new_outerctl[PATH_MAX + 8];
    char quoted_old_user_apps[PATH_MAX + 8];
    char quoted_new_user_apps[PATH_MAX + 8];
    char quoted_legacy_system_root[PATH_MAX + 8];
    char quoted_legacy_system_apps_root[PATH_MAX + 8];
    char quoted_new_root[PATH_MAX + 8];
    char quoted_new_system_apps_root[PATH_MAX + 8];
    shell_quote(old_outerctl, quoted_old_outerctl, sizeof(quoted_old_outerctl));
    shell_quote(old_outer_shell_outerctl, quoted_old_outer_shell_outerctl, sizeof(quoted_old_outer_shell_outerctl));
    shell_quote(new_outerctl, quoted_new_outerctl, sizeof(quoted_new_outerctl));
    shell_quote(old_user_apps, quoted_old_user_apps, sizeof(quoted_old_user_apps));
    shell_quote(new_user_apps, quoted_new_user_apps, sizeof(quoted_new_user_apps));
    shell_quote(legacy_system_root, quoted_legacy_system_root, sizeof(quoted_legacy_system_root));
    shell_quote(legacy_system_apps_root, quoted_legacy_system_apps_root, sizeof(quoted_legacy_system_apps_root));
    shell_quote(kSystemOuterShellRoot, quoted_new_root, sizeof(quoted_new_root));
    shell_quote(new_system_apps_root, quoted_new_system_apps_root, sizeof(quoted_new_system_apps_root));

    char script_template[] = "/tmp/outershell-root-migration-XXXXXX";
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
            "python3 - <<'__OUTERSHELL_ROOT_MIGRATION__'\n"
            "import os, sqlite3, urllib.parse\n"
            "old_db = os.environ['OLD_DB']\n"
            "new_db = os.environ['NEW_DB']\n"
            "replacements = [\n"
            "    (os.environ['OLD_OUTERCTL'], os.environ['NEW_OUTERCTL']),\n"
            "    (os.environ['OLD_OUTER_SHELL_OUTERCTL'], os.environ['NEW_OUTERCTL']),\n"
            "    (os.environ['OLD_USER_APPS'], os.environ['NEW_USER_APPS']),\n"
            "    (os.environ['OLD_SYSTEM_APPS'], os.environ['NEW_SYSTEM_APPS']),\n"
            "    (os.environ['OLD_ROOT'], os.environ['NEW_ROOT']),\n"
            "    ('OUTERAGENT_ROOT', 'OUTERSHELL_HOME'),\n"
            "    ('outeragent.log', 'backend.log'),\n"
            "]\n"
            "os.makedirs(os.path.dirname(new_db), exist_ok=True)\n"
            "db = sqlite3.connect(new_db)\n"
            "db.executescript('''\n"
            "CREATE TABLE IF NOT EXISTS backends (service_id TEXT PRIMARY KEY, display_name TEXT NOT NULL DEFAULT '', service_unit TEXT);\n"
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
            "            db.execute('INSERT OR REPLACE INTO backends(service_id, display_name, service_unit) VALUES (?, ?, ?)',\n"
            "                       (row['service_id'], row['display_name'] if 'display_name' in row.keys() and row['display_name'] is not None else '', row['service_unit'] if 'service_unit' in row.keys() else None))\n"
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
            "__OUTERSHELL_ROOT_MIGRATION__\n"
            "chmod 0755 \"$NEW_ROOT\" >/dev/null 2>&1 || true\n"
            "if [ -d \"$OLD_ROOT\" ]; then mv \"$OLD_ROOT\" \"$OLD_ROOT.migrated.$(date +%%s)\" >/dev/null 2>&1 || true; fi\n"
            "chmod 0644 \"$NEW_DB\" >/dev/null 2>&1 || true\n"
            "if command -v launchctl >/dev/null 2>&1; then\n"
            "  for plist in /Library/LaunchDaemons/*.plist; do\n"
            "    [ -f \"$plist\" ] || continue\n"
            "    if grep -E -q 'outershell|dev[.]outergroup|outergroup' \"$plist\" 2>/dev/null; then\n"
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
            quoted_old_outer_shell_outerctl,
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

static bool append_inline_startup_file(StringBuilder *builder, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", home_directory(), name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return true;
    if (st.st_size > 128 * 1024) {
        return sb_append(builder, "\n# Outer Shell skipped ") &&
               sb_append(builder, name) &&
               sb_append(builder, " because it is larger than 128 KiB.\n");
    }
    size_t size = 0;
    char *contents = read_text_file_alloc(path, &size);
    if (!contents) return true;
    bool ok = sb_append(builder, "\n# --- Begin inline ") &&
              sb_append(builder, name) &&
              sb_append(builder, " ---\n") &&
              sb_append_n(builder, contents, size);
    if (ok && (size == 0 || contents[size - 1] != '\n')) {
        ok = sb_append(builder, "\n");
    }
    ok = ok &&
         sb_append(builder, "# --- End inline ") &&
         sb_append(builder, name) &&
         sb_append(builder, " ---\n");
    free(contents);
    return ok;
}

static bool append_inline_shell_startup_snapshot(StringBuilder *builder) {
#ifdef __APPLE__
    const char *shell_name = "zsh";
    const char *files[] = {".zprofile", ".zshrc"};
#else
    const char *shell_name = "bash";
    const char *files[] = {".bash_profile", ".bash_login", ".profile", ".bashrc"};
#endif
    bool ok = sb_append(builder,
        "\n"
        "# Outer Shell inlined a snapshot of common startup files so this\n"
        "# script behaves more like commands pasted into an interactive ") &&
        sb_append(builder, shell_name) &&
        sb_append(builder,
        " window.\n"
        "__outershell_inline_startup() {\n");
    for (size_t i = 0; ok && i < sizeof(files) / sizeof(files[0]); i++) {
        ok = append_inline_startup_file(builder, files[i]);
    }
    return ok && sb_append(builder,
        "}\n"
        "__outershell_inline_startup_had_nounset=0\n"
        "case $- in *u*) __outershell_inline_startup_had_nounset=1; set +u ;; esac\n"
        "__outershell_inline_startup\n"
        "if [ \"$__outershell_inline_startup_had_nounset\" = 1 ]; then set -u; fi\n"
        "unset __outershell_inline_startup_had_nounset\n"
        "unset -f __outershell_inline_startup 2>/dev/null || true\n");
}

static bool append_generated_script_log_redirect(StringBuilder *builder) {
#ifndef __APPLE__
    (void)builder;
    return true;
#else
    return sb_append(builder,
        "\n"
        "if [ -n \"${OUTERSHELL_LOG_PATH:-}\" ]; then\n"
        "    log_dir=${OUTERSHELL_LOG_PATH%/*}\n"
        "    if [ -n \"$log_dir\" ] && [ \"$log_dir\" != \"$OUTERSHELL_LOG_PATH\" ]; then\n"
        "        mkdir -p \"$log_dir\"\n"
        "    fi\n"
        "    exec >> \"$OUTERSHELL_LOG_PATH\" 2>&1\n"
        "fi\n");
#endif
}

static bool make_blank_script(const char *service_id, const char *display_name, StringBuilder *builder) {
    char quoted_id[512];
    char quoted_name[1024];
    shell_quote(service_id, quoted_id, sizeof(quoted_id));
    shell_quote(display_name, quoted_name, sizeof(quoted_name));
#ifdef __APPLE__
    const char *script_shebang = "#!/bin/zsh\n";
#else
    const char *script_shebang = "#!/usr/bin/env bash\n";
#endif
    return sb_append(builder,
        script_shebang) &&
        sb_append(builder,
        "set -eu\n"
        "\n"
        "BACKEND_ID=") &&
        sb_append(builder, quoted_id) &&
        sb_append(builder, "\nDISPLAY_NAME=") &&
        sb_append(builder, quoted_name) &&
        sb_append(builder,
        "\nexport BACKEND_ID DISPLAY_NAME\n"
        ) &&
        append_generated_script_log_redirect(builder) &&
        sb_append(builder,
        "\n"
        "# Outer Loop sets OUTERCTL_PATH before this script runs.\n"
        "# Keep your own startup logic here, in a file you control.\n"
        "# If your app chooses a dynamic port or URL, announce it after it starts:\n"
        "# \"$OUTERCTL_PATH\" app add --backend \"$BACKEND_ID\" --port 9000 --name \"$DISPLAY_NAME\" --url \"127.0.0.1:9000/\"\n"
        "\n"
        "python3 -m http.server 9000\n");
}

static bool make_fixed_port_script(const char *service_id,
                                   const char *display_name,
                                   const char *port,
                                   const char *socket_path,
                                   bool use_unix_socket,
                                   const char *command,
                                   StringBuilder *builder) {
    char quoted_id[512];
    char quoted_name[1024];
    char quoted_port[64];
    char quoted_socket_path[PATH_MAX * 2];
    shell_quote(service_id, quoted_id, sizeof(quoted_id));
    shell_quote(display_name, quoted_name, sizeof(quoted_name));
    shell_quote(port ? port : "", quoted_port, sizeof(quoted_port));
    shell_quote(socket_path ? socket_path : "", quoted_socket_path, sizeof(quoted_socket_path));
#ifdef __APPLE__
    const char *script_shebang = "#!/bin/zsh\n";
#else
    const char *script_shebang = "#!/usr/bin/env bash\n";
#endif
    return sb_append(builder,
        script_shebang) &&
        sb_append(builder,
        "set -eu\n"
        "\n"
        "BACKEND_ID=") &&
        sb_append(builder, quoted_id) &&
        sb_append(builder, "\nDISPLAY_NAME=") &&
        sb_append(builder, quoted_name) &&
        sb_append(builder, "\nPORT=") &&
        sb_append(builder, quoted_port) &&
        sb_append(builder, "\nSOCKET_PATH=") &&
        sb_append(builder, quoted_socket_path) &&
        sb_append(builder, "\nENDPOINT_KIND=") &&
        sb_append(builder, use_unix_socket ? "unixSocket" : "port") &&
        sb_append(builder,
        "\nexport BACKEND_ID DISPLAY_NAME PORT SOCKET_PATH ENDPOINT_KIND\n"
        ) &&
        append_generated_script_log_redirect(builder) &&
        append_inline_shell_startup_snapshot(builder) &&
        sb_append(builder,
        "\n"
        "if [ \"$ENDPOINT_KIND\" = unixSocket ]; then\n"
        "    socket_dir=${SOCKET_PATH%/*}\n"
        "    if [ -n \"$socket_dir\" ] && [ \"$socket_dir\" != \"$SOCKET_PATH\" ]; then\n"
        "        mkdir -p \"$socket_dir\"\n"
        "    fi\n"
        "    rm -f \"$SOCKET_PATH\"\n"
        "fi\n"
        "\n") &&
        sb_append(builder, command) &&
        sb_append(builder, "\n");
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
        "    if sys.platform.startswith(\"linux\") and os.geteuid() == 0:\n"
        "        return \"/run\"\n"
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
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--frontend-id\", f\"{BACKEND_ID}:main\", \"--socket-path\", SOCKET_PATH, \"--name\", DISPLAY_NAME, \"--url\", app_url]\n"
        "                else:\n"
        "                    port, app_url, icon_path = discovered\n"
        "                    add_args = [\"app\", \"add\", \"--backend\", BACKEND_ID, \"--frontend-id\", f\"{BACKEND_ID}:main\", \"--port\", port, \"--name\", DISPLAY_NAME, \"--url\", app_url]\n"
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
        "    pass\n");
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
                                     const char *frontend_icon_path,
                                     const char *frontend_list,
                                     char *error,
                                     size_t error_size) {
    RegistryStore database;
    if (!registry_store_open_user_readwrite(&database, error, error_size)) return false;
    bool ok = true;
    if (registry_store_find_backend(&database, service_id)) {
        snprintf(error, error_size, "A backend with this identifier already exists.");
        ok = false;
    }
    if (ok) {
#ifdef __APPLE__
        ok = registry_store_upsert_backend(&database, service_id, display_name, "", unit_name, true);
#else
        ok = registry_store_upsert_backend(&database, service_id, display_name, unit_name, "", true);
#endif
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    if (ok) {
#ifdef __APPLE__
        (void)unit_name;
#else
        (void)unit_name;
#endif
    }
    if (ok) {
        ok = registry_store_upsert_log(&database, log_path, service_id);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    if (ok && frontend_id && frontend_id[0]) {
        ok = registry_store_upsert_frontend(&database,
                                            frontend_id,
                                            frontend_url ? frontend_url : "",
                                            service_id,
                                            display_name,
                                            frontend_port,
                                            frontend_socket_path ? frontend_socket_path : "",
                                            frontend_icon_path ? frontend_icon_path : "",
                                            frontend_list ? frontend_list : "",
                                            false);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    ok = registry_store_close(&database, ok, error, error_size) && ok;
    if (ok && frontend_socket_path && frontend_socket_path[0]) {
        ok = append_outerloop_http_unix_allowlist_entry(frontend_socket_path,
                                                        direct_root_session_uses_system_scope(),
                                                        error,
                                                        error_size);
    }
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
        snprintf(out, out_size, "/");
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

typedef struct {
    const char *identifier;
    const char *display_name;
    const char *conforms_to;
    const char *extensions;
    const char *filenames;
    const char *mime_types;
} BuiltInContentType;

static const BuiltInContentType kBuiltInContentTypes[] = {
    {"public.data", "Data", "", "", "", "application/octet-stream"},
    {"public.text", "Text", "public.data", "txt,text,log,conf,ini,cfg", "README,LICENSE,Makefile,Dockerfile,.gitignore,.env,CMakeLists.txt", "text/plain"},
    {"public.plain-text", "Plain Text", "public.text", "txt,text,log,conf,ini,cfg", "README,LICENSE,.gitignore,.env", "text/plain"},
    {"public.markdown", "Markdown", "public.text", "md,markdown,mdown,mkdn", "README.md", "text/markdown"},
    {"public.json", "JSON", "public.text", "json", "", "application/json"},
    {"public.xml", "XML", "public.text", "xml", "", "application/xml,text/xml"},
    {"public.shell-script", "Shell Script", "public.text", "sh,bash,zsh,fish,command", "", "text/x-shellscript"},
    {"public.python-script", "Python Script", "public.text", "py", "", "text/x-python"},
    {"public.javascript", "JavaScript", "public.text", "js,mjs,cjs", "", "text/javascript,application/javascript"},
    {"public.html", "HTML", "public.text", "html,htm", "", "text/html"},
    {"public.css", "CSS", "public.text", "css", "", "text/css"},
    {"public.image", "Image", "public.data", "", "", "image/*"},
    {"public.png", "PNG", "public.image", "png", "", "image/png"},
    {"public.jpeg", "JPEG", "public.image", "jpg,jpeg,jpe", "", "image/jpeg"},
    {"public.gif", "GIF", "public.image", "gif", "", "image/gif"},
    {"public.pdf", "PDF", "public.data", "pdf", "", "application/pdf"},
    {"public.zip-archive", "ZIP Archive", "public.data", "zip", "", "application/zip"}
};

static bool normalize_content_type_identifier(const char *raw, char *out, size_t out_size) {
    if (!raw || !raw[0] || !out || out_size == 0) return false;
    size_t offset = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && offset + 1 < out_size; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return false;
        out[offset++] = (char)tolower(*p);
    }
    if (raw[offset] != '\0') return false;
    out[offset] = '\0';
    return offset > 0 && strchr(out, '.') != NULL;
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

typedef struct {
    char values[32][160];
    size_t count;
} ContentTypeList;

static bool content_type_list_contains(const ContentTypeList *list, const char *identifier) {
    if (!list || !identifier || !identifier[0]) return false;
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->values[i], identifier) == 0) return true;
    }
    return false;
}

static bool content_type_list_append(ContentTypeList *list, const char *identifier) {
    if (!list || !identifier || !identifier[0]) return true;
    char normalized[160];
    if (!normalize_content_type_identifier(identifier, normalized, sizeof(normalized))) return true;
    if (content_type_list_contains(list, normalized)) return true;
    if (list->count >= sizeof(list->values) / sizeof(list->values[0])) return false;
    snprintf(list->values[list->count++], sizeof(list->values[0]), "%s", normalized);
    return true;
}

static bool csv_contains_token(const char *csv, const char *token, bool case_sensitive) {
    if (!csv || !csv[0] || !token || !token[0]) return false;
    const char *cursor = csv;
    size_t token_len = strlen(token);
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) cursor++;
        const char *end = cursor;
        while (*end && *end != ',') end++;
        const char *trim_end = end;
        while (trim_end > cursor && isspace((unsigned char)trim_end[-1])) trim_end--;
        size_t len = (size_t)(trim_end - cursor);
        if (len == token_len) {
            if (case_sensitive ? strncmp(cursor, token, len) == 0 : strncasecmp(cursor, token, len) == 0) {
                return true;
            }
        }
        cursor = end;
    }
    return false;
}

static const BuiltInContentType *builtin_content_type_for_identifier(const char *identifier) {
    for (size_t i = 0; i < sizeof(kBuiltInContentTypes) / sizeof(kBuiltInContentTypes[0]); i++) {
        if (strcmp(kBuiltInContentTypes[i].identifier, identifier ? identifier : "") == 0) {
            return &kBuiltInContentTypes[i];
        }
    }
    return NULL;
}

static bool append_content_type_parents(const RegistryStore *store, ContentTypeList *list, const char *identifier, int depth);

static bool append_content_type_and_parents(const RegistryStore *store, ContentTypeList *list, const char *identifier) {
    return content_type_list_append(list, identifier) &&
           append_content_type_parents(store, list, identifier, 0);
}

static bool append_parent_csv(const RegistryStore *store, ContentTypeList *list, const char *parents, int depth) {
    if (!parents || !parents[0]) return true;
    const char *cursor = parents;
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) cursor++;
        const char *end = cursor;
        while (*end && *end != ',') end++;
        const char *trim_end = end;
        while (trim_end > cursor && isspace((unsigned char)trim_end[-1])) trim_end--;
        if (trim_end > cursor) {
            char parent[160];
            size_t len = (size_t)(trim_end - cursor);
            if (len >= sizeof(parent)) len = sizeof(parent) - 1;
            memcpy(parent, cursor, len);
            parent[len] = '\0';
            if (!content_type_list_append(list, parent) ||
                !append_content_type_parents(store, list, parent, depth + 1)) {
                return false;
            }
        }
        cursor = end;
        if (depth > 16) break;
    }
    return true;
}

static bool append_content_type_parents(const RegistryStore *store, ContentTypeList *list, const char *identifier, int depth) {
    if (depth > 16) return true;
    const RegistryContentTypeRecord *custom = store ? registry_store_find_content_type_const(store, identifier) : NULL;
    if (custom && !append_parent_csv(store, list, custom->conforms_to, depth + 1)) return false;
    const BuiltInContentType *builtin = builtin_content_type_for_identifier(identifier);
    if (builtin && !append_parent_csv(store, list, builtin->conforms_to, depth + 1)) return false;
    return true;
}

static bool content_type_matches_extension(const RegistryStore *store, const char *identifier, const char *extension) {
    if (!identifier || !extension || !extension[0]) return false;
    const RegistryContentTypeRecord *custom = store ? registry_store_find_content_type_const(store, identifier) : NULL;
    if (custom && csv_contains_token(custom->extensions, extension, false)) return true;
    const BuiltInContentType *builtin = builtin_content_type_for_identifier(identifier);
    return builtin && csv_contains_token(builtin->extensions, extension, false);
}

static bool content_type_matches_filename(const RegistryStore *store, const char *identifier, const char *filename) {
    if (!identifier || !filename || !filename[0]) return false;
    const RegistryContentTypeRecord *custom = store ? registry_store_find_content_type_const(store, identifier) : NULL;
    if (custom && csv_contains_token(custom->filenames, filename, true)) return true;
    const BuiltInContentType *builtin = builtin_content_type_for_identifier(identifier);
    return builtin && csv_contains_token(builtin->filenames, filename, true);
}

static bool path_file_extension(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const char *name = strrchr(path ? path : "", '/');
    name = name ? name + 1 : (path ? path : "");
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return false;
    return normalize_file_extension(dot + 1, out, out_size);
}

static const char *path_file_name(const char *path) {
    const char *name = strrchr(path ? path : "", '/');
    return name ? name + 1 : (path ? path : "");
}

static bool file_sample(const char *path, unsigned char *buffer, size_t capacity, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!path || !path[0] || !buffer || capacity == 0) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(buffer, 1, capacity, file);
    fclose(file);
    if (out_count) *out_count = count;
    return true;
}

static bool sample_looks_like_text(const unsigned char *bytes, size_t count) {
    if (!bytes) return false;
    if (count == 0) return true;
    size_t suspicious = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned char c = bytes[i];
        if (c == 0) return false;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != '\b') suspicious++;
    }
    return suspicious * 8 <= count;
}

static bool sample_contains_ascii(const unsigned char *bytes, size_t count, const char *needle) {
    if (!bytes || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    if (needle_len > count) return false;
    for (size_t i = 0; i <= count - needle_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            unsigned char a = (unsigned char)tolower(bytes[i + j]);
            unsigned char b = (unsigned char)tolower((unsigned char)needle[j]);
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static bool infer_content_types_for_path(const RegistryStore *store, const char *path, ContentTypeList *list) {
    if (!list) return false;
    const char *filename = path_file_name(path);
    char extension[128] = "";
    (void)path_file_extension(path, extension, sizeof(extension));

    for (size_t i = 0; i < sizeof(kBuiltInContentTypes) / sizeof(kBuiltInContentTypes[0]); i++) {
        const char *identifier = kBuiltInContentTypes[i].identifier;
        if ((extension[0] && content_type_matches_extension(store, identifier, extension)) ||
            (filename[0] && content_type_matches_filename(store, identifier, filename))) {
            if (!append_content_type_and_parents(store, list, identifier)) return false;
        }
    }
    if (store) {
        for (size_t i = 0; i < store->content_type_count; i++) {
            const char *identifier = store->content_types[i].identifier;
            if ((extension[0] && content_type_matches_extension(store, identifier, extension)) ||
                (filename[0] && content_type_matches_filename(store, identifier, filename))) {
                if (!append_content_type_and_parents(store, list, identifier)) return false;
            }
        }
    }

    unsigned char sample[4096];
    size_t count = 0;
    if (file_sample(path, sample, sizeof(sample), &count)) {
        if (count >= 4 && memcmp(sample, "%PDF", 4) == 0) {
            if (!append_content_type_and_parents(store, list, "public.pdf")) return false;
        } else if (count >= 8 && memcmp(sample, "\x89PNG\r\n\x1a\n", 8) == 0) {
            if (!append_content_type_and_parents(store, list, "public.png")) return false;
        } else if (count >= 3 && sample[0] == 0xff && sample[1] == 0xd8 && sample[2] == 0xff) {
            if (!append_content_type_and_parents(store, list, "public.jpeg")) return false;
        } else if (count >= 6 && (memcmp(sample, "GIF87a", 6) == 0 || memcmp(sample, "GIF89a", 6) == 0)) {
            if (!append_content_type_and_parents(store, list, "public.gif")) return false;
        } else if (count >= 4 && memcmp(sample, "PK\x03\x04", 4) == 0) {
            if (!append_content_type_and_parents(store, list, "public.zip-archive")) return false;
        }
        if (count >= 2 && sample[0] == '#' && sample[1] == '!') {
            if (sample_contains_ascii(sample, count, "python")) {
                if (!append_content_type_and_parents(store, list, "public.python-script")) return false;
            } else {
                if (!append_content_type_and_parents(store, list, "public.shell-script")) return false;
            }
        } else if (sample_looks_like_text(sample, count)) {
            if (!append_content_type_and_parents(store, list, "public.text")) return false;
        }
    }

    if (list->count == 0) {
        if (!append_content_type_and_parents(store, list, "public.data")) return false;
    }
    return true;
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

static bool outerctl_print_headers(StringBuilder *out, const char *const *headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && !sb_append(out, "\t")) return false;
        if (!outerctl_tsv_field(out, headers[i])) return false;
    }
    return sb_append(out, "\n");
}

static bool outerctl_print_registry_list(const RegistryStore *database,
                                         const char *resource,
                                         const char *backend_filter,
                                         const char *content_type_filter,
                                         const char *file_path,
                                         StringBuilder *out,
                                         char *error,
                                         size_t error_size) {
    bool ok = true;
    if (strcmp(resource, "backend") == 0) {
        const char *headers[] = {"service_id", "display_name", "unit_name", "unit_path", "owns_unit"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->backend_count; i++) {
            const RegistryBackendRecord *record = &database->backends[i];
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            const char *fields[] = {
                record->service_id,
                record->display_name,
                record->unit_name,
                record->unit_path,
                record->owns_unit ? "1" : "0"
            };
            for (size_t column = 0; ok && column < sizeof(fields) / sizeof(fields[0]); column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "app") == 0) {
        const char *headers[] = {"frontend_id", "url", "service_id", "display_name", "port", "socket_path", "icon_path", "list"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->frontend_count; i++) {
            const RegistryFrontendRecord *record = &database->frontends[i];
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            char port_buffer[32];
            snprintf(port_buffer, sizeof(port_buffer), "%d", record->port);
            const RegistryFrontendLayoutRecord *layout =
                registry_store_find_layout_const(database, record->frontend_id && record->frontend_id[0] ? record->frontend_id : record->url);
            if (!layout) layout = registry_store_find_layout_const(database, record->url);
            const char *fields[] = {
                record->frontend_id,
                record->url,
                record->service_id,
                record->display_name,
                port_buffer,
                record->socket_path,
                record->icon_path,
                layout ? layout->list : record->list
            };
            for (size_t column = 0; ok && column < sizeof(fields) / sizeof(fields[0]); column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "log") == 0) {
        const char *headers[] = {"path", "service_id"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->log_count; i++) {
            const RegistryLogFileRecord *record = &database->logs[i];
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            const char *fields[] = {record->path, record->service_id};
            for (size_t column = 0; ok && column < 2; column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "systemd") == 0) {
        const char *headers[] = {"service_id", "unit_name"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->backend_count; i++) {
            const RegistryBackendRecord *record = &database->backends[i];
            if (!record->unit_name || !record->unit_name[0] || (record->unit_path && record->unit_path[0])) continue;
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            const char *fields[] = {record->service_id, record->unit_name};
            for (size_t column = 0; ok && column < 2; column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "launchd") == 0) {
        const char *headers[] = {"service_id", "plist_path", "owns_plist"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->backend_count; i++) {
            const RegistryBackendRecord *record = &database->backends[i];
            if (!record->unit_path || !record->unit_path[0]) continue;
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            const char *fields[] = {record->service_id, record->unit_path, record->owns_unit ? "1" : "0"};
            for (size_t column = 0; ok && column < 3; column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "content-type") == 0 || strcmp(resource, "type") == 0) {
        const char *headers[] = {"identifier", "display_name", "conforms_to", "extensions", "filenames", "mime_types"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < sizeof(kBuiltInContentTypes) / sizeof(kBuiltInContentTypes[0]); i++) {
            const BuiltInContentType *record = &kBuiltInContentTypes[i];
            if (content_type_filter && content_type_filter[0] && strcmp(record->identifier, content_type_filter) != 0) continue;
            const char *fields[] = {record->identifier, record->display_name, record->conforms_to, record->extensions, record->filenames, record->mime_types};
            for (size_t column = 0; ok && column < sizeof(fields) / sizeof(fields[0]); column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
        for (size_t i = 0; ok && i < database->content_type_count; i++) {
            const RegistryContentTypeRecord *record = &database->content_types[i];
            if (content_type_filter && content_type_filter[0] && strcmp(record->identifier, content_type_filter) != 0) continue;
            const char *fields[] = {record->identifier, record->display_name, record->conforms_to, record->extensions, record->filenames, record->mime_types};
            for (size_t column = 0; ok && column < sizeof(fields) / sizeof(fields[0]); column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else if (strcmp(resource, "opener") == 0) {
        const char *headers[] = {"content_type", "service_id", "display_name", "socket_path", "url_template", "rank", "url"};
        ok = outerctl_print_headers(out, headers, sizeof(headers) / sizeof(headers[0]));
        for (size_t i = 0; ok && i < database->opener_count; i++) {
            const RegistryFileOpenerRecord *record = &database->openers[i];
            if (backend_filter && backend_filter[0] && strcmp(record->service_id, backend_filter) != 0) continue;
            if (content_type_filter && content_type_filter[0] && strcmp(record->extension, content_type_filter) != 0) continue;
            char rank_buffer[32];
            snprintf(rank_buffer, sizeof(rank_buffer), "%d", record->rank);
            const char *fields[] = {record->extension, record->service_id, record->display_name, record->socket_path, record->url_template, rank_buffer};
            for (size_t column = 0; ok && column < 6; column++) {
                if (column > 0 && !sb_append(out, "\t")) ok = false;
                if (ok && !outerctl_tsv_field(out, fields[column])) ok = false;
            }
            if (ok && !sb_append(out, "\t")) ok = false;
            if (ok && file_path && file_path[0]) {
                StringBuilder url = {0};
                ok = append_file_opener_url(&url, record->socket_path, record->url_template, file_path) &&
                     outerctl_tsv_field(out, url.data ? url.data : "");
                free(url.data);
            }
            if (ok && !sb_append(out, "\n")) ok = false;
        }
    } else {
        snprintf(error, error_size, "Unknown registry resource.");
        return false;
    }
    if (!ok && !error[0]) snprintf(error, error_size, "out of memory");
    return ok;
}

static bool registry_backend_has_dependencies(RegistryStore *database, const char *backend) {
    const RegistryBackendRecord *record = registry_store_find_backend_const(database, backend);
    if (record && record->unit_name && record->unit_name[0]) return true;
    if (record && record->unit_path && record->unit_path[0]) return true;
    for (size_t i = 0; i < database->log_count; i++) if (strcmp(database->logs[i].service_id, backend) == 0) return true;
    for (size_t i = 0; i < database->frontend_count; i++) if (strcmp(database->frontends[i].service_id, backend) == 0) return true;
    for (size_t i = 0; i < database->opener_count; i++) if (strcmp(database->openers[i].service_id, backend) == 0) return true;
    return false;
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
    const char *content_type = NULL;
    const char *conforms_to = NULL;
    const char *extensions = NULL;
    const char *filenames = NULL;
    const char *mime_types = NULL;
    const char *url_template = NULL;
    const char *file_path = NULL;
    int port = 0;
    int rank = 0;
    bool has_port = false;
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
        } else if (strcmp(arg, "--plist") == 0 || strcmp(arg, "--launchd-plist") == 0) {
            REQUIRE_VALUE("--plist", plist_path);
        } else if (strcmp(arg, "--unit") == 0 || strcmp(arg, "--systemd-unit") == 0) {
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
        } else if (strcmp(arg, "--content-type") == 0 || strcmp(arg, "--type") == 0) {
            REQUIRE_VALUE("--content-type", content_type);
        } else if (strcmp(arg, "--conforms-to") == 0) {
            REQUIRE_VALUE("--conforms-to", conforms_to);
        } else if (strcmp(arg, "--extensions") == 0) {
            REQUIRE_VALUE("--extensions", extensions);
        } else if (strcmp(arg, "--filenames") == 0) {
            REQUIRE_VALUE("--filenames", filenames);
        } else if (strcmp(arg, "--mime-types") == 0) {
            REQUIRE_VALUE("--mime-types", mime_types);
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
        } else {
            sb_append(stderr_buffer, "Unknown argument: ");
            sb_append(stderr_buffer, arg);
            sb_append(stderr_buffer, "\n");
            return 1;
        }
#undef REQUIRE_VALUE
    }

    bool is_list = strcmp(action, "list") == 0;
    bool is_content_type_resource = strcmp(resource, "content-type") == 0 || strcmp(resource, "type") == 0;
    if (!is_list && !is_content_type_resource && (!backend || !backend[0])) {
        sb_append(stderr_buffer, "Missing backend identifier.\n");
        return 1;
    }

    char error[2048] = "";
    RegistryStore database;
    bool opened = is_list
        ? registry_store_open_user_readonly(&database, error, sizeof(error))
        : registry_store_open_user_readwrite(&database, error, sizeof(error));
    if (!opened) {
        sb_append(stderr_buffer, error[0] ? error : "Failed to open registry.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }

    bool ok = true;
    bool changed = false;
    char allowlist_socket_path[PATH_MAX] = "";

    if (is_list) {
        (void)include_icons;
        char normalized_content_type[160] = "";
        if ((strcmp(resource, "opener") == 0 || strcmp(resource, "content-type") == 0 || strcmp(resource, "type") == 0) &&
            content_type && content_type[0]) {
            if (!normalize_content_type_identifier(content_type, normalized_content_type, sizeof(normalized_content_type))) {
                snprintf(error, sizeof(error), "Invalid content type.");
                ok = false;
            }
        }
        if (ok) ok = outerctl_print_registry_list(&database, resource, backend, normalized_content_type[0] ? normalized_content_type : NULL, file_path, stdout_buffer, error, sizeof(error));
        registry_store_free(&database);
        if (!ok) {
            sb_append(stderr_buffer, error[0] ? error : "Failed to list registry rows.");
            sb_append(stderr_buffer, "\n");
            return 1;
        }
        return 0;
    }

    if (ok && strcmp(resource, "backend") == 0) {
        if (strcmp(action, "upsert") == 0) {
            if (icon_path && icon_path[0]) {
                snprintf(error, sizeof(error), "Backend icons are no longer supported. Put icons on app entries instead.");
                ok = false;
            } else if (systemd_unit && systemd_unit[0] && plist_path && plist_path[0]) {
                snprintf(error, sizeof(error), "Specify either --unit or --plist, not both.");
                ok = false;
            } else {
                ok = registry_store_upsert_backend(&database,
                                                   backend,
                                                   (display_name && display_name[0]) ? display_name : backend,
                                                   (systemd_unit && systemd_unit[0]) ? systemd_unit : (plist_path && plist_path[0] ? backend : ""),
                                                   (plist_path && plist_path[0]) ? plist_path : "",
                                                   (systemd_unit && systemd_unit[0]) || owns_plist);
                if (!ok) snprintf(error, sizeof(error), "Out of memory.");
            }
            changed = ok;
        } else if (strcmp(action, "remove") == 0) {
            if (!registry_store_find_backend(&database, backend)) {
                snprintf(error, sizeof(error), "Backend not registered.");
                ok = false;
            }
            if (ok && registry_backend_has_dependencies(&database, backend)) {
                snprintf(error, sizeof(error), "Backend still has service-manager, log, app, or opener records. Clear those first.");
                ok = false;
            }
            if (ok) {
                ok = registry_store_remove_backend(&database, backend);
                changed = ok;
            }
        } else {
            snprintf(error, sizeof(error), "Unknown backend action.");
            ok = false;
        }
    } else if (ok && !is_content_type_resource) {
        bool backend_exists = registry_store_find_backend(&database, backend) != NULL;
        bool can_clear_orphaned_records =
            strcmp(action, "clear") == 0 &&
            (strcmp(resource, "systemd") == 0 ||
             strcmp(resource, "launchd") == 0 ||
             strcmp(resource, "log") == 0 ||
             strcmp(resource, "app") == 0 ||
             strcmp(resource, "opener") == 0);
        if (!backend_exists && !can_clear_orphaned_records) {
            snprintf(error, sizeof(error), "Backend not registered. Run outerctl backend upsert first.");
            ok = false;
        }
    }

    if (ok && strcmp(resource, "systemd") == 0) {
        if (strcmp(action, "set") == 0) {
            snprintf(error, sizeof(error), "Use backend upsert --systemd-unit to set a systemd unit.");
            ok = false;
        } else if (strcmp(action, "clear") == 0) {
            RegistryBackendRecord *record = registry_store_find_backend(&database, backend);
            ok = !record || registry_assign_string(&record->unit_name, "");
            if (!ok) snprintf(error, sizeof(error), "Out of memory.");
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown systemd action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "launchd") == 0) {
        if (strcmp(action, "set") == 0) {
            snprintf(error, sizeof(error), "Use backend upsert --launchd-plist to set a launchd plist.");
            ok = false;
        } else if (strcmp(action, "clear") == 0) {
            RegistryBackendRecord *record = registry_store_find_backend(&database, backend);
            ok = !record ||
                 (registry_assign_string(&record->unit_name, "") &&
                  registry_assign_string(&record->unit_path, ""));
            if (!ok) snprintf(error, sizeof(error), "Out of memory.");
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
                ok = registry_store_upsert_log(&database, log_path, backend);
                if (!ok) snprintf(error, sizeof(error), "Out of memory.");
                changed = ok;
            }
        } else if (strcmp(action, "remove") == 0) {
            if (!log_path || !log_path[0]) {
                snprintf(error, sizeof(error), "Missing log path.");
                ok = false;
            } else {
                for (size_t i = database.log_count; i > 0; i--) {
                    if (strcmp(database.logs[i - 1].service_id, backend) == 0 &&
                        strcmp(database.logs[i - 1].path, log_path) == 0) {
                        registry_store_remove_log_at(&database, i - 1);
                    }
                }
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            registry_store_clear_backend_logs(&database, backend);
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
            if (!display_name || !display_name[0]) {
                snprintf(error, sizeof(error), "Missing app display name.");
                ok = false;
            }
            char frontend_url[PATH_MAX * 2] = "";
            if (ok && (has_port || has_socket || (url && url[0])) &&
                !outerctl_make_frontend_url(url, port, socket_path, frontend_url, sizeof(frontend_url))) {
                snprintf(error, sizeof(error), "Could not build app URL.");
                ok = false;
            }
            if (ok) {
                ok = registry_store_upsert_frontend(&database,
                                                    stable_frontend_id,
                                                    frontend_url,
                                                    backend,
                                                    display_name,
                                                    port,
                                                    has_socket ? socket_path : "",
                                                    icon_path ? icon_path : "",
                                                    frontend_list ? frontend_list : "",
                                                    true);
                if (!ok) snprintf(error, sizeof(error), "Out of memory.");
            }
            if (ok && has_socket) {
                snprintf(allowlist_socket_path, sizeof(allowlist_socket_path), "%s", socket_path);
            }
            if (ok) {
                const char *layout_key = (has_socket && (!url || !url[0]))
                    ? stable_frontend_id
                    : (frontend_url[0] ? frontend_url : stable_frontend_id);
                ok = registry_store_upsert_layout(&database,
                                                  layout_key,
                                                  frontend_list ? frontend_list : "");
                if (!ok) snprintf(error, sizeof(error), "Out of memory.");
            }
            changed = ok;
        } else if (strcmp(action, "remove") == 0) {
            if (!frontend_id && !has_port && !has_socket) {
                snprintf(error, sizeof(error), "Missing app endpoint.");
                ok = false;
            } else if (frontend_id && frontend_id[0]) {
                for (size_t i = database.frontend_count; i > 0; i--) {
                    if (strcmp(database.frontends[i - 1].service_id, backend) == 0 &&
                        strcmp(database.frontends[i - 1].frontend_id, frontend_id) == 0) {
                        registry_store_remove_frontend_at(&database, i - 1);
                    }
                }
                changed = ok;
            } else if (has_socket) {
                for (size_t i = database.frontend_count; i > 0; i--) {
                    if (strcmp(database.frontends[i - 1].service_id, backend) == 0 &&
                        strcmp(database.frontends[i - 1].socket_path, socket_path) == 0) {
                        registry_store_remove_frontend_at(&database, i - 1);
                    }
                }
                changed = ok;
            } else {
                for (size_t i = database.frontend_count; i > 0; i--) {
                    if (strcmp(database.frontends[i - 1].service_id, backend) == 0 &&
                        database.frontends[i - 1].port == port) {
                        registry_store_remove_frontend_at(&database, i - 1);
                    }
                }
                changed = ok;
            }
        } else if (strcmp(action, "clear") == 0) {
            registry_store_clear_backend_frontends(&database, backend);
            changed = ok;
        } else {
            snprintf(error, sizeof(error), "Unknown app action.");
            ok = false;
        }
    } else if (ok && is_content_type_resource) {
        char normalized_content_type[160] = "";
        if (strcmp(action, "clear") != 0) {
            if (!content_type || !content_type[0]) {
                snprintf(error, sizeof(error), "Missing content type.");
                ok = false;
            } else if (!normalize_content_type_identifier(content_type, normalized_content_type, sizeof(normalized_content_type))) {
                snprintf(error, sizeof(error), "Invalid content type.");
                ok = false;
            }
        }
        if (ok && strcmp(action, "add") == 0) {
            ok = registry_store_upsert_content_type(&database,
                                                    normalized_content_type,
                                                    (display_name && display_name[0]) ? display_name : normalized_content_type,
                                                    conforms_to ? conforms_to : "",
                                                    extensions ? extensions : "",
                                                    filenames ? filenames : "",
                                                    mime_types ? mime_types : "");
            if (!ok) snprintf(error, sizeof(error), "Out of memory.");
            changed = ok;
        } else if (ok && strcmp(action, "remove") == 0) {
            for (size_t i = database.content_type_count; i > 0; i--) {
                if (strcmp(database.content_types[i - 1].identifier, normalized_content_type) == 0) {
                    registry_store_remove_content_type_at(&database, i - 1);
                }
            }
            changed = ok;
        } else if (ok && strcmp(action, "clear") == 0) {
            registry_store_clear_content_types(&database);
            changed = ok;
        } else if (ok) {
            snprintf(error, sizeof(error), "Unknown content-type action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "opener") == 0) {
        char opener_key[160] = "";
        if (strcmp(action, "clear") != 0) {
            if (!content_type || !content_type[0]) {
                snprintf(error, sizeof(error), "Missing opener content type.");
                ok = false;
            } else if (!normalize_content_type_identifier(content_type, opener_key, sizeof(opener_key))) {
                snprintf(error, sizeof(error), "Invalid content type.");
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
                ok = registry_store_upsert_opener(&database,
                                                  opener_key,
                                                  backend,
                                                  display_name,
                                                  socket_path,
                                                  (url_template && url_template[0]) ? url_template : "?file={file}",
                                                  rank);
                if (!ok) snprintf(error, sizeof(error), "Out of memory.");
                if (ok) snprintf(allowlist_socket_path, sizeof(allowlist_socket_path), "%s", socket_path);
                changed = ok;
            }
        } else if (ok && strcmp(action, "remove") == 0) {
            for (size_t i = database.opener_count; i > 0; i--) {
                if (strcmp(database.openers[i - 1].service_id, backend) == 0 &&
                    strcmp(database.openers[i - 1].extension, opener_key) == 0) {
                    registry_store_remove_opener_at(&database, i - 1);
                }
            }
            changed = ok;
        } else if (ok && strcmp(action, "clear") == 0) {
            registry_store_clear_backend_openers(&database, backend);
            changed = ok;
        } else if (ok) {
            snprintf(error, sizeof(error), "Unknown opener action.");
            ok = false;
        }
    } else if (ok && strcmp(resource, "backend") != 0) {
        snprintf(error, sizeof(error), "Unknown registry resource.");
        ok = false;
    }

    bool close_ok = registry_store_close(&database, ok, error, sizeof(error));
    ok = ok && close_ok;
    if (!ok) {
        sb_append(stderr_buffer, error[0] ? error : "Registry operation failed.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }
    if (allowlist_socket_path[0] &&
        !append_outerloop_http_unix_allowlist_entry_for_current_scope(allowlist_socket_path, error, sizeof(error))) {
        sb_append(stderr_buffer, error[0] ? error : "Failed to update Outer Loop Unix socket allowlist.");
        sb_append(stderr_buffer, "\n");
        return 1;
    }
    if (changed) mark_backend_event_changed();
    return 0;
}

static bool registry_store_upsert_bundled_app_openers(RegistryStore *database,
                                                      const BundledAppDefinition *app,
                                                      const char *socket_path,
                                                      char *error,
                                                      size_t error_size) {
    if (!database || !app) return true;
    registry_store_clear_backend_openers(database, app->service_id);
    if (!app->openers || app->opener_count == 0) return true;
    if (!socket_path || !socket_path[0]) {
        snprintf(error, error_size, "Missing opener socket path.");
        return false;
    }
    for (size_t i = 0; i < app->opener_count; i++) {
        char normalized_content_type[160] = "";
        if (!normalize_content_type_identifier(app->openers[i].content_type, normalized_content_type, sizeof(normalized_content_type))) {
            snprintf(error, error_size, "Invalid bundled opener content type.");
            return false;
        }
        if (!registry_store_upsert_opener(database,
                                          normalized_content_type,
                                          app->service_id,
                                          app->display_name,
                                          socket_path,
                                          app->openers[i].url_template,
                                          app->openers[i].rank)) {
            snprintf(error, error_size, "Out of memory.");
            return false;
        }
    }
    return true;
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
    RegistryStore database;
    if (!registry_store_open_user_readwrite(&database, error, error_size)) return false;
    bool ok = registry_store_upsert_backend(&database, service_id, display_name, unit_name, "", true);
    if (!ok) snprintf(error, error_size, "Out of memory.");
    if (ok) registry_store_clear_backend_frontends(&database, service_id);
    if (ok && socket_path && socket_path[0]) {
        char frontend_id[PATH_MAX * 2];
        snprintf(frontend_id, sizeof(frontend_id), "%s:main", service_id);
        char *icon_value = registry_icon_path_value(icon_path);
        ok = registry_store_upsert_frontend(&database, frontend_id, "", service_id, display_name, 0, socket_path, icon_value ? icon_value : "", "", false);
        free(icon_value);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    if (ok) registry_store_clear_backend_logs(&database, service_id);
    if (ok) {
        ok = registry_store_upsert_log(&database, log_path, service_id);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
    if (ok && app) {
        ok = registry_store_upsert_bundled_app_openers(&database, app, socket_path, error, error_size);
    }
    ok = registry_store_close(&database, ok, error, error_size) && ok;
    if (ok && socket_path && socket_path[0]) {
        ok = append_outerloop_http_unix_allowlist_entry(socket_path,
                                                        scope && strcmp(scope, "system") == 0,
                                                        error,
                                                        error_size);
    }
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
    RegistryStore database;
    if (!registry_store_open_at(&database, database_path, true, error, error_size)) return false;
    bool ok = registry_store_upsert_backend(&database, service_id, display_name, service_id, plist_path, true);
    if (!ok) snprintf(error, error_size, "Out of memory.");
    if (ok) registry_store_clear_backend_frontends(&database, service_id);
    if (ok && socket_path && socket_path[0]) {
        char frontend_id[PATH_MAX * 2];
        snprintf(frontend_id, sizeof(frontend_id), "%s:main", service_id);
        char *icon_value = registry_icon_path_value(icon_path);
        ok = registry_store_upsert_frontend(&database, frontend_id, "", service_id, display_name, 0, socket_path, icon_value ? icon_value : "", "", false);
        free(icon_value);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    if (ok) registry_store_clear_backend_logs(&database, service_id);
    if (ok) {
        ok = registry_store_upsert_log(&database, log_path, service_id);
        if (!ok) snprintf(error, error_size, "Out of memory.");
    }
    const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
    if (ok && app) {
        ok = registry_store_upsert_bundled_app_openers(&database, app, socket_path, error, error_size);
    }
    ok = registry_store_close(&database, ok, error, error_size) && ok;
    if (ok && socket_path && socket_path[0]) {
        bool system_scope = database_path && strcmp(database_path, g_system_registry_database_path) == 0;
        ok = append_outerloop_http_unix_allowlist_entry(socket_path, system_scope, error, error_size);
    }
    return ok;
}
#endif

static bool unregister_backend_records(const char *service_id, char *error, size_t error_size) {
    RegistryStore database;
    if (!registry_store_open_user_readwrite(&database, error, error_size)) return false;
    registry_store_clear_backend_openers(&database, service_id);
    registry_store_clear_backend_frontends(&database, service_id);
    registry_store_clear_backend_logs(&database, service_id);
    (void)registry_store_remove_backend(&database, service_id);
    return registry_store_close(&database, true, error, error_size);
}

static bool uninstall_local_home_screen(const char *sudo_password,
                                        bool *needs_password,
                                        bool remove_user_state,
                                        char *message,
                                        size_t message_size) {
#ifdef __APPLE__
    if (needs_password) *needs_password = false;
    char error[1024] = "";

    char outershell_root[PATH_MAX];
    char install_root[PATH_MAX];
    char outerctl_path[PATH_MAX];
    char cache_root[PATH_MAX];
    char outer_shell_cache_root[PATH_MAX];
    char apps_root[PATH_MAX];
    default_user_outershell_root(outershell_root, sizeof(outershell_root));
    default_outershell_install_root(install_root, sizeof(install_root));
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));
    default_user_outershell_cache_root(cache_root, sizeof(cache_root));
    default_outer_shell_cache_root(outer_shell_cache_root, sizeof(outer_shell_cache_root));
    default_user_outershell_apps_root(apps_root, sizeof(apps_root));

    const char *home = home_directory();
    char plist_path[PATH_MAX] = "";
    if (home[0]) {
        snprintf(plist_path, sizeof(plist_path), "%s/Library/LaunchAgents/org.outershell.OuterShell.plist", home);
    }
    char socket_path[PATH_MAX] = "";
    snprintf(socket_path, sizeof(socket_path), "%s", g_listen_socket_path);
    char api_socket_path[PATH_MAX] = "";
    snprintf(api_socket_path, sizeof(api_socket_path), "%s", g_api_socket_path);

    char system_cleanup_message[1024] = "";
    if (remove_user_state) {
        char script_template[] = "/tmp/outershell-system-cleanup-XXXXXX";
        int script_fd = mkstemp(script_template);
        if (script_fd < 0) {
            snprintf(message, message_size, "Failed to create system cleanup script: %s", strerror(errno));
            return false;
        }
        FILE *script = fdopen(script_fd, "w");
        if (!script) {
            close(script_fd);
            unlink(script_template);
            snprintf(message, message_size, "Failed to write system cleanup script: %s", strerror(errno));
            return false;
        }
        char quoted_system_root[PATH_MAX + 8];
        shell_quote(kSystemOuterShellRoot, quoted_system_root, sizeof(quoted_system_root));
        fprintf(script,
                "set -eu\n"
                "root=%s\n"
                "apps=\"$root/apps\"\n"
                "if [ ! -d \"$apps\" ] || ! find \"$apps\" -mindepth 1 -print -quit 2>/dev/null | grep -q .; then\n"
                "  rm -f \"$root/registry.orwa\" \"$root/registry.orwa.lock\"\n"
                "  rm -f /usr/local/libexec/outershelld-root-tool /usr/local/libexec/outershelld-root-helper\n"
                "  rm -f \"$root/bin/outerctl\"\n"
                "  rm -rf \"$root/outershelld\"\n"
                "  rmdir \"$apps\" \"$root/bin\" \"$root\" >/dev/null 2>&1 || true\n"
                "fi\n",
                quoted_system_root);
        fclose(script);
        chmod(script_template, 0700);

        bool root_ok = run_root_script(script_template, sudo_password, needs_password, system_cleanup_message, sizeof(system_cleanup_message));
        unlink(script_template);
        if (!root_ok && needs_password && *needs_password) {
            snprintf(message, message_size, "%s", system_cleanup_message[0] ? system_cleanup_message : "Administrator password required.");
            return false;
        }
        if (!root_ok) {
            log_event("Outer Shell system cleanup skipped: %s", system_cleanup_message[0] ? system_cleanup_message : "unknown error");
        }
    }

    unlink_advertised_home_screen_socket();

    pid_t parent_pid = getpid();
    pid_t child = fork();
    if (child < 0) {
        snprintf(message, message_size, "Failed to start uninstall cleanup process: %s", strerror(errno));
        return false;
    }
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
        if (plist_path[0]) {
            unlink(plist_path);
        }
        if (socket_path[0]) {
            unlink(socket_path);
        }
        if (api_socket_path[0]) {
            unlink(api_socket_path);
        }
        if (remove_user_state) {
            char quoted_install_root[PATH_MAX + 8];
            char quoted_outerctl_path[PATH_MAX + 8];
            char quoted_registry[PATH_MAX + 32];
            char quoted_registry_lock[PATH_MAX + 32];
            char quoted_apps_root[PATH_MAX + 8];
            char quoted_bin_root[PATH_MAX + 8];
            char quoted_outershell_root[PATH_MAX + 8];
            char quoted_outer_shell_cache_root[PATH_MAX + 8];
            char quoted_cache_root[PATH_MAX + 8];
            char bin_root[PATH_MAX];
            snprintf(bin_root, sizeof(bin_root), "%s/bin", outershell_root);
            char registry_path[PATH_MAX];
            char registry_lock_path[PATH_MAX];
            snprintf(registry_path, sizeof(registry_path), "%s/registry.orwa", outershell_root);
            snprintf(registry_lock_path, sizeof(registry_lock_path), "%s/registry.orwa.lock", outershell_root);
            shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
            shell_quote(outerctl_path, quoted_outerctl_path, sizeof(quoted_outerctl_path));
            shell_quote(registry_path, quoted_registry, sizeof(quoted_registry));
            shell_quote(registry_lock_path, quoted_registry_lock, sizeof(quoted_registry_lock));
            shell_quote(apps_root, quoted_apps_root, sizeof(quoted_apps_root));
            shell_quote(bin_root, quoted_bin_root, sizeof(quoted_bin_root));
            shell_quote(outershell_root, quoted_outershell_root, sizeof(quoted_outershell_root));
            shell_quote(outer_shell_cache_root, quoted_outer_shell_cache_root, sizeof(quoted_outer_shell_cache_root));
            shell_quote(cache_root, quoted_cache_root, sizeof(quoted_cache_root));

            char cleanup_command[PATH_MAX * 8];
            snprintf(cleanup_command,
                     sizeof(cleanup_command),
                     "rm -rf -- %s %s; rm -f -- %s %s %s; rmdir -- %s %s %s %s >/dev/null 2>&1 || true",
                     quoted_install_root,
                     quoted_outer_shell_cache_root,
                     quoted_outerctl_path,
                     quoted_registry,
                     quoted_registry_lock,
                     quoted_apps_root,
                     quoted_bin_root,
                     quoted_outershell_root,
                     quoted_cache_root);
            run_shell_ignored(cleanup_command);
        } else {
            char quoted_install_root[PATH_MAX + 8];
            char quoted_outer_shell_cache_root[PATH_MAX + 8];
            shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
            shell_quote(outer_shell_cache_root, quoted_outer_shell_cache_root, sizeof(quoted_outer_shell_cache_root));
            char cleanup_command[PATH_MAX * 4];
            snprintf(cleanup_command,
                     sizeof(cleanup_command),
                     "rm -rf -- %s; rm -rf -- %s/install; rmdir -- %s >/dev/null 2>&1 || true",
                     quoted_install_root,
                     quoted_outer_shell_cache_root,
                     quoted_outer_shell_cache_root);
            run_shell_ignored(cleanup_command);
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

    bool registry_ok = unregister_backend_records(kOuterShellServiceID, error, sizeof(error));
    if (!registry_ok) {
        snprintf(message,
                 message_size,
                 "Removed the Outer Shell LaunchAgent. Registry cleanup failed: %s",
                 error[0] ? error : "unknown error");
        return true;
    }
    snprintf(message, message_size, "Outer Shell uninstalled. The app will stop momentarily.");
    return true;
#else
    (void)sudo_password;
    (void)needs_password;
    (void)remove_user_state;
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
    char actual_socket_path[PATH_MAX] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit, sizeof(socket_unit));
        if (safe_unit_name(socket_unit)) {
            bundled_socket_path_for_scope(app, "user", actual_socket_path, sizeof(actual_socket_path));
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
    if (actual_socket_path[0]) {
        unlink(actual_socket_path);
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
        default_user_outershell_app_root(app->install_directory_name, install_root, sizeof(install_root));
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

    char quoted_socket_path[PATH_MAX + 32];
    char actual_socket_path[PATH_MAX] = "";
    char systemd_socket_path[PATH_MAX] = "";
    if (app->socket_name && app->socket_name[0]) {
        snprintf(systemd_socket_path, sizeof(systemd_socket_path), "%%t/%s", app->socket_name);
        shell_quote(systemd_socket_path, quoted_socket_path, sizeof(quoted_socket_path));
        bundled_socket_path_for_scope(app, "user", actual_socket_path, sizeof(actual_socket_path));
    } else {
        quoted_socket_path[0] = '\0';
    }

    char exec_start[PATH_MAX * 6];
    bundled_systemd_exec_start(binary_path,
                               app->service_id,
                               systemd_socket_path,
                               bundles_dir,
                               icon_path,
                               exec_start,
                               sizeof(exec_start));

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
             "ExecStart=%s\n"
             "Restart=on-failure\n"
             "KillMode=control-group\n"
             "StandardOutput=append:%s\n"
             "StandardError=append:%s\n"
             "\n"
             "[Install]\n"
             "WantedBy=default.target\n",
             description,
             working_directory,
             home_directory(),
             user_name,
             user_name,
             outerctl_path,
             exec_start,
             log_path,
             log_path);
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
                                      const char *requested_stage_root,
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

static bool install_bundled_app(const BundledAppDefinition *app,
                                const char *scope,
                                const char *requested_stage_root,
                                const char *sudo_password,
                                bool *needs_password,
                                char *message,
                                size_t message_size) {
#ifdef __APPLE__
    return install_bundled_app_macos(app, scope, requested_stage_root, sudo_password, needs_password, message, message_size);
#else
    if (needs_password) *needs_password = false;
    if (!app) {
        snprintf(message, message_size, "Unknown app.");
        return false;
    }
    bool install_as_root = direct_root_session_uses_system_scope() || (scope && strcmp(scope, "system") == 0);
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
    if (!resolve_bundled_app_stage_root(app, requested_stage_root, stage_root, sizeof(stage_root), message, message_size)) {
        return false;
    }
    char source_binary[PATH_MAX];
    snprintf(source_binary, sizeof(source_binary), "%s/RemoteLinuxBinaries/%s/%s", stage_root, architecture, app->binary_name);
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
    if (!has_source_binary) {
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
        const bool direct_root_install = direct_root_session_uses_system_scope();
        if (!direct_root_install && !ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
            return false;
        }

        char user_name[128] = "";
        struct passwd *pw = getpwuid(getuid());
        snprintf(user_name, sizeof(user_name), "%s", pw && pw->pw_name ? pw->pw_name : "");

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
        default_system_outerctl_path(wrapper_path, sizeof(wrapper_path));
        char system_users_dir[PATH_MAX];
        char root_apps_marker[PATH_MAX];
        system_binary_users_dir(system_users_dir, sizeof(system_users_dir));
        system_binary_root_apps_marker_path(root_apps_marker, sizeof(root_apps_marker));
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

        if (!direct_root_install) {
            cleanup_user_systemd_bundled_app(app, true, false);
        }

        char quoted_unit[320];
        shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));

        const char *binary_source = source_binary;
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
        char quoted_system_users_dir[PATH_MAX + 8];
        char quoted_root_apps_marker[PATH_MAX + 8];
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
        shell_quote(system_users_dir, quoted_system_users_dir, sizeof(quoted_system_users_dir));
        shell_quote(root_apps_marker, quoted_root_apps_marker, sizeof(quoted_root_apps_marker));
        shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
        shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
        if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
        if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';

        char quoted_display_name[512];
        char quoted_target_icon_for_registry[PATH_MAX + 8];
        char quoted_socket_path[PATH_MAX + 32];
        char actual_socket_path[PATH_MAX] = "";
        char systemd_socket_path[PATH_MAX] = "";
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

        char exec_start[PATH_MAX * 6];
        bundled_systemd_exec_start(target_binary,
                                   app->service_id,
                                   systemd_socket_path,
                                   bundles_dir,
                                   target_icon,
                                   exec_start,
                                   sizeof(exec_start));

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
                 "Environment=OUTERSHELLD_API_SOCKET=/run/outershelld-api\n"
                 "ExecStart=%s\n"
                 "Restart=on-failure\n"
                 "KillMode=control-group\n"
                 "StandardOutput=append:%s\n"
                 "StandardError=append:%s\n"
                 "\n"
                 "[Install]\n"
                 "WantedBy=multi-user.target\n",
                 description,
                 install_root,
                 home_directory(),
                 user_name,
                 user_name,
                 wrapper_path,
                 exec_start,
                 log_path,
                 log_path);

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

        char quoted_system_outershell_root[PATH_MAX + 8];
        shell_quote(kSystemOuterShellRoot, quoted_system_outershell_root, sizeof(quoted_system_outershell_root));

        char script_template[] = "/tmp/backends-root-install-XXXXXX";
        int script_fd = mkstemp(script_template);
        if (script_fd < 0) {
            snprintf(message, message_size, "Failed to create privileged install script: %s", strerror(errno));
            return false;
        }
        FILE *script = fdopen(script_fd, "w");
        if (!script) {
            close(script_fd);
            unlink(script_template);
            snprintf(message, message_size, "Failed to write privileged install script: %s", strerror(errno));
            return false;
        }
        fprintf(script,
                "set -eu\n"
                "timeout 12s systemctl --system disable --now outerloop-rootd.service >/dev/null 2>&1 || true\n"
                "rm -f /etc/systemd/system/outerloop-rootd.service\n"
                "systemctl --system daemon-reload >/dev/null 2>&1 || true\n"
                "timeout 5s systemctl --system reset-failed outerloop-rootd.service >/dev/null 2>&1 || true\n"
                "timeout 12s systemctl --system stop %s >/dev/null 2>&1 || true\n"
                "timeout 5s systemctl --system reset-failed %s >/dev/null 2>&1 || true\n"
                "rm -rf -- %s\n"
                "mkdir -p %s %s /var/log/outergroup %s\n"
                "chmod 0755 %s\n"
                "install -m 0755 %s %s\n"
                "install -m 0644 %s %s\n"
                "install -m 0644 %s %s\n",
                quoted_unit,
                quoted_unit,
                quoted_bundles_dir,
                quoted_install_root,
                quoted_bundles_dir,
                quoted_system_outershell_root,
                quoted_system_outershell_root,
                quoted_binary_source,
                quoted_target_binary,
                quoted_source_bundle_arm,
                quoted_target_bundle_arm,
                quoted_source_bundle_x86,
                quoted_target_bundle_x86);
        fprintf(script,
                "mkdir -p %s\n"
                "chmod 1777 %s\n"
                "touch %s\n"
                "chown 0:0 %s 2>/dev/null || true\n",
                quoted_system_users_dir,
                quoted_system_users_dir,
                quoted_root_apps_marker,
                quoted_root_apps_marker);
        if (quoted_socket_unit[0]) {
            fprintf(script,
                    "timeout 12s systemctl --system disable --now %s >/dev/null 2>&1 || true\n"
                    "timeout 12s systemctl --system disable %s >/dev/null 2>&1 || true\n"
                    "timeout 12s systemctl --system stop %s >/dev/null 2>&1 || true\n"
                    "timeout 5s systemctl --system reset-failed %s >/dev/null 2>&1 || true\n"
                    "%s%s%s",
                    quoted_socket_unit,
                    quoted_unit,
                    quoted_unit,
                    quoted_socket_unit,
                    quoted_actual_socket_path[0] ? "rm -f -- " : "",
                    quoted_actual_socket_path[0] ? quoted_actual_socket_path : "",
                    quoted_actual_socket_path[0] ? "\n" : "");
        }
        if (quoted_source_icon[0] && quoted_target_icon[0]) {
            fprintf(script, "install -m 0644 %s %s\n", quoted_source_icon, quoted_target_icon);
        }
        fprintf(script,
                "cat > %s <<'__BACKENDS_VERSION__'\n%s\n__BACKENDS_VERSION__\n"
                "chmod 0644 %s\n"
                "cat > %s <<'__BACKENDS_UNIT__'\n%s__BACKENDS_UNIT__\n"
                "chmod 0644 %s\n"
                "touch %s\n"
                "chmod 0644 %s\n"
                "chmod 0666 %s/registry.orwa.lock 2>/dev/null || true\n"
                "systemctl --system daemon-reload\n",
                quoted_version_path,
                app->version,
                quoted_version_path,
                quoted_unit_path,
                unit_contents,
                quoted_unit_path,
                quoted_log_path,
                quoted_log_path,
                quoted_system_outershell_root);
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
        if (!root_ok) return false;

        if (direct_root_install) {
            if (!upsert_systemd_backend_registry(app->service_id,
                                                 app->display_name,
                                                 app->unit_name,
                                                 "system",
                                                 actual_socket_path,
                                                 log_path,
                                                 target_icon,
                                                 error,
                                                 sizeof(error))) {
                snprintf(message, message_size, "%s", error);
                return false;
            }
        } else {
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
            if (!root_helper_registry_upsert_bundled_openers(app,
                                                             actual_socket_path,
                                                             sudo_password,
                                                             needs_password,
                                                             message,
                                                             message_size)) {
                return false;
            }
        }

        if (!app->root_only && !direct_root_install) {
            char user_state_root[PATH_MAX];
            default_user_outershell_app_root(app->install_directory_name, user_state_root, sizeof(user_state_root));
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

        snprintf(message,
                 message_size,
                 direct_root_install ? "Installed %s." : "Installed %s with root support.",
                 app->display_name);
        return true;
    }

    char install_root[PATH_MAX];
    default_user_outershell_app_root(app->install_directory_name, install_root, sizeof(install_root));
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
    if (!copy_file(source_binary, target_binary, 0700, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
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

    char quoted_socket_path[PATH_MAX + 32];
    char actual_socket_path[PATH_MAX] = "";
    char systemd_socket_path[PATH_MAX] = "";
    if (app->socket_name && app->socket_name[0]) {
        snprintf(systemd_socket_path, sizeof(systemd_socket_path), "%%t/%s", app->socket_name);
        shell_quote(systemd_socket_path, quoted_socket_path, sizeof(quoted_socket_path));
        bundled_socket_path_for_scope(app, "user", actual_socket_path, sizeof(actual_socket_path));
    } else {
        quoted_socket_path[0] = '\0';
    }

    char exec_start[PATH_MAX * 6];
    bundled_systemd_exec_start(target_binary,
                               app->service_id,
                               systemd_socket_path,
                               bundles_dir,
                               target_icon,
                               exec_start,
                               sizeof(exec_start));

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
             "ExecStart=%s\n"
             "Restart=on-failure\n"
             "KillMode=control-group\n"
             "StandardOutput=append:%s\n"
             "StandardError=append:%s\n"
             "\n"
             "[Install]\n"
             "WantedBy=default.target\n",
             description,
             install_root,
             home_directory(),
             user_name,
             user_name,
             outerctl_path,
             exec_start,
             log_path,
             log_path);
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
    char socket_path[PATH_MAX] = "";
    if (app->socket_activated) {
        systemd_socket_unit_name(app->unit_name, socket_unit_name, sizeof(socket_unit_name));
        snprintf(socket_unit_path, sizeof(socket_unit_path), "/etc/systemd/system/%s", socket_unit_name);
        bundled_socket_path_for_scope(app, "system", socket_path, sizeof(socket_path));
    }

    char quoted_unit[320];
    char quoted_install_root[PATH_MAX + 8];
    char quoted_log_path[PATH_MAX + 8];
    char quoted_unit_path[PATH_MAX + 8];
    char quoted_socket_unit[320] = "";
    char quoted_socket_unit_path[PATH_MAX + 8] = "";
    char quoted_socket_path[PATH_MAX + 8] = "";
    shell_quote(app->unit_name, quoted_unit, sizeof(quoted_unit));
    shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
    shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
    shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
    if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
    if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';
    if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path)); else quoted_socket_path[0] = '\0';

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
            "rm -f -- %s %s %s\n"
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
            quoted_socket_path[0] ? quoted_socket_path : "''",
            quoted_install_root,
            quoted_log_path);
    write_root_apps_marker_cleanup_shell(script);
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
    snprintf(install_root, sizeof(install_root), "%s/apps/%s", kSystemOuterShellRoot, app->install_directory_name);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "/Library/Logs/%s.log", app->service_id);
    char plist_path[PATH_MAX];
    snprintf(plist_path, sizeof(plist_path), "/Library/LaunchDaemons/%s.plist", app->service_id);
    char socket_path[PATH_MAX] = "";
    bundled_socket_path_for_scope(app, "system", socket_path, sizeof(socket_path));

    char target[320];
    char quoted_target[384];
    char quoted_install_root[PATH_MAX + 8];
    char quoted_log_path[PATH_MAX + 8];
    char quoted_plist_path[PATH_MAX + 8];
    char quoted_socket_path[PATH_MAX + 8] = "";
    snprintf(target, sizeof(target), "system/%s", app->service_id);
    shell_quote(target, quoted_target, sizeof(quoted_target));
    shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
    shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
    shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));
    if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path));

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
            "launchctl bootout %s >/dev/null 2>&1 || true\n"
            "rm -f -- %s %s\n"
            "rm -rf -- %s\n"
            "rm -f -- %s\n",
            quoted_target,
            quoted_plist_path,
            quoted_socket_path[0] ? quoted_socket_path : "''",
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
    if (bundles_dir && bundles_dir[0]) {
        ok = ok && append_xml_string_element(builder, "--bundles-dir");
        ok = ok && append_xml_string_element(builder, bundles_dir);
    }
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
                         "        <key>OUTERSHELL_LOG_PATH</key>\n"
                         "        <string>");
    ok = ok && sb_append_xml_escaped(builder, log_path);
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
                         "</dict>\n"
                         "</plist>\n");
    return ok;
}

static bool install_bundled_app_user_launchagent_for_system_payload(const BundledAppDefinition *app,
                                                                    char *message,
                                                                    size_t message_size) {
    if (!app) {
        snprintf(message, message_size, "Missing app definition.");
        return false;
    }

    char system_install_root[PATH_MAX];
    snprintf(system_install_root, sizeof(system_install_root), "%s/apps/%s", kSystemOuterShellRoot, app->install_directory_name);
    char system_app_bundle[PATH_MAX];
    bundled_app_macos_app_bundle_path(app, system_install_root, system_app_bundle, sizeof(system_app_bundle));
    bool uses_app_bundle = bundled_app_macos_app_has_expected_files(app, system_app_bundle);
    char bundles_dir[PATH_MAX];
    if (uses_app_bundle) {
        bundled_app_macos_app_bundles_dir(system_app_bundle, bundles_dir, sizeof(bundles_dir));
    } else {
        snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", system_install_root);
    }
    char target_binary[PATH_MAX];
    if (uses_app_bundle) {
        bundled_app_macos_app_binary_path(app, system_app_bundle, target_binary, sizeof(target_binary));
    } else {
        snprintf(target_binary, sizeof(target_binary), "%s/%s", system_install_root, app->binary_name);
    }
    char bundle_arm[PATH_MAX];
    snprintf(bundle_arm, sizeof(bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
    char bundle_x86[PATH_MAX];
    snprintf(bundle_x86, sizeof(bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
    char target_icon[PATH_MAX] = "";
    if (app->icon_name && app->icon_name[0]) {
        if (uses_app_bundle) {
            bundled_app_macos_app_icon_path(app, system_app_bundle, target_icon, sizeof(target_icon));
        } else {
            snprintf(target_icon, sizeof(target_icon), "%s/%s", system_install_root, app->icon_name);
        }
    }

    struct stat st;
    if (stat(target_binary, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing root-installed %s backend at %s.", app->display_name, target_binary);
        return false;
    }
    if (stat(bundle_arm, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(bundle_x86, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(message, message_size, "Missing root-installed %s content archives under %s.", app->display_name, bundles_dir);
        return false;
    }
    if (target_icon[0] && (stat(target_icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        snprintf(message, message_size, "Missing root-installed %s icon at %s.", app->display_name, target_icon);
        return false;
    }

    char socket_path[PATH_MAX] = "";
    bundled_socket_path_for_scope(app, "user", socket_path, sizeof(socket_path));
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), app->service_id);
    char plist_path[PATH_MAX];
    snprintf(plist_path, sizeof(plist_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), app->service_id);
    char outerctl_path[PATH_MAX];
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));

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

    char user_install_root[PATH_MAX];
    default_user_outershell_app_root(app->install_directory_name, user_install_root, sizeof(user_install_root));
    char quoted_user_install_root[PATH_MAX + 8];
    shell_quote(user_install_root, quoted_user_install_root, sizeof(quoted_user_install_root));
    char cleanup_command[PATH_MAX + 64];
    snprintf(cleanup_command, sizeof(cleanup_command), "rm -rf -- %s", quoted_user_install_root);
    run_shell_ignored(cleanup_command);

    StringBuilder plist = {0};
    if (!make_bundled_launchd_plist(app->service_id,
                                    target_binary,
                                    uses_app_bundle ? "" : bundles_dir,
                                    uses_app_bundle ? "" : target_icon,
                                    socket_path,
                                    0600,
                                    uses_app_bundle ? system_app_bundle : system_install_root,
                                    outerctl_path,
                                    log_path,
                                    &plist)) {
        free(plist.data);
        snprintf(message, message_size, "Failed to generate LaunchAgent plist.");
        return false;
    }

    char error[1024] = "";
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

    char quoted_plist[PATH_MAX + 8];
    shell_quote(plist_path, quoted_plist, sizeof(quoted_plist));
    char bootstrap_message[4096] = "";
    char bootstrap_command[PATH_MAX + 128];
    snprintf(bootstrap_command, sizeof(bootstrap_command), "launchctl bootstrap gui/%d %s 2>&1", (int)getuid(), quoted_plist);
    if (!run_launchctl_capture(bootstrap_command, bootstrap_message, sizeof(bootstrap_message))) {
        snprintf(message, message_size, "Installed %s user LaunchAgent, but failed to bootstrap its socket: %s",
                 app->display_name,
                 bootstrap_message);
        return false;
    }

    snprintf(message, message_size, "Installed %s user LaunchAgent using the root payload.", app->display_name);
    return true;
}

static bool install_bundled_app_macos(const BundledAppDefinition *app,
                                      const char *scope,
                                      const char *requested_stage_root,
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
    if (!resolve_bundled_app_stage_root(app, requested_stage_root, stage_root, sizeof(stage_root), message, message_size)) {
        return false;
    }
    char source_app_bundle[PATH_MAX];
    bundled_app_macos_app_bundle_path(app, stage_root, source_app_bundle, sizeof(source_app_bundle));
    bool source_is_app_bundle = bundled_app_macos_app_has_expected_files(app, source_app_bundle);
    char source_binary[PATH_MAX];
    if (source_is_app_bundle) {
        bundled_app_macos_app_binary_path(app, source_app_bundle, source_binary, sizeof(source_binary));
    } else {
        snprintf(source_binary, sizeof(source_binary), "%s/MacOS/%s", stage_root, app->binary_name);
    }
    char source_bundle_arm[PATH_MAX];
    if (source_is_app_bundle) {
        char source_bundles_dir[PATH_MAX];
        bundled_app_macos_app_bundles_dir(source_app_bundle, source_bundles_dir, sizeof(source_bundles_dir));
        snprintf(source_bundle_arm, sizeof(source_bundle_arm), "%s/%s.bundle.macos-arm.aar", source_bundles_dir, app->bundle_prefix);
    } else {
        snprintf(source_bundle_arm, sizeof(source_bundle_arm), "%s/bundles/%s.bundle.macos-arm.aar", stage_root, app->bundle_prefix);
    }
    char source_bundle_x86[PATH_MAX];
    if (source_is_app_bundle) {
        char source_bundles_dir[PATH_MAX];
        bundled_app_macos_app_bundles_dir(source_app_bundle, source_bundles_dir, sizeof(source_bundles_dir));
        snprintf(source_bundle_x86, sizeof(source_bundle_x86), "%s/%s.bundle.macos-x86.aar", source_bundles_dir, app->bundle_prefix);
    } else {
        snprintf(source_bundle_x86, sizeof(source_bundle_x86), "%s/bundles/%s.bundle.macos-x86.aar", stage_root, app->bundle_prefix);
    }
    char source_icon[PATH_MAX] = "";
    if (app->icon_name && app->icon_name[0]) {
        if (source_is_app_bundle) {
            bundled_app_macos_app_icon_path(app, source_app_bundle, source_icon, sizeof(source_icon));
        } else {
            snprintf(source_icon, sizeof(source_icon), "%s/%s", stage_root, app->icon_name);
        }
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
        if (!ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
            return false;
        }

        char install_root[PATH_MAX];
        snprintf(install_root, sizeof(install_root), "%s/apps/%s", kSystemOuterShellRoot, app->install_directory_name);
        char target_app_bundle[PATH_MAX];
        bundled_app_macos_app_bundle_path(app, install_root, target_app_bundle, sizeof(target_app_bundle));
        char bundles_dir[PATH_MAX];
        if (source_is_app_bundle) {
            bundled_app_macos_app_bundles_dir(target_app_bundle, bundles_dir, sizeof(bundles_dir));
        } else {
            snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
        }
        char target_binary[PATH_MAX];
        if (source_is_app_bundle) {
            bundled_app_macos_app_binary_path(app, target_app_bundle, target_binary, sizeof(target_binary));
        } else {
            snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
        }
        char target_bundle_arm[PATH_MAX];
        snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
        char target_bundle_x86[PATH_MAX];
        snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
        char target_icon[PATH_MAX] = "";
        if (source_icon[0]) {
            if (source_is_app_bundle) {
                bundled_app_macos_app_icon_path(app, target_app_bundle, target_icon, sizeof(target_icon));
            } else {
                snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
            }
        }
        char outerctl_path[PATH_MAX];
        default_system_outerctl_path(outerctl_path, sizeof(outerctl_path));
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "/Library/Logs/%s.log", app->service_id);
        char plist_path[PATH_MAX];
        snprintf(plist_path, sizeof(plist_path), "/Library/LaunchDaemons/%s.plist", app->service_id);

        StringBuilder plist = {0};
        if (!make_bundled_launchd_plist(app->service_id,
                                        target_binary,
                                        source_is_app_bundle ? "" : bundles_dir,
                                        source_is_app_bundle ? "" : target_icon,
                                        socket_path,
                                        0600,
                                        source_is_app_bundle ? target_app_bundle : install_root,
                                        outerctl_path,
                                        log_path,
                                        &plist)) {
            free(plist.data);
            snprintf(message, message_size, "Failed to generate LaunchDaemon plist.");
            return false;
        }

        char quoted_source_binary[PATH_MAX + 8];
        char quoted_source_app_bundle[PATH_MAX + 8];
        char quoted_source_bundle_arm[PATH_MAX + 8];
        char quoted_source_bundle_x86[PATH_MAX + 8];
        char quoted_source_icon[PATH_MAX + 8] = "";
        char quoted_install_root[PATH_MAX + 8];
        char quoted_bundles_dir[PATH_MAX + 8];
        char quoted_system_root[PATH_MAX + 8];
        char quoted_target_binary[PATH_MAX + 8];
        char quoted_target_app_bundle[PATH_MAX + 8];
        char quoted_target_bundle_arm[PATH_MAX + 8];
        char quoted_target_bundle_x86[PATH_MAX + 8];
        char quoted_target_icon[PATH_MAX + 8] = "";
        char quoted_log_path[PATH_MAX + 8];
        char quoted_plist_path[PATH_MAX + 8];
        shell_quote(source_binary, quoted_source_binary, sizeof(quoted_source_binary));
        shell_quote(source_app_bundle, quoted_source_app_bundle, sizeof(quoted_source_app_bundle));
        shell_quote(source_bundle_arm, quoted_source_bundle_arm, sizeof(quoted_source_bundle_arm));
        shell_quote(source_bundle_x86, quoted_source_bundle_x86, sizeof(quoted_source_bundle_x86));
        if (source_icon[0]) shell_quote(source_icon, quoted_source_icon, sizeof(quoted_source_icon));
        shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
        shell_quote(bundles_dir, quoted_bundles_dir, sizeof(quoted_bundles_dir));
        shell_quote(kSystemOuterShellRoot, quoted_system_root, sizeof(quoted_system_root));
        shell_quote(target_binary, quoted_target_binary, sizeof(quoted_target_binary));
        shell_quote(target_app_bundle, quoted_target_app_bundle, sizeof(quoted_target_app_bundle));
        shell_quote(target_bundle_arm, quoted_target_bundle_arm, sizeof(quoted_target_bundle_arm));
        shell_quote(target_bundle_x86, quoted_target_bundle_x86, sizeof(quoted_target_bundle_x86));
        if (target_icon[0]) shell_quote(target_icon, quoted_target_icon, sizeof(quoted_target_icon));
        shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
        shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));

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
                "rm -rf -- %s\n"
                "mkdir -p %s /Library/LaunchDaemons /Library/Logs %s\n"
                "chmod 0755 %s\n",
                app->service_id,
                quoted_install_root,
                quoted_install_root,
                quoted_system_root,
                quoted_system_root);
        if (source_is_app_bundle) {
            fprintf(script,
                    "/usr/bin/ditto %s %s\n",
                    quoted_source_app_bundle,
                    quoted_target_app_bundle);
        } else {
            fprintf(script,
                    "mkdir -p %s\n"
                    "install -m 0755 %s %s\n"
                    "install -m 0644 %s %s\n"
                    "install -m 0644 %s %s\n",
                    quoted_bundles_dir,
                    quoted_source_binary,
                    quoted_target_binary,
                    quoted_source_bundle_arm,
                    quoted_target_bundle_arm,
                    quoted_source_bundle_x86,
                    quoted_target_bundle_x86);
            if (quoted_source_icon[0] && quoted_target_icon[0]) {
                fprintf(script, "install -m 0644 %s %s\n", quoted_source_icon, quoted_target_icon);
            }
        }
        fprintf(script,
                "rm -f %s/outerctl-system\n"
                "cat > %s <<'__BACKENDS_PLIST__'\n%s__BACKENDS_PLIST__\n"
                "chmod 0644 %s\n"
                "touch %s\n"
                "chmod 0644 %s\n"
                "launchctl bootstrap system %s\n",
                quoted_install_root,
                quoted_plist_path,
                plist.data ? plist.data : "",
                quoted_plist_path,
                quoted_log_path,
                quoted_log_path,
                quoted_plist_path);
        fclose(script);
        free(plist.data);
        chmod(script_template, 0700);

        bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
        unlink(script_template);
        if (!root_ok) return false;

        if (!root_helper_registry_upsert_launchd(app->service_id,
                                                 app->display_name,
                                                 plist_path,
                                                 socket_path,
                                                 log_path,
                                                 target_icon,
                                                 sudo_password,
                                                 needs_password,
                                                 message,
                                                 message_size)) {
            return false;
        }
        if (!root_helper_registry_upsert_bundled_openers(app,
                                                         socket_path,
                                                         sudo_password,
                                                         needs_password,
                                                         message,
                                                         message_size)) {
            return false;
        }

        snprintf(message, message_size, "Installed %s as root.", app->display_name);
        return true;
    }

    char install_root[PATH_MAX];
    default_user_outershell_app_root(app->install_directory_name, install_root, sizeof(install_root));
    char target_app_bundle[PATH_MAX];
    bundled_app_macos_app_bundle_path(app, install_root, target_app_bundle, sizeof(target_app_bundle));
    char bundles_dir[PATH_MAX];
    if (source_is_app_bundle) {
        bundled_app_macos_app_bundles_dir(target_app_bundle, bundles_dir, sizeof(bundles_dir));
    } else {
        snprintf(bundles_dir, sizeof(bundles_dir), "%s/bundles", install_root);
    }
    char target_binary[PATH_MAX];
    if (source_is_app_bundle) {
        bundled_app_macos_app_binary_path(app, target_app_bundle, target_binary, sizeof(target_binary));
    } else {
        snprintf(target_binary, sizeof(target_binary), "%s/%s", install_root, app->binary_name);
    }
    char target_bundle_arm[PATH_MAX];
    snprintf(target_bundle_arm, sizeof(target_bundle_arm), "%s/%s.bundle.macos-arm.aar", bundles_dir, app->bundle_prefix);
    char target_bundle_x86[PATH_MAX];
    snprintf(target_bundle_x86, sizeof(target_bundle_x86), "%s/%s.bundle.macos-x86.aar", bundles_dir, app->bundle_prefix);
    char target_icon[PATH_MAX] = "";
    if (source_icon[0]) {
        if (source_is_app_bundle) {
            bundled_app_macos_app_icon_path(app, target_app_bundle, target_icon, sizeof(target_icon));
        } else {
            snprintf(target_icon, sizeof(target_icon), "%s/%s", install_root, app->icon_name);
        }
    }
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), app->service_id);
    char plist_path[PATH_MAX];
    snprintf(plist_path, sizeof(plist_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), app->service_id);
    char outerctl_path[PATH_MAX];
    default_user_outerctl_path(outerctl_path, sizeof(outerctl_path));

    char error[1024] = "";
    if (!mkdir_p(source_is_app_bundle ? install_root : bundles_dir)) {
        snprintf(message, message_size, "Failed to create %s: %s", source_is_app_bundle ? install_root : bundles_dir, strerror(errno));
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

    char quoted_install_root[PATH_MAX + 8];
    shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
    char remove_install_command[PATH_MAX + 64];
    snprintf(remove_install_command, sizeof(remove_install_command), "rm -rf -- %s", quoted_install_root);
    run_shell_ignored(remove_install_command);
    if (!mkdir_p(source_is_app_bundle ? install_root : bundles_dir)) {
        snprintf(message, message_size, "Failed to create %s: %s", source_is_app_bundle ? install_root : bundles_dir, strerror(errno));
        return false;
    }

    if (source_is_app_bundle) {
        char quoted_source_app_bundle[PATH_MAX + 8];
        char quoted_target_app_bundle[PATH_MAX + 8];
        shell_quote(source_app_bundle, quoted_source_app_bundle, sizeof(quoted_source_app_bundle));
        shell_quote(target_app_bundle, quoted_target_app_bundle, sizeof(quoted_target_app_bundle));
        char copy_command[PATH_MAX * 2 + 128];
        snprintf(copy_command, sizeof(copy_command), "rm -rf -- %s && /usr/bin/ditto %s %s",
                 quoted_target_app_bundle,
                 quoted_source_app_bundle,
                 quoted_target_app_bundle);
        if (system(copy_command) != 0) {
            snprintf(message, message_size, "Failed to copy %s app bundle.", app->display_name);
            return false;
        }
    } else {
        if (!copy_file(source_binary, target_binary, 0700, error, sizeof(error)) ||
            !copy_file(source_bundle_arm, target_bundle_arm, 0600, error, sizeof(error)) ||
            !copy_file(source_bundle_x86, target_bundle_x86, 0600, error, sizeof(error)) ||
            (source_icon[0] && !copy_file(source_icon, target_icon, 0600, error, sizeof(error)))) {
            snprintf(message, message_size, "%s", error);
            return false;
        }
    }

    StringBuilder plist = {0};
    if (!make_bundled_launchd_plist(app->service_id,
                                    target_binary,
                                    source_is_app_bundle ? "" : bundles_dir,
                                    source_is_app_bundle ? "" : target_icon,
                                    socket_path,
                                    0600,
                                    source_is_app_bundle ? target_app_bundle : install_root,
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

    char quoted_plist[PATH_MAX + 8];
    shell_quote(plist_path, quoted_plist, sizeof(quoted_plist));
    char bootstrap_message[4096] = "";
    char bootstrap_command[PATH_MAX + 128];
    snprintf(bootstrap_command, sizeof(bootstrap_command), "launchctl bootstrap gui/%d %s 2>&1", (int)getuid(), quoted_plist);
    if (!run_launchctl_capture(bootstrap_command, bootstrap_message, sizeof(bootstrap_message))) {
        snprintf(message, message_size, "Installed %s, but failed to bootstrap its socket: %s", app->display_name, bootstrap_message);
        return false;
    }

    snprintf(message, message_size, "Installed %s.", app->display_name);
    return true;
}
#endif

static bool uninstall_backend(const char *service_id, const char *sudo_password, bool *needs_password, char *message, size_t message_size) {
    if (needs_password) *needs_password = false;
    char error[1024] = "";
    bool found_any = false;

#ifdef __APPLE__
    const char *launchd_scopes[] = {"system", "user"};
    for (size_t i = 0; i < sizeof(launchd_scopes) / sizeof(launchd_scopes[0]); i++) {
        char plist_path[PATH_MAX] = "";
        int owns_plist = 0;
        if (!lookup_launchd_backend_any_for_scope(service_id, launchd_scopes[i], plist_path, sizeof(plist_path), &owns_plist)) {
            continue;
        }
        found_any = true;
        if (strncmp(plist_path, "/Library/LaunchDaemons/", 23) == 0) {
            if (!ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
                return false;
            }

            const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
            char install_name[PATH_MAX];
            snprintf(install_name, sizeof(install_name), "%s", app ? app->install_directory_name : service_id);
            char install_root[PATH_MAX] = "";
            if (safe_service_directory_name(install_name)) {
                snprintf(install_root, sizeof(install_root), "%s/apps/%s", kSystemOuterShellRoot, install_name);
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
            shell_quote(plist_path, quoted_plist_path, sizeof(quoted_plist_path));
            if (install_root[0]) shell_quote(install_root, quoted_install_root, sizeof(quoted_install_root));
            shell_quote(log_path, quoted_log_path, sizeof(quoted_log_path));
            if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path));

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
                    "rm -f -- %s %s\n",
                    service_id,
                    owns_plist && plist_path[0] ? quoted_plist_path : "''",
                    quoted_socket_path[0] ? quoted_socket_path : "''");
            if (quoted_install_root[0]) {
                fprintf(script, "rm -rf -- %s\n", quoted_install_root);
            }
            fprintf(script, "rm -f -- %s\n", quoted_log_path);
            fclose(script);
            chmod(script_template, 0700);
            bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
            unlink(script_template);
            if (!root_ok) return false;
            if (!root_helper_registry_remove_backend(service_id, sudo_password, needs_password, message, message_size)) {
                return false;
            }
        } else {
            char stop_message[1024] = "";
            (void)run_launchd_operation(service_id, plist_path, "stop", stop_message, sizeof(stop_message));
            if (owns_plist && plist_path[0]) {
                unlink(plist_path);
            }
        }
    }
#endif

    const char *systemd_scopes[] = {"system", "user"};
    for (size_t i = 0; i < sizeof(systemd_scopes) / sizeof(systemd_scopes[0]); i++) {
        char unit_name[256] = "";
        char scope[32] = "";
        if (!lookup_systemd_backend_any_for_scope(service_id, systemd_scopes[i], unit_name, sizeof(unit_name), scope, sizeof(scope)) ||
            !safe_unit_name(unit_name)) {
            continue;
        }
        found_any = true;
        char quoted_unit[320];
        shell_quote(unit_name, quoted_unit, sizeof(quoted_unit));
        if (strcmp(scope, "system") == 0) {
            bool direct_root_uninstall = direct_root_session_uses_system_scope();
            if (!direct_root_uninstall &&
                !ensure_root_helper_installed(sudo_password, needs_password, message, message_size)) {
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
            char socket_path[PATH_MAX] = "";
            if (app && app->socket_activated) {
                systemd_socket_unit_name(unit_name, socket_unit_name, sizeof(socket_unit_name));
                snprintf(socket_unit_path, sizeof(socket_unit_path), "/etc/systemd/system/%s", socket_unit_name);
                bundled_socket_path_for_scope(app, "system", socket_path, sizeof(socket_path));
            }

            char quoted_unit_path[PATH_MAX + 8];
            char quoted_socket_unit[320] = "";
            char quoted_socket_unit_path[PATH_MAX + 8] = "";
            char quoted_socket_path[PATH_MAX + 8] = "";
            char quoted_install_root[PATH_MAX + 8];
            char quoted_log_path[PATH_MAX + 8];
            shell_quote(unit_path, quoted_unit_path, sizeof(quoted_unit_path));
            if (socket_unit_name[0]) shell_quote(socket_unit_name, quoted_socket_unit, sizeof(quoted_socket_unit)); else quoted_socket_unit[0] = '\0';
            if (socket_unit_path[0]) shell_quote(socket_unit_path, quoted_socket_unit_path, sizeof(quoted_socket_unit_path)); else quoted_socket_unit_path[0] = '\0';
            if (socket_path[0]) shell_quote(socket_path, quoted_socket_path, sizeof(quoted_socket_path)); else quoted_socket_path[0] = '\0';
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
                    "rm -f -- %s %s %s\n",
                    quoted_unit,
                    quoted_socket_unit[0] ? "timeout 12s systemctl --system disable --now " : "",
                    quoted_socket_unit[0] ? quoted_socket_unit : "",
                    quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
                    quoted_socket_unit[0] ? "timeout 12s systemctl --system stop " : "",
                    quoted_socket_unit[0] ? quoted_unit : "",
                    quoted_socket_unit[0] ? " >/dev/null 2>&1 || true\n" : "",
                    quoted_unit_path,
                    quoted_socket_unit_path[0] ? quoted_socket_unit_path : "''",
                    quoted_socket_path[0] ? quoted_socket_path : "''");
            if (quoted_install_root[0]) {
                fprintf(script, "rm -rf -- %s\n", quoted_install_root);
            }
            fprintf(script,
                    "rm -f -- %s\n"
                    "systemctl --system daemon-reload\n",
                    quoted_log_path);
#ifndef __APPLE__
            write_root_apps_marker_cleanup_shell(script);
#endif
            fclose(script);
            chmod(script_template, 0700);
            bool root_ok = run_root_script(script_template, sudo_password, needs_password, message, message_size);
            unlink(script_template);
            if (!root_ok) return false;
            if (direct_root_uninstall) {
                char registry_error[1024] = "";
                if (!unregister_backend_records(service_id, registry_error, sizeof(registry_error))) {
                    snprintf(message, message_size, "%s", registry_error);
                    return false;
                }
            } else {
                if (!root_helper_registry_remove_backend(service_id, sudo_password, needs_password, message, message_size)) {
                    return false;
                }
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
    if (!found_any && bundled_app) {
        cleanup_user_systemd_bundled_app(bundled_app, true, false);
    }

    if (!unregister_backend_records(service_id, error, sizeof(error))) {
        snprintf(message, message_size, "%s", error);
        return false;
    }

    const BundledAppDefinition *app = bundled_app_for_service_id(service_id);
    if (app) {
        cleanup_bundled_app_cache(app);
    }
    if (app || safe_service_directory_name(service_id)) {
        char install_name[PATH_MAX];
        snprintf(install_name, sizeof(install_name), "%s", app ? app->install_directory_name : service_id);
        if (safe_service_directory_name(install_name)) {
            char install_root[PATH_MAX];
            default_user_outershell_app_root(install_name, install_root, sizeof(install_root));
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
    char command_socket_placeholder[PATH_MAX];
    runtime_socket_path("my-service.sock", command_socket_placeholder, sizeof(command_socket_placeholder));
    BinaryPayloadList fields = {0};
    ok = ok &&
         append_field_payload(&fields, "command", "Command", "", "text", "bundle exec jekyll serve --host 0.0.0.0", NULL, NULL, 0) &&
         append_field_payload(&fields, "workdir", "Working Dir", "~", "directory", "~", NULL, NULL, 0) &&
         append_field_payload(&fields, "scriptPath", "Script Path", "", "file", "~/dev/run-service.sh", NULL, NULL, 0) &&
         append_field_payload(&fields, "frontendTransport", "Connection", "port", "choice", "", NULL, connection_choices, 2) &&
         append_field_payload(&fields, "port", "Port", "", "text", "4000", NULL, NULL, 0) &&
         append_field_payload(&fields, "socketPath", "Socket Path", "", "text", command_socket_placeholder, NULL, NULL, 0) &&
         append_field_payload(&fields, "name", "Display Name", "", "text", "My Service", NULL, NULL, 0) &&
         append_field_payload(&fields, "identifier", "Identifier", "", "text", "my-service", NULL, NULL, 0) &&
         append_recipe_payload(&recipes, "command-port", "Run a command, use a fixed endpoint",
                               "Create a script that runs a command you choose and registers a frontend on a port or Unix socket.",
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
                               "Create a script that launches Jupyter Lab and finds its browser URL with `jupyter server list`.",
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
                               "Create a script that launches Jupyter Lab from .venv and finds its browser URL with `jupyter server list`.",
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
    char initial_frontend_icon_path[PATH_MAX] = "";
    char initial_frontend_list[PATH_MAX] = "";
    int initial_frontend_port = 0;
    query_value(query, "iconPath", initial_frontend_icon_path, sizeof(initial_frontend_icon_path));
    default_user_outershell_app_root(service_id, backend_dir, sizeof(backend_dir));
#ifdef __APPLE__
    snprintf(log_path, sizeof(log_path), "%s/Library/Logs/%s/output.log", home_directory(), service_id);
    snprintf(unit_path, sizeof(unit_path), "%s/Library/LaunchAgents/%s.plist", home_directory(), unit_name);
#else
    if (direct_root_session_uses_system_scope()) {
        snprintf(log_path, sizeof(log_path), "/var/log/outergroup/%s.log", service_id);
        snprintf(unit_path, sizeof(unit_path), "/etc/systemd/system/%s", unit_name);
    } else {
        snprintf(log_path, sizeof(log_path), "%s/output.log", backend_dir);
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home_directory(), unit_name);
    }
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
            char transport[64] = "port";
            char socket_path_raw[PATH_MAX] = "";
            char socket_path[PATH_MAX] = "";
            query_value(query, "command", command, sizeof(command));
            query_value_or_default(query, "frontendTransport", "port", transport, sizeof(transport));
            query_value(query, "port", port, sizeof(port));
            query_value(query, "socketPath", socket_path_raw, sizeof(socket_path_raw));
            bool use_unix_socket = strcmp(transport, "unixSocket") == 0;
            if (!command[0]) {
                send_action_response(fd, 400, false, "Command is required.");
                return;
            }
            snprintf(initial_frontend_id, sizeof(initial_frontend_id), "%s:main", service_id);
            snprintf(initial_frontend_list, sizeof(initial_frontend_list), "bash commands");
            if (use_unix_socket) {
                expand_tilde_path(socket_path_raw, socket_path, sizeof(socket_path));
                if (!socket_path[0] || socket_path[0] != '/') {
                    send_action_response(fd, 400, false, "Socket Path must be an absolute path.");
                    return;
                }
                snprintf(initial_frontend_url, sizeof(initial_frontend_url), "/");
                snprintf(initial_frontend_socket_path, sizeof(initial_frontend_socket_path), "%s", socket_path);
            } else {
                if (!valid_port_text(port)) {
                    send_action_response(fd, 400, false, "A valid port is required.");
                    return;
                }
                snprintf(initial_frontend_url, sizeof(initial_frontend_url), "127.0.0.1:%s/", port);
                initial_frontend_port = atoi(port);
            }
            StringBuilder script = {0};
            if (!make_fixed_port_script(service_id,
                                        display_name,
                                        use_unix_socket ? "" : port,
                                        use_unix_socket ? socket_path : "",
                                        use_unix_socket,
                                        command,
                                        &script)) {
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
    bool system_scope = direct_root_session_uses_system_scope();
    const char *systemd_wanted_by = system_scope ? "multi-user.target" : "default.target";
    const char *api_socket_environment = system_scope ? "Environment=OUTERSHELLD_API_SOCKET=/run/outershelld-api\n" : "";
    snprintf(unit_contents, sizeof(unit_contents),
             "[Unit]\n"
             "Description=%s\n"
             "After=network.target\n"
             "\n"
             "[Service]\n"
             "Type=simple\n"
             "WorkingDirectory=%s\n"
             "Environment=OUTERCTL_PATH=%s\n"
             "%s"
             "Environment=OUTERSHELL_LOG_PATH=%s\n"
             "ExecStart=/bin/sh -lc %s\n"
             "Restart=on-failure\n"
             "RestartSec=2\n"
             "StandardOutput=append:%s\n"
             "StandardError=append:%s\n"
             "SuccessExitStatus=143 SIGTERM\n"
             "\n"
             "[Install]\n"
             "WantedBy=%s\n",
             description,
             working_directory,
             outerctl_path,
             api_socket_environment,
             log_path,
             quoted_command,
             log_path,
             log_path,
             systemd_wanted_by);

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
    if (direct_root_session_uses_system_scope()) {
        if (!mkdir_p("/var/log/outergroup")) {
            snprintf(error, sizeof(error), "Failed to create /var/log/outergroup: %s", strerror(errno));
            send_action_response(fd, 500, false, error);
            return;
        }
    } else {
        char user_units_dir[PATH_MAX];
        snprintf(user_units_dir, sizeof(user_units_dir), "%s/.config/systemd/user", home_directory());
        if (!mkdir_p(user_units_dir)) {
            snprintf(error, sizeof(error), "Failed to create %s: %s", user_units_dir, strerror(errno));
            send_action_response(fd, 500, false, error);
            return;
        }
    }
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
                                  initial_frontend_icon_path,
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
    const char *systemd_scope = direct_root_session_uses_system_scope() ? "system" : "user";
    const char *systemd_scope_arg = direct_root_session_uses_system_scope() ? "--system" : "--user";
    snprintf(enable_command, sizeof(enable_command), "systemctl %s enable %s >/dev/null 2>&1", systemd_scope_arg, quoted_unit);
    char daemon_reload_command[128];
    snprintf(daemon_reload_command, sizeof(daemon_reload_command), "systemctl %s daemon-reload >/dev/null 2>&1", systemd_scope_arg);
    system(daemon_reload_command);
    system(enable_command);
    char message[4096] = "";
    bool started = run_systemd_operation(unit_name, systemd_scope, "start", NULL, NULL, message, sizeof(message));
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

static void process_ui_route_request(uint16_t route, const char *query, const char *body, size_t body_length, UiApiResponse *response) {
    UiApiResponse *previous_capture = g_captured_ui_response;
    g_captured_ui_response = response;
    response->status = 500;
    response->content_kind = UI_API_CONTENT_TEXT;

    switch (route) {
    case OUTERSHELLD_UI_ROUTE_BACKENDS:
        send_backends_response(-1);
        break;
    case OUTERSHELLD_UI_ROUTE_LOGS:
        send_logs_response(-1, query);
        break;
    case OUTERSHELLD_UI_ROUTE_CONTROL: {
        char *body_copy = malloc(body_length + 1);
        if (!body_copy) {
            send_text_response(-1, 500, "out of memory\n");
            break;
        }
        if (body_length > 0) memcpy(body_copy, body ? body : "", body_length);
        body_copy[body_length] = '\0';
        send_control_response(-1, query, body_copy);
        free(body_copy);
        break;
    }
    case OUTERSHELLD_UI_ROUTE_CREATE:
        send_create_response(-1, query);
        break;
    case OUTERSHELLD_UI_ROUTE_RECIPES:
        send_recipes_response(-1);
        break;
    case OUTERSHELLD_UI_ROUTE_FILE_PICKER:
        send_file_picker_response(-1, query);
        break;
    default:
        send_text_response(-1, 400, "unsupported UI API route\n");
        break;
    }

    g_captured_ui_response = previous_capture;
    if (response->status == 0) response->status = 500;
}

static bool process_api_ui_request(ReactorClient *client, const unsigned char *message, size_t message_length) {
    char *query = NULL;
    const unsigned char *body = NULL;
    size_t body_length = 0;
    bool ok = message_length >= 24 &&
              api_read_string_ref(message, message_length, 8, &query) &&
              api_read_data_ref(message, message_length, 16, &body, &body_length);
    uint16_t route = ok ? read_uint16_le(message + 2) : OUTERSHELLD_UI_ROUTE_NONE;

    UiApiResponse response = {0};
    if (!ok) {
        ui_api_set_text_response(&response, 400, "invalid UI API request\n");
    } else if (route == OUTERSHELLD_UI_ROUTE_EVENTS) {
        UiApiResponse *previous_capture = g_captured_ui_response;
        g_captured_ui_response = &response;
        bool waiting = prepare_events_response_or_wait(client, query ? query : "");
        g_captured_ui_response = previous_capture;
        if (waiting) {
            client->event_response_is_api = true;
            ui_api_response_free(&response);
            free(query);
            return true;
        }
    } else {
        process_ui_route_request(route, query ? query : "", (const char *)body, body_length, &response);
    }

    api_send_ui_response(client->fd, &response);
    ui_api_response_free(&response);
    free(query);
    return false;
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

    char system_outerctl[PATH_MAX];
    default_system_outerctl_path(system_outerctl, sizeof(system_outerctl));
    char quoted_outerctl[PATH_MAX + 8];
    shell_quote(system_outerctl, quoted_outerctl, sizeof(quoted_outerctl));
    StringBuilder command = {0};
    bool ok = sb_append(&command, quoted_outerctl);
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

static bool root_helper_registry_upsert_bundled_openers(const BundledAppDefinition *app,
                                                        const char *socket_path,
                                                        const char *sudo_password,
                                                        bool *needs_password,
                                                        char *message,
                                                        size_t message_size) {
    if (!app) return true;
    char *clear_argv[] = {"outerctl", "opener", "clear", "--backend", (char *)app->service_id, NULL};
    if (!root_helper_outerctl(5, clear_argv, sudo_password, needs_password, message, message_size)) return false;
    if (!app->openers || app->opener_count == 0) return true;
    if (!socket_path || !socket_path[0]) {
        snprintf(message, message_size, "Missing opener socket path.");
        return false;
    }
    for (size_t i = 0; i < app->opener_count; i++) {
        char rank_string[32];
        snprintf(rank_string, sizeof(rank_string), "%d", app->openers[i].rank);
        char *add_argv[] = {
            "outerctl",
            "opener",
            "add",
            "--backend",
            (char *)app->service_id,
            "--content-type",
            (char *)app->openers[i].content_type,
            "--socket-path",
            (char *)socket_path,
            "--name",
            (char *)app->display_name,
            "--url-template",
            (char *)(app->openers[i].url_template ? app->openers[i].url_template : "?file={file}"),
            "--rank",
            rank_string,
            NULL
        };
        if (!root_helper_outerctl(15, add_argv, sudo_password, needs_password, message, message_size)) return false;
    }
    return true;
}

#ifndef __APPLE__
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
    char *backend_argv[] = {"outerctl", "backend", "upsert", "--backend", (char *)service_id, "--name", (char *)display_name, "--systemd-unit", (char *)unit_name, NULL};
    if (!root_helper_outerctl(9, backend_argv, sudo_password, needs_password, message, message_size)) return false;

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
#else
static bool root_helper_registry_upsert_launchd(const char *service_id,
                                                const char *display_name,
                                                const char *plist_path,
                                                const char *socket_path,
                                                const char *log_path,
                                                const char *icon_path,
                                                const char *sudo_password,
                                                bool *needs_password,
                                                char *message,
                                                size_t message_size) {
    char *backend_argv[] = {"outerctl", "backend", "upsert", "--backend", (char *)service_id, "--name", (char *)display_name, "--launchd-plist", (char *)plist_path, "--owns-plist", "true", NULL};
    if (!root_helper_outerctl(11, backend_argv, sudo_password, needs_password, message, message_size)) return false;

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
    char *launchd_clear_argv[] = {"outerctl", "launchd", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, launchd_clear_argv, sudo_password, needs_password, message, message_size);
    char *systemd_clear_argv[] = {"outerctl", "systemd", "clear", "--backend", (char *)service_id, NULL};
    (void)root_helper_outerctl(5, systemd_clear_argv, sudo_password, needs_password, message, message_size);
    char *backend_remove_argv[] = {"outerctl", "backend", "remove", "--backend", (char *)service_id, NULL};
    if (root_helper_outerctl(5, backend_remove_argv, sudo_password, needs_password, message, message_size)) return true;
    return contains_case_insensitive(message, "Backend not registered");
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

static bool api_append_file_openers_from_database(const RegistryStore *database,
                                                  const ContentTypeList *content_types,
                                                  const char *file_path,
                                                  bool require_socket_access,
                                                  StringBuilder *rows,
                                                  StringBuilder *variable,
                                                  uint32_t *row_count,
                                                  char *error,
                                                  size_t error_size) {
    bool ok = true;
    for (size_t i = 0; ok && database && i < database->opener_count; i++) {
        const RegistryFileOpenerRecord *record = &database->openers[i];
        if (!content_type_list_contains(content_types, record->extension ? record->extension : "")) continue;
        const char *socket_path = record->socket_path ? record->socket_path : "";
        if (require_socket_access && !unix_socket_path_accessible_to_current_user(socket_path)) {
            continue;
        }
        StringBuilder url = {0};
        ok = append_file_opener_url(&url,
                                    socket_path,
                                    record->url_template,
                                    file_path ? file_path : "") &&
             api_append_string_ref32(rows, variable, record->extension) &&
             api_append_string_ref32(rows, variable, record->service_id) &&
             api_append_string_ref32(rows, variable, record->display_name) &&
             api_append_string_ref32(rows, variable, socket_path) &&
             api_append_string_ref32(rows, variable, url.data ? url.data : "");
        free(url.data);
        if (ok && row_count) *row_count += 1;
    }
    if (!ok && !error[0]) snprintf(error, error_size, "out of memory");
    return ok;
}

static bool content_type_list_for_opener_query(const RegistryStore *database,
                                               const char *file_path,
                                               const char *content_type,
                                               ContentTypeList *list,
                                               char *error,
                                               size_t error_size) {
    if (content_type && content_type[0]) {
        char normalized[160];
        if (!normalize_content_type_identifier(content_type, normalized, sizeof(normalized))) {
            snprintf(error, error_size, "Invalid content type.");
            return false;
        }
        return append_content_type_and_parents(database, list, normalized);
    }
    if (file_path && file_path[0]) {
        return infer_content_types_for_path(database, file_path, list);
    }
    snprintf(error, error_size, "Missing file opener query.");
    return false;
}

static bool api_query_file_openers(const char *file_path,
                                   const char *content_type,
                                   const char *requester_user,
                                   StringBuilder *rows,
                                   StringBuilder *variable,
                                   uint32_t *row_count,
                                   char *error,
                                   size_t error_size) {
    if (row_count) *row_count = 0;

    char requester_registry_path[PATH_MAX];
    const char *user_registry_path = g_registry_database_path;
    if (requester_registry_database_path(requester_user, requester_registry_path, sizeof(requester_registry_path))) {
        user_registry_path = requester_registry_path;
    }

    RegistryStore database;
    if (!registry_store_open_at(&database, user_registry_path, false, error, error_size)) return false;
    bool ok = true;
    ContentTypeList user_types = {0};
    ok = content_type_list_for_opener_query(&database, file_path, content_type, &user_types, error, error_size);
    if (ok) {
        ok = api_append_file_openers_from_database(&database,
                                                  &user_types,
                                                  file_path,
                                                  false,
                                                  rows,
                                                  variable,
                                                  row_count,
                                                  error,
                                                  error_size);
    }
    registry_store_free(&database);

    if (ok &&
        strcmp(user_registry_path, g_system_registry_database_path) != 0 &&
        g_system_registry_database_path[0] &&
        registry_storage_exists_at(g_system_registry_database_path)) {
        char system_error[512] = "";
        RegistryStore system_database;
        if (registry_store_open_system_readonly(&system_database, system_error, sizeof(system_error))) {
            ContentTypeList system_types = {0};
            ok = content_type_list_for_opener_query(&system_database, file_path, content_type, &system_types, error, error_size);
            if (ok) {
                ok = api_append_file_openers_from_database(&system_database,
                                                          &system_types,
                                                          file_path,
                                                          false,
                                                          rows,
                                                          variable,
                                                          row_count,
                                                          error,
                                                          error_size);
            }
            registry_store_free(&system_database);
        }
    }
    return ok;
}

static bool process_api_file_openers_request(ReactorClient *client, const unsigned char *message, size_t message_length) {
    char *file_path = NULL;
    char *content_type = NULL;
    char *requester_user = NULL;
    char error[512] = "";
    StringBuilder rows = {0};
    StringBuilder variable = {0};
    uint32_t row_count = 0;
    bool ok = message_length >= 18 &&
              api_read_string_ref(message, message_length, 2, &file_path) &&
              api_read_string_ref(message, message_length, 10, &content_type);
    if (ok && message_length >= 26) {
        ok = api_read_string_ref(message, message_length, 18, &requester_user);
    }
    ok = ok && api_query_file_openers(file_path, content_type, requester_user, &rows, &variable, &row_count, error, sizeof(error));
    if (!ok && !error[0]) snprintf(error, sizeof(error), "Invalid file openers request.");
    api_send_file_openers_response(client->fd, ok ? 0u : 1u, error, &rows, &variable, ok ? row_count : 0);
    free(file_path);
    free(content_type);
    free(requester_user);
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
    if (message_type == OUTERSHELLD_API_UI_REQUEST) {
        return process_api_ui_request(client, message, message_length);
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
    char log_path[PATH_MAX] = "";
    char log_index_raw[64] = "";
    query_value(query, "sinceBackends", since_backends_raw, sizeof(since_backends_raw));
    query_value(query, "sinceLog", since_log_raw, sizeof(since_log_raw));
    query_value(query, "serviceID", service_id, sizeof(service_id));
    query_value(query, "path", log_path, sizeof(log_path));
    query_value(query, "logIndex", log_index_raw, sizeof(log_index_raw));
    int log_index = log_index_raw[0] ? atoi(log_index_raw) : 0;
    if (log_index < 0) log_index = 0;

    uint64_t since_backends = parse_u64_or_zero(since_backends_raw);
    uint64_t since_log = parse_u64_or_zero(since_log_raw);
    uint64_t backends_version = current_backends_event_version();
    uint64_t log_version = log_path[0] ? current_log_path_event_version(log_path) : current_log_event_version(service_id, log_index);
    bool has_log_selection = log_path[0] || service_id[0];
    bool backends_changed = since_backends == 0 || backends_version != since_backends;
    bool log_changed = has_log_selection && (since_log == 0 || log_version != since_log);
    if (backends_changed || log_changed) {
        send_events_response(client->fd, backends_changed, log_changed, false, backends_version, log_version);
        return false;
    }

    client->waiting_for_events = true;
    client->event_deadline_ms = monotonic_milliseconds() + 25000;
    client->event_since_backends = since_backends;
    client->event_since_log = since_log;
    snprintf(client->event_log_service_id, sizeof(client->event_log_service_id), "%s", service_id);
    snprintf(client->event_log_path, sizeof(client->event_log_path), "%s", log_path);
    client->event_log_index = log_index;
    return true;
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
            (void)parse_api_frame;
            bool complete = api_request_is_complete(client->request, client->length, complete_length);
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

static bool event_client_ready(ReactorClient *client, bool *timed_out,
                               uint64_t *backends_version, uint64_t *log_version) {
    *timed_out = monotonic_milliseconds() >= client->event_deadline_ms;
    *backends_version = current_backends_event_version();
    *log_version = client->event_log_path[0]
        ? current_log_path_event_version(client->event_log_path)
        : current_log_event_version(client->event_log_service_id, client->event_log_index);
    bool backends_changed = *backends_version != client->event_since_backends;
    bool has_log_selection = client->event_log_path[0] || client->event_log_service_id[0];
    bool log_changed = has_log_selection && *log_version != client->event_since_log;
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
        if (client->event_response_is_api) {
            UiApiResponse response = {0};
            UiApiResponse *previous_capture = g_captured_ui_response;
            g_captured_ui_response = &response;
            send_events_response(-1,
                                 backends_version != client->event_since_backends,
                                 (client->event_log_path[0] || client->event_log_service_id[0]) && log_version != client->event_since_log,
                                 timed_out,
                                 backends_version,
                                 log_version);
            g_captured_ui_response = previous_capture;
            api_send_ui_response(client->fd, &response);
            ui_api_response_free(&response);
        } else {
            send_events_response(client->fd,
                                 backends_version != client->event_since_backends,
                                 (client->event_log_path[0] || client->event_log_service_id[0]) && log_version != client->event_since_log,
                                 timed_out,
                                 backends_version,
                                 log_version);
        }
        close_reactor_client(clients, client_count, index);
    }
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
        flush_ready_event_clients(clients, &client_count);
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
                        close_reactor_client(clients, &client_count, index);
                    } else {
                        bool keep_open = process_api_client_request(&clients[index],
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

static void outershelld_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--api-socket-path PATH] [--database PATH] [--system-database PATH] [--bundled-apps-dir DIR] [--public-base-url URL] [--stay-alive] [--root-helper --root-helper-owner-uid UID]\n", program);
}

static void initialize_runtime_paths(char *api_socket_path, size_t api_socket_path_size) {
    const char *public_base_url = getenv("OUTER_SHELL_PUBLIC_BASE_URL");
    if (public_base_url && public_base_url[0]) {
        snprintf(g_home_screen_public_base_url, sizeof(g_home_screen_public_base_url), "%s", public_base_url);
    }
    default_registry_database_path(g_registry_database_path, sizeof(g_registry_database_path));
    default_system_registry_database_path(g_system_registry_database_path, sizeof(g_system_registry_database_path));
    default_api_socket_path(api_socket_path, api_socket_path_size);
    const char *api_socket_env = getenv("OUTERSHELLD_API_SOCKET");
    if (api_socket_env && api_socket_env[0]) {
        expand_tilde_path(api_socket_env, api_socket_path, api_socket_path_size);
    }
}

int OuterShelldMain(int argc, char **argv) {
    char api_socket_path[PATH_MAX] = "";
    initialize_runtime_paths(api_socket_path, sizeof(api_socket_path));
#ifndef __APPLE__
    bool root_helper_mode = false;
#endif

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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api-socket-path") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], api_socket_path, sizeof(api_socket_path));
        } else if (strcmp(argv[i], "--bundled-apps-dir") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_bundled_apps_directory, sizeof(g_bundled_apps_directory));
        } else if (strcmp(argv[i], "--app-base-url") == 0 && i + 1 < argc) {
            i++;
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
            outershelld_usage(argv[0]);
            return 2;
#else
            root_helper_mode = true;
#endif
        } else if (strcmp(argv[i], "--root-helper-owner-uid") == 0 && i + 1 < argc) {
#ifdef __APPLE__
            outershelld_usage(argv[0]);
            return 2;
#else
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0') {
                outershelld_usage(argv[0]);
                return 2;
            }
            g_root_helper_owner_uid = (uid_t)value;
#endif
        } else {
            outershelld_usage(argv[0]);
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

    migrate_user_outershell_state();

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

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
    fprintf(stderr, "outershelld API listening on %s\n", api_socket_path[0] ? api_socket_path : "(socket activated)");
    fprintf(stderr, "Registry database: %s\n", g_registry_database_path);
    if (g_system_registry_database_path[0]) {
        fprintf(stderr, "System registry database: %s\n", g_system_registry_database_path);
    }
#ifndef __APPLE__
    start_systemd_status_watcher();
#endif
    run_api_reactor(api_listener);
    close(api_listener);
    g_api_listener_fd = -1;
    if (g_api_socket_path[0] && !g_api_systemd_socket_activation) {
        unlink(g_api_socket_path);
    }
    return 0;
}

#if !defined(OUTER_SHELL_BACKEND_LIBRARY) || defined(OUTER_SHELL_BACKEND_STANDALONE)
int main(int argc, char **argv) {
    return OuterShelldMain(argc, argv);
}
#endif
