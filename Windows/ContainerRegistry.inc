static WslRegistryRequest g_container_registry;
static void *g_container_registry_context;

enum {
    CONTAINER_OWNS_RUNTIME = 0x01,
    CONTAINER_OWNS_BUILD_CONTEXT = 0x04,
    CONTAINER_SELF_MODIFYING = 0x08,
    CONTAINER_MANAGED_FLAGS = CONTAINER_OWNS_RUNTIME | CONTAINER_OWNS_BUILD_CONTEXT | CONTAINER_SELF_MODIFYING,
    CONTAINER_SET_NAME = 0x01,
    CONTAINER_SET_RUNTIME = 0x02,
    CONTAINER_SET_BUILD_CONTEXT = 0x04,
    CONTAINER_SET_OWNS_RUNTIME = 0x08,
    CONTAINER_SET_CPUS = 0x10,
    CONTAINER_SET_MEMORY = 0x20,
    CONTAINER_SET_ALL = 0x7f
};

typedef struct {
    char *id, *name, *provider, *runtime_name, *build_context;
    uint32_t flags, cpus, memory_mb;
} ContainerRow;

static bool container_registry_exchange(StringBuilder *request, StringBuilder *response) {
    unsigned char *reply = NULL;
    uint32_t length = 0;
    bool ok = g_container_registry && request->data && request->length <= UINT32_MAX &&
              g_container_registry(g_container_registry_context, (const unsigned char *)request->data,
                                   (uint32_t)request->length, &reply, &length);
    free(request->data);
    *request = (StringBuilder){0};
    if (!ok) return false;
    *response = (StringBuilder){(char *)reply, length, length};
    return true;
}

static bool container_registry_command(StringBuilder *request) {
    StringBuilder response = {0};
    bool ok = container_registry_exchange(request, &response) && response.length >= 22 &&
              read_uint16_le((const unsigned char *)response.data) == 100 &&
              read_uint32_le((const unsigned char *)response.data + 2) == 0;
    free(response.data);
    return ok;
}

static bool container_registry_upsert(const char *id, const char *name, ContainerRuntime runtime,
                                      const char *runtime_name, unsigned cpus, unsigned memory_mb,
                                      uint32_t flags, uint32_t mask) {
    StringBuilder request = {0};
    const char *refs[] = {id, name ? name : "", container_provider_id(runtime), runtime_name ? runtime_name : "", ""};
    bool ok = binary_append_zero(&request, 68) && binary_write_u16_at(&request, 0, 31) &&
              binary_write_u32_at(&request, 4, flags) && binary_write_u32_at(&request, 8, mask) &&
              binary_write_u32_at(&request, 12, cpus) && binary_write_u32_at(&request, 16, memory_mb);
    for (size_t i = 0; ok && i < 5; i++) ok = binary_append_string_ref_at(&request, 28 + i * 8, refs[i]);
    if (!ok) {
        free(request.data);
        return false;
    }
    return container_registry_command(&request);
}

static bool container_registry_remove(const char *id) {
    StringBuilder request = {0};
    bool ok = binary_append_zero(&request, 10) && binary_write_u16_at(&request, 0, 32) &&
              binary_append_string_ref_at(&request, 2, id);
    if (!ok) {
        free(request.data);
        return false;
    }
    return container_registry_command(&request);
}

static char *container_registry_text(const StringBuilder *response, size_t ref) {
    const unsigned char *raw = (const unsigned char *)response->data;
    uint32_t offset = read_uint32_le(raw + ref), length = read_uint32_le(raw + ref + 4);
    if (offset > response->length || length > response->length - offset) return NULL;
    char *text = malloc((size_t)length + 1);
    if (!text) return NULL;
    memcpy(text, raw + offset, length);
    text[length] = 0;
    return text;
}

static void container_rows_free(ContainerRow *rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(rows[i].id); free(rows[i].name); free(rows[i].provider);
        free(rows[i].runtime_name); free(rows[i].build_context);
    }
    free(rows);
}

static bool container_registry_rows(const char *filter, ContainerRow **rows, size_t *count) {
    *rows = NULL;
    *count = 0;
    StringBuilder request = {0}, response = {0};
    bool ok = binary_append_zero(&request, 10) && binary_write_u16_at(&request, 0, 33) &&
              binary_append_string_ref_at(&request, 2, filter ? filter : "") &&
              container_registry_exchange(&request, &response);
    free(request.data);
    const unsigned char *raw = (const unsigned char *)response.data;
    ok = ok && response.length >= 22 && read_uint16_le(raw) == 108 && read_uint32_le(raw + 2) == 0 &&
         read_uint32_le(raw + 18) == 64 && read_uint32_le(raw + 14) <= (response.length - 22) / 64;
    size_t total = ok ? read_uint32_le(raw + 14) : 0;
    ContainerRow *values = ok && total ? calloc(total, sizeof(*values)) : NULL;
    if (ok && total && !values) ok = false;
    for (size_t i = 0; ok && i < total; i++) {
        size_t row = 22 + i * 64;
        values[i].flags = read_uint32_le(raw + row);
        values[i].cpus = read_uint32_le(raw + row + 4);
        values[i].memory_mb = read_uint32_le(raw + row + 8);
        values[i].id = container_registry_text(&response, row + 24);
        values[i].name = container_registry_text(&response, row + 32);
        values[i].provider = container_registry_text(&response, row + 40);
        values[i].runtime_name = container_registry_text(&response, row + 48);
        values[i].build_context = container_registry_text(&response, row + 56);
        ok = values[i].id && values[i].name && values[i].provider && values[i].runtime_name && values[i].build_context;
    }
    free(response.data);
    if (!ok) {
        container_rows_free(values, total);
        return false;
    }
    *rows = values;
    *count = total;
    return true;
}

static bool container_registered(const char *id) {
    ContainerRow *rows;
    size_t count;
    bool found = container_registry_rows(id, &rows, &count) && count > 0;
    if (found) container_rows_free(rows, count);
    return found;
}

static char *container_registered_build_context(const char *id) {
    ContainerRow *rows;
    size_t count;
    char *path = NULL;
    if (container_registry_rows(id, &rows, &count)) {
        if (count && rows[0].build_context[0]) path = win_strdup(rows[0].build_context);
        container_rows_free(rows, count);
    }
    return path;
}

static char *container_resource_key(const char *id, const char *name) {
    StringBuilder key = {0};
    append(&key, "containers/");
    append(&key, id);
    append(&key, "/");
    append(&key, name);
    return key.data;
}

static bool container_resource_get(const char *id, const char *name, StringBuilder *data) {
    char *key = container_resource_key(id, name);
    StringBuilder request = {0}, response = {0};
    bool ok = binary_append_zero(&request, 10) && binary_write_u16_at(&request, 0, 28) &&
              binary_append_string_ref_at(&request, 2, key) && container_registry_exchange(&request, &response);
    free(request.data);
    free(key);
    const unsigned char *raw = (const unsigned char *)response.data;
    ok = ok && response.length >= 22 && read_uint16_le(raw) == 100 && read_uint32_le(raw + 2) == 0;
    uint32_t offset = ok ? read_uint32_le(raw + 6) : 0, length = ok ? read_uint32_le(raw + 10) : 0;
    ok = ok && offset <= response.length && length <= response.length - offset && length > 0;
    if (ok) bytes(data, raw + offset, length);
    free(response.data);
    return ok;
}

static bool container_resource_set(const char *id, const char *name, const void *data, size_t length) {
    char *key = container_resource_key(id, name);
    StringBuilder request = {0};
    bool ok = binary_append_zero(&request, 18) && binary_write_u16_at(&request, 0, 29) &&
              binary_append_string_ref_at(&request, 2, key) &&
              binary_append_data_ref_at(&request, 10, data, length);
    free(key);
    if (!ok) {
        free(request.data);
        return false;
    }
    return container_registry_command(&request);
}

static bool container_resource_remove(const char *id, const char *name) {
    char *key = container_resource_key(id, name);
    StringBuilder request = {0};
    bool ok = binary_append_zero(&request, 10) && binary_write_u16_at(&request, 0, 30) &&
              binary_append_string_ref_at(&request, 2, key);
    free(key);
    if (!ok) {
        free(request.data);
        return false;
    }
    return container_registry_command(&request);
}
