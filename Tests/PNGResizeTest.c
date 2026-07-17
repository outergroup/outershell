#include "../Backend/OuterShellImage.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s SOURCE DESTINATION MAX_PIXEL_SIZE\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    unsigned long requested_size = strtoul(argv[3], &end, 10);
    if (!end || *end || requested_size == 0) return 2;

    char error[256] = "";
    if (!outer_shell_resize_png(argv[1], argv[2], requested_size, error, sizeof(error))) {
        fprintf(stderr, "resize failed: %s\n", error);
        return 1;
    }
    size_t width = 0;
    size_t height = 0;
    if (!outer_shell_png_dimensions(argv[2], &width, &height, error, sizeof(error))) {
        fprintf(stderr, "dimension read failed: %s\n", error);
        return 1;
    }
    if (width != requested_size && height != requested_size) {
        fprintf(stderr, "unexpected resized dimensions: %zux%zu\n", width, height);
        return 1;
    }
    printf("%zux%zu\n", width, height);
    return 0;
}
