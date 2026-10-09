#ifndef OUTER_WINDOWS_RUNTIME_H
#define OUTER_WINDOWS_RUNTIME_H
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../Backend/OuterShellBuffer.h"
#define HTTP_BODY_LIMIT (16u * 1024u * 1024u)
typedef struct PipeServer PipeServer;
typedef struct {
    HANDLE pipe, stop, cancel;
    unsigned char buffer[8192];
    size_t offset, length;
    bool responded;
} Connection;
typedef void (*PipeHandler)(Connection *, void *);
typedef struct {
    char *method, *path, *query, *host, *origin;
    StringBuilder headers, body;
    bool accepts_outerframe;
} HttpRequest;
void *win_alloc(size_t size);
char *win_strdup(const char *value);
wchar_t *win_wide(const char *value);
char *win_utf8(const wchar_t *value);
char *win_join(const char *base, const char *name);
char *win_folder(int folder);
char *win_executable_directory(void);
char *win_sid(void);
char *win_pipe(const char *app);
bool win_secure_directory(const char *path);
bool win_atomic_write(const char *path, const void *data, size_t length);
bool win_read_file(const char *path, size_t limit, StringBuilder *result);
bool win_pipe_is_allowed(const char *pipe);
bool win_disallow_pipe(const char *pipe);
bool win_allow_pipes(const char *const *paths, size_t count);
PipeServer *pipe_server_start(const char *name, PipeHandler handler, void *context);
void pipe_server_stop(PipeServer *server);
bool connection_read_some(Connection *connection, void *data, size_t capacity, size_t *length);
bool connection_read(Connection *connection, void *data, size_t length);
bool connection_write(Connection *connection, const void *data, size_t length);
bool http_read(Connection *connection, HttpRequest *request);
void http_free(HttpRequest *request);
char *http_form(const HttpRequest *request, const char *name);
char *http_query(const HttpRequest *request, const char *name);
bool http_authorize(Connection *connection, const HttpRequest *request);
bool http_header(Connection *connection, int status, const char *type, uint64_t length,
                 const char *disposition);
void http_send(Connection *connection, const HttpRequest *request, int status, const char *type,
               const void *body, size_t length);
void http_text(Connection *connection, const HttpRequest *request, int status, const char *message);
void http_file(Connection *connection, const HttpRequest *request, const char *path, bool preview,
               bool download);
void http_static(Connection *connection, const HttpRequest *request, const char *root);
void http_windows_error(Connection *connection, const HttpRequest *request, DWORD error);
const char *http_content_type(const char *path);
StringBuilder payload(size_t length);
void payload_text(StringBuilder *p, size_t offset, const char *text);
void payload_data(StringBuilder *p, size_t offset, const void *data, size_t length);
void payload_u32(StringBuilder *p, size_t offset, uint32_t value);
void payload_u64(StringBuilder *p, size_t offset, uint64_t value);
char *payload_string(const void *bytes, size_t length, size_t offset);
uint64_t payload_read64(const void *bytes);
bool win_set_pipe_activation(const char *pipe, const char *task);

void pipe_servers_wait_idle(HANDLE stop, DWORD timeout_ms);

#endif
