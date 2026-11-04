#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    StringBuilder script = {0};
    if (!make_fixed_port_script("inline-startup-test", "Inline Startup Test", "8123", NULL, false, argv[1], &script)) {
        free(script.data);
        return 1;
    }
    fputs(script.data ? script.data : "", stdout);
    free(script.data);
    return 0;
}
