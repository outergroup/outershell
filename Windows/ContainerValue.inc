#include <math.h>
#include "../Backend/OffsetArchive.inc"

static void container_write64(unsigned char *p, uint64_t value) {
    write_uint32_le(p, (uint32_t)value);
    write_uint32_le(p + 4, (uint32_t)(value >> 32));
}

static OrwvValue *container_value_parse(const unsigned char **cursor, const unsigned char *end, unsigned depth, bool *absent) {
    *absent = false;
    if (depth > 64) return NULL;
    layout_json_space(cursor, end);
    const unsigned char *p = *cursor;
    if (p == end) return NULL;
    if (*p == '[' || *p == '{') {
        bool dictionary = *p++ == '{';
        OrwvValue *value = dictionary ? orwv_dictionary() : orwv_array();
        layout_json_space(&p, end);
        while (value && p < end && *p != (dictionary ? '}' : ']')) {
            char *key = NULL;
            if (dictionary) {
                if (!layout_json_string_decode(&p, end, &key)) goto invalid;
                layout_json_space(&p, end);
                if (p == end || *p++ != ':') { free(key); goto invalid; }
            }
            bool skipped = false;
            OrwvValue *item = container_value_parse(&p, end, depth + 1, &skipped);
            if (!item && !skipped) { free(key); goto invalid; }
            if (item) {
                bool added = dictionary ? orwv_dictionary_set(value, key, item) : orwv_array_append(value, item);
                if (!added) { free(key); goto invalid; }
            }
            free(key);
            layout_json_space(&p, end);
            if (p < end && *p == ',') { p++; layout_json_space(&p, end); }
            else break;
        }
        if (!value || p == end || *p++ != (dictionary ? '}' : ']')) goto invalid;
        *cursor = p;
        return value;
invalid:
        orwv_free(value);
        return NULL;
    }
    if (*p == '"') {
        char *text = NULL;
        if (!layout_json_string_decode(&p, end, &text)) return NULL;
        OrwvValue *value = orwv_string(text);
        free(text);
        *cursor = p;
        return value;
    }
    const unsigned char *start = p;
    if (!layout_json_value(&p, end, 0)) return NULL;
    *cursor = p;
    if (*start == 't' || *start == 'f') return orwv_bool(*start == 't');
    if (*start == 'n') {
        *absent = true;
        return NULL;
    }
    char number[48];
    size_t size = (size_t)(p - start);
    if (size >= sizeof(number)) return NULL;
    memcpy(number, start, size);
    number[size] = 0;
    char *tail = NULL;
    long long integer = strtoll(number, &tail, 10);
    if (!*tail) return orwv_int(integer);
    double value = strtod(number, &tail);
    return *tail ? NULL : orwv_double(value);
}

static bool container_value_encode(const char *json, StringBuilder *out) {
    const unsigned char *p = (const unsigned char *)json, *end = p + strlen(json);
    bool absent = false;
    OrwvValue *value = container_value_parse(&p, end, 0, &absent);
    layout_json_space(&p, end);
    unsigned char *encoded = NULL;
    size_t length = 0;
    bool ok = value && p == end && orwv_encode(value, &encoded, &length);
    orwv_free(value);
    if (ok) {
        free(out->data);
        *out = (StringBuilder){(char *)encoded, length, length};
    }
    return ok;
}

static bool container_value_write_json(const OrwvValue *value, StringBuilder *json, unsigned depth) {
    if (!value || depth > 64 || json->length > WSL_LIMIT) return false;
    switch (value->type) {
    case ORWV_BOOLEAN:
        append(json, value->as.boolean ? "true" : "false");
        return true;
    case ORWV_INTEGER: {
        char number[32];
        snprintf(number, sizeof(number), "%lld", (long long)value->as.integer);
        append(json, number);
        return true;
    }
    case ORWV_DOUBLE:
    case ORWV_DATE: {
        if (!isfinite(value->as.number)) return false;
        char number[64];
        snprintf(number, sizeof(number), "%.17g", value->as.number);
        append(json, number);
        return true;
    }
    case ORWV_STRING: {
        char *text = orwv_copy_string(value);
        if (!text) return false;
        json_string(json, text);
        free(text);
        return true;
    }
    case ORWV_ARRAY:
        append(json, "[");
        for (size_t i = 0; i < value->as.collection.count; i++) {
            if (i) append(json, ",");
            if (!container_value_write_json(value->as.collection.items[i], json, depth + 1)) return false;
        }
        append(json, "]");
        return true;
    case ORWV_DICTIONARY:
        append(json, "{");
        for (size_t i = 0; i < value->as.collection.count; i++) {
            const OrwvEntry *entry = &value->as.collection.entries[i];
            if (memchr(entry->key, 0, entry->key_length)) return false;
            char *key = malloc(entry->key_length + 1);
            if (!key) return false;
            memcpy(key, entry->key, entry->key_length);
            key[entry->key_length] = 0;
            if (i) append(json, ",");
            json_string(json, key);
            free(key);
            append(json, ":");
            if (!container_value_write_json(entry->value, json, depth + 1)) return false;
        }
        append(json, "}");
        return true;
    default:
        return false;
    }
}

static bool container_value_decode(const StringBuilder *data, StringBuilder *json) {
    OrwvValue *value = data->data ? orwv_decode(data->data, data->length) : NULL;
    bool ok = value && container_value_write_json(value, json, 0);
    orwv_free(value);
    return ok;
}

static void container_send_reply(Wsl *w, Connection *c, HttpRequest *r, const char *error, const char *extra,
                                 bool include_snapshot) {
    StringBuilder result = {0}, snapshot = {0}, message = {0}, wrapped = {0};
    bool ok = true;
    if (extra && *extra) {
        append(&wrapped, "{");
        append(&wrapped, extra);
        append(&wrapped, "}");
        ok = container_value_encode(wrapped.data, &result);
    }
    if (ok && include_snapshot && !error) {
        EnterCriticalSection(&w->gate);
        char *copy = win_strdup(w->snapshot.data ? w->snapshot.data : "{\"hostPlatform\":\"windows\",\"providers\":[],\"workspaces\":[]}");
        LeaveCriticalSection(&w->gate);
        ok = container_value_encode(copy, &snapshot);
        free(copy);
    }
    if (!ok) error = "Outer Shell could not encode the container response.";
    bool built = binary_append_zero(&message, 30) && binary_write_u16_at(&message, 0, 109) &&
                 binary_write_u32_at(&message, 2, error ? 1 : 0) &&
                 binary_append_data_ref_at(&message, 6, error, error ? strlen(error) : 0) &&
                 binary_append_data_ref_at(&message, 14, ok ? result.data : NULL, ok ? result.length : 0) &&
                 binary_append_data_ref_at(&message, 22, ok ? snapshot.data : NULL, ok ? snapshot.length : 0);
    if (built) http_send(c, r, 200, "application/octet-stream", message.data, message.length);
    else http_text(c, r, 500, "Outer Shell could not encode the container response.");
    free(result.data);
    free(snapshot.data);
    free(message.data);
    free(wrapped.data);
}
