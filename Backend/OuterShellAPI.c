#define _GNU_SOURCE

#include "OuterShellAPI.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>

static bool api_write_all(int fd, const void *data, size_t len) {
    const char *bytes = (const char *)data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

bool api_read_string_ref(const unsigned char *message,
                         size_t message_length,
                         size_t ref_offset,
                         char **out) {
    *out = NULL;
    if (ref_offset + 8 > message_length) return false;
    uint32_t offset = read_uint32_le(message + ref_offset);
    uint32_t length = read_uint32_le(message + ref_offset + 4);
    if (offset > message_length || length > message_length - offset) return false;
    char *value = malloc((size_t)length + 1);
    if (!value) return false;
    if (length) memcpy(value, message + offset, length);
    value[length] = '\0';
    *out = value;
    return true;
}

bool api_read_data_ref(const unsigned char *message,
                       size_t message_length,
                       size_t ref_offset,
                       const unsigned char **out,
                       size_t *out_length) {
    if (out) *out = NULL;
    if (out_length) *out_length = 0;
    if (ref_offset + 8 > message_length) return false;
    uint32_t offset = read_uint32_le(message + ref_offset);
    uint32_t length = read_uint32_le(message + ref_offset + 4);
    if (offset > message_length || length > message_length - offset) return false;
    if (out) *out = message + offset;
    if (out_length) *out_length = length;
    return true;
}

bool api_send_frame(int fd, StringBuilder *message) {
    if (!message || message->length > UINT32_MAX) return false;
    unsigned char prefix[4];
    write_uint32_le(prefix, (uint32_t)message->length);
    return api_write_all(fd, prefix, sizeof(prefix)) &&
           api_write_all(fd, message->data, message->length);
}

