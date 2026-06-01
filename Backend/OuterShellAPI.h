#ifndef OUTER_SHELL_API_H
#define OUTER_SHELL_API_H

#include "OuterShellBuffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OUTERSHELL_API_MAX_FRAME_SIZE (16u * 1024u * 1024u)

enum {
    OUTERSHELLD_API_OUTERCTL_INVOKE = 1,
    OUTERSHELLD_API_OUTERCTL_INVOKE_RESPONSE = 2,
    OUTERSHELLD_API_FILE_OPENERS_QUERY = 3,
    OUTERSHELLD_API_FILE_OPENERS_RESPONSE = 4,
    OUTERSHELLD_API_UI_REQUEST = 5,
    OUTERSHELLD_API_UI_RESPONSE = 6,
    OUTERSHELLD_API_FILE_OPENERS_ROW_SIZE = 40
};

enum {
    OUTERSHELLD_UI_ROUTE_NONE = 0,
    OUTERSHELLD_UI_ROUTE_BACKENDS = 1,
    OUTERSHELLD_UI_ROUTE_LOGS = 2,
    OUTERSHELLD_UI_ROUTE_CONTROL = 3,
    OUTERSHELLD_UI_ROUTE_CREATE = 4,
    OUTERSHELLD_UI_ROUTE_RECIPES = 5,
    OUTERSHELLD_UI_ROUTE_FILE_PICKER = 6,
    OUTERSHELLD_UI_ROUTE_EVENTS = 7
};

typedef enum {
    UI_API_CONTENT_BINARY = 1,
    UI_API_CONTENT_TEXT = 2
} UiApiContentKind;

typedef struct {
    int status;
    UiApiContentKind content_kind;
    StringBuilder body;
} UiApiResponse;

bool api_read_string_ref(const unsigned char *message, size_t message_length, size_t ref_offset, char **out);
bool api_read_data_ref(const unsigned char *message, size_t message_length, size_t ref_offset, const unsigned char **out, size_t *out_length);
bool api_send_frame(int fd, StringBuilder *message);

#endif
