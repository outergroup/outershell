#ifndef OUTER_SHELL_LAYOUT_H
#define OUTER_SHELL_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#define LAYOUT_MAX_BYTES (256u * 1024u)

static bool layout_json_value(const unsigned char **cursor, const unsigned char *end, unsigned depth);

static void layout_json_space(const unsigned char **cursor, const unsigned char *end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\n' || **cursor == '\r' || **cursor == '\t'))
        ++*cursor;
}

static bool layout_json_string(const unsigned char **cursor, const unsigned char *end) {
    if (*cursor == end || *(*cursor)++ != '"')
        return false;
    while (*cursor < end) {
        unsigned char c = *(*cursor)++;
        if (c == '"')
            return true;
        if (c < 32)
            return false;
        if (c == '\\') {
            if (*cursor == end)
                return false;
            c = *(*cursor)++;
            if (c == 'u') {
                for (unsigned i = 0; i < 4; i++) {
                    if (*cursor == end || !isxdigit(*(*cursor)++))
                        return false;
                }
            } else if (c == 0 || !strchr("\"\\/bfnrt", c))
                return false;
        }
    }
    return false;
}

static bool layout_json_value(const unsigned char **cursor, const unsigned char *end, unsigned depth) {
    layout_json_space(cursor, end);
    if (*cursor == end || depth > 32)
        return false;
    unsigned char c = **cursor;
    if (c == '"')
        return layout_json_string(cursor, end);
    if (c == '{' || c == '[') {
        ++*cursor;
        unsigned char close = c == '{' ? '}' : ']';
        layout_json_space(cursor, end);
        if (*cursor < end && **cursor == close) {
            ++*cursor;
            return true;
        }
        while (*cursor < end) {
            if (c == '{') {
                if (!layout_json_string(cursor, end))
                    return false;
                layout_json_space(cursor, end);
                if (*cursor == end || *(*cursor)++ != ':')
                    return false;
            }
            if (!layout_json_value(cursor, end, depth + 1))
                return false;
            layout_json_space(cursor, end);
            if (*cursor == end)
                return false;
            unsigned char separator = *(*cursor)++;
            if (separator == close)
                return true;
            if (separator != ',')
                return false;
            layout_json_space(cursor, end);
        }
        return false;
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        size_t length = strlen(literal);
        if ((size_t)(end - *cursor) < length || memcmp(*cursor, literal, length))
            return false;
        *cursor += length;
        return true;
    }
    const unsigned char *p = *cursor;
    if (*p == '-') {
        if (++p == end)
            return false;
    }
    if (*p == '0')
        ++p;
    else {
        if (*p < '1' || *p > '9')
            return false;
        while (p < end && isdigit(*p))
            ++p;
    }
    if (p < end && *p == '.') {
        if (++p == end || !isdigit(*p))
            return false;
        while (p < end && isdigit(*p))
            ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        if (++p < end && (*p == '+' || *p == '-'))
            ++p;
        if (p == end || !isdigit(*p))
            return false;
        while (p < end && isdigit(*p))
            ++p;
    }
    *cursor = p;
    return true;
}

static bool layout_utf8_valid(const unsigned char *p, const unsigned char *end) {
    while (p < end) {
        uint32_t value = *p++;
        if (value < 0x80)
            continue;
        unsigned continuation;
        uint32_t minimum;
        if (value >= 0xc2 && value <= 0xdf) {
            continuation = 1;
            minimum = 0x80;
            value &= 0x1f;
        } else if (value >= 0xe0 && value <= 0xef) {
            continuation = 2;
            minimum = 0x800;
            value &= 0x0f;
        } else if (value >= 0xf0 && value <= 0xf4) {
            continuation = 3;
            minimum = 0x10000;
            value &= 0x07;
        } else
            return false;
        for (unsigned i = 0; i < continuation; i++) {
            if (p == end || (*p & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (*p++ & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

static bool layout_document_valid(const char *data, size_t length) {
    if (length < 18 || length > LAYOUT_MAX_BYTES || memcmp(data, "OSLAY001", 8))
        return false;
    const unsigned char *cursor = (const unsigned char *)data + 16;
    const unsigned char *end = (const unsigned char *)data + length;
    if (!layout_utf8_valid(cursor, end))
        return false;
    layout_json_space(&cursor, end);
    if (cursor == end || *cursor != '{' || !layout_json_value(&cursor, end, 0))
        return false;
    layout_json_space(&cursor, end);
    return cursor == end;
}

typedef struct {
    char **keys;
    char **values;
    size_t count;
} LayoutNames;

static inline unsigned layout_json_hex4(const unsigned char *p) {
    unsigned value = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned char c = p[i];
        value = value * 16 + (unsigned)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return value;
}

static inline bool layout_json_string_decode(const unsigned char **cursor, const unsigned char *end, char **out) {
    const unsigned char *start = *cursor;
    if (!layout_json_string(cursor, end))
        return false;
    const unsigned char *p = start + 1, *stop = *cursor - 1;
    char *text = (char *)malloc((size_t)(stop - p) + 1);
    if (!text)
        return false;
    size_t length = 0;
    while (p < stop) {
        unsigned char c = *p++;
        if (c != '\\') {
            text[length++] = (char)c;
            continue;
        }
        c = *p++;
        if (c != 'u') {
            const char *escapes = "\"\\/bfnrt", *replacements = "\"\\/\b\f\n\r\t";
            text[length++] = replacements[strchr(escapes, c) - escapes];
            continue;
        }
        uint32_t value = layout_json_hex4(p);
        p += 4;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (stop - p < 6 || p[0] != '\\' || p[1] != 'u' || !isxdigit(p[2]) || !isxdigit(p[3]) ||
                !isxdigit(p[4]) || !isxdigit(p[5])) {
                free(text);
                return false;
            }
            uint32_t low = layout_json_hex4(p + 2);
            if (low < 0xdc00 || low > 0xdfff) {
                free(text);
                return false;
            }
            p += 6;
            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
        } else if (value == 0 || (value >= 0xdc00 && value <= 0xdfff)) {
            free(text);
            return false;
        }
        if (value < 0x80) {
            text[length++] = (char)value;
        } else if (value < 0x800) {
            text[length++] = (char)(0xc0 | (value >> 6));
            text[length++] = (char)(0x80 | (value & 0x3f));
        } else if (value < 0x10000) {
            text[length++] = (char)(0xe0 | (value >> 12));
            text[length++] = (char)(0x80 | ((value >> 6) & 0x3f));
            text[length++] = (char)(0x80 | (value & 0x3f));
        } else {
            text[length++] = (char)(0xf0 | (value >> 18));
            text[length++] = (char)(0x80 | ((value >> 12) & 0x3f));
            text[length++] = (char)(0x80 | ((value >> 6) & 0x3f));
            text[length++] = (char)(0x80 | (value & 0x3f));
        }
    }
    text[length] = '\0';
    *out = text;
    return true;
}

static inline void layout_names_free(LayoutNames *names) {
    for (size_t i = 0; i < names->count; i++) {
        free(names->keys[i]);
        free(names->values[i]);
    }
    free(names->keys);
    free(names->values);
    memset(names, 0, sizeof(*names));
}

static inline bool layout_names_add(LayoutNames *names, char *key, char *value) {
    char **keys = (char **)realloc(names->keys, (names->count + 1) * sizeof(*keys));
    if (!keys)
        return false;
    names->keys = keys;
    char **values = (char **)realloc(names->values, (names->count + 1) * sizeof(*values));
    if (!values)
        return false;
    names->values = values;
    names->keys[names->count] = key;
    names->values[names->count] = value;
    names->count++;
    return true;
}

static inline bool layout_names_parse_object(const unsigned char **cursor, const unsigned char *end, LayoutNames *names) {
    ++*cursor;
    layout_json_space(cursor, end);
    if (*cursor < end && **cursor == '}') {
        ++*cursor;
        return true;
    }
    while (*cursor < end) {
        char *key = NULL, *value = NULL;
        if (!layout_json_string_decode(cursor, end, &key))
            return false;
        layout_json_space(cursor, end);
        if (*cursor == end || *(*cursor)++ != ':') {
            free(key);
            return false;
        }
        layout_json_space(cursor, end);
        if (*cursor < end && **cursor == '"') {
            if (!layout_json_string_decode(cursor, end, &value)) {
                free(key);
                return false;
            }
            if (!value[0]) {
                free(key);
                free(value);
            } else if (!layout_names_add(names, key, value)) {
                free(key);
                free(value);
                return false;
            }
        } else {
            free(key);
            if (!layout_json_value(cursor, end, 1))
                return false;
        }
        layout_json_space(cursor, end);
        if (*cursor == end)
            return false;
        unsigned char separator = *(*cursor)++;
        if (separator == '}')
            return true;
        if (separator != ',')
            return false;
        layout_json_space(cursor, end);
    }
    return false;
}

static inline bool layout_names_parse(const char *data, size_t length, LayoutNames *names) {
    memset(names, 0, sizeof(*names));
    if (!layout_document_valid(data, length))
        return false;
    const unsigned char *cursor = (const unsigned char *)data + 16;
    const unsigned char *end = (const unsigned char *)data + length;
    layout_json_space(&cursor, end);
    ++cursor;
    layout_json_space(&cursor, end);
    if (cursor < end && *cursor == '}')
        return true;
    while (cursor < end) {
        char *key = NULL;
        if (!layout_json_string_decode(&cursor, end, &key))
            break;
        layout_json_space(&cursor, end);
        ++cursor;
        layout_json_space(&cursor, end);
        bool is_names = !strcmp(key, "names") && cursor < end && *cursor == '{';
        free(key);
        if (is_names) {
            layout_names_free(names);
            if (!layout_names_parse_object(&cursor, end, names))
                break;
        } else if (!layout_json_value(&cursor, end, 1)) {
            break;
        }
        layout_json_space(&cursor, end);
        if (cursor == end)
            break;
        unsigned char separator = *cursor++;
        if (separator == '}')
            return true;
        layout_json_space(&cursor, end);
    }
    layout_names_free(names);
    return false;
}

static inline const char *layout_names_find(const LayoutNames *names, const char *key) {
    for (size_t i = 0; i < names->count; i++) {
        if (!strcmp(names->keys[i], key))
            return names->values[i];
    }
    return NULL;
}

#endif
