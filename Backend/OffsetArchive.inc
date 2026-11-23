#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    ORWV_BOOLEAN = 2,
    ORWV_INTEGER = 3,
    ORWV_DOUBLE = 4,
    ORWV_STRING = 5,
    ORWV_DATA = 6,
    ORWV_ARRAY = 7,
    ORWV_DICTIONARY = 8,
    ORWV_DATE = 9,
    ORWV_HEADER_SIZE = 16,
    ORWV_NODE_SIZE = 24,
    ORWV_MAX_DEPTH = 128
};

typedef struct OrwvValue OrwvValue;

typedef struct {
    char *key;
    size_t key_length;
    OrwvValue *value;
} OrwvEntry;

struct OrwvValue {
    unsigned type;
    bool failed;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            unsigned char *bytes;
            size_t length;
        } data;
        struct {
            OrwvValue **items;
            OrwvEntry *entries;
            size_t count;
            size_t capacity;
        } collection;
    } as;
};

typedef struct {
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} OrwvBuffer;

static inline bool orwv_utf8_valid(const unsigned char *bytes, size_t length) {
    size_t index = 0;
    while (index < length) {
        unsigned char lead = bytes[index];
        size_t extra;
        uint32_t scalar;
        if (lead < 0x80) {
            index++;
            continue;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1; scalar = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2; scalar = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3; scalar = lead & 0x07;
        } else {
            return false;
        }
        if (extra > length - index - 1) return false;
        for (size_t offset = 1; offset <= extra; offset++) {
            unsigned char next = bytes[index + offset];
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((extra == 2 && scalar < 0x800) || (extra == 3 && scalar < 0x10000) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
            return false;
        }
        index += extra + 1;
    }
    return true;
}

static inline OrwvValue *orwv_new(unsigned type) {
    OrwvValue *value = (OrwvValue *)calloc(1, sizeof(*value));
    if (value) value->type = type;
    return value;
}

static inline void orwv_free(OrwvValue *value) {
    if (!value) return;
    if (value->type == ORWV_STRING || value->type == ORWV_DATA) {
        free(value->as.data.bytes);
    } else if (value->type == ORWV_ARRAY) {
        for (size_t index = 0; index < value->as.collection.count; index++) {
            orwv_free(value->as.collection.items[index]);
        }
        free(value->as.collection.items);
    } else if (value->type == ORWV_DICTIONARY) {
        for (size_t index = 0; index < value->as.collection.count; index++) {
            free(value->as.collection.entries[index].key);
            orwv_free(value->as.collection.entries[index].value);
        }
        free(value->as.collection.entries);
    }
    free(value);
}

static inline OrwvValue *orwv_bytes_value(unsigned type, const void *bytes, size_t length) {
    if (length && !bytes) return NULL;
    if (type == ORWV_STRING && !orwv_utf8_valid((const unsigned char *)bytes, length)) return NULL;
    OrwvValue *value = orwv_new(type);
    if (!value) return NULL;
    value->as.data.bytes = (unsigned char *)malloc(length ? length : 1);
    if (!value->as.data.bytes) {
        free(value);
        return NULL;
    }
    if (length) memcpy(value->as.data.bytes, bytes, length);
    value->as.data.length = length;
    return value;
}

static inline OrwvValue *orwv_string_n(const char *text, size_t length) {
    return orwv_bytes_value(ORWV_STRING, text, length);
}

static inline OrwvValue *orwv_string(const char *text) {
    return text ? orwv_string_n(text, strlen(text)) : NULL;
}

static inline OrwvValue *orwv_data(const void *bytes, size_t length) {
    return orwv_bytes_value(ORWV_DATA, bytes, length);
}

static inline OrwvValue *orwv_bool(bool flag) {
    OrwvValue *value = orwv_new(ORWV_BOOLEAN);
    if (value) value->as.boolean = flag;
    return value;
}

static inline OrwvValue *orwv_int(int64_t number) {
    OrwvValue *value = orwv_new(ORWV_INTEGER);
    if (value) value->as.integer = number;
    return value;
}

static inline OrwvValue *orwv_double(double number) {
    OrwvValue *value = orwv_new(ORWV_DOUBLE);
    if (value) value->as.number = number;
    return value;
}

static inline OrwvValue *orwv_date(double seconds_since_1970) {
    OrwvValue *value = orwv_new(ORWV_DATE);
    if (value) value->as.number = seconds_since_1970;
    return value;
}

static inline OrwvValue *orwv_array(void) {
    return orwv_new(ORWV_ARRAY);
}

static inline OrwvValue *orwv_dictionary(void) {
    return orwv_new(ORWV_DICTIONARY);
}

static inline bool orwv_reserve(OrwvValue *collection, size_t element_size) {
    if (collection->as.collection.count < collection->as.collection.capacity) return true;
    size_t capacity = collection->as.collection.capacity ? collection->as.collection.capacity * 2 : 4;
    if (capacity > SIZE_MAX / element_size) return false;
    void **storage = collection->type == ORWV_ARRAY
        ? (void **)&collection->as.collection.items
        : (void **)&collection->as.collection.entries;
    void *grown = realloc(*storage, capacity * element_size);
    if (!grown) return false;
    *storage = grown;
    collection->as.collection.capacity = capacity;
    return true;
}

static inline bool orwv_array_append(OrwvValue *array, OrwvValue *item) {
    if (!array || array->type != ORWV_ARRAY) {
        orwv_free(item);
        return false;
    }
    if (!item || !orwv_reserve(array, sizeof(OrwvValue *))) {
        orwv_free(item);
        array->failed = true;
        return false;
    }
    array->as.collection.items[array->as.collection.count++] = item;
    return true;
}

static inline OrwvEntry *orwv_dictionary_entry(const OrwvValue *dictionary, const char *key, size_t key_length) {
    if (!dictionary || dictionary->type != ORWV_DICTIONARY) return NULL;
    for (size_t index = 0; index < dictionary->as.collection.count; index++) {
        OrwvEntry *entry = &dictionary->as.collection.entries[index];
        if (entry->key_length == key_length && !memcmp(entry->key, key, key_length)) return entry;
    }
    return NULL;
}

static inline bool orwv_dictionary_set_n(OrwvValue *dictionary, const char *key, size_t key_length, OrwvValue *value) {
    if (!dictionary || dictionary->type != ORWV_DICTIONARY) {
        orwv_free(value);
        return false;
    }
    if (!value || !key || !orwv_utf8_valid((const unsigned char *)key, key_length)) {
        orwv_free(value);
        dictionary->failed = true;
        return false;
    }
    OrwvEntry *existing = orwv_dictionary_entry(dictionary, key, key_length);
    if (existing) {
        orwv_free(existing->value);
        existing->value = value;
        return true;
    }
    char *copy = (char *)malloc(key_length ? key_length : 1);
    if (!copy || !orwv_reserve(dictionary, sizeof(OrwvEntry))) {
        free(copy);
        orwv_free(value);
        dictionary->failed = true;
        return false;
    }
    if (key_length) memcpy(copy, key, key_length);
    OrwvEntry *entry = &dictionary->as.collection.entries[dictionary->as.collection.count++];
    entry->key = copy;
    entry->key_length = key_length;
    entry->value = value;
    return true;
}

static inline bool orwv_dictionary_set(OrwvValue *dictionary, const char *key, OrwvValue *value) {
    return orwv_dictionary_set_n(dictionary, key, key ? strlen(key) : 0, value);
}

static inline bool orwv_dictionary_remove(OrwvValue *dictionary, const char *key) {
    OrwvEntry *entry = orwv_dictionary_entry(dictionary, key, strlen(key));
    if (!entry) return false;
    size_t index = (size_t)(entry - dictionary->as.collection.entries);
    free(entry->key);
    orwv_free(entry->value);
    memmove(entry, entry + 1, (dictionary->as.collection.count - index - 1) * sizeof(OrwvEntry));
    dictionary->as.collection.count--;
    return true;
}

static inline const OrwvValue *orwv_dictionary_get(const OrwvValue *dictionary, const char *key) {
    OrwvEntry *entry = key ? orwv_dictionary_entry(dictionary, key, strlen(key)) : NULL;
    return entry ? entry->value : NULL;
}

static inline size_t orwv_count(const OrwvValue *value) {
    return value && (value->type == ORWV_ARRAY || value->type == ORWV_DICTIONARY)
        ? value->as.collection.count
        : 0;
}

static inline const OrwvValue *orwv_array_at(const OrwvValue *array, size_t index) {
    return array && array->type == ORWV_ARRAY && index < array->as.collection.count
        ? array->as.collection.items[index]
        : NULL;
}

static inline bool orwv_get_bool(const OrwvValue *value, bool *result) {
    if (!value || value->type != ORWV_BOOLEAN) return false;
    *result = value->as.boolean;
    return true;
}

static inline bool orwv_get_int(const OrwvValue *value, int64_t *result) {
    if (!value || value->type != ORWV_INTEGER) return false;
    *result = value->as.integer;
    return true;
}

static inline bool orwv_get_double(const OrwvValue *value, double *result) {
    if (!value) return false;
    if (value->type == ORWV_DOUBLE || value->type == ORWV_DATE) {
        *result = value->as.number;
        return true;
    }
    if (value->type == ORWV_INTEGER) {
        *result = (double)value->as.integer;
        return true;
    }
    return false;
}

static inline bool orwv_get_bytes(const OrwvValue *value, unsigned type, const unsigned char **bytes, size_t *length) {
    if (!value || value->type != type) return false;
    *bytes = value->as.data.bytes;
    *length = value->as.data.length;
    return true;
}

static inline char *orwv_copy_string(const OrwvValue *value) {
    const unsigned char *bytes;
    size_t length;
    if (!orwv_get_bytes(value, ORWV_STRING, &bytes, &length) || memchr(bytes, 0, length)) return NULL;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, bytes, length);
    copy[length] = 0;
    return copy;
}

static inline void orwv_buffer_append(OrwvBuffer *buffer, const void *bytes, size_t length) {
    if (buffer->failed) return;
    if (length > SIZE_MAX - buffer->length) {
        buffer->failed = true;
        return;
    }
    if (buffer->length + length > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < buffer->length + length) {
            if (capacity > SIZE_MAX / 2) {
                buffer->failed = true;
                return;
            }
            capacity *= 2;
        }
        unsigned char *grown = (unsigned char *)realloc(buffer->bytes, capacity);
        if (!grown) {
            buffer->failed = true;
            return;
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    if (length) memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
}

static inline void orwv_write_u64(unsigned char *bytes, uint64_t value) {
    for (unsigned index = 0; index < 8; index++) bytes[index] = (unsigned char)(value >> (index * 8));
}

static inline uint64_t orwv_read_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; index++) value |= (uint64_t)bytes[index] << (index * 8);
    return value;
}

static inline void orwv_buffer_append_u64(OrwvBuffer *buffer, uint64_t value) {
    unsigned char bytes[8];
    orwv_write_u64(bytes, value);
    orwv_buffer_append(buffer, bytes, sizeof(bytes));
}

static inline uint64_t orwv_buffer_append_node(OrwvBuffer *buffer, unsigned type, uint64_t payload, uint64_t count) {
    uint64_t offset = buffer->length;
    unsigned char node[ORWV_NODE_SIZE] = {0};
    node[0] = (unsigned char)type;
    orwv_write_u64(node + 8, payload);
    orwv_write_u64(node + 16, count);
    orwv_buffer_append(buffer, node, sizeof(node));
    return offset;
}

static inline uint64_t orwv_double_bits(double number) {
    uint64_t bits;
    memcpy(&bits, &number, sizeof(bits));
    return bits;
}

static inline int orwv_entry_compare(const void *left, const void *right) {
    const OrwvEntry *a = *(const OrwvEntry *const *)left;
    const OrwvEntry *b = *(const OrwvEntry *const *)right;
    size_t shared = a->key_length < b->key_length ? a->key_length : b->key_length;
    int order = shared ? memcmp(a->key, b->key, shared) : 0;
    if (order) return order;
    return a->key_length < b->key_length ? -1 : a->key_length > b->key_length;
}

static inline uint64_t orwv_encode_value(OrwvBuffer *buffer, const OrwvValue *value, unsigned depth) {
    if (!value || value->failed || depth >= ORWV_MAX_DEPTH) {
        buffer->failed = true;
        return 0;
    }
    switch (value->type) {
    case ORWV_BOOLEAN:
        return orwv_buffer_append_node(buffer, ORWV_BOOLEAN, value->as.boolean ? 1 : 0, 0);
    case ORWV_INTEGER:
        return orwv_buffer_append_node(buffer, ORWV_INTEGER, (uint64_t)value->as.integer, 0);
    case ORWV_DOUBLE:
    case ORWV_DATE:
        return orwv_buffer_append_node(buffer, value->type, orwv_double_bits(value->as.number), 0);
    case ORWV_STRING:
    case ORWV_DATA: {
        uint64_t payload = buffer->length;
        orwv_buffer_append(buffer, value->as.data.bytes, value->as.data.length);
        return orwv_buffer_append_node(buffer, value->type, payload, value->as.data.length);
    }
    case ORWV_ARRAY: {
        size_t count = value->as.collection.count;
        uint64_t *offsets = (uint64_t *)calloc(count ? count : 1, sizeof(uint64_t));
        if (!offsets) {
            buffer->failed = true;
            return 0;
        }
        for (size_t index = 0; index < count; index++) {
            offsets[index] = orwv_encode_value(buffer, value->as.collection.items[index], depth + 1);
        }
        uint64_t payload = buffer->length;
        for (size_t index = 0; index < count; index++) orwv_buffer_append_u64(buffer, offsets[index]);
        free(offsets);
        return orwv_buffer_append_node(buffer, ORWV_ARRAY, payload, count);
    }
    case ORWV_DICTIONARY: {
        size_t count = value->as.collection.count;
        const OrwvEntry **sorted = (const OrwvEntry **)calloc(count ? count : 1, sizeof(*sorted));
        uint64_t *offsets = (uint64_t *)calloc(count ? count * 2 : 1, sizeof(uint64_t));
        if (!sorted || !offsets) {
            free(sorted);
            free(offsets);
            buffer->failed = true;
            return 0;
        }
        for (size_t index = 0; index < count; index++) sorted[index] = &value->as.collection.entries[index];
        qsort(sorted, count, sizeof(*sorted), orwv_entry_compare);
        for (size_t index = 0; index < count; index++) {
            uint64_t key_payload = buffer->length;
            orwv_buffer_append(buffer, sorted[index]->key, sorted[index]->key_length);
            offsets[index * 2] = orwv_buffer_append_node(buffer, ORWV_STRING, key_payload, sorted[index]->key_length);
            offsets[index * 2 + 1] = orwv_encode_value(buffer, sorted[index]->value, depth + 1);
        }
        uint64_t payload = buffer->length;
        for (size_t index = 0; index < count * 2; index++) orwv_buffer_append_u64(buffer, offsets[index]);
        free(sorted);
        free(offsets);
        return orwv_buffer_append_node(buffer, ORWV_DICTIONARY, payload, count);
    }
    default:
        buffer->failed = true;
        return 0;
    }
}

static inline bool orwv_encode(const OrwvValue *value, unsigned char **bytes, size_t *length) {
    OrwvBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    unsigned char header[ORWV_HEADER_SIZE] = {'O', 'R', 'W', 'V', 1, 0, 0, 0};
    orwv_buffer_append(&buffer, header, sizeof(header));
    uint64_t root = orwv_encode_value(&buffer, value, 0);
    if (buffer.failed) {
        free(buffer.bytes);
        return false;
    }
    orwv_write_u64(buffer.bytes + 8, root);
    *bytes = buffer.bytes;
    *length = buffer.length;
    return true;
}

static inline bool orwv_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

static inline OrwvValue *orwv_decode_value(const unsigned char *bytes, size_t size, uint64_t offset, unsigned depth) {
    if (depth >= ORWV_MAX_DEPTH || offset < ORWV_HEADER_SIZE || !orwv_range(size, offset, ORWV_NODE_SIZE)) return NULL;
    unsigned type = bytes[offset];
    uint64_t payload = orwv_read_u64(bytes + offset + 8);
    uint64_t count = orwv_read_u64(bytes + offset + 16);
    switch (type) {
    case ORWV_BOOLEAN:
        return payload > 1 ? NULL : orwv_bool(payload != 0);
    case ORWV_INTEGER:
        return orwv_int((int64_t)payload);
    case ORWV_DOUBLE:
    case ORWV_DATE: {
        double number;
        memcpy(&number, &payload, sizeof(number));
        return type == ORWV_DATE ? orwv_date(number) : orwv_double(number);
    }
    case ORWV_STRING:
    case ORWV_DATA:
        if (!orwv_range(size, payload, count)) return NULL;
        return orwv_bytes_value(type, bytes + payload, (size_t)count);
    case ORWV_ARRAY: {
        if (count > (size - (payload <= size ? payload : size)) / 8 || !orwv_range(size, payload, count * 8)) return NULL;
        OrwvValue *array = orwv_array();
        for (uint64_t index = 0; array && index < count; index++) {
            OrwvValue *item = orwv_decode_value(bytes, size, orwv_read_u64(bytes + payload + index * 8), depth + 1);
            if (!item || !orwv_array_append(array, item)) {
                orwv_free(array);
                return NULL;
            }
        }
        return array;
    }
    case ORWV_DICTIONARY: {
        if (count > (size - (payload <= size ? payload : size)) / 16 || !orwv_range(size, payload, count * 16)) return NULL;
        OrwvValue *dictionary = orwv_dictionary();
        for (uint64_t index = 0; dictionary && index < count; index++) {
            const unsigned char *entry = bytes + payload + index * 16;
            OrwvValue *key = orwv_decode_value(bytes, size, orwv_read_u64(entry), depth + 1);
            OrwvValue *item = key && key->type == ORWV_STRING
                ? orwv_decode_value(bytes, size, orwv_read_u64(entry + 8), depth + 1)
                : NULL;
            bool added = item &&
                !orwv_dictionary_entry(dictionary, (const char *)key->as.data.bytes, key->as.data.length) &&
                orwv_dictionary_set_n(dictionary, (const char *)key->as.data.bytes, key->as.data.length, item);
            if (!added && item && !dictionary->failed) orwv_free(item);
            orwv_free(key);
            if (!added) {
                orwv_free(dictionary);
                return NULL;
            }
        }
        return dictionary;
    }
    default:
        return NULL;
    }
}

static inline OrwvValue *orwv_decode(const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    if (!bytes || size < ORWV_HEADER_SIZE || memcmp(bytes, "ORWV\1\0\0\0", 8)) return NULL;
    return orwv_decode_value(bytes, size, orwv_read_u64(bytes + 8), 0);
}
