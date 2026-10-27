#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 4);
    char error[1024] = {0};
    mode_t mode = strtol(argv[3], NULL, 8);
    assert(copy_directory_tree(argv[1], argv[2], mode, mode | 0100, error, sizeof(error)));
    return 0;
}
