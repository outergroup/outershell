#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool outer_shell_test_run_internal_lifecycle_script(const char *subcommand,
                                                    const char *script_path,
                                                    char *message,
                                                    size_t message_size);

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s INSTALLER MARKER\n", argv[0]);
        return 2;
    }

    if (setenv("OUTER_SHELL_LIFECYCLE_MARKER", argv[2], 1) != 0) {
        perror("setenv");
        return 1;
    }

    char message[512] = "";
    if (!outer_shell_test_run_internal_lifecycle_script("uninstall",
                                                        argv[1],
                                                        message,
                                                        sizeof(message))) {
        fprintf(stderr, "internal lifecycle launch failed: %s\n", message);
        return 1;
    }
    if (strcmp(message, "Outer Shell uninstall started.") != 0) {
        fprintf(stderr, "unexpected lifecycle response: %s\n", message);
        return 1;
    }

    for (int attempts = 0; attempts < 100; attempts++) {
        if (access(argv[2], F_OK) == 0) {
            printf("internal home-screen lifecycle launch test passed\n");
            return 0;
        }
        usleep(50000);
    }

    fprintf(stderr, "detached installer did not run\n");
    return 1;
}
