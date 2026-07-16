#include "../Backend/OuterShellBuffer.h"
#include "../Backend/OuterShellPlatform.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *service_manager = argc > 1 ? argv[1] : NULL;
    StringBuilder query = {0};
    if (!outer_shell_append_update_query(&query,
                                         "day bucket",
                                         "0.2.2+dev",
                                         service_manager)) {
        free(query.data);
        return 1;
    }
    puts(query.data ? query.data : "");
    free(query.data);
    return 0;
}
