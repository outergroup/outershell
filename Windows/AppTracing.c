#include "AppProcess.h"
#include "AppResources.h"
#include "Runtime.h"
#include <stdio.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
static HANDLE tracing_stop, tracing_job;
static SRWLOCK tracing_lock = SRWLOCK_INIT;
static char *tracing_directory, *tracing_web;
typedef struct {
    char *key, *runtime, *id, *name, *state;
} Target;
typedef struct {
    char *key;
    AppProcess child;
    uint32_t sequence;
} Session;
static Session tracing_sessions[32];
static void free_targets(Target *targets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(targets[i].key);
        free(targets[i].runtime);
        free(targets[i].id);
        free(targets[i].name);
        free(targets[i].state);
    }
    free(targets);
}
static Target *targets_read(size_t *count) {
    *count = 0;
    Target *targets = NULL;
    Connection c = {0};
    c.stop = tracing_stop;
    c.pipe = app_open_registry(tracing_stop);
    if (c.pipe == INVALID_HANDLE_VALUE)
        return NULL;
    unsigned char request[] = {2, 0, 0, 0, 34, 0}, header[4];
    StringBuilder body = {0};
    bool ok =
        connection_write(&c, request, 6) && connection_read(&c, header, 4);
    size_t n = ok ? read_uint32_le(header) : 0;
    if (n >= 48 && n <= HTTP_BODY_LIMIT) {
        body = payload(n);
        ok = connection_read(&c, body.data, n);
    } else
        ok = false;
    CloseHandle(c.pipe);
    if (ok && !memcmp(body.data, "OSED", 4) &&
        read_uint32_le((unsigned char *)body.data + 4) == 1 &&
        read_uint32_le((unsigned char *)body.data + 8) == n) {
        size_t rows = read_uint32_le((unsigned char *)body.data + 12);
        if (rows <= (n - 48) / 72 &&
            read_uint32_le((unsigned char *)body.data + 24) == 48 + rows * 72) {
            targets = win_alloc(rows * sizeof(*targets));
            for (size_t i = 0; i < rows; i++) {
                Target t = {0};
                t.name = payload_string(body.data, n, 48 + i * 72 + 8);
                t.runtime = payload_string(body.data, n, 48 + i * 72 + 16);
                t.id = payload_string(body.data, n, 48 + i * 72 + 24);
                t.state = payload_string(body.data, n, 48 + i * 72 + 32);
                if (t.name && t.runtime && t.id && t.state &&
                    (!strcmp(t.runtime, "docker") ||
                     !strcmp(t.runtime, "wslc")) &&
                    strlen(t.id) == 64 &&
                    strspn(t.id, "0123456789abcdef") == 64) {
                    StringBuilder key = {0};
                    sb_append(&key, t.runtime);
                    sb_append(&key, ":");
                    sb_append(&key, t.id);
                    t.key = key.data;
                    targets[(*count)++] = t;
                } else {
                    free(t.name);
                    free(t.runtime);
                    free(t.id);
                    free(t.state);
                }
            }
        }
    }
    free(body.data);
    return targets;
}
static void html_text(StringBuilder *out, const char *text) {
    for (const char *p = text; *p; p++) {
        if (*p == '&')
            sb_append(out, "&amp;");
        else if (*p == '<')
            sb_append(out, "&lt;");
        else if (*p == '>')
            sb_append(out, "&gt;");
        else
            sb_append_n(out, p, 1);
    }
}
static void session_close(Session *s) {
    app_process_close(&s->child);
    free(s->key);
    memset(s, 0, sizeof(*s));
}
static bool docker_command(const char *const *args) {
    AppProcess child = {0};
    if (!app_process_start(tracing_stop, tracing_job, "docker", args, &child))
        return false;
    child.output.unlimited_io_wait = true;
    unsigned char buffer[4096];
    size_t length;
    while (
        connection_read_some(&child.output, buffer, sizeof(buffer), &length) &&
        length) {
    }
    DWORD code = 1;
    bool ok = WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.process, &code) && code == 0;
    app_process_close(&child);
    return ok;
}
static bool docker_image(void) {
    const char *inspect[] = {"image", "inspect", "outershell/container-base:22",
                             NULL};
    if (docker_command(inspect))
        return true;
    char *context = win_join(tracing_directory, "container-bootstrap");
    const char *build[] = {"build", "--tag", "outershell/container-base:22",
                           context, NULL};
    bool ok = docker_command(build);
    free(context);
    return ok;
}
static bool tracing_request(Connection *c, HttpRequest *r, Target *target) {
    AcquireSRWLockExclusive(&tracing_lock);
    Session *session = NULL;
    for (size_t i = 0; i < 32; i++)
        if (tracing_sessions[i].key &&
            !strcmp(tracing_sessions[i].key, target->key))
            session = &tracing_sessions[i];
    if (!session)
        for (size_t i = 0; i < 32; i++)
            if (!tracing_sessions[i].key) {
                session = &tracing_sessions[i];
                break;
            }
    if (!session) {
        session = &tracing_sessions[0];
        session_close(session);
    }
    bool ok = true;
    if (!session->key) {
        StringBuilder context = {0}, name = {0};
        sb_append(&context, "OUTER_TRACING_CONTEXT_ID=");
        sb_append(&context, target->key);
        sb_append(&name, "OUTER_TRACING_CONTEXT_NAME=");
        sb_append(&name, target->name);
        bool docker = !strcmp(target->runtime, "docker");
        char *local = win_folder(CSIDL_LOCAL_APPDATA);
        char *state_root = win_join(local, "OuterShell/tracing");
        char *app_state = win_join(state_root, APP_NAME);
        char *state = win_join(app_state, target->id);
        bool state_ready = !docker || (win_secure_directory(state_root) &&
                                      win_secure_directory(app_state) &&
                                      win_secure_directory(state));
        StringBuilder mount = {0}, pid = {0};
        sb_append(&mount, state);
        sb_append(&mount, ":/state");
        sb_append(&pid, "container:");
        sb_append(&pid, target->id);
        const char *args[64] = {0};
        size_t argument = 0;
        args[argument++] = docker ? "run" : "exec";
        args[argument++] = "--interactive";
        if (docker) {
            args[argument++] = "--rm";
            args[argument++] = "--label";
            args[argument++] = "org.outershell.tracing-worker=" APP_NAME;
            args[argument++] = "--pid";
            args[argument++] = pid.data;
            const char *caps[] = {"BPF", "PERFMON", "SYS_PTRACE", "SYS_ADMIN",
                                  "IPC_LOCK"};
            for (size_t i = 0; i < sizeof(caps) / sizeof(*caps); i++) {
                args[argument++] = "--cap-add";
                args[argument++] = caps[i];
            }
            args[argument++] = "--security-opt";
            args[argument++] = "seccomp=unconfined";
            args[argument++] = "--volume";
            args[argument++] = mount.data;
            args[argument++] = "-e";
            args[argument++] = "HOME=/state";
        }
        args[argument++] = "--user";
        args[argument++] = "0";
        args[argument++] = "-e";
        args[argument++] = context.data;
        args[argument++] = "-e";
        args[argument++] = name.data;
        args[argument++] = docker ? "outershell/container-base:22" : target->id;
        args[argument] = NULL;
        const char *serve[] = {"serve", NULL};
        char error[512];
        ok = (!docker || (state_ready && docker_image())) &&
             app_helper_start(tracing_stop, tracing_job, target->runtime, args,
                              argument, 64, APP_SERVICE, APP_HELPER_NAME, serve,
                              &session->child, error, sizeof(error));
        free(local);
        free(state_root);
        free(app_state);
        free(state);
        free(mount.data);
        free(pid.data);
        free(context.data);
        free(name.data);
        if (ok)
            session->key = win_strdup(target->key);
    }
    StringBuilder request = {0};
    sb_append(&request, r->method);
    sb_append(&request, " ");
    sb_append(&request, r->path);
    if (r->query && *r->query) {
        sb_append(&request, "?");
        sb_append(&request, r->query);
    }
    sb_append(&request,
              " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    unsigned char header[24] = {0};
    memcpy(header, "TRPC", 4);
    write_uint32_le(header + 4, 1);
    write_uint32_le(header + 8, (uint32_t)(24 + request.length));
    write_uint32_le(header + 12, ++session->sequence);
    write_uint32_le(header + 16, 24);
    write_uint32_le(header + 20, (uint32_t)request.length);
    ok =
        ok && connection_write(&session->child.input, header, 24) &&
        connection_write(&session->child.input, request.data, request.length) &&
        connection_read(&session->child.output, header, 24);
    free(request.data);
    size_t size = ok ? read_uint32_le(header + 20) : 0;
    ok = ok && !memcmp(header, "TRPC", 4) && read_uint32_le(header + 4) == 1 &&
         read_uint32_le(header + 8) == size + 24 &&
         read_uint32_le(header + 12) == session->sequence &&
         read_uint32_le(header + 16) == 24 && size <= HTTP_BODY_LIMIT;
    StringBuilder response = {0};
    if (ok) {
        response = payload(size);
        ok = connection_read(&session->child.output, response.data, size) &&
             size >= 12 && !memcmp(response.data, "HTTP/1.1 ", 9);
    }
    if (ok) {
        c->responded = true;
        ok = connection_write(c, response.data, response.length);
    } else
        session_close(session);
    free(response.data);
    ReleaseSRWLockExclusive(&tracing_lock);
    return ok;
}
static void tracing_serve(Connection *c, void *context) {
    (void)context;
    HttpRequest r = {0};
    if (!http_read(c, &r) || !http_authorize(c, &r)) {
        http_free(&r);
        return;
    }
    if (strcmp(r.method, "GET") && strcmp(r.method, "HEAD")) {
        http_text(c, &r, 405, "Use GET or HEAD.");
        http_free(&r);
        return;
    }
    size_t count = 0;
    Target *targets = targets_read(&count);
    Target *selected = NULL;
    HttpRequest forwarded = r;
    if (!strncmp(r.path, "/targets/", 9)) {
        char *end = strchr(r.path + 9, '/');
        if (end)
            for (size_t i = 0; i < count; i++)
                if (strlen(targets[i].key) == (size_t)(end - r.path - 9) &&
                    !memcmp(targets[i].key, r.path + 9,
                            (size_t)(end - r.path - 9))) {
                    selected = &targets[i];
                    forwarded.path = end;
                    break;
                }
    }
    if (!strcmp(r.path, "/")) {
        StringBuilder html = {0};
        sb_append(
            &html,
            "<!doctype html><meta name=viewport "
            "content='width=device-width,initial-scale=1'><title>" APP_NAME
            "</title><style>body{font:16px system-ui;margin:40px "
            "auto;padding:0 "
            "24px;max-width:640px;color:CanvasText;background:Canvas;color-"
            "scheme:"
            "light dark}a{display:block;padding:18px;margin:12px 0;border:1px "
            "solid "
            "#8886;border-radius:12px;color:inherit;text-decoration:none}small{"
            "display:block;opacity:.6}</style><h1>" APP_NAME
            "</h1><p>Choose a container.</p>");
        for (size_t i = 0; i < count; i++) {
            sb_append(&html, "<a href='/targets/");
            sb_append(&html, targets[i].key);
            sb_append(&html, "/'>");
            html_text(&html, targets[i].name);
            sb_append(&html, "<small>");
            html_text(&html, targets[i].runtime);
            sb_append(&html, " · ");
            html_text(&html, targets[i].state);
            if (!strcmp(targets[i].runtime, "wslc"))
                sb_append(&html, " · Live capture requires tracing permissions "
                                 "that WSLc currently does not expose.");
            sb_append(&html, "</small></a>");
        }
        if (!count)
            sb_append(&html,
                      "<p>No registered containers found. Start a container "
                      "in Outer Shell, then refresh.</p>");
        http_send(c, &r, 200, "text/html; charset=utf-8", html.data,
                  html.length);
        free(html.data);
    } else if (app_resources(c, &forwarded, tracing_directory, APP_BUNDLE,
                             "{}")) {
    } else if (!strcmp(forwarded.path, "/") ||
               !strncmp(forwarded.path, "/web/", 5) ||
               !strcmp(forwarded.path, "/favicon.ico")) {
        if (!strncmp(forwarded.path, "/web/", 5))
            forwarded.path += 4;
        if (!strcmp(forwarded.path, "/favicon.ico"))
            forwarded.path = "/app-icon.png";
        http_static(c, &forwarded, tracing_web);
    } else if (!selected || strcmp(selected->state, "running"))
        http_text(c, &r, 503,
                  "The selected container is not running or is no longer "
                  "registered.");
    else if (!tracing_request(c, &forwarded, selected) && !c->responded)
        http_text(
            c, &r, 503,
            "The container tracing helper could not start. Profile also "
            "requires perf and profiling capabilities in the container.");
    free_targets(targets, count);
    http_free(&r);
}
int wmain(int argc, wchar_t **argv) {
    (void)argv;
    if (argc != 1)
        return 2;
    tracing_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    tracing_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!tracing_stop || !tracing_job ||
        !SetInformationJobObject(tracing_job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return 1;
    tracing_directory = win_executable_directory();
    tracing_web = win_join(tracing_directory, APP_WEB);
    char *name = win_pipe(APP_SERVICE);
    PipeServer *server = pipe_server_start(name, tracing_serve, NULL);
    free(name);
    if (!server)
        return 1;
    pipe_servers_wait_idle(tracing_stop, 300000);
    pipe_server_stop(server);
    for (size_t i = 0; i < 32; i++)
        session_close(&tracing_sessions[i]);
    CloseHandle(tracing_job);
    CloseHandle(tracing_stop);
    free(tracing_directory);
    free(tracing_web);
    return 0;
}
