#include <launch.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int *listeners = NULL;
    size_t count = 0;
    int result = launch_activate_socket("Listener", &listeners, &count);
    fprintf(stderr, "pid=%d launch_activate_socket=%d count=%zu\n", getpid(), result, count);
    fflush(stderr);
    if (result != 0 || count == 0) {
        free(listeners);
        return 1;
    }

    int status = 1;
    struct pollfd descriptor = { .fd = listeners[0], .events = POLLIN };
    if (poll(&descriptor, 1, 10000) > 0) {
        int client = accept(listeners[0], NULL, NULL);
        if (client >= 0) {
            char request[4096];
            if (read(client, request, sizeof(request)) > 0) {
                const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nOK\n";
                if (write(client, reply, sizeof(reply) - 1) == sizeof(reply) - 1) {
                    status = 0;
                }
            }
            close(client);
        }
    }
    for (size_t index = 0; index < count; index++) {
        close(listeners[index]);
    }
    free(listeners);
    return status;
}
