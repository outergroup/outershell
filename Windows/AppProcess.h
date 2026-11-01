#ifndef OUTER_APP_PROCESS_H
#define OUTER_APP_PROCESS_H
#include "Runtime.h"
typedef struct {
    HANDLE process, cancel;
    Connection input, output;
} AppProcess;
wchar_t *app_runtime_executable(const char *runtime);
bool app_process_start(HANDLE stop, HANDLE job, const char *runtime,
                       const char *const *arguments, AppProcess *process);
void app_process_close(AppProcess *process);
// Runs this app's helper in a container: appends the helper command to args
// (count used of capacity), starts the runtime with them, and uploads the
// helper from the helpers folder beside the executable if the container needs
// it. See Resources/ContainerHelper.h.
bool app_helper_start(HANDLE stop, HANDLE job, const char *runtime,
                      const char **args, size_t count, size_t capacity,
                      const char *id, const char *name,
                      const char *const *helper_args, AppProcess *process,
                      char *error, size_t error_capacity);
HANDLE app_open_registry(HANDLE stop);
#endif
