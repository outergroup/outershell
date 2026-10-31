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
HANDLE app_open_registry(HANDLE stop);
#endif
