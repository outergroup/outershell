#include "AppResources.h"
#include "Runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *environment_request(const void *, size_t, size_t *);
#ifdef APP_TOP
#include "Host.h"
char *environment_processes(size_t *);
char *environment_detail(const char *, uint32_t, const char *, size_t *);
bool environment_signal_request(const char *, uint32_t, const char *, bool,
                                char *, size_t);
#endif
static HANDLE app_stop;
static char *app_directory, *app_web;
static BOOL WINAPI stop_app(DWORD event) {
    (void)event;
    SetEvent(app_stop);
    return TRUE;
}
#ifndef APP_TOP
static void watch_document(Connection *c, HttpRequest *r) {
    if (r->body.length < 72 ||
        read_uint32_le((const unsigned char *)r->body.data) != 0x51434c50 ||
        read_uint32_le((const unsigned char *)r->body.data + 12) != 10) {
        http_text(c, r, 400, "Invalid file notification request.");
        return;
    }
    write_uint32_le((unsigned char *)r->body.data + 12, 11);
    size_t length = 0;
    char *previous = environment_request(r->body.data, r->body.length, &length);
    if (!previous || length < 48 ||
        read_uint32_le((unsigned char *)previous + 12)) {
        free(previous);
        http_text(c, r, 503, "File notifications unavailable.");
        return;
    }
    const char *header =
        "HTTP/1.1 200 OK\r\nContent-Type: "
        "application/x-ndjson\r\nCache-Control: "
        "no-store\r\nConnection: close\r\n\r\n{\"ready\":true}\n";
    c->responded = true;
    bool ok = connection_write(c, header, strlen(header));
    while (ok && WaitForSingleObject(app_stop, 1000) == WAIT_TIMEOUT) {
        size_t next_length = 0;
        char *next =
            environment_request(r->body.data, r->body.length, &next_length);
        if (!next)
            break;
        bool changed = next_length != length || memcmp(previous, next, length);
        free(previous);
        previous = next;
        length = next_length;
        const char *event = changed ? "{\"changed\":true}\n" : "{}\n";
        ok = connection_write(c, event, strlen(event));
    }
    free(previous);
}
#endif
static void serve_app(Connection *c, void *context) {
    (void)context;
    HttpRequest r = {0};
    if (!http_read(c, &r) || !http_authorize(c, &r)) {
        http_free(&r);
        return;
    }
#ifdef APP_TOP
    if (top_request(c, &r, app_directory)) {
        http_free(&r);
        return;
    }
    bool resource = app_resources(c, &r, app_directory, APP_BUNDLE, "{\"windowsHost\":true}");
#else
    bool resource =
        app_resources(c, &r, app_directory, APP_BUNDLE, APP_INITIAL);
#endif
    if (resource) {
        http_free(&r);
        return;
    }
#ifdef APP_TOP
    if (!strcmp(r.path, "/api/environments") && !strcmp(r.method, "GET")) {
        size_t length = 0;
        char *body = environment_processes(&length);
        if (body)
            http_send(c, &r, 200, "application/octet-stream", body, length);
        else
            http_text(c, &r, 503, "Container discovery failed.");
        free(body);
    } else if (!strcmp(r.path, "/api/environments/signal") &&
               !strcmp(r.method, "POST")) {
        char *target = http_query(&r, "target"), *pid = http_query(&r, "pid"),
             *start = http_query(&r, "start"), *force = http_query(&r, "force");
        char error[1024] = "Invalid process identity.";
        char *end = NULL;
        unsigned long value = pid ? strtoul(pid, &end, 10) : 0;
        bool ok = target && pid && *pid && end && !*end && value > 0 &&
                  value <= INT_MAX && start && *start &&
                  environment_signal_request(target, (uint32_t)value, start,
                                             force && !strcmp(force, "1"),
                                             error, sizeof(error));
        http_text(c, &r, ok ? 200 : 400, ok ? "Signal sent." : error);
        free(target);
        free(pid);
        free(start);
        free(force);
    } else
#else
    if (!strcmp(r.path, "/api/watch") && !strcmp(r.method, "POST")) {
        watch_document(c, &r);
    } else if (!strcmp(r.path, "/api/environments") &&
               !strcmp(r.method, "POST")) {
        size_t length = 0;
        char *body = environment_request(r.body.data, r.body.length, &length);
        if (body)
            http_send(c, &r, 200, "application/octet-stream", body, length);
        else
            http_text(c, &r, 400, "Invalid environment request.");
        free(body);
    } else
#endif
        if (!strncmp(r.path, "/api/", 5))
        http_text(c, &r, 404, "Unknown app operation.");
    else {
        HttpRequest resource = r;
        if (!strncmp(r.path, "/web/", 5))
            resource.path = r.path + 4;
        http_static(c, &resource, app_web);
    }
    http_free(&r);
}
int wmain(int argc, wchar_t **argv) {
    (void)argv;
    if (argc != 1)
        return 2;
    app_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app_stop)
        return 1;
    SetConsoleCtrlHandler(stop_app, TRUE);
    app_directory = win_executable_directory();
    app_web = win_join(app_directory, APP_WEB);
#ifdef APP_TOP
    if (!top_start(app_stop)) { top_stop(); return 1; }
#endif
    char *name = win_pipe(APP_SERVICE);
    PipeServer *server = pipe_server_start(name, serve_app, NULL);
    free(name);
    if (!server)
        return 1;
    pipe_servers_wait_idle(app_stop, 300000);
    pipe_server_stop(server);
#ifdef APP_TOP
    top_stop();
#endif
    CloseHandle(app_stop);
    free(app_directory);
    free(app_web);
    return 0;
}
