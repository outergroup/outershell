#include "../Backend/OuterShellImage.h"

#include <stdio.h>

bool outer_shell_png_dimensions(const char *path,
                                size_t *width,
                                size_t *height,
                                char *error,
                                size_t error_size) {
    (void)path;
    (void)width;
    (void)height;
    if (error && error_size > 0) snprintf(error, error_size, "PNG support is unavailable in this test build.");
    return false;
}

bool outer_shell_resize_png(const char *source_path,
                            const char *destination_path,
                            size_t max_pixel_size,
                            char *error,
                            size_t error_size) {
    (void)source_path;
    (void)destination_path;
    (void)max_pixel_size;
    if (error && error_size > 0) snprintf(error, error_size, "PNG support is unavailable in this test build.");
    return false;
}
