#include "Runtime.h"
#include "Wsl.h"
#include "NativeRegistry.c"
#include "../Backend/OuterShellLayout.h"
#include <shlobj.h>

static const char *shell_id = "org.outershell.OuterShell", *files_id = "org.outershell.Files";
typedef struct {
    char *state, *registry, *directory, *web, *service_name;
    HANDLE stop, files, job;
    CRITICAL_SECTION gate;
    CONDITION_VARIABLE changed;
    uint64_t version;
    PipeServer *http, *api;
    Wsl *wsl;
} ShellHost;
static ShellHost shell;
static SERVICE_STATUS_HANDLE service_handle;
static SERVICE_STATUS service_status;
static void changed(ShellHost *s) {
    s->version++;
    WakeAllConditionVariable(&s->changed);
}
static void log_error(ShellHost *s, const char *message) {
    char *path = win_join(s->state, "daemon.log");
    StringBuilder text = {0};
    if (!win_read_file(path, 4 * 1024 * 1024, &text)) {
        free(text.data);
        text = (StringBuilder){0};
    }
    SYSTEMTIME time;
    GetSystemTime(&time);
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ ", time.wYear, time.wMonth, time.wDay,
             time.wHour, time.wMinute, time.wSecond);
    if (sb_append(&text, stamp) && sb_append(&text, message) && sb_append(&text, "\n"))
        win_atomic_write(path, text.data, text.length);
    free(text.data);
    free(path);
    fprintf(stderr, "%s\n", message);
}
static bool store_open(ShellHost *s, RegistryStore *store, bool write) {
    char error[1024] = "Registry open failed.";
    if (registry_store_open_at(store, s->registry, write, error, sizeof(error)))
        return true;
    log_error(s, error);
    return false;
}
static bool store_close(ShellHost *s, RegistryStore *store, bool commit) {
    char error[1024] = "Registry save failed.";
    if (registry_store_close(store, commit, error, sizeof(error)))
        return true;
    log_error(s, error);
    return false;
}
static bool update_allowlist(ShellHost *s) {
    RegistryStore store;
    if (!store_open(s, &store, false))
        return false;
    const char **paths = win_alloc(store.frontend_count * sizeof(*paths));
    size_t count = 0;
    for (size_t i = 0; i < store.frontend_count; i++)
        if (store.frontends[i].endpoint_kind == 3)
            paths[count++] = store.frontends[i].socket_path;
    bool ok = win_allow_pipes(paths, count);
    free(paths);
    return store_close(s, &store, false) && ok;
}
static bool files_running(ShellHost *s) {
    if (!s->files)
        return false;
    if (WaitForSingleObject(s->files, 0) == WAIT_TIMEOUT)
        return true;
    CloseHandle(s->files);
    s->files = NULL;
    changed(s);
    return false;
}
static bool start_files(ShellHost *s) {
    if (files_running(s))
        return true;
    if (WaitForSingleObject(s->stop, 0) == WAIT_OBJECT_0)
        return false;
    char *path = win_join(s->directory, "FilesBackend.exe");
    wchar_t *exe = win_wide(path);
    free(path);
    if (!exe)
        return false;
    size_t count = wcslen(exe) + 3;
    wchar_t *command = win_alloc(count * sizeof(wchar_t));
    swprintf(command, count, L"\"%s\"", exe);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    bool ok = CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL,
                             &startup, &process) != 0;
    free(exe);
    free(command);
    if (ok) {
        ok = AssignProcessToJobObject(s->job, process.hProcess) && ResumeThread(process.hThread) != (DWORD)-1;
        if (ok)
            s->files = process.hProcess;
        else {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        CloseHandle(process.hThread);
    }
    if (ok)
        changed(s);
    else
        log_error(s, "Files process could not start.");
    return ok;
}
static bool stop_files(ShellHost *s) {
    if (!files_running(s))
        return true;
    if (!TerminateProcess(s->files, 0) || WaitForSingleObject(s->files, 5000) != WAIT_OBJECT_0)
        return false;
    CloseHandle(s->files);
    s->files = NULL;
    changed(s);
    return true;
}
static bool set_files_enabled(ShellHost *s, bool enabled) {
    RegistryStore store;
    if (!store_open(s, &store, true))
        return false;
    unsigned char flag = enabled ? 1 : 0;
    bool ok = registry_store_upsert_resource(&store, "windows.files.enabled", &flag, 1);
    return store_close(s, &store, ok) && ok;
}
static DWORD service_query(const char *name) {
    SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return 0;
    wchar_t *wide = win_wide(name);
    SC_HANDLE service = wide ? OpenServiceW(manager, wide, SERVICE_QUERY_STATUS) : NULL;
    free(wide);
    SERVICE_STATUS status = {0};
    if (service) {
        QueryServiceStatus(service, &status);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return status.dwCurrentState;
}
static bool service_wait(ShellHost *s, SC_HANDLE service, DWORD target) {
    ULONGLONG deadline = GetTickCount64() + 15000;
    do {
        SERVICE_STATUS status;
        if (!QueryServiceStatus(service, &status))
            return false;
        if (status.dwCurrentState == target)
            return true;
        if (WaitForSingleObject(s->stop, 100) == WAIT_OBJECT_0)
            return false;
    } while (GetTickCount64() < deadline);
    return false;
}
static bool service_control(ShellHost *s, const char *name, const char *operation) {
    bool start = !strcmp(operation, "start") || !strcmp(operation, "restart"),
         stop = !strcmp(operation, "stop") || !strcmp(operation, "restart");
    if (!start && !stop)
        return false;
    SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return false;
    wchar_t *wide = win_wide(name);
    SC_HANDLE service =
        wide ? OpenServiceW(manager, wide,
                            SERVICE_QUERY_STATUS | (start ? SERVICE_START : 0) | (stop ? SERVICE_STOP : 0))
             : NULL;
    free(wide);
    bool ok = service != NULL;
    SERVICE_STATUS status;
    if (ok && stop)
        ok = (ControlService(service, SERVICE_CONTROL_STOP, &status) ||
              GetLastError() == ERROR_SERVICE_NOT_ACTIVE) &&
             service_wait(s, service, SERVICE_STOPPED);
    if (ok && start)
        ok = (StartServiceW(service, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) &&
             service_wait(s, service, SERVICE_RUNNING);
    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}
static const char *service_label(DWORD status) {
    switch (status) {
    case SERVICE_RUNNING:
        return "running";
    case SERVICE_STOPPED:
        return "stopped";
    case SERVICE_START_PENDING:
        return "starting";
    case SERVICE_STOP_PENDING:
        return "stopping";
    case SERVICE_PAUSED:
        return "paused";
    default:
        return "service unavailable";
    }
}
static int compare_backends(const void *a, const void *b) {
    return _stricmp(((const RegistryBackendRecord *)a)->display_name,
                    ((const RegistryBackendRecord *)b)->display_name);
}
static StringBuilder backends(ShellHost *s) {
    RegistryStore store;
    if (!store_open(s, &store, false))
        return (StringBuilder){0};
    qsort(store.backends, store.backend_count, sizeof(*store.backends), compare_backends);
    StringBuilder result = payload(12 + store.backend_count * 8);
    payload_u32(&result, 8, (uint32_t)store.backend_count);
    char *sid = win_sid();
    const char *scope = sid && !strcmp(sid, "S-1-5-18") ? "system" : "user";
    free(sid);
    for (size_t i = 0; i < store.backend_count; i++) {
        RegistryBackendRecord *b = &store.backends[i];
        bool running = false, controls = false;
        const char *status = "registered";
        if (!strcmp(b->service_id, shell_id)) {
            running = true;
            status = "running";
        } else if (!strcmp(b->service_id, files_id)) {
            running = files_running(s);
            status = running ? "running" : "stopped";
            controls = true;
        } else if (b->unit_name[0]) {
            DWORD state = service_query(b->unit_name);
            running = state == SERVICE_RUNNING;
            status = service_label(state);
            controls = b->owns_unit;
        }
        size_t count = 0;
        for (size_t j = 0; j < store.frontend_count; j++)
            if (!strcmp(store.frontends[j].service_id, b->service_id))
                count++;
        StringBuilder apps = payload(4 + count * 8);
        payload_u32(&apps, 0, (uint32_t)count);
        size_t index = 0;
        for (size_t j = 0; j < store.frontend_count; j++) {
            RegistryFrontendRecord *a = &store.frontends[j];
            if (strcmp(a->service_id, b->service_id))
                continue;
            StringBuilder app = payload(72);
            payload_text(&app, 0, a->display_name);
            payload_text(&app, 8, a->endpoint_kind == 3 ? a->path : a->url);
            payload_text(&app, 16, a->socket_path);
            payload_text(&app, 24, a->icon_path);
            if (!strpbrk(b->service_id, "/\\:")) {
                char name[512];
                snprintf(name, sizeof(name), "icons/%s.png", b->service_id);
                char *path = win_join(s->web, name);
                StringBuilder icon = {0};
                if (win_read_file(path, 1024 * 1024, &icon))
                    payload_data(&app, 32, icon.data, icon.length);
                free(icon.data);
                free(path);
            }
            payload_u32(&app, 48, (uint32_t)a->port);
            payload_text(&app, 52, a->frontend_id);
            payload_u32(&app, 60,
                        running || (strcmp(b->service_id, files_id) && strcmp(b->service_id, shell_id)) ? 1
                                                                                                        : 0);
            payload_data(&apps, 4 + index++ * 8, app.data, app.length);
            free(app.data);
        }
        StringBuilder row = payload(116);
        payload_text(&row, 0, b->service_id);
        payload_text(&row, 8, b->display_name);
        payload_text(&row, 16, b->unit_name);
        payload_text(&row, 32, scope);
        payload_text(&row, 40, status);
        payload_u32(&row, 64, 8u | (controls ? 1u : 0u));
        payload_data(&row, 68, apps.data, apps.length);
        uint32_t empty = 0;
        payload_data(&row, 76, &empty, 4);
        payload_data(&result, 12 + i * 8, row.data, row.length);
        free(row.data);
        free(apps.data);
    }
    if (!store_close(s, &store, false)) {
        free(result.data);
        result = (StringBuilder){0};
    }
    return result;
}
static StringBuilder control(ShellHost *s, HttpRequest *r) {
    char *id = http_query(r, "serviceID"), *operation = http_query(r, "operation");
    bool ok = false;
    const char *message = "This operation is not supported for this backend.";
    if (id && operation && !strcmp(id, files_id)) {
        if (!strcmp(operation, "stop") || !strcmp(operation, "disable"))
            ok = stop_files(s) && set_files_enabled(s, false);
        else if (!strcmp(operation, "start") || !strcmp(operation, "enable") || !strcmp(operation, "restart"))
            ok = (strcmp(operation, "restart") || stop_files(s)) && set_files_enabled(s, true) &&
                 start_files(s);
    } else if (id && operation) {
        RegistryStore store;
        if (store_open(s, &store, false)) {
            RegistryBackendRecord *b = registry_store_find_backend(&store, id);
            char *service = b && b->owns_unit && b->unit_name[0] ? win_strdup(b->unit_name) : NULL;
            store_close(s, &store, false);
            if (service) {
                ok = service_control(s, service, operation);
                free(service);
            }
        }
    }
    if (ok) {
        message = "Completed.";
        changed(s);
    }
    free(id);
    free(operation);
    StringBuilder result = payload(28);
    payload_u32(&result, 0, ok ? 1 : 0);
    payload_text(&result, 4, message);
    return result;
}
static StringBuilder layout(ShellHost *s, HttpRequest *r, int *status) {
    bool write = !strcmp(r->method, "POST");
    if (write && !layout_document_valid(r->body.data, r->body.length)) {
        *status = 400;
        return (StringBuilder){0};
    }
    RegistryStore store;
    if (!store_open(s, &store, write))
        return (StringBuilder){0};
    RegistryResourceRecord *resource = registry_store_find_resource(&store, "outerloop.home.layout");
    StringBuilder current = {0};
    if (resource) {
        if (!layout_document_valid((const char *)resource->data, resource->data_length)) {
            store_close(s, &store, false);
            return current;
        }
        if (!sb_append_n(&current, (const char *)resource->data, resource->data_length))
            ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    } else {
        current = payload(16);
        memcpy(current.data, "OSLAY001", 8);
        if (!sb_append(&current, "{\"version\":1,\"pins\":{},\"order\":{},\"groups\":[],\"names\":{}}"))
            ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    }
    bool commit = false, ok = true;
    if (write) {
        uint64_t revision = payload_read64(current.data + 8);
        if (revision != payload_read64(r->body.data + 8))
            *status = 409;
        else if (revision == UINT64_MAX)
            ok = false;
        else {
            free(current.data);
            current = payload(r->body.length);
            memcpy(current.data, r->body.data, r->body.length);
            payload_u64(&current, 8, revision + 1);
            commit = ok = registry_store_upsert_resource(&store, "outerloop.home.layout",
                                                         (const unsigned char *)current.data, current.length);
        }
    }
    ok = store_close(s, &store, commit) && ok;
    if (!ok) {
        free(current.data);
        return (StringBuilder){0};
    }
    if (commit)
        changed(s);
    return current;
}
static bool serve_outerframe(ShellHost *s, Connection *c, const HttpRequest *r) {
    bool navigator = !strcmp(r->path, "/") || !strcmp(r->path, "/apps") ||
                     !strcmp(r->path, "/backends") || !strcmp(r->path, "/new") ||
                     !strcmp(r->path, "/backends.outer");
    if (!(navigator && (r->accepts_outerframe || !strcmp(r->path, "/backends.outer"))) &&
        strncmp(r->path, "/bundles/OuterShell.bundle", 26))
        return false;
    if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
        http_text(c, r, 405, "Unsupported method.");
        return true;
    }
    char *directory = win_join(s->directory, "bundles");
    char *arm = win_join(directory, "OuterShell.bundle.macos-arm.aar");
    char *x86 = win_join(directory, "OuterShell.bundle.macos-x86.aar");
    wchar_t *arm_wide = win_wide(arm), *x86_wide = win_wide(x86);
    WIN32_FILE_ATTRIBUTE_DATA a, x;
    bool available = GetFileAttributesExW(arm_wide, GetFileExInfoStandard, &a) &&
                     GetFileAttributesExW(x86_wide, GetFileExInfoStandard, &x) &&
                     !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                     !(x.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    free(arm_wide); free(x86_wide);
    if (!available) {
        http_text(c, r, 503, "The macOS outerframe bundles are not installed.");
    } else {
        char url[256], arm_url[272], x86_url[272];
        snprintf(url, sizeof(url), "/bundles/OuterShell.bundle-%08lx%08lx-%08lx%08lx-%08lx%08lx-%08lx%08lx",
                 a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime,
                 a.nFileSizeHigh, a.nFileSizeLow, x.ftLastWriteTime.dwHighDateTime,
                 x.ftLastWriteTime.dwLowDateTime, x.nFileSizeHigh, x.nFileSizeLow);
        snprintf(arm_url, sizeof(arm_url), "%s/macos-arm", url);
        snprintf(x86_url, sizeof(x86_url), "%s/macos-x86", url);
        if (navigator) {
            const char *json = "{\"hostPlatform\":\"windows\",\"backendsAPIPath\":\"/api/backends\",\"logsAPIPath\":\"/api/logs\",\"controlAPIPath\":\"/api/control\",\"createAPIPath\":\"/api/create\",\"recipesAPIPath\":\"/api/recipes\",\"filePickerAPIPath\":\"/api/file-picker\"}";
            StringBuilder descriptor = payload(40);
            memcpy(descriptor.data, "OUTR", 4);
            payload_u32(&descriptor, 4, 1);
            payload_u64(&descriptor, 8, 40);
            payload_u64(&descriptor, 16, strlen(url));
            payload_u64(&descriptor, 24, 40 + strlen(url));
            payload_u64(&descriptor, 32, strlen(json));
            sb_append(&descriptor, url);
            sb_append(&descriptor, json);
            http_send(c, r, 200, "application/vnd.outerframe", descriptor.data, descriptor.length);
            free(descriptor.data);
        } else if (!strcmp(r->path, url)) {
            http_text(c, r, 200, "macos-arm\nmacos-x86\n");
        } else if (!strcmp(r->path, arm_url) || !strcmp(r->path, x86_url)) {
            http_file(c, r, !strcmp(r->path, arm_url) ? arm : x86, false, false);
        } else {
            http_text(c, r, 404, "Bundle not found.");
        }
    }
    free(directory); free(arm); free(x86);
    return true;
}
static void serve_http(Connection *c, void *context) {
    ShellHost *s = context;
    HttpRequest r = {0};
    if (!http_read(c, &r) || !http_authorize(c, &r)) {
        http_free(&r);
        return;
    }
    if (serve_outerframe(s, c, &r)) {
        http_free(&r);
        return;
    }
    if (!strcmp(r.path, "/api/container-snapshot") || !strcmp(r.path, "/api/safe-spaces") || !strncmp(r.path, "/api/wsl/", 9)) {
        wsl_serve(s->wsl, c, &r);
        http_free(&r);
        return;
    }
    EnterCriticalSection(&s->gate);
    StringBuilder body = {0};
    int status = 200;
    if (!strcmp(r.path, "/api/backends"))
        body = backends(s);
    else if (!strcmp(r.path, "/api/layout"))
        body = layout(s, &r, &status);
    else if (!strcmp(r.path, "/api/events")) {
        char *value = http_query(&r, "sinceBackends");
        uint64_t previous = value ? _strtoui64(value, NULL, 10) : 0;
        free(value);
        char *overview = http_query(&r, "sinceOverview");
        uint64_t prior_overview = overview ? _strtoui64(overview, NULL, 10) : 0;
        free(overview);
        uint64_t current_overview = (s->version << 32) ^ wsl_version(s->wsl);
        ULONGLONG deadline = GetTickCount64() + 15000;
        while (previous == s->version && GetTickCount64() < deadline &&
               WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
            files_running(s);
            current_overview = (s->version << 32) ^ wsl_version(s->wsl);
            if (previous != s->version || prior_overview != current_overview)
                break;
            SleepConditionVariableCS(&s->changed, &s->gate, 250);
        }
        body = payload(32);
        payload_u32(&body, 0, (previous == s->version ? 4 : 1) |
                    (prior_overview != current_overview ? 8 : 0));
        payload_u64(&body, 8, s->version);
        payload_u64(&body, 24, current_overview);
    } else if (!strcmp(r.path, "/api/control")) {
        if (strcmp(r.method, "POST"))
            status = 405;
        else
            body = control(s, &r);
    } else if (!strcmp(r.path, "/api/recipes")) {
        body = payload(16);
        uint32_t empty = 0;
        payload_data(&body, 8, &empty, 4);
    } else {
        LeaveCriticalSection(&s->gate);
        if (!strncmp(r.path, "/api/", 5))
            http_text(c, &r, 501, "This operation is not available on Windows.");
        else
            http_static(c, &r, s->web);
        http_free(&r);
        return;
    }
    LeaveCriticalSection(&s->gate);
    if (body.data)
        http_send(c, &r, status, "application/octet-stream", body.data, body.length);
    else
        http_text(c, &r, status == 200 ? 500 : status, "The request could not be completed.");
    free(body.data);
    http_free(&r);
}
static void serve_registry(Connection *c, void *context) {
    ShellHost *s = context;
    unsigned char prefix[4];
    if (!connection_read(c, prefix, 4))
        return;
    uint32_t length = read_uint32_le(prefix);
    if (length < 2 || length > OUTERSHELL_API_MAX_FRAME_SIZE)
        return;
    unsigned char *request = win_alloc(length), *response = NULL;
    uint32_t response_length = 0;
    if (connection_read(c, request, length)) {
        EnterCriticalSection(&s->gate);
        bool ok = OuterShellRegistryRequest(s->registry, request, length, &response, &response_length) != 0;
        if (ok && response_length >= 22 && read_uint16_le(response) == 100 &&
            read_uint32_le(response + 2) == 0) {
            if (!update_allowlist(s))
                log_error(s, "Could not update the HTTP pipe allowlist.");
            changed(s);
        }
        LeaveCriticalSection(&s->gate);
        if (ok) {
            write_uint32_le(prefix, response_length);
            if (connection_write(c, prefix, 4))
                connection_write(c, response, response_length);
        }
    }
    free(request);
    free(response);
}
static bool shell_start(ShellHost *s) {
    if (!win_secure_directory(s->state))
        return false;
    RegistryStore store;
    if (!store_open(s, &store, true))
        return false;
    const char *ids[] = {shell_id, files_id}, *names[] = {"Outer Shell", "Files"};
    bool ok = true;
    for (size_t i = 0; ok && i < 2; i++) {
        char *pipe = win_pipe(ids[i]);
        char app[128];
        snprintf(app, sizeof(app), "%s:main", ids[i]);
        ok = pipe &&
             registry_store_upsert_backend(&store, ids[i], names[i], i ? "" : s->service_name, "", false) &&
             registry_store_upsert_frontend_endpoint(&store, app, ids[i], names[i], 3, 1, 0, "", 0, "/", pipe,
                                                     "", true);
        free(pipe);
    }
    RegistryResourceRecord *enabled = registry_store_find_resource(&store, "windows.files.enabled");
    bool start = !enabled || !enabled->data_length || enabled->data[0];
    ok = store_close(s, &store, ok) && ok;
    if (!ok || !update_allowlist(s))
        return false;
    s->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!s->job ||
        !SetInformationJobObject(s->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return false;
    s->wsl = wsl_start(s->stop);
    char *api = win_pipe("org.outershell.outershelld"), *http = win_pipe(shell_id);
    s->api = api ? pipe_server_start(api, serve_registry, s) : NULL;
    s->http = http ? pipe_server_start(http, serve_http, s) : NULL;
    free(api);
    free(http);
    if (!s->api || !s->http || (start && !start_files(s)))
        return false;
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%lu", GetCurrentProcessId());
    char *path = win_join(s->state, "daemon.pid");
    ok = win_atomic_write(path, pid, (size_t)n);
    free(path);
    return ok;
}
static void shell_stop(ShellHost *s) {
    SetEvent(s->stop);
    WakeAllConditionVariable(&s->changed);
    pipe_server_stop(s->http);
    pipe_server_stop(s->api);
    s->http = s->api = NULL;
    wsl_stop(s->wsl);
    s->wsl = NULL;
    EnterCriticalSection(&s->gate);
    stop_files(s);
    LeaveCriticalSection(&s->gate);
    if (s->job) {
        CloseHandle(s->job);
        s->job = NULL;
    }
}
static BOOL WINAPI console_control(DWORD event) {
    (void)event;
    SetEvent(shell.stop);
    return TRUE;
}
static void report_service(DWORD state, DWORD error) {
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwWin32ExitCode = error;
    service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    service_status.dwWaitHint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 20000 : 0;
    service_status.dwCheckPoint = service_status.dwWaitHint ? 1 : 0;
    SetServiceStatus(service_handle, &service_status);
}
static DWORD WINAPI service_control_handler(DWORD control_code, DWORD event, void *data, void *context) {
    (void)event;
    (void)data;
    (void)context;
    if (control_code == SERVICE_CONTROL_STOP || control_code == SERVICE_CONTROL_SHUTDOWN) {
        report_service(SERVICE_STOP_PENDING, 0);
        SetEvent(shell.stop);
    }
    return NO_ERROR;
}
static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
    (void)argc;
    (void)argv;
    wchar_t *name = win_wide(shell.service_name);
    service_handle = RegisterServiceCtrlHandlerExW(name, service_control_handler, NULL);
    free(name);
    if (!service_handle)
        return;
    report_service(SERVICE_START_PENDING, 0);
    EnterCriticalSection(&shell.gate);
    bool ok = shell_start(&shell);
    LeaveCriticalSection(&shell.gate);
    if (ok) {
        report_service(SERVICE_RUNNING, 0);
        WaitForSingleObject(shell.stop, INFINITE);
    }
    shell_stop(&shell);
    report_service(SERVICE_STOPPED, ok ? 0 : ERROR_SERVICE_SPECIFIC_ERROR);
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--wsl-list")) return wsl_inspect();
    bool service = false, initialize = false;
    shell.service_name = win_strdup("");
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    if (!local)
        return 1;
    shell.state = win_join(local, "OuterShell");
    free(local);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--pipe")) {
            char *pipe = win_pipe(shell_id);
            if (!pipe)
                return 1;
            puts(pipe);
            free(pipe);
            return 0;
        }
        if (!wcscmp(argv[i], L"--service")) {
            service = true;
            if (!shell.service_name[0]) {
                free(shell.service_name);
                shell.service_name = win_strdup("OuterShell");
            }
        } else if (!wcscmp(argv[i], L"--service-name") && i + 1 < argc) {
            free(shell.service_name);
            shell.service_name = win_utf8(argv[++i]);
        } else if (!wcscmp(argv[i], L"--state") && i + 1 < argc) {
            wchar_t full[32768];
            DWORD n = GetFullPathNameW(argv[++i], 32768, full, NULL);
            if (!n || n >= 32768)
                return 1;
            free(shell.state);
            shell.state = win_utf8(full);
        } else if (!wcscmp(argv[i], L"--initialize"))
            initialize = true;
        else {
            fputs("Unsupported Outer Shell option.\n", stderr);
            return 1;
        }
    }
    if (!shell.state || !shell.service_name)
        return 1;
    if (initialize) {
        char *a = win_pipe(shell_id), *b = win_pipe(files_id);
        const char *paths[] = {a, b};
        bool ok = a && b && win_allow_pipes(paths, 2);
        free(a);
        free(b);
        return ok ? 0 : 1;
    }
    shell.directory = win_executable_directory();
    if (!shell.directory)
        return 1;
    shell.web = win_join(shell.directory, "web");
    shell.registry = win_join(shell.state, "registry.orwa");
    for (char *p = shell.registry; *p; p++)
        if (*p == '\\')
            *p = '/';
    shell.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!shell.stop)
        return 1;
    InitializeCriticalSection(&shell.gate);
    InitializeConditionVariable(&shell.changed);
    shell.version = 1;
    bool ok;
    if (service) {
        wchar_t *name = win_wide(shell.service_name);
        SERVICE_TABLE_ENTRYW table[] = {{name, service_main}, {NULL, NULL}};
        ok = StartServiceCtrlDispatcherW(table) != 0;
        free(name);
    } else {
        SetConsoleCtrlHandler(console_control, TRUE);
        EnterCriticalSection(&shell.gate);
        ok = shell_start(&shell);
        LeaveCriticalSection(&shell.gate);
        if (ok) {
            puts("Outer Shell is listening.");
            WaitForSingleObject(shell.stop, INFINITE);
        } else
            log_error(&shell, "Outer Shell could not start.");
        shell_stop(&shell);
    }
    DeleteCriticalSection(&shell.gate);
    CloseHandle(shell.stop);
    free(shell.state);
    free(shell.registry);
    free(shell.directory);
    free(shell.web);
    free(shell.service_name);
    return ok ? 0 : 1;
}
