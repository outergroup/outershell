#ifndef OUTER_APP_RESOURCES_H
#define OUTER_APP_RESOURCES_H
#include "Runtime.h"
bool app_resources(Connection *connection, const HttpRequest *request,
                   const char *directory, const char *bundle,
                   const char *initial);
bool app_resources_payload(Connection *, const HttpRequest *, const char *,
                           const char *, const void *, size_t);
#endif
