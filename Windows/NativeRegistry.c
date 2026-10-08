#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../Backend/OuterShellAPI.h"
#define PATH_MAX 32768
#define LOCK_EX 2
#define LOCK_UN 8
#define close _close
#define strdup _strdup
#define strncasecmp _strnicmp
#define S_ISREG(mode) ((mode) == 1)
static int g_registry_write_lock_fd = -1;
static char g_registry_write_lock_path[PATH_MAX];
static SRWLOCK registry_mutex = SRWLOCK_INIT;
static wchar_t *windows_path(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *result = count > 0 ? malloc((size_t)count * sizeof(wchar_t)) : NULL;
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, result, count)) { free(result); return NULL; }
    return result;
}
struct windows_stat { int st_mode; };
static int windows_stat(const char *path, struct windows_stat *info) {
    wchar_t *wide = windows_path(path);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    DWORD error = GetLastError(); free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) { errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EIO; return -1; }
    info->st_mode = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1; return 0;
}
#define stat windows_stat
static int flock(int fd, int operation) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED overlap = {0};
    BOOL ok = operation == LOCK_UN ? UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlap)
        : LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlap);
    return ok ? 0 : -1;
}
static uint64_t read_uint64_le(const unsigned char *bytes) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; i--) result = (result << 8) | bytes[i];
    return result;
}
static uint32_t normalize_opener_capabilities(uint32_t value) {
    value &= OUTERSHELLD_API_OPENER_CAPABILITY_VIEW | OUTERSHELLD_API_OPENER_CAPABILITY_EDIT;
    return value ? value : OUTERSHELLD_API_OPENER_CAPABILITY_DEFAULT;
}
static bool ensure_parent_directory(const char *path, char *error, size_t error_size) {
    wchar_t *wide = windows_path(path);
    if (!wide) return false;
    wchar_t *slash = wcsrchr(wide, L'/');
    if (!slash) slash = wcsrchr(wide, L'\\');
    if (slash) *slash = 0;
    DWORD attributes = slash ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        snprintf(error, error_size, "Registry directory must already exist and must not be a reparse point."); return false;
    }
    return true;
}
static char *read_text_file_alloc(const char *path, size_t *size) {
    *size = 0;
    wchar_t *wide = windows_path(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER length;
    char *result = NULL;
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 0 && length.QuadPart <= 32 * 1024 * 1024) result = malloc((size_t)length.QuadPart + 1);
    size_t offset = 0;
    while (result && offset < (size_t)length.QuadPart) {
        DWORD count = 0;
        if (!ReadFile(file, result + offset, (DWORD)((size_t)length.QuadPart - offset), &count, NULL) || !count) { free(result); result = NULL; break; }
        offset += count;
    }
    CloseHandle(file);
    if (result) { result[offset] = 0; *size = offset; }
    return result;
}
static bool quarantine_invalid_registry_binary(const char *path, const char *reason, char *error, size_t error_size) {
    (void)path;
    snprintf(error, error_size, "Registry could not be loaded: %s", reason);
    return false;
}
#include "../outershelld/RegistryStore.inc"

static bool request_text(const unsigned char *request, size_t length, size_t ref, char **value) {
    *value = NULL;
    if (ref > length || length - ref < 8) return false;
    uint32_t offset = read_uint32_le(request + ref), size = read_uint32_le(request + ref + 4);
    if (offset > length || size > length - offset || size > 32767 || memchr(request + offset, 0, size)) return false;
    if (size && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)request + offset, (int)size, NULL, 0)) return false;
    *value = malloc((size_t)size + 1);
    if (!*value) return false;
    memcpy(*value, request + offset, size); (*value)[size] = 0; return true;
}
static bool response_text(StringBuilder *response, size_t ref, const char *value) {
    return binary_append_string_ref_at(response, ref, value ? value : "");
}
static bool valid_pipe(const char *path) {
    size_t length = strlen(path);
    if (length <= 9 || length > 110 || strncmp(path, "\\\\.\\pipe\\", 9)) return false;
    for (size_t i = 9; i < length; i++) {
        unsigned char c = path[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}
static bool build_list(RegistryStore *store, uint16_t type, const char *filter, StringBuilder *response) {
    size_t count = 0, row_size = type == 12 ? 36 : type == 15 ? 88 : 16;
    if (type == 12) for (size_t i = 0; i < store->backend_count; i++) if (!filter[0] || !strcmp(filter, store->backends[i].service_id)) count++;
    if (type == 15) for (size_t i = 0; i < store->frontend_count; i++) if (!filter[0] || !strcmp(filter, store->frontends[i].service_id)) count++;
    if (type == 18) for (size_t i = 0; i < store->log_count; i++) if (!filter[0] || !strcmp(filter, store->logs[i].service_id)) count++;
    if (count > (OUTERSHELL_API_MAX_FRAME_SIZE - 22) / row_size || !binary_append_zero(response, 22 + count * row_size)) return false;
    uint16_t response_type = type == 12 ? 101 : type == 15 ? 102 : 103;
    binary_write_u16_at(response, 0, response_type); binary_write_u32_at(response, 14, (uint32_t)count); binary_write_u32_at(response, 18, (uint32_t)row_size);
    size_t row = 22;
    bool ok = true;
    if (type == 12) for (size_t i = 0; ok && i < store->backend_count; i++) {
        RegistryBackendRecord *record = &store->backends[i];
        if (filter[0] && strcmp(filter, record->service_id)) continue;
        binary_write_u32_at(response, row, record->owns_unit ? 1 : 0);
        ok = response_text(response, row + 4, record->service_id) && response_text(response, row + 12, record->display_name) &&
            response_text(response, row + 20, record->unit_name) && response_text(response, row + 28, record->unit_path);
        row += row_size;
    }
    if (type == 15) for (size_t i = 0; ok && i < store->frontend_count; i++) {
        RegistryFrontendRecord *record = &store->frontends[i];
        if (filter[0] && strcmp(filter, record->service_id)) continue;
        binary_write_u16_at(response, row, record->endpoint_kind); binary_write_u16_at(response, row + 2, record->endpoint_scheme);
        binary_write_u16_at(response, row + 4, record->endpoint_flags); binary_write_u16_at(response, row + 6, (uint16_t)record->port);
        const char *fields[] = {record->frontend_id, record->service_id, record->display_name, record->host, record->socket_path,
            record->external_socket_path, record->path, record->url, record->icon_path};
        for (size_t field = 0; ok && field < 9; field++) ok = response_text(response, row + 8 + field * 8, fields[field]);
        row += row_size;
    }
    if (type == 18) for (size_t i = 0; ok && i < store->log_count; i++) {
        RegistryLogFileRecord *record = &store->logs[i];
        if (filter[0] && strcmp(filter, record->service_id)) continue;
        ok = response_text(response, row, record->path) && response_text(response, row + 8, record->service_id); row += row_size;
    }
    return ok;
}

__declspec(dllexport) void __cdecl OuterShellRegistryFree(void *bytes) { free(bytes); }
__declspec(dllexport) int __cdecl OuterShellRegistryRequest(const char *path, const unsigned char *request,
    uint32_t length, unsigned char **bytes, uint32_t *byte_count)
{
    if (!path || !request || !bytes || !byte_count || length < 2 || length > OUTERSHELL_API_MAX_FRAME_SIZE) return 0;
    *bytes = NULL; *byte_count = 0;
    char normalized_path[PATH_MAX];
    size_t path_length = strlen(path);
    if (path_length >= sizeof(normalized_path)) return 0;
    for (size_t i = 0; i <= path_length; i++) normalized_path[i] = path[i] == '\\' ? '/' : path[i];
    path = normalized_path;
    AcquireSRWLockExclusive(&registry_mutex);
    uint16_t type = read_uint16_le(request);
    bool writable = type != 12 && type != 15 && type != 18 && type != 28;
    bool opened = false, ok = false;
    char error[1024] = "Invalid registry request.";
    char *fields[10] = {0};
    RegistryStore store;
    StringBuilder response = {0}, output = {0};
    opened = registry_store_open_at(&store, path, writable, error, sizeof(error));
    if (opened) {
#define READ_REF(slot, offset) request_text(request, length, offset, &fields[slot])
        switch (type) {
        case 10:
            ok = length >= 28 && READ_REF(0, 4) && READ_REF(1, 12) && READ_REF(2, 20) && fields[0][0] && fields[1][0] &&
                registry_store_upsert_backend(&store, fields[0], fields[1], fields[2], "", (read_uint16_le(request + 2) & 1) != 0);
            break;
        case 11:
            ok = READ_REF(0, 2) && fields[0][0];
            if (ok) registry_store_remove_backend_and_owned_records(&store, fields[0]);
            break;
        case 12: case 15: case 18:
            ok = READ_REF(0, 2) && build_list(&store, type, fields[0], &response);
            break;
        case 13:
            ok = length >= 82;
            for (size_t i = 0; ok && i < 7; i++) ok = READ_REF(i, 10 + i * 8);
            if (ok) ok = READ_REF(7, 74);
            if (ok) {
                uint16_t kind = read_uint16_le(request + 2), scheme = read_uint16_le(request + 4);
                ok = fields[0][0] && fields[1][0] && fields[2][0] && registry_store_find_backend(&store, fields[0]) &&
                    (kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP || (kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE && valid_pipe(fields[5]))) &&
                    (scheme <= OUTERSHELLD_API_FRONTEND_SCHEME_HTTPS) && !read_uint16_le(request + 6) &&
                    (!fields[7][0] || valid_pipe(fields[7]));
                if (ok) ok = registry_store_upsert_frontend_endpoint(&store, fields[1], fields[0], fields[2], kind, scheme, 0,
                    fields[4], read_uint16_le(request + 8), fields[3], fields[5], fields[6], true);
                if (ok) ok = registry_assign_string(&registry_store_find_frontend(&store, fields[1])->external_socket_path, fields[7]);
            }
            break;
        case 14:
            ok = length >= 30 && READ_REF(0, 6) && READ_REF(1, 14) && READ_REF(2, 22);
            if (ok) for (size_t i = store.frontend_count; i > 0; i--) {
                RegistryFrontendRecord *record = &store.frontends[i - 1];
                if (strcmp(fields[0], record->service_id)) continue;
                if ((fields[1][0] && !strcmp(fields[1], record->frontend_id)) ||
                    (!fields[1][0] && fields[2][0] && !strcmp(fields[2], record->socket_path)) ||
                    (!fields[1][0] && !fields[2][0] && read_uint32_le(request + 2) == (uint32_t)record->port)) registry_store_remove_frontend_at(&store, i - 1);
            }
            break;
        case 16: case 17:
            ok = READ_REF(0, 2) && READ_REF(1, 10) && fields[0][0] && fields[1][0];
            if (ok && type == 16) ok = registry_store_upsert_log(&store, fields[1], fields[0]);
            if (ok && type == 17) for (size_t i = store.log_count; i > 0; i--) {
                if (!strcmp(store.logs[i - 1].service_id, fields[0]) && !strcmp(store.logs[i - 1].path, fields[1])) registry_store_remove_log_at(&store, i - 1);
            }
            break;
        case 28: case 29: case 30:
            ok = READ_REF(0, 2) && fields[0][0];
            if (ok && type == 28) {
                RegistryResourceRecord *record = registry_store_find_resource(&store, fields[0]);
                ok = record && sb_append_n(&output, (const char *)record->data, record->data_length);
                if (!ok) snprintf(error, sizeof(error), "Resource not found.");
            } else if (ok && type == 29) {
                if (length < 18) ok = false;
                else {
                    uint32_t offset = read_uint32_le(request + 10), size = read_uint32_le(request + 14);
                    ok = offset <= length && size <= length - offset && registry_store_upsert_resource(&store, fields[0], request + offset, size);
                }
            } else if (ok && type == 30) registry_store_remove_resource(&store, fields[0]);
            break;
        default: snprintf(error, sizeof(error), "This registry operation is not supported on Windows."); break;
        }
#undef READ_REF
        bool committed = registry_store_close(&store, ok && writable, error, sizeof(error));
        ok = ok && committed;
    }
    if (!ok || !response.length) {
        free(response.data); response = (StringBuilder){0};
        bool list = type == 12 || type == 15 || type == 18;
        uint16_t response_type = list ? (type == 12 ? 101 : type == 15 ? 102 : 103) : 100;
        bool built = binary_append_zero(&response, 22) && binary_write_u16_at(&response, 0, response_type) &&
            binary_write_u32_at(&response, 2, ok ? 0 : 1);
        if (built && list) built = response_text(&response, 6, ok ? "" : error) &&
            binary_write_u32_at(&response, 18, type == 12 ? 36 : type == 15 ? 88 : 16);
        else if (built) built = binary_append_data_ref_at(&response, 6, output.data, output.length) &&
            response_text(&response, 14, ok ? "" : error);
        if (!built) { free(response.data); response = (StringBuilder){0}; }
    }
    for (size_t i = 0; i < 10; i++) free(fields[i]);
    free(output.data);
    ReleaseSRWLockExclusive(&registry_mutex);
    if (!response.data || response.length > OUTERSHELL_API_MAX_FRAME_SIZE) { free(response.data); return 0; }
    *bytes = (unsigned char *)response.data; *byte_count = (uint32_t)response.length; return 1;
}
