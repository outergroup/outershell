#ifndef OUTER_SHELL_BUFFER_H
#define OUTER_SHELL_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

void write_uint32_le(unsigned char *dst, uint32_t value);
void write_uint16_le(unsigned char *dst, uint16_t value);
void write_uint64_le(unsigned char *dst, uint64_t value);
uint32_t read_uint32_le(const unsigned char *src);
uint16_t read_uint16_le(const unsigned char *src);

bool sb_reserve(StringBuilder *builder, size_t additional);
bool sb_append_n(StringBuilder *builder, const char *text, size_t length);
bool sb_append(StringBuilder *builder, const char *text);

bool binary_reserve(StringBuilder *builder, size_t additional);
bool binary_append_zero(StringBuilder *builder, size_t length);
bool binary_write_u32_at(StringBuilder *builder, size_t offset, uint32_t value);
bool binary_write_ref32_at(StringBuilder *builder, size_t ref_offset, size_t data_offset, size_t data_length);
bool binary_append_data_ref_at(StringBuilder *builder, size_t ref_offset, const void *data, size_t data_length);
bool binary_append_string_ref_at(StringBuilder *builder, size_t ref_offset, const char *text);
bool binary_write_u16_at(StringBuilder *builder, size_t offset, uint16_t value);

#endif
