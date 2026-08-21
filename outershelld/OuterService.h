#ifndef OUTER_SERVICE_H
#define OUTER_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct OuterServiceManager OuterServiceManager;

typedef void (*OuterServiceEventCallback)(void *context, const char *service_id);

typedef struct {
    const char *services_directory;
    const char *launcher_path;
    const char *api_socket_path;
    OuterServiceEventCallback event_callback;
    void *event_context;
} OuterServiceManagerOptions;

typedef struct {
    bool available;
    bool running;
    bool failed;
    int exit_status;
    const char *state;
} OuterServiceStatus;

OuterServiceManager *outer_service_manager_create(const OuterServiceManagerOptions *options,
                                                  char *error,
                                                  size_t error_size);
void outer_service_manager_destroy(OuterServiceManager *manager);

bool outer_service_manager_reload(OuterServiceManager *manager,
                                  char *error,
                                  size_t error_size);
bool outer_service_manager_load_service(OuterServiceManager *manager,
                                        const char *service_id,
                                        char *error,
                                        size_t error_size);
bool outer_service_manager_unload_service(OuterServiceManager *manager,
                                          const char *service_id,
                                          char *error,
                                          size_t error_size);
bool outer_service_manager_has_service(OuterServiceManager *manager, const char *service_id);
bool outer_service_manager_status(OuterServiceManager *manager,
                                  const char *service_id,
                                  OuterServiceStatus *status);
bool outer_service_manager_operate(OuterServiceManager *manager,
                                   const char *service_id,
                                   const char *operation,
                                   char *error,
                                   size_t error_size);

/* Stops every child process and waits for the supervisor thread to finish. */
void outer_service_manager_shutdown(OuterServiceManager *manager);
bool outer_service_manager_exit_requested(OuterServiceManager *manager);
int outer_service_manager_exit_status(OuterServiceManager *manager);

/* Used by the posix_spawn helper before normal daemon option parsing. */
int outer_service_exec_child(int argc, char **argv);

#endif
