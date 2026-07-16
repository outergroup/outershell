#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void stop_fixture(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

int main(void) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    const char *listen_names = getenv("LISTEN_FDNAMES");
    char expected_pid[64];
    snprintf(expected_pid, sizeof(expected_pid), "%ld", (long)getpid());
    if (!listen_pid || strcmp(listen_pid, expected_pid) != 0 ||
        !listen_fds || strcmp(listen_fds, "1") != 0 ||
        !listen_names || strcmp(listen_names, "http") != 0) {
        fprintf(stderr, "invalid socket activation environment\n");
        return 10;
    }

    const char *start_log = getenv("FIXTURE_START_LOG");
    if (start_log) {
        FILE *log = fopen(start_log, "a");
        if (!log) return 11;
        fputs("started\n", log);
        fclose(log);
    }
    const char *fail_once = getenv("FIXTURE_FAIL_ONCE");
    if (fail_once && access(fail_once, F_OK) != 0) {
        FILE *marker = fopen(fail_once, "w");
        if (!marker) return 12;
        fputs("failed\n", marker);
        fclose(marker);
        return 23;
    }

    signal(SIGINT, stop_fixture);
    signal(SIGTERM, stop_fixture);
    while (!stopping) {
        int client = accept(3, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            return 13;
        }
        char request[1024];
        (void)read(client, request, sizeof(request));
        const char response[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 18\r\n"
            "Connection: close\r\n\r\n"
            "outerservice works\n";
        (void)write(client, response, sizeof(response) - 1);
        close(client);
        return 0;
    }
    return 0;
}
