#ifndef OUTER_WINDOWS_WSL_H
#define OUTER_WINDOWS_WSL_H
#include "Runtime.h"
typedef struct Wsl Wsl;
typedef bool (*WslRegistryRequest)(void *context, const unsigned char *request, uint32_t length,
                                   unsigned char **response, uint32_t *response_length);
StringBuilder wsl_environments(Wsl *wsl);
int wsl_inspect(void);
Wsl *wsl_start(HANDLE stop, WslRegistryRequest registry, void *registry_context);
void wsl_refresh(Wsl *wsl);
void wsl_stop(Wsl *wsl);
uint64_t wsl_version(Wsl *wsl);
void wsl_serve(Wsl *wsl, Connection *connection, HttpRequest *request);
#endif
