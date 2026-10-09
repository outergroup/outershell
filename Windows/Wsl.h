#ifndef OUTER_WINDOWS_WSL_H
#define OUTER_WINDOWS_WSL_H
#include "Runtime.h"
typedef struct Wsl Wsl;
int wsl_inspect(void);
Wsl *wsl_start(HANDLE stop);
void wsl_stop(Wsl *wsl);
uint64_t wsl_version(Wsl *wsl);
void wsl_serve(Wsl *wsl, Connection *connection, HttpRequest *request);
#endif
