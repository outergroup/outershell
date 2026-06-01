#include "OuterShellBuffer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

void write_uint32_le(unsigned char *dst, uint32_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
    dst[2] = (unsigned char)((value >> 16) & 0xffu);
    dst[3] = (unsigned char)((value >> 24) & 0xffu);
}

void write_uint16_le(unsigned char *dst, uint16_t value) {
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

void write_uint64_le(unsigned char *dst, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

uint32_t read_uint32_le(const unsigned char *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

uint16_t read_uint16_le(const unsigned char *src) {
    return (uint16_t)(((uint16_t)src[0]) | ((uint16_t)src[1] << 8));
}

bool sb_reserve(StringBuilder *builder, size_t additional) {
    if (builder->length + additional + 1 <= builder->capacity) return true;
    size_t new_capacity = builder->capacity ? builder->capacity * 2 : 4096;
    while (new_capacity < builder->length + additional + 1) {
        new_capacity *= 2;
    }
    char *new_data = realloc(builder->data, new_capacity);
    if (!new_data) return false;
    builder->data = new_data;
    builder->capacity = new_capacity;
    return true;
}

bool sb_append_n(StringBuilder *builder, const char *text, size_t length) {
    if (!sb_reserve(builder, length)) return false;
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

bool sb_append(StringBuilder *builder, const char *text) {
    return sb_append_n(builder, text, strlen(text));
}

bool binary_reserve(StringBuilder *builder, size_t additional) {
    return sb_reserve(builder, additional);
}

bool binary_append_zero(StringBuilder *builder, size_t length) {
    if (!binary_reserve(builder, length)) return false;
    memset(builder->data + builder->length, 0, length);
    builder->length += length;
    return true;
}

bool binary_write_u32_at(StringBuilder *builder, size_t offset, uint32_t value) {
    if (!builder || offset + 4 > builder->length) return false;
    write_uint32_le((unsigned char *)builder->data + offset, value);
    return true;
}

bool binary_write_ref32_at(StringBuilder *builder, size_t ref_offset, size_t data_offset, size_t data_length) {
    if (data_offset > UINT32_MAX || data_length > UINT32_MAX) return false;
    return binary_write_u32_at(builder, ref_offset, (uint32_t)data_offset) &&
           binary_write_u32_at(builder, ref_offset + 4, (uint32_t)data_length);
}

bool binary_append_data_ref_at(StringBuilder *builder, size_t ref_offset, const void *data, size_t data_length) {
    size_t data_offset = builder->length;
    if (data_length > 0 && !sb_append_n(builder, (const char *)data, data_length)) return false;
    return binary_write_ref32_at(builder, ref_offset, data_offset, data_length);
}

bool binary_append_string_ref_at(StringBuilder *builder, size_t ref_offset, const char *text) {
    const char *safe_text = text ? text : "";
    return binary_append_data_ref_at(builder, ref_offset, safe_text, strlen(safe_text));
}

bool binary_write_u16_at(StringBuilder *builder, size_t offset, uint16_t value) {
    if (!builder || offset + 2 > builder->length) return false;
    write_uint16_le((unsigned char *)builder->data + offset, value);
    return true;
}

