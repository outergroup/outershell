enum {
    ORWA_TABLE_BACKENDS = 0,
    ORWA_TABLE_FRONTENDS = 1,
    ORWA_TABLE_FRONTEND_LAYOUTS = 2,
    ORWA_TABLE_LOG_FILES = 3,
    ORWA_TABLE_CONTENT_TYPES = 4,
    ORWA_TABLE_FILE_OPENERS = 5,
    ORWA_TABLE_RESOURCES = 6,
    ORWA_TABLE_CONTAINERS = 7,
    ORWA_TABLE_COUNT = 8,
    ORWA_LEGACY_SEVEN_TABLE_COUNT = 7,
    ORWA_LEGACY_SIX_TABLE_COUNT = 6,
    ORWA_LEGACY_FOUR_TABLE_COUNT = 4,
    ORWA_LEGACY_THREE_TABLE_COUNT = 3,
    ORWA_TABLE_DESCRIPTOR_SIZE = 20,
    ORWA_HEADER_SIZE = 8 + ORWA_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_SEVEN_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_SEVEN_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_SIX_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_SIX_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_FOUR_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_FOUR_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_LEGACY_THREE_TABLE_HEADER_SIZE = 8 + ORWA_LEGACY_THREE_TABLE_COUNT * ORWA_TABLE_DESCRIPTOR_SIZE,
    ORWA_BACKENDS_ROW_SIZE = 68,
    ORWA_LEGACY_BACKENDS_ROW_SIZE = 84,
    ORWA_LEGACY_FRONTENDS_ROW_SIZE = 97,
    ORWA_FRONTENDS_ROW_SIZE = 113,
    ORWA_FRONTENDS_ROW_SIZE_WITH_FLAGS = 117,
    ORWA_STRUCTURED_FRONTENDS_ROW_SIZE = 80,
    ORWA_FRONTEND_LAYOUTS_ROW_SIZE = 32,
    ORWA_LOG_FILES_ROW_SIZE = 32,
    ORWA_LEGACY_CONTENT_TYPES_ROW_SIZE = 112,
    ORWA_CONTENT_TYPES_ROW_SIZE = 96,
    ORWA_FILE_OPENERS_ROW_SIZE = 56,
    ORWA_RESOURCES_ROW_SIZE = 32,
    ORWA_CONTAINERS_ROW_SIZE = 104
};

typedef struct {
    char **items;
    size_t count;
} RegistryStringList;

typedef struct {
    const char *const *items;
    size_t count;
} StaticStringList;

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

static bool binary_append_u16(StringBuilder *builder, uint16_t value) {
    unsigned char bytes[2];
    write_uint16_le(bytes, value);
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

static bool registry_binary_append_data_ref(RegistryBinaryStringPool *pool,
                                            StringBuilder *variable_region,
                                            StringBuilder *rows,
                                            const unsigned char *data,
                                            size_t data_length) {
    if (data_length == 0) return binary_append_string_ref(rows, 0, 0);
    uint64_t absolute_offset = pool->variable_base_offset + (uint64_t)variable_region->length;
    return binary_append_string_ref(rows, absolute_offset, (uint64_t)data_length) &&
           sb_append_n(variable_region, (const char *)data, data_length);
}

static bool registry_binary_append_string_ref32(RegistryBinaryStringPool *pool,
                                                StringBuilder *variable_region,
                                                StringBuilder *rows,
                                                const char *text) {
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!registry_binary_string_ref(pool, variable_region, text, &offset, &length)) return false;
    if (offset > UINT32_MAX || length > UINT32_MAX) return false;
    return binary_append_u32(rows, (uint32_t)offset) &&
           binary_append_u32(rows, (uint32_t)length);
}

static bool registry_binary_append_string_list_ref(RegistryBinaryStringPool *pool,
                                                   StringBuilder *variable_region,
                                                   StringBuilder *rows,
                                                   const RegistryStringList *list) {
    size_t count = list ? list->count : 0;
    if (count == 0) return binary_append_string_ref(rows, 0, 0);
    if (count > SIZE_MAX / 16) return false;
    uint64_t absolute_offset = pool->variable_base_offset + (uint64_t)variable_region->length;
    size_t list_relative_offset = variable_region->length;
    if (!binary_append_string_ref(rows, absolute_offset, (uint64_t)count) ||
        !binary_append_zero(variable_region, count * 16)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        uint64_t item_offset = 0;
        uint64_t item_length = 0;
        if (!registry_binary_string_ref(pool,
                                        variable_region,
                                        list->items[i] ? list->items[i] : "",
                                        &item_offset,
                                        &item_length)) {
            return false;
        }
        write_uint64_le((unsigned char *)variable_region->data + list_relative_offset + i * 16, item_offset);
        write_uint64_le((unsigned char *)variable_region->data + list_relative_offset + i * 16 + 8, item_length);
    }
    return true;
}

static bool registry_binary_output_path(const char *registry_path, char *out, size_t out_size) {
    if (!registry_path || !registry_path[0]) return false;
    const char *basename = strrchr(registry_path, '/');
    basename = basename ? basename + 1 : registry_path;
    if (strcmp(basename, "registry.orwa") == 0) {
        int written = snprintf(out, out_size, "%s", registry_path);
        return written >= 0 && (size_t)written < out_size;
    }
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", registry_path);
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

static bool registry_storage_exists_at(const char *registry_path) {
    char binary_path[PATH_MAX];
    struct stat st;
    return registry_binary_output_path(registry_path, binary_path, sizeof(binary_path)) &&
           stat(binary_path, &st) == 0 &&
           S_ISREG(st.st_mode);
}

static bool registry_binary_lock_path(const char *registry_path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s.lock", registry_path);
    return written >= 0 && (size_t)written < out_size;
}

#ifdef _WIN32
#include "../Windows/RegistryStorage.inc"
#else
#include "RegistryStoragePOSIX.inc"
#endif

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

static bool registry_binary_read_string_ref32(const unsigned char *file,
                                              size_t file_size,
                                              uint64_t variable_offset,
                                              const unsigned char *row_bytes,
                                              size_t offset,
                                              char **out,
                                              char *error,
                                              size_t error_size) {
    return registry_binary_read_string(file,
                                       file_size,
                                       variable_offset,
                                       read_uint32_le(row_bytes + offset),
                                       read_uint32_le(row_bytes + offset + 4),
                                       out,
                                       error,
                                       error_size);
}

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
    uint16_t endpoint_kind;
    uint16_t endpoint_scheme;
    uint16_t endpoint_flags;
    int port;
    char *host;
    char *path;
    char *socket_path;
    char *external_socket_path;
    char *icon_path;
} RegistryFrontendRecord;

typedef struct {
    char *path;
    char *service_id;
} RegistryLogFileRecord;

typedef struct {
    char *service_id;
    char *identifier;
    char *display_name;
    RegistryStringList conforms_to;
    RegistryStringList extensions;
    RegistryStringList mime_types;
} RegistryContentTypeRecord;

typedef struct {
    char *extension;
    char *frontend_id;
    char *url_template;
    int rank;
    uint32_t capabilities;
} RegistryFileOpenerRecord;

typedef struct {
    char *key;
    unsigned char *data;
    size_t data_length;
} RegistryResourceRecord;

typedef struct {
    char *identifier;
    char *display_name;
    char *provider_id;
    char *runtime_name;
    char *project_resource_key;
    uint64_t created_at_milliseconds;
    uint32_t cpus;
    uint32_t memory_in_gb;
    bool owns_container;
} RegistryContainerRecord;

typedef struct {
    RegistryBackendRecord *backends;
    size_t backend_count;
    size_t backend_capacity;
    RegistryFrontendRecord *frontends;
    size_t frontend_count;
    size_t frontend_capacity;
    RegistryLogFileRecord *logs;
    size_t log_count;
    size_t log_capacity;
    RegistryContentTypeRecord *content_types;
    size_t content_type_count;
    size_t content_type_capacity;
    RegistryFileOpenerRecord *openers;
    size_t opener_count;
    size_t opener_capacity;
    RegistryResourceRecord *resources;
    size_t resource_count;
    size_t resource_capacity;
    RegistryContainerRecord *containers;
    size_t container_count;
    size_t container_capacity;
    char binary_path[PATH_MAX];
    int lock_fd;
    bool needs_rewrite;
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

static uint16_t registry_frontend_normalize_endpoint_kind(uint16_t kind) {
    switch (kind) {
    case OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP:
    case OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX:
    case OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE:
        return kind;
    default:
        return OUTERSHELLD_API_FRONTEND_ENDPOINT_NONE;
    }
}

static uint16_t registry_frontend_normalize_scheme(uint16_t scheme) {
    switch (scheme) {
    case OUTERSHELLD_API_FRONTEND_SCHEME_HTTPS:
        return scheme;
    case OUTERSHELLD_API_FRONTEND_SCHEME_HTTP:
    case OUTERSHELLD_API_FRONTEND_SCHEME_DEFAULT:
    default:
        return OUTERSHELLD_API_FRONTEND_SCHEME_HTTP;
    }
}

static const char *registry_frontend_scheme_text(uint16_t scheme) {
    return registry_frontend_normalize_scheme(scheme) == OUTERSHELLD_API_FRONTEND_SCHEME_HTTPS ? "https" : "http";
}

static void registry_frontend_normalize_path(const char *raw, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    const char *path = raw && raw[0] ? raw : "/";
    if (path[0] == '/' || path[0] == '?' || path[0] == '#') {
        if (path[0] == '/' || out_size < 2) {
            snprintf(out, out_size, "%s", path);
        } else {
            snprintf(out, out_size, "/%s", path);
        }
    } else {
        snprintf(out, out_size, "/%s", path);
    }
}

static const char *first_url_suffix(const char *authority) {
    const char *slash = strchr(authority, '/');
    const char *query = strchr(authority, '?');
    const char *fragment = strchr(authority, '#');
    const char *suffix = slash;
    if (!suffix || (query && query < suffix)) suffix = query;
    if (!suffix || (fragment && fragment < suffix)) suffix = fragment;
    return suffix;
}

static void parse_host_port(const char *start,
                            size_t length,
                            char *host,
                            size_t host_size,
                            uint16_t *port) {
    if (host && host_size) host[0] = '\0';
    if (!start || length == 0) return;
    const char *host_end = start + length;
    const char *port_start = NULL;
    if (start[0] == '[') {
        const char *closing = memchr(start, ']', length);
        if (closing) {
            host_end = closing + 1;
            if ((size_t)(host_end - start) < length && *host_end == ':') {
                port_start = host_end + 1;
            }
        }
    } else {
        const char *colon = NULL;
        for (const char *cursor = start; cursor < start + length; cursor++) {
            if (*cursor == ':') colon = cursor;
        }
        if (colon) {
            host_end = colon;
            port_start = colon + 1;
        }
    }
    size_t host_length = (size_t)(host_end - start);
    if (host_length >= host_size) host_length = host_size ? host_size - 1 : 0;
    if (host && host_size) {
        memcpy(host, start, host_length);
        host[host_length] = '\0';
    }
    if (port_start && port_start < start + length && port) {
        char buffer[16];
        size_t port_length = (size_t)((start + length) - port_start);
        if (port_length >= sizeof(buffer)) port_length = sizeof(buffer) - 1;
        memcpy(buffer, port_start, port_length);
        buffer[port_length] = '\0';
        char *end = NULL;
        long value = strtol(buffer, &end, 10);
        if (end && *end == '\0' && value > 0 && value <= 65535) {
            *port = (uint16_t)value;
        }
    }
}

static void registry_frontend_parse_legacy_endpoint(const char *raw_url,
                                                    uint32_t raw_port,
                                                    const char *raw_socket_path,
                                                    uint16_t *endpoint_kind,
                                                    uint16_t *endpoint_scheme,
                                                    char *host,
                                                    size_t host_size,
                                                    uint16_t *port,
                                                    char *path,
                                                    size_t path_size) {
    bool has_socket = raw_socket_path && raw_socket_path[0];
    const char *url = raw_url ? raw_url : "";
    if (endpoint_kind) {
        *endpoint_kind = has_socket
            ? OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX
            : (raw_port > 0 || url[0] ? OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP : OUTERSHELLD_API_FRONTEND_ENDPOINT_NONE);
    }
    if (endpoint_scheme) *endpoint_scheme = OUTERSHELLD_API_FRONTEND_SCHEME_HTTP;
    if (host && host_size) snprintf(host, host_size, "127.0.0.1");
    if (port) *port = raw_port > 65535 ? 0 : (uint16_t)raw_port;
    registry_frontend_normalize_path("/", path, path_size);

    const char *authority = url;
    const char *scheme = strstr(url, "://");
    if (scheme) {
        size_t scheme_length = (size_t)(scheme - url);
        if (endpoint_scheme && scheme_length == 5 && strncmp(url, "https", 5) == 0) {
            *endpoint_scheme = OUTERSHELLD_API_FRONTEND_SCHEME_HTTPS;
        } else if (endpoint_scheme && scheme_length == 4 && strncmp(url, "http", 4) == 0) {
            *endpoint_scheme = OUTERSHELLD_API_FRONTEND_SCHEME_HTTP;
        }
        authority = scheme + 3;
    }

    if (url[0] == '/' || url[0] == '?' || url[0] == '#') {
        registry_frontend_normalize_path(url, path, path_size);
        return;
    }

    const char *suffix = first_url_suffix(authority);
    bool looks_like_authority = scheme != NULL;
    if (!looks_like_authority) {
        const char *slash = suffix ? suffix : authority + strlen(authority);
        looks_like_authority = memchr(authority, ':', (size_t)(slash - authority)) != NULL;
    }
    if (looks_like_authority) {
        const char *authority_end = suffix ? suffix : authority + strlen(authority);
        if (!has_socket) {
            parse_host_port(authority,
                            (size_t)(authority_end - authority),
                            host,
                            host_size,
                            port);
        }
        registry_frontend_normalize_path(suffix ? suffix : "/", path, path_size);
        return;
    }

    if (url[0]) {
        registry_frontend_normalize_path(url, path, path_size);
    }
}

static bool registry_frontend_build_url(uint16_t endpoint_kind,
                                        uint16_t endpoint_scheme,
                                        const char *host,
                                        uint16_t port,
                                        const char *path,
                                        char *out,
                                        size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    char normalized_path[PATH_MAX * 2];
    registry_frontend_normalize_path(path, normalized_path, sizeof(normalized_path));
    endpoint_kind = registry_frontend_normalize_endpoint_kind(endpoint_kind);
    if ((endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX || endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE)) {
        snprintf(out, out_size, "%s", normalized_path);
        return true;
    }
    if (endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP) {
        const char *safe_host = host && host[0] ? host : "127.0.0.1";
        const char *scheme = registry_frontend_scheme_text(endpoint_scheme);
        if (port > 0) {
            snprintf(out, out_size, "%s://%s:%u%s", scheme, safe_host, (unsigned)port, normalized_path);
        } else {
            snprintf(out, out_size, "%s://%s%s", scheme, safe_host, normalized_path);
        }
        return true;
    }
    snprintf(out, out_size, "%s", path && path[0] ? normalized_path : "");
    return true;
}

static void registry_string_list_free(RegistryStringList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static bool registry_string_list_append_copy(RegistryStringList *list, const char *value, size_t length) {
    if (!list) return false;
    char *copy = malloc(length + 1);
    if (!copy) return false;
    if (length > 0) memcpy(copy, value ? value : "", length);
    copy[length] = '\0';
    char **new_items = realloc(list->items, (list->count + 1) * sizeof(*list->items));
    if (!new_items) {
        free(copy);
        return false;
    }
    list->items = new_items;
    list->items[list->count++] = copy;
    return true;
}

static bool registry_next_csv_token(const char **cursor, const char **start, size_t *length) {
    if (!cursor || !*cursor || !start || !length) return false;
    while (**cursor) {
        while (**cursor == ',' || isspace((unsigned char)**cursor)) *cursor += 1;
        if (!**cursor) return false;
        const char *token_start = *cursor;
        const char *token_end = token_start;
        while (*token_end && *token_end != ',') token_end++;
        const char *trim_end = token_end;
        while (trim_end > token_start && isspace((unsigned char)trim_end[-1])) trim_end--;
        *cursor = token_end;
        if (**cursor == ',') *cursor += 1;
        if (trim_end > token_start) {
            *start = token_start;
            *length = (size_t)(trim_end - token_start);
            return true;
        }
    }
    return false;
}

static bool registry_string_list_assign_from_csv(RegistryStringList *slot, const char *csv) {
    RegistryStringList next = {0};
    const char *cursor = csv ? csv : "";
    const char *start = NULL;
    size_t length = 0;
    while (registry_next_csv_token(&cursor, &start, &length)) {
        if (!registry_string_list_append_copy(&next, start, length)) {
            registry_string_list_free(&next);
            return false;
        }
    }
    registry_string_list_free(slot);
    *slot = next;
    return true;
}

static bool registry_string_list_assign_copy(RegistryStringList *slot, const RegistryStringList *source) {
    RegistryStringList next = {0};
    for (size_t i = 0; source && i < source->count; i++) {
        const char *item = source->items[i] ? source->items[i] : "";
        if (!registry_string_list_append_copy(&next, item, strlen(item))) {
            registry_string_list_free(&next);
            return false;
        }
    }
    registry_string_list_free(slot);
    *slot = next;
    return true;
}

static bool string_list_contains(const char *const *items, size_t count, const char *token, bool case_sensitive) {
    if (!items || !token || !token[0]) return false;
    size_t token_len = strlen(token);
    for (size_t i = 0; i < count; i++) {
        const char *item = items[i] ? items[i] : "";
        size_t item_len = strlen(item);
        if (item_len != token_len) continue;
        if (case_sensitive ? strncmp(item, token, token_len) == 0 : strncasecmp(item, token, token_len) == 0) return true;
    }
    return false;
}

static bool registry_string_list_contains(const RegistryStringList *list, const char *token, bool case_sensitive) {
    return list && string_list_contains((const char *const *)list->items, list->count, token, case_sensitive);
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
        free(store->frontends[i].host);
        free(store->frontends[i].path);
        free(store->frontends[i].socket_path);
        free(store->frontends[i].external_socket_path);
        free(store->frontends[i].icon_path);
    }
    free(store->frontends);
    for (size_t i = 0; i < store->log_count; i++) {
        free(store->logs[i].path);
        free(store->logs[i].service_id);
    }
    free(store->logs);
    for (size_t i = 0; i < store->content_type_count; i++) {
        free(store->content_types[i].service_id);
        free(store->content_types[i].identifier);
        free(store->content_types[i].display_name);
        registry_string_list_free(&store->content_types[i].conforms_to);
        registry_string_list_free(&store->content_types[i].extensions);
        registry_string_list_free(&store->content_types[i].mime_types);
    }
    free(store->content_types);
    for (size_t i = 0; i < store->opener_count; i++) {
        free(store->openers[i].extension);
        free(store->openers[i].frontend_id);
        free(store->openers[i].url_template);
    }
    free(store->openers);
    for (size_t i = 0; i < store->resource_count; i++) {
        free(store->resources[i].key);
        free(store->resources[i].data);
    }
    free(store->resources);
    for (size_t i = 0; i < store->container_count; i++) {
        free(store->containers[i].identifier);
        free(store->containers[i].display_name);
        free(store->containers[i].provider_id);
        free(store->containers[i].runtime_name);
        free(store->containers[i].project_resource_key);
    }
    free(store->containers);
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

static const RegistryFrontendRecord *registry_store_find_frontend_const(const RegistryStore *store, const char *frontend_id) {
    for (size_t i = 0; store && i < store->frontend_count; i++) {
        if (strcmp(store->frontends[i].frontend_id, frontend_id ? frontend_id : "") == 0) return &store->frontends[i];
    }
    return NULL;
}

static bool registry_resolve_frontend_id_for_backend(const RegistryStore *store,
                                                     const char *backend,
                                                     const char *frontend_id,
                                                     char *out,
                                                     size_t out_size,
                                                     char *error,
                                                     size_t error_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (frontend_id && frontend_id[0]) {
        snprintf(out, out_size, "%s", frontend_id);
        const RegistryFrontendRecord *frontend = registry_store_find_frontend_const(store, frontend_id);
        if (backend && backend[0] && frontend && strcmp(frontend->service_id ? frontend->service_id : "", backend) != 0) {
            snprintf(error, error_size, "Frontend does not belong to backend.");
            return false;
        }
        return true;
    }
    if (backend && backend[0]) {
        snprintf(out, out_size, "%s:main", backend);
        return true;
    }
    snprintf(error, error_size, "Missing frontend id.");
    return false;
}

static RegistryLogFileRecord *registry_store_find_log_by_path(RegistryStore *store, const char *path) {
    for (size_t i = 0; i < store->log_count; i++) {
        if (strcmp(store->logs[i].path, path ? path : "") == 0) return &store->logs[i];
    }
    return NULL;
}

static RegistryContentTypeRecord *registry_store_find_content_type(RegistryStore *store, const char *service_id, const char *identifier) {
    for (size_t i = 0; i < store->content_type_count; i++) {
        if (strcmp(store->content_types[i].service_id, service_id ? service_id : "") == 0 &&
            strcmp(store->content_types[i].identifier, identifier ? identifier : "") == 0) {
            return &store->content_types[i];
        }
    }
    return NULL;
}

static RegistryFileOpenerRecord *registry_store_find_opener(RegistryStore *store, const char *extension, const char *frontend_id) {
    for (size_t i = 0; i < store->opener_count; i++) {
        if (strcmp(store->openers[i].extension, extension ? extension : "") == 0 &&
            strcmp(store->openers[i].frontend_id, frontend_id ? frontend_id : "") == 0) {
            return &store->openers[i];
        }
    }
    return NULL;
}

static RegistryResourceRecord *registry_store_find_resource(RegistryStore *store, const char *key) {
    for (size_t i = 0; i < store->resource_count; i++) {
        if (strcmp(store->resources[i].key, key ? key : "") == 0) return &store->resources[i];
    }
    return NULL;
}

static bool registry_store_upsert_resource(RegistryStore *store,
                                           const char *key,
                                           const unsigned char *data,
                                           size_t data_length) {
    RegistryResourceRecord *record = registry_store_find_resource(store, key);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, resource, RegistryResourceRecord);
        record = &store->resources[store->resource_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->key, key)) return false;
    }
    unsigned char *copy = NULL;
    if (data_length > 0) {
        copy = malloc(data_length);
        if (!copy) return false;
        memcpy(copy, data, data_length);
    }
    free(record->data);
    record->data = copy;
    record->data_length = data_length;
    return true;
}

static bool registry_store_remove_resource(RegistryStore *store, const char *key) {
    for (size_t index = 0; index < store->resource_count; index++) {
        RegistryResourceRecord *record = &store->resources[index];
        if (strcmp(record->key, key ? key : "") != 0) continue;
        free(record->key);
        free(record->data);
        memmove(record,
                record + 1,
                (store->resource_count - index - 1) * sizeof(*record));
        store->resource_count--;
        return true;
    }
    return false;
}

static RegistryContainerRecord *registry_store_find_container(RegistryStore *store,
                                                               const char *identifier) {
    for (size_t index = 0; index < store->container_count; index++) {
        if (strcmp(store->containers[index].identifier, identifier ? identifier : "") == 0) {
            return &store->containers[index];
        }
    }
    return NULL;
}

static bool registry_store_upsert_container(RegistryStore *store,
                                            const char *identifier,
                                            const char *display_name,
                                            const char *provider_id,
                                            const char *runtime_name,
                                            const char *project_resource_key,
                                            uint64_t created_at_milliseconds,
                                            uint32_t cpus,
                                            uint32_t memory_in_gb,
                                            bool owns_container) {
    RegistryContainerRecord *record = registry_store_find_container(store, identifier);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, container, RegistryContainerRecord);
        record = &store->containers[store->container_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->identifier, identifier)) return false;
    }
    if (!registry_assign_string(&record->display_name, display_name) ||
        !registry_assign_string(&record->provider_id, provider_id) ||
        !registry_assign_string(&record->runtime_name, runtime_name) ||
        !registry_assign_string(&record->project_resource_key, project_resource_key)) {
        return false;
    }
    record->created_at_milliseconds = created_at_milliseconds;
    record->cpus = cpus;
    record->memory_in_gb = memory_in_gb;
    record->owns_container = owns_container;
    return true;
}

static bool registry_store_remove_container(RegistryStore *store, const char *identifier) {
    for (size_t index = 0; index < store->container_count; index++) {
        RegistryContainerRecord *record = &store->containers[index];
        if (strcmp(record->identifier, identifier ? identifier : "") != 0) continue;
        free(record->identifier);
        free(record->display_name);
        free(record->provider_id);
        free(record->runtime_name);
        free(record->project_resource_key);
        memmove(record,
                record + 1,
                (store->container_count - index - 1) * sizeof(*record));
        store->container_count--;
        return true;
    }
    return false;
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

static bool registry_store_upsert_frontend_endpoint(RegistryStore *store,
                                                    const char *frontend_id,
                                                    const char *service_id,
                                                    const char *display_name,
                                                    uint16_t endpoint_kind,
                                                    uint16_t endpoint_scheme,
                                                    uint16_t endpoint_flags,
                                                    const char *host,
                                                    uint16_t port,
                                                    const char *path,
                                                    const char *socket_path,
                                                    const char *icon_path,
                                                    bool preserve_empty_icon) {
    RegistryFrontendRecord *record = registry_store_find_frontend(store, frontend_id);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, frontend, RegistryFrontendRecord);
        record = &store->frontends[store->frontend_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->frontend_id, frontend_id)) return false;
    }
    char normalized_path[PATH_MAX * 2];
    char derived_url[PATH_MAX * 2];
    endpoint_kind = registry_frontend_normalize_endpoint_kind(endpoint_kind);
    endpoint_scheme = registry_frontend_normalize_scheme(endpoint_scheme);
    registry_frontend_normalize_path(path, normalized_path, sizeof(normalized_path));
    if (!registry_frontend_build_url(endpoint_kind,
                                     endpoint_scheme,
                                     host,
                                     port,
                                     normalized_path,
                                     derived_url,
                                     sizeof(derived_url))) {
        return false;
    }
    if (!registry_assign_string(&record->url, derived_url) ||
        !registry_assign_string(&record->service_id, service_id) ||
        !registry_assign_string(&record->display_name, display_name) ||
        !registry_assign_string(&record->host, endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP ? (host && host[0] ? host : "127.0.0.1") : "") ||
        !registry_assign_string(&record->path, normalized_path) ||
        !registry_assign_string(&record->socket_path, socket_path) ||
        (!record->external_socket_path &&
         !registry_assign_string(&record->external_socket_path, ""))) {
        return false;
    }
    record->endpoint_kind = endpoint_kind;
    record->endpoint_scheme = endpoint_scheme;
    record->endpoint_flags = endpoint_flags;
    record->port = endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP ? (int)port : 0;
    if (!preserve_empty_icon || (icon_path && icon_path[0])) {
        if (!registry_assign_string(&record->icon_path, icon_path)) return false;
    } else if (!record->icon_path && !registry_assign_string(&record->icon_path, "")) {
        return false;
    }
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
                                           bool preserve_empty_icon) {
    uint16_t endpoint_kind = OUTERSHELLD_API_FRONTEND_ENDPOINT_NONE;
    uint16_t endpoint_scheme = OUTERSHELLD_API_FRONTEND_SCHEME_HTTP;
    uint16_t endpoint_port = 0;
    char host[PATH_MAX] = "";
    char path[PATH_MAX * 2] = "";
    registry_frontend_parse_legacy_endpoint(url,
                                            port > 0 ? (uint32_t)port : 0,
                                            socket_path,
                                            &endpoint_kind,
                                            &endpoint_scheme,
                                            host,
                                            sizeof(host),
                                            &endpoint_port,
                                            path,
                                            sizeof(path));
    return registry_store_upsert_frontend_endpoint(store,
                                                   frontend_id,
                                                   service_id,
                                                   display_name,
                                                   endpoint_kind,
                                                   endpoint_scheme,
                                                   0,
                                                   host,
                                                   endpoint_port,
                                                   path,
                                                   socket_path,
                                                   icon_path,
                                                   preserve_empty_icon);
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

static bool registry_store_upsert_content_type_lists(RegistryStore *store,
                                                     const char *service_id,
                                                     const char *identifier,
                                                     const char *display_name,
                                                     const RegistryStringList *conforms_to,
                                                     const RegistryStringList *extensions,
                                                     const RegistryStringList *mime_types) {
    RegistryContentTypeRecord *record = registry_store_find_content_type(store, service_id, identifier);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, content_type, RegistryContentTypeRecord);
        record = &store->content_types[store->content_type_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->service_id, service_id) ||
            !registry_assign_string(&record->identifier, identifier)) {
            return false;
        }
    }
    return registry_assign_string(&record->display_name, display_name) &&
           registry_string_list_assign_copy(&record->conforms_to, conforms_to) &&
           registry_string_list_assign_copy(&record->extensions, extensions) &&
           registry_string_list_assign_copy(&record->mime_types, mime_types);
}

static bool registry_store_upsert_content_type(RegistryStore *store,
                                               const char *service_id,
                                               const char *identifier,
                                               const char *display_name,
                                               const char *conforms_to,
                                               const char *extensions,
                                               const char *mime_types) {
    RegistryStringList conforms_to_list = {0};
    RegistryStringList extensions_list = {0};
    RegistryStringList mime_types_list = {0};
    bool ok = registry_string_list_assign_from_csv(&conforms_to_list, conforms_to) &&
              registry_string_list_assign_from_csv(&extensions_list, extensions) &&
              registry_string_list_assign_from_csv(&mime_types_list, mime_types);
    if (ok) {
        ok = registry_store_upsert_content_type_lists(store,
                                                      service_id,
                                                      identifier,
                                                      display_name,
                                                      &conforms_to_list,
                                                      &extensions_list,
                                                      &mime_types_list);
    }
    registry_string_list_free(&conforms_to_list);
    registry_string_list_free(&extensions_list);
    registry_string_list_free(&mime_types_list);
    return ok;
}

static bool registry_store_upsert_opener(RegistryStore *store,
                                         const char *extension,
                                         const char *frontend_id,
                                         const char *url_template,
                                         int rank,
                                         uint32_t capabilities) {
    RegistryFileOpenerRecord *record = registry_store_find_opener(store, extension, frontend_id);
    if (!record) {
        REGISTRY_ENSURE_CAPACITY(store, opener, RegistryFileOpenerRecord);
        record = &store->openers[store->opener_count++];
        memset(record, 0, sizeof(*record));
        if (!registry_assign_string(&record->extension, extension) ||
            !registry_assign_string(&record->frontend_id, frontend_id)) {
            return false;
        }
    }
    if (!registry_assign_string(&record->url_template, (url_template && url_template[0]) ? url_template : "?file={file}")) {
        return false;
    }
    record->rank = rank;
    record->capabilities = normalize_opener_capabilities(capabilities);
    return true;
}

static void registry_store_remove_opener_at(RegistryStore *store, size_t index);

static void registry_store_remove_frontend_at(RegistryStore *store, size_t index) {
    if (index >= store->frontend_count) return;
    RegistryFrontendRecord *frontend = &store->frontends[index];
    for (size_t i = store->opener_count; i > 0; i--) {
        if (frontend->frontend_id && strcmp(store->openers[i - 1].frontend_id ? store->openers[i - 1].frontend_id : "", frontend->frontend_id) == 0) {
            registry_store_remove_opener_at(store, i - 1);
        }
    }
    RegistryFrontendRecord *record = &store->frontends[index];
    free(record->frontend_id);
    free(record->url);
    free(record->service_id);
    free(record->display_name);
    free(record->host);
    free(record->path);
    free(record->socket_path);
    free(record->external_socket_path);
    free(record->icon_path);
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
    free(record->service_id);
    free(record->identifier);
    free(record->display_name);
    registry_string_list_free(&record->conforms_to);
    registry_string_list_free(&record->extensions);
    registry_string_list_free(&record->mime_types);
    memmove(record, record + 1, (store->content_type_count - index - 1) * sizeof(*record));
    store->content_type_count--;
}

static void registry_store_clear_content_types(RegistryStore *store) {
    for (size_t i = store->content_type_count; i > 0; i--) {
        registry_store_remove_content_type_at(store, i - 1);
    }
}

static void registry_store_remove_opener_at(RegistryStore *store, size_t index) {
    if (index >= store->opener_count) return;
    RegistryFileOpenerRecord *record = &store->openers[index];
    free(record->extension);
    free(record->frontend_id);
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

static void registry_store_clear_backend_content_types(RegistryStore *store, const char *service_id) {
    for (size_t i = store->content_type_count; i > 0; i--) {
        if (strcmp(store->content_types[i - 1].service_id, service_id ? service_id : "") == 0) {
            registry_store_remove_content_type_at(store, i - 1);
        }
    }
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
        const RegistryFrontendRecord *frontend = registry_store_find_frontend_const(store, store->openers[i - 1].frontend_id);
        if (frontend && strcmp(frontend->service_id ? frontend->service_id : "", service_id ? service_id : "") == 0) {
            registry_store_remove_opener_at(store, i - 1);
        }
    }
}

static bool registry_store_remove_backend_and_owned_records(RegistryStore *store, const char *service_id) {
    registry_store_clear_backend_content_types(store, service_id);
    registry_store_clear_backend_openers(store, service_id);
    registry_store_clear_backend_frontends(store, service_id);
    registry_store_clear_backend_logs(store, service_id);
    return registry_store_remove_backend(store, service_id);
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

static bool registry_store_read_data(const unsigned char *bytes,
                                     size_t file_size,
                                     uint64_t variable_offset,
                                     const unsigned char *row_bytes,
                                     size_t offset,
                                     unsigned char **out,
                                     size_t *out_length,
                                     char *error,
                                     size_t error_size) {
    if (!out || !out_length) return false;
    *out = NULL;
    *out_length = 0;
    uint64_t data_offset = read_uint64_le(row_bytes + offset);
    uint64_t data_length = read_uint64_le(row_bytes + offset + 8);
    if (data_offset == 0 && data_length == 0) return true;
    if (data_offset < variable_offset ||
        data_offset > (uint64_t)file_size ||
        data_length > (uint64_t)file_size - data_offset ||
        data_length > SIZE_MAX) {
        snprintf(error, error_size, "Registry binary data reference is out of bounds.");
        return false;
    }
    unsigned char *copy = malloc((size_t)data_length);
    if (!copy) {
        snprintf(error, error_size, "Out of memory.");
        return false;
    }
    memcpy(copy, bytes + data_offset, (size_t)data_length);
    *out = copy;
    *out_length = (size_t)data_length;
    return true;
}

static bool registry_store_read_string_list(const unsigned char *bytes,
                                            size_t file_size,
                                            uint64_t variable_offset,
                                            const unsigned char *row_bytes,
                                            size_t offset,
                                            RegistryStringList *out,
                                            char *error,
                                            size_t error_size) {
    if (!out) return false;
    out->items = NULL;
    out->count = 0;
    uint64_t list_offset = read_uint64_le(row_bytes + offset);
    uint64_t count = read_uint64_le(row_bytes + offset + 8);
    if (count == 0) return true;
    if (list_offset < variable_offset ||
        list_offset > (uint64_t)file_size ||
        count > (UINT64_MAX - list_offset) / 16 ||
        count > SIZE_MAX ||
        list_offset + count * 16 > (uint64_t)file_size) {
        snprintf(error, error_size, "Registry binary string list is invalid.");
        return false;
    }
    const unsigned char *list_bytes = bytes + list_offset;
    for (uint64_t i = 0; i < count; i++) {
        char *item = NULL;
        bool ok = registry_binary_read_string(bytes,
                                              file_size,
                                              variable_offset,
                                              read_uint64_le(list_bytes + i * 16),
                                              read_uint64_le(list_bytes + i * 16 + 8),
                                              &item,
                                              error,
                                              error_size);
        if (!ok) {
            registry_string_list_free(out);
            return false;
        }
        ok = registry_string_list_append_copy(out, item ? item : "", strlen(item ? item : ""));
        free(item);
        if (!ok) {
            registry_string_list_free(out);
            snprintf(error, error_size, "Out of memory.");
            return false;
        }
    }
    return true;
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
    bool content_types_table_supported = true;
    /* Reset only the opener table when a bounded pre-release row layout is stale. */
    bool file_openers_table_supported = true;
    if (ok) {
        uint64_t first_table_offset = read_uint64_le(bytes + 8);
        table_count = first_table_offset == ORWA_HEADER_SIZE ? ORWA_TABLE_COUNT :
                      first_table_offset == ORWA_LEGACY_SEVEN_TABLE_HEADER_SIZE ? ORWA_LEGACY_SEVEN_TABLE_COUNT :
                      first_table_offset == ORWA_LEGACY_SIX_TABLE_HEADER_SIZE ? ORWA_LEGACY_SIX_TABLE_COUNT :
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
                                     i == ORWA_TABLE_CONTENT_TYPES ? descriptors[i].row_size :
                                     i == ORWA_TABLE_FILE_OPENERS ? ORWA_FILE_OPENERS_ROW_SIZE :
                                     i == ORWA_TABLE_RESOURCES ? ORWA_RESOURCES_ROW_SIZE :
                                     i == ORWA_TABLE_CONTAINERS ? ORWA_CONTAINERS_ROW_SIZE :
                                     ORWA_LOG_FILES_ROW_SIZE;
        bool row_size_supported = descriptors[i].row_size == expected_row_size;
        bool bounds_valid = descriptors[i].offset <= (uint64_t)file_size &&
                            !(descriptors[i].row_count > 0 && descriptors[i].row_size == 0) &&
                            !(descriptors[i].row_size > 0 && descriptors[i].row_count > UINT64_MAX / descriptors[i].row_size);
        uint64_t table_end = 0;
        if (bounds_valid) {
            uint64_t table_size = descriptors[i].row_count * descriptors[i].row_size;
            bounds_valid = table_size <= (uint64_t)file_size - descriptors[i].offset;
            table_end = descriptors[i].offset + table_size;
        }
        if ((i == ORWA_TABLE_BACKENDS
                ? (descriptors[i].row_size != ORWA_BACKENDS_ROW_SIZE && descriptors[i].row_size != ORWA_LEGACY_BACKENDS_ROW_SIZE)
                : i == ORWA_TABLE_FRONTENDS
                ? (descriptors[i].row_size != ORWA_STRUCTURED_FRONTENDS_ROW_SIZE &&
                   descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE &&
                   descriptors[i].row_size != ORWA_FRONTENDS_ROW_SIZE_WITH_FLAGS &&
                   descriptors[i].row_size != ORWA_LEGACY_FRONTENDS_ROW_SIZE)
                : i == ORWA_TABLE_CONTENT_TYPES
                ? (descriptors[i].row_size != ORWA_CONTENT_TYPES_ROW_SIZE &&
                   descriptors[i].row_size != ORWA_LEGACY_CONTENT_TYPES_ROW_SIZE)
                : i == ORWA_TABLE_FILE_OPENERS
                ? false
                : !row_size_supported) ||
            (i != ORWA_TABLE_CONTENT_TYPES && !bounds_valid)) {
            snprintf(error, error_size, "Registry binary table descriptor is invalid.");
            ok = false;
            break;
        }
        if (i == ORWA_TABLE_CONTENT_TYPES) {
            if (descriptors[i].row_size == ORWA_LEGACY_CONTENT_TYPES_ROW_SIZE) store->needs_rewrite = true;
            if (!bounds_valid) {
                content_types_table_supported = false;
                store->needs_rewrite = true;
            }
        }
        if (i == ORWA_TABLE_FILE_OPENERS && descriptors[i].row_size != ORWA_FILE_OPENERS_ROW_SIZE) {
            file_openers_table_supported = false;
            store->needs_rewrite = true;
        }
        if (bounds_valid && table_end > variable_offset) variable_offset = table_end;
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
        char *url = NULL, *service_id = NULL, *display_name = NULL, *icon_path = NULL, *socket_path = NULL, *frontend_id = NULL;
        char *host = NULL, *path = NULL, *external_socket_path = NULL;
        uint16_t endpoint_kind = OUTERSHELLD_API_FRONTEND_ENDPOINT_NONE;
        uint16_t endpoint_scheme = OUTERSHELLD_API_FRONTEND_SCHEME_HTTP;
        uint16_t endpoint_flags = 0;
        uint16_t port = 0;
        if (descriptors[ORWA_TABLE_FRONTENDS].row_size == ORWA_STRUCTURED_FRONTENDS_ROW_SIZE) {
            ok = registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 0, &frontend_id, error, error_size) &&
                 registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 8, &service_id, error, error_size) &&
                 registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 16, &display_name, error, error_size) &&
                 registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 24, &icon_path, error, error_size) &&
                 registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 48, &path, error, error_size);
            if (ok) {
                endpoint_kind = read_uint16_le(row_bytes + 40);
                endpoint_flags = read_uint16_le(row_bytes + 42);
                endpoint_scheme = read_uint16_le(row_bytes + 44);
                if (endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP) {
                    ok = registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 56, &host, error, error_size);
                    port = read_uint16_le(row_bytes + 64);
                    socket_path = registry_strdup("");
                } else if ((endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX || endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE)) {
                    ok = registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 56, &socket_path, error, error_size) &&
                         registry_binary_read_string_ref32(bytes, file_size, variable_offset, row_bytes, 64, &external_socket_path, error, error_size);
                    host = registry_strdup("");
                } else if (endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NONE) {
                    host = registry_strdup("");
                    socket_path = registry_strdup("");
                    external_socket_path = registry_strdup("");
                } else {
                    snprintf(error, error_size, "Registry binary frontend endpoint kind is unsupported.");
                    ok = false;
                }
                if (ok && (!host || !socket_path ||
                           ((endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX || endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE) && !external_socket_path))) {
                    snprintf(error, error_size, "Out of memory.");
                    ok = false;
                }
            }
            if (ok) {
                ok = registry_store_upsert_frontend_endpoint(store,
                                                             frontend_id,
                                                             service_id,
                                                             display_name,
                                                             endpoint_kind,
                                                             endpoint_scheme,
                                                             endpoint_flags,
                                                             host,
                                                             port,
                                                             path,
                                                             socket_path,
                                                             icon_path,
                                                             false);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            if (ok && (endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX || endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE)) {
                RegistryFrontendRecord *record = registry_store_find_frontend(store, frontend_id);
                ok = record && registry_assign_string(&record->external_socket_path,
                                                      external_socket_path ? external_socket_path : "");
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
        } else {
            uint32_t legacy_port = 0;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &url, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &service_id, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &display_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48, &icon_path, error, error_size);
            if (ok) {
                uint8_t legacy_endpoint_kind = row_bytes[80];
                if (legacy_endpoint_kind == 1u) {
                    legacy_port = read_uint32_le(row_bytes + 81);
                    socket_path = registry_strdup("");
                } else if (legacy_endpoint_kind == 2u) {
                    ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 81, &socket_path, error, error_size);
                } else if (legacy_endpoint_kind == 0u) {
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
                ok = registry_store_upsert_frontend(store, frontend_id, url, service_id, display_name, (int)legacy_port, socket_path, icon_path, false);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
        }
        free(url);
        free(service_id);
        free(display_name);
        free(icon_path);
        free(socket_path);
        free(frontend_id);
        free(host);
        free(path);
        free(external_socket_path);
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

    if (ok && table_count >= ORWA_LEGACY_SIX_TABLE_COUNT && content_types_table_supported) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_CONTENT_TYPES].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_CONTENT_TYPES].offset + row * descriptors[ORWA_TABLE_CONTENT_TYPES].row_size;
            char *service_id = NULL, *identifier = NULL, *display_name = NULL;
            RegistryStringList conforms_to = {0};
            RegistryStringList extensions = {0};
            RegistryStringList mime_types = {0};
            const size_t mime_types_offset = descriptors[ORWA_TABLE_CONTENT_TYPES].row_size == ORWA_LEGACY_CONTENT_TYPES_ROW_SIZE ? 96 : 80;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &service_id, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &identifier, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &display_name, error, error_size) &&
                 registry_store_read_string_list(bytes, file_size, variable_offset, row_bytes, 48, &conforms_to, error, error_size) &&
                 registry_store_read_string_list(bytes, file_size, variable_offset, row_bytes, 64, &extensions, error, error_size) &&
                 registry_store_read_string_list(bytes, file_size, variable_offset, row_bytes, mime_types_offset, &mime_types, error, error_size);
            if (ok) {
                ok = registry_store_upsert_content_type_lists(store,
                                                              service_id,
                                                              identifier,
                                                              display_name,
                                                              &conforms_to,
                                                              &extensions,
                                                              &mime_types);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(service_id);
            free(identifier);
            free(display_name);
            registry_string_list_free(&conforms_to);
            registry_string_list_free(&extensions);
            registry_string_list_free(&mime_types);
        }
        if (!ok) {
            registry_store_clear_content_types(store);
            store->needs_rewrite = true;
            error[0] = '\0';
            ok = true;
        }
    }

    if (ok && table_count >= ORWA_LEGACY_SIX_TABLE_COUNT && file_openers_table_supported) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_FILE_OPENERS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_FILE_OPENERS].offset + row * descriptors[ORWA_TABLE_FILE_OPENERS].row_size;
            char *extension = NULL, *frontend_id = NULL, *url_template = NULL;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0, &extension, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16, &frontend_id, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32, &url_template, error, error_size);
            if (ok) {
                ok = registry_store_upsert_opener(store,
                                                  extension,
                                                  frontend_id,
                                                  url_template,
                                                  (int)read_uint32_le(row_bytes + 48),
                                                  read_uint32_le(row_bytes + 52));
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(extension);
            free(frontend_id);
            free(url_template);
        }
    }

    if (ok && table_count >= ORWA_LEGACY_SEVEN_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_RESOURCES].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_RESOURCES].offset +
                row * ORWA_RESOURCES_ROW_SIZE;
            char *key = NULL;
            unsigned char *data = NULL;
            size_t data_length = 0;
            ok = registry_store_read_string(bytes,
                                            file_size,
                                            variable_offset,
                                            row_bytes,
                                            0,
                                            &key,
                                            error,
                                            error_size) &&
                 registry_store_read_data(bytes,
                                          file_size,
                                          variable_offset,
                                          row_bytes,
                                          16,
                                          &data,
                                          &data_length,
                                          error,
                                          error_size);
            if (ok) {
                ok = registry_store_upsert_resource(store, key, data, data_length);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(key);
            free(data);
        }
    }

    if (ok && table_count == ORWA_TABLE_COUNT) {
        for (uint64_t row = 0; ok && row < descriptors[ORWA_TABLE_CONTAINERS].row_count; row++) {
            const unsigned char *row_bytes = bytes + descriptors[ORWA_TABLE_CONTAINERS].offset +
                row * ORWA_CONTAINERS_ROW_SIZE;
            char *identifier = NULL;
            char *display_name = NULL;
            char *provider_id = NULL;
            char *runtime_name = NULL;
            char *project_resource_key = NULL;
            ok = registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 0,
                                            &identifier, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 16,
                                            &display_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 32,
                                            &provider_id, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 48,
                                            &runtime_name, error, error_size) &&
                 registry_store_read_string(bytes, file_size, variable_offset, row_bytes, 64,
                                            &project_resource_key, error, error_size);
            if (ok) {
                ok = registry_store_upsert_container(store,
                                                     identifier,
                                                     display_name,
                                                     provider_id,
                                                     runtime_name,
                                                     project_resource_key,
                                                     read_uint64_le(row_bytes + 80),
                                                     read_uint32_le(row_bytes + 88),
                                                     read_uint32_le(row_bytes + 92),
                                                     (read_uint32_le(row_bytes + 96) & 1u) != 0);
                if (!ok) snprintf(error, error_size, "Out of memory.");
            }
            free(identifier);
            free(display_name);
            free(provider_id);
            free(runtime_name);
            free(project_resource_key);
        }
    }

    free(file_data);
    return ok;
}

static bool registry_store_write_orwa_file(RegistryStore *store, const char *path, char *error, size_t error_size) {
    RegistryBinaryTableDescriptor descriptors[ORWA_TABLE_COUNT] = {
        {.row_count = store->backend_count, .row_size = ORWA_BACKENDS_ROW_SIZE},
        {.row_count = store->frontend_count, .row_size = ORWA_STRUCTURED_FRONTENDS_ROW_SIZE},
        {.row_count = 0, .row_size = ORWA_FRONTEND_LAYOUTS_ROW_SIZE},
        {.row_count = store->log_count, .row_size = ORWA_LOG_FILES_ROW_SIZE},
        {.row_count = store->content_type_count, .row_size = ORWA_CONTENT_TYPES_ROW_SIZE},
        {.row_count = store->opener_count, .row_size = ORWA_FILE_OPENERS_ROW_SIZE},
        {.row_count = store->resource_count, .row_size = ORWA_RESOURCES_ROW_SIZE},
        {.row_count = store->container_count, .row_size = ORWA_CONTAINERS_ROW_SIZE},
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
        unsigned char reserved2[2] = {0};
        unsigned char reserved14[14] = {0};
        unsigned char reserved8[8] = {0};
        unsigned char reserved16[16] = {0};
        ok = registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->frontend_id) &&
             registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->service_id) &&
             registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->display_name) &&
             registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->icon_path) &&
             sb_append_n(&rows, (const char *)reserved8, sizeof(reserved8)) &&
             binary_append_u16(&rows, record->endpoint_kind) &&
             binary_append_u16(&rows, record->endpoint_flags) &&
             binary_append_u16(&rows, record->endpoint_scheme) &&
             sb_append_n(&rows, (const char *)reserved2, sizeof(reserved2)) &&
             registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->path);
        if (ok && (record->endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_UNIX || record->endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_NAMED_PIPE)) {
            ok = registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->socket_path) &&
                 registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->external_socket_path) &&
                 sb_append_n(&rows, (const char *)reserved8, sizeof(reserved8));
        } else if (ok && record->endpoint_kind == OUTERSHELLD_API_FRONTEND_ENDPOINT_TCP) {
            ok = registry_binary_append_string_ref32(&pool, &variable_region, &rows, record->host) &&
                 binary_append_u16(&rows, record->port > 0 ? (uint16_t)record->port : 0) &&
                 sb_append_n(&rows, (const char *)reserved14, sizeof(reserved14));
        } else if (ok) {
            ok = sb_append_n(&rows, (const char *)reserved8, sizeof(reserved8)) &&
                 sb_append_n(&rows, (const char *)reserved16, sizeof(reserved16));
        }
    }
    for (size_t i = 0; ok && i < store->log_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->logs[i].path) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->logs[i].service_id);
    }
    for (size_t i = 0; ok && i < store->content_type_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].service_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].identifier) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->content_types[i].display_name) &&
             registry_binary_append_string_list_ref(&pool, &variable_region, &rows, &store->content_types[i].conforms_to) &&
             registry_binary_append_string_list_ref(&pool, &variable_region, &rows, &store->content_types[i].extensions) &&
             registry_binary_append_string_list_ref(&pool, &variable_region, &rows, &store->content_types[i].mime_types);
    }
    for (size_t i = 0; ok && i < store->opener_count; i++) {
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].extension) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].frontend_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, store->openers[i].url_template) &&
             binary_append_u32(&rows, (uint32_t)store->openers[i].rank) &&
             binary_append_u32(&rows, normalize_opener_capabilities(store->openers[i].capabilities));
    }
    for (size_t i = 0; ok && i < store->resource_count; i++) {
        ok = registry_binary_append_string_ref(&pool,
                                               &variable_region,
                                               &rows,
                                               store->resources[i].key) &&
             registry_binary_append_data_ref(&pool,
                                             &variable_region,
                                             &rows,
                                             store->resources[i].data,
                                             store->resources[i].data_length);
    }
    for (size_t i = 0; ok && i < store->container_count; i++) {
        RegistryContainerRecord *record = &store->containers[i];
        ok = registry_binary_append_string_ref(&pool, &variable_region, &rows, record->identifier) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->display_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->provider_id) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->runtime_name) &&
             registry_binary_append_string_ref(&pool, &variable_region, &rows, record->project_resource_key) &&
             binary_append_u64(&rows, record->created_at_milliseconds) &&
             binary_append_u32(&rows, record->cpus) &&
             binary_append_u32(&rows, record->memory_in_gb) &&
             binary_append_u32(&rows, record->owns_container ? 1u : 0u) &&
             binary_append_u32(&rows, 0u);
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
    if (!ok && writable) {
        char load_error[512];
        snprintf(load_error, sizeof(load_error), "%s", error && error[0] ? error : "unknown error");
        char binary_path[PATH_MAX];
        snprintf(binary_path, sizeof(binary_path), "%s", store->binary_path);
        if (quarantine_invalid_registry_binary(store->binary_path, load_error, error, error_size)) {
            int lock_fd = store->lock_fd;
            store->lock_fd = -1;
            registry_store_free(store);
            memset(store, 0, sizeof(*store));
            store->lock_fd = lock_fd;
            snprintf(store->binary_path, sizeof(store->binary_path), "%s", binary_path);
            ok = true;
            error[0] = '\0';
        }
    }
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

