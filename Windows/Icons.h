#ifndef OUTER_WINDOWS_ICONS_H
#define OUTER_WINDOWS_ICONS_H
#include "Runtime.h"
/* Reads the PNG that Outer Loop cached for an endpoint in OuterSSH's per-user
 * endpoint icon cache, %LOCALAPPDATA%\OuterSSH\endpoint-icons. The endpoint is
 * a named pipe path, or else a TCP port on host (empty or loopback hosts are
 * localhost). Leaves image empty when there is no usable icon. */
bool icons_endpoint(const char *socket_path, const char *host, int port, StringBuilder *image);
/* Writes the cache file name: the lowercase hex SHA-256 of the endpoint's
 * canonical endpoints.d line, followed by ".png". */
bool icons_endpoint_file_name(const char *socket_path, const char *host, int port, char name[69]);
#endif
