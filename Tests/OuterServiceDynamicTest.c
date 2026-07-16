#include "../outershelld/OuterService.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void pause_milliseconds(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000
    };
    nanosleep(&delay, NULL);
}

static bool write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    if (!file) return false;
    bool ok = fputs(contents, file) >= 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--outerservice-exec") == 0) {
        return outer_service_exec_child(argc, argv);
    }
    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/') {
        fprintf(stderr, "usage: %s /absolute/test-root /absolute/fixture\n", argv[0]);
        return 2;
    }

    char services[4096];
    char resident_file[4096];
    char dynamic_file[4096];
    char socket_path[4096];
    snprintf(services, sizeof(services), "%s/services", argv[1]);
    snprintf(resident_file, sizeof(resident_file), "%s/resident.outerservice", services);
    snprintf(dynamic_file, sizeof(dynamic_file), "%s/dynamic.http.outerservice", services);
    snprintf(socket_path, sizeof(socket_path), "%s/dynamic.socket", argv[1]);
    if (mkdir(argv[1], 0755) != 0 && errno != EEXIST) return 3;
    if (mkdir(services, 0755) != 0 && errno != EEXIST) return 3;

    const char resident_contents[] =
        "[Service]\n"
        "Format=1\n"
        "Executable=/bin/sh\n"
        "Argument=-c\n"
        "Argument=sleep 30\n"
        "Start=eager\n"
        "Restart=never\n"
        "StopTimeoutMilliseconds=500\n";
    if (!write_file(resident_file, resident_contents)) return 4;

    char error[1024] = "";
    OuterServiceManagerOptions options = {
        .services_directory = services,
        .launcher_path = argv[0]
    };
    OuterServiceManager *manager = outer_service_manager_create(&options, error, sizeof(error));
    if (!manager) {
        fprintf(stderr, "create: %s\n", error);
        return 5;
    }

    OuterServiceStatus resident = {0};
    for (int attempt = 0; attempt < 100; attempt++) {
        if (outer_service_manager_status(manager, "resident", &resident) && resident.running) break;
        pause_milliseconds(20);
    }
    if (!resident.running) {
        fprintf(stderr, "resident service did not start\n");
        outer_service_manager_destroy(manager);
        return 6;
    }

    char dynamic_contents[12000];
    snprintf(dynamic_contents, sizeof(dynamic_contents),
             "[Service]\n"
             "Format=1\n"
             "Executable=%s\n"
             "Start=socket\n"
             "Restart=never\n"
             "StopTimeoutMilliseconds=500\n"
             "\n"
             "[Socket.http]\n"
             "Type=unix\n"
             "Path=%s\n"
             "Mode=0600\n",
             argv[2], socket_path);
    if (!write_file(dynamic_file, dynamic_contents) ||
        !outer_service_manager_load_service(manager, "dynamic.http", error, sizeof(error))) {
        fprintf(stderr, "load: %s\n", error);
        outer_service_manager_destroy(manager);
        return 7;
    }

    resident = (OuterServiceStatus){0};
    if (!outer_service_manager_status(manager, "resident", &resident) || !resident.running) {
        fprintf(stderr, "loading a service interrupted the resident service\n");
        outer_service_manager_destroy(manager);
        return 8;
    }

    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (client < 0 || connect(client, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("connect dynamic socket");
        if (client >= 0) close(client);
        outer_service_manager_destroy(manager);
        return 9;
    }
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    (void)write(client, request, sizeof(request) - 1);
    char response[1024] = "";
    ssize_t response_length = read(client, response, sizeof(response) - 1);
    close(client);
    if (response_length <= 0 || !strstr(response, "outerservice works")) {
        fprintf(stderr, "dynamic service returned an unexpected response\n");
        outer_service_manager_destroy(manager);
        return 10;
    }

    if (!outer_service_manager_unload_service(manager, "dynamic.http", error, sizeof(error)) ||
        access(socket_path, F_OK) == 0 ||
        outer_service_manager_has_service(manager, "dynamic.http")) {
        fprintf(stderr, "unload: %s\n", error);
        outer_service_manager_destroy(manager);
        return 11;
    }
    if (!outer_service_manager_unload_service(manager, "resident", error, sizeof(error))) {
        fprintf(stderr, "unload resident: %s\n", error);
        outer_service_manager_destroy(manager);
        return 12;
    }
    outer_service_manager_destroy(manager);
    return 0;
}
