#ifndef OUTER_SHELL_PLATFORM_H
#define OUTER_SHELL_PLATFORM_H

#include "OuterShellBuffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

bool queue_all(int fd, const void *data, size_t len);
int64_t monotonic_milliseconds(void);
bool set_fd_nonblocking(int fd, bool nonblocking);
bool mkdir_p(const char *path);

const char *home_directory(void);
void expand_tilde_path(const char *path, char *out, size_t out_size);
bool outer_shell_append_update_query(StringBuilder *builder, const char *heartbeat, const char *app_version);

#endif
