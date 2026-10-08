#ifndef OUTER_SHELL_LAYOUT_H
#define OUTER_SHELL_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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

#endif
