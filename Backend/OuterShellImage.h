#ifndef OUTER_SHELL_IMAGE_H
#define OUTER_SHELL_IMAGE_H

#include <stdbool.h>
#include <stddef.h>

bool outer_shell_png_dimensions(const char *path,
                                size_t *width,
                                size_t *height,
                                char *error,
                                size_t error_size);

bool outer_shell_resize_png(const char *source_path,
                            const char *destination_path,
                            size_t max_pixel_size,
                            char *error,
                            size_t error_size);

#endif
