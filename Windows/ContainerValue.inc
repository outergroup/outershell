#include <math.h>
static void container_write64(unsigned char *p, uint64_t value) {
    write_uint32_le(p, (uint32_t)value); write_uint32_le(p + 4, (uint32_t)(value >> 32));
}
static uint64_t container_value_node(StringBuilder *out, unsigned type, uint64_t payload, uint64_t count) {
    uint64_t offset = out->length;
    unsigned char node[24] = {0}; node[0] = (unsigned char)type;
    container_write64(node + 8, payload); container_write64(node + 16, count);
    bytes(out, node, sizeof(node)); return offset;
}

static bool container_value_encode_node(StringBuilder *out, const unsigned char **cursor,
                                        const unsigned char *end, unsigned depth, uint64_t *offset) {
    if (depth > 64) return false;
    layout_json_space(cursor, end);
    const unsigned char *p = *cursor;
    if (p == end) return false;
    if (*p == '[' || *p == '{') {
        bool dictionary = *p++ == '{';
        StringBuilder references = {0}; uint64_t count = 0;
        layout_json_space(&p, end);
        while (p < end && *p != (dictionary ? '}' : ']')) {
            uint64_t key = 0, value = 0;
            if (dictionary) {
                if (*p != '"' || !container_value_encode_node(out, &p, end, depth + 1, &key)) goto invalid;
                layout_json_space(&p, end);
                if (p == end || *p++ != ':') goto invalid;
            }
            if (!container_value_encode_node(out, &p, end, depth + 1, &value)) goto invalid;
            unsigned char ref[8];
            if (dictionary) { container_write64(ref, key); bytes(&references, ref, 8); }
            container_write64(ref, value); bytes(&references, ref, 8); count++;
            layout_json_space(&p, end);
            if (p < end && *p == ',') { p++; layout_json_space(&p, end); }
            else break;
        }
        if (p == end || *p++ != (dictionary ? '}' : ']')) goto invalid;
        uint64_t payload = out->length;
        bytes(out, references.data, references.length);
        *offset = container_value_node(out, dictionary ? 8 : 7, payload, count);
        free(references.data); *cursor = p; return true;
invalid:
        free(references.data); return false;
    }
    const unsigned char *start = p;
    if (!layout_json_value(&p, end, 0)) return false;
    if (*start == '"') {
        StringBuilder wrapped = {0}; append(&wrapped, "{\"v\":"); bytes(&wrapped, start, (size_t)(p - start)); append(&wrapped, "}");
        char *value = container_string(wrapped.data, wrapped.length, "v"); free(wrapped.data);
        if (!value) return false;
        uint64_t payload = out->length, size = strlen(value); bytes(out, value, (size_t)size); free(value);
        *offset = container_value_node(out, 5, payload, size);
    } else if (*start == 't' || *start == 'f') *offset = container_value_node(out, 2, *start == 't', 0);
    else if (*start == 'n') *offset = container_value_node(out, 0, 0, 0);
    else {
        char number[32]; size_t size = (size_t)(p - start);
        if (size >= sizeof(number)) return false;
        memcpy(number, start, size); number[size] = 0;
        char *tail = NULL; long long integer = strtoll(number, &tail, 10);
        if (*tail) {
            double number_value = strtod(number, &tail); uint64_t bits;
            if (*tail) return false;
            memcpy(&bits, &number_value, sizeof(bits));
            *offset = container_value_node(out, 4, bits, 0);
        } else *offset = container_value_node(out, 3, (uint64_t)integer, 0);
    }
    *cursor = p; return true;
}
static bool container_value_encode(const char *json, StringBuilder *out) {
    unsigned char header[16] = {'O','R','W','V',1}; bytes(out, header, 16);
    const unsigned char *p = (const unsigned char *)json, *end = p + strlen(json); uint64_t offset;
    if (!container_value_encode_node(out, &p, end, 0, &offset)) return false;
    layout_json_space(&p, end);
    if (p != end) return false;
    container_write64((unsigned char *)out->data + 8, offset); return true;
}
static bool container_value_decode_node(const unsigned char *data, size_t size, uint64_t offset,
                                        unsigned depth, StringBuilder *json) {
    if (depth > 64 || size < 24 || offset > size - 24 || json->length > WSL_LIMIT) return false;
    unsigned type = data[offset];
    uint64_t payload = payload_read64(data + offset + 8), count = payload_read64(data + offset + 16);
    if (type == 0) append(json, "null");
    else if (type == 2) append(json, payload ? "true" : "false");
    else if (type == 3) { char number[32]; snprintf(number, sizeof(number), "%lld", (long long)payload); append(json, number); }
    else if (type == 4 || type == 9) {
        double value; memcpy(&value, &payload, sizeof(value));
        if (!isfinite(value)) return false;
        char number[64]; snprintf(number, sizeof(number), "%.17g", value); append(json, number);
    }
    else if (type == 5) {
        if (payload > size || count > size - payload || memchr(data + payload, 0, (size_t)count)) return false;
        StringBuilder value = {0}; bytes(&value, data + payload, (size_t)count); if (!value.data) append(&value, "");
        json_string(json, value.data); free(value.data);
    } else if (type == 7 || type == 8) {
        size_t stride = type == 8 ? 16 : 8;
        if (payload > size || count > (size - payload) / stride) return false;
        append(json, type == 8 ? "{" : "[");
        for (uint64_t i = 0; i < count; i++) {
            if (i) append(json, ",");
            uint64_t ref = payload_read64(data + payload + i * stride);
            if (type == 8 && (ref >= size || data[ref] != 5)) return false;
            if (!container_value_decode_node(data, size, ref, depth + 1, json)) return false;
            if (type == 8) {
                append(json, ":");
                if (!container_value_decode_node(data, size, payload_read64(data + payload + i * stride + 8), depth + 1, json)) return false;
            }
        }
        append(json, type == 8 ? "}" : "]");
    } else return false;
    return true;
}
static bool container_value_decode(const StringBuilder *data, StringBuilder *json) {
    return data->length >= 16 && !memcmp(data->data, "ORWV\1\0\0\0", 8) &&
           container_value_decode_node((const unsigned char *)data->data, data->length,
                                       payload_read64((const unsigned char *)data->data + 8), 0, json);
}
