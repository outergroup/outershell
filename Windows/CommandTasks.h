#ifndef OUTER_COMMAND_TASKS_H
#define OUTER_COMMAND_TASKS_H
#include <windows.h>
HRESULT command_task(const char *name, const char *operation, const char *xml, DWORD *state);
#endif
