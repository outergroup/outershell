#include "Wsl.h"
#include "../Backend/OuterShellLayout.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSL_LIMIT (4u * 1024u * 1024u)
#define WSL_BRIDGE "/usr/local/bin/outer-socket-bridge"
typedef struct {
    HANDLE process, cancel;
    Connection input, output;
} Child;
typedef enum { RUNTIME_WSL, RUNTIME_WSLC, RUNTIME_DOCKER, RUNTIME_TAR } ContainerRuntime;
typedef struct Endpoint Endpoint;
typedef struct Distribution Distribution;
struct Endpoint {
    Endpoint *next;
    Distribution *distribution;
    char *socket, *pipe;
    PipeServer *server;
    bool enabled, system;
};
struct Distribution {
    Distribution *next;
    struct Wsl *owner;
    char *id, *name, *user;
    DWORD uid;
    bool present, running, managed, building;
    HANDLE build_thread;
    char *build_error;
    ContainerRuntime container;
    unsigned cpus, memory_gb;
    char *runtime_id;
    StringBuilder apps;
    Child keepalive;
};
struct Wsl {
    HANDLE stop, refresh, thread, job;
    CRITICAL_SECTION gate, commands;
    CONDITION_VARIABLE ready;
    Distribution *distributions;
    Endpoint *endpoints;
    size_t endpoint_count;
    bool inspect;
    bool containers_available[3];
    LONG active_relays;
    StringBuilder snapshot;
    uint64_t version;
    ULONGLONG last_read;
};

static void append(StringBuilder *b, const char *text) {
    if (!sb_append(b, text))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
static void bytes(StringBuilder *b, const void *text, size_t length) {
    if (!sb_append_n(b, text, length))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
static void json_string(StringBuilder *b, const char *text) {
    append(b, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char escape[7];
        if (*p == '"' || *p == '\\') {
            escape[0] = '\\';
            escape[1] = (char)*p;
            escape[2] = 0;
            append(b, escape);
        } else if (*p < 32) {
            snprintf(escape, sizeof(escape), "\\u%04x", *p);
            append(b, escape);
        } else
            bytes(b, p, 1);
    }
    append(b, "\"");
}
static void field(StringBuilder *b, const char *name, const char *value) {
    json_string(b, name);
    append(b, ":");
    json_string(b, value);
}
static void json_base64(StringBuilder *json, StringBuilder data) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    append(json, "\"");
    for (size_t i = 0; i < data.length; i += 3) {
        unsigned a = (unsigned char)data.data[i];
        unsigned b = i + 1 < data.length ? (unsigned char)data.data[i + 1] : 0;
        unsigned c = i + 2 < data.length ? (unsigned char)data.data[i + 2] : 0;
        char encoded[] = {alphabet[a >> 2], alphabet[((a & 3) << 4) | (b >> 4)],
                          i + 1 < data.length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=',
                          i + 2 < data.length ? alphabet[c & 63] : '='};
        bytes(json, encoded, sizeof(encoded));
    }
    append(json, "\"");
}
static void quote_argument(StringBuilder *b, const char *value) {
    if (*value && !strpbrk(value, " \t\r\n\"")) {
        append(b, " ");
        append(b, value);
        return;
    }
    append(b, " \"");
    size_t slashes = 0;
    for (const char *p = value;; p++) {
        if (*p == '\\') {
            slashes++;
            continue;
        }
        size_t n = slashes * ((*p == '"' || !*p) ? 2 : 1);
        while (n--)
            append(b, "\\");
        slashes = 0;
        if (!*p)
            break;
        if (*p == '"')
            append(b, "\\");
        bytes(b, p, 1);
    }
    append(b, "\"");
}
static bool child_pipe(HANDLE *parent, HANDLE *child, bool input) {
    static LONG sequence;
    wchar_t name[160];
    swprintf(name, 160, L"\\\\.\\pipe\\outer-wsl-io-%lu-%ld", GetCurrentProcessId(),
             InterlockedIncrement(&sequence));
    *parent = CreateNamedPipeW(name,
                               (input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
                                   FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                               65536, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) {
        *parent = NULL;
        return false;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    *child =
        CreateFileW(name, input ? GENERIC_READ : GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (*child == INVALID_HANDLE_VALUE) {
        *child = NULL;
        CloseHandle(*parent);
        *parent = NULL;
        return false;
    }
    return true;
}
static void child_close(Child *p) {
    if (p->cancel)
        SetEvent(p->cancel);
    if (p->input.pipe)
        CloseHandle(p->input.pipe);
    if (p->output.pipe)
        CloseHandle(p->output.pipe);
    if (p->process) {
        if (WaitForSingleObject(p->process, 1000) != WAIT_OBJECT_0) {
            TerminateProcess(p->process, 1);
            WaitForSingleObject(p->process, 5000);
        }
        CloseHandle(p->process);
    }
    if (p->cancel)
        CloseHandle(p->cancel);
    memset(p, 0, sizeof(*p));
}
static bool child_start_runtime(Wsl *w, const char *const *arguments, Child *p, ContainerRuntime container) {
    wchar_t system[32768];
    UINT n = GetSystemDirectoryW(system, 32768);
    if (!n || n + 20 >= 32768)
        return false;
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64 && sizeof(void *) == 4) {
        n = GetWindowsDirectoryW(system, 32768);
        if (!n || n + 20 >= 32768)
            return false;
        wcscat_s(system, 32768, L"\\Sysnative\\wsl.exe");
    } else
        wcscat_s(system, 32768, L"\\wsl.exe");
    if (container == RUNTIME_WSLC) {
        DWORD length = GetEnvironmentVariableW(L"ProgramW6432", system, 32768);
        if (!length)
            length = GetEnvironmentVariableW(L"ProgramFiles", system, 32768);
        if (!length || length + 32 >= 32768)
            return false;
        wcscat_s(system, 32768, L"\\WSL\\wslc.exe");
    }
    if (container == RUNTIME_DOCKER) {
        wchar_t *local = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)))
            return false;
        int written = swprintf_s(system, 32768, L"%s\\Programs\\DockerDesktop\\resources\\bin\\docker.exe", local);
        CoTaskMemFree(local);
        if (written < 0) return false;
        if (GetFileAttributesW(system) == INVALID_FILE_ATTRIBUTES) {
            DWORD length = GetEnvironmentVariableW(L"ProgramW6432", system, 32768);
            if (!length) length = GetEnvironmentVariableW(L"ProgramFiles", system, 32768);
            if (!length || length + 60 >= 32768) return false;
            wcscat_s(system, 32768, L"\\Docker\\Docker\\resources\\bin\\docker.exe");
        }
    }
    if (container == RUNTIME_TAR) {
        UINT length = GetSystemDirectoryW(system, 32768);
        if (!length || length + 10 >= 32768) return false;
        wcscat_s(system, 32768, L"\\tar.exe");
    }
    char *exe = win_utf8(system);
    if (!exe)
        return false;
    StringBuilder command = {0};
    quote_argument(&command, exe);
    free(exe);
    for (size_t i = 0; arguments[i]; i++)
        quote_argument(&command, arguments[i]);
    wchar_t *wide = win_wide(command.data + 1);
    free(command.data);
    HANDLE input = NULL, output = NULL, error = NULL;
    bool ok = wide && child_pipe(&p->input.pipe, &input, true) &&
              child_pipe(&p->output.pipe, &output, false);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (ok)
        error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, NULL);
    if (ok && container) {
        char *local = win_folder(CSIDL_LOCAL_APPDATA);
        char *root = local ? win_join(local, "OuterShell") : NULL;
        char *path = root ? win_join(root, "container-runtime.log") : NULL;
        wchar_t *log_path = path ? win_wide(path) : NULL;
        HANDLE log = log_path ? CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (log != INVALID_HANDLE_VALUE) {
            if (error && error != INVALID_HANDLE_VALUE)
                CloseHandle(error);
            error = log;
        }
        free(log_path);
        free(path);
        free(root);
        free(local);
    }
    HANDLE inherited_error = NULL;
    HANDLE standard_error = GetStdHandle(STD_ERROR_HANDLE);
    if (standard_error && standard_error != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), standard_error, GetCurrentProcess(), &inherited_error,
                        0, TRUE, DUPLICATE_SAME_ACCESS)) {
        if (error && error != INVALID_HANDLE_VALUE)
            CloseHandle(error);
        error = inherited_error;
    }
    if (ok && container && !strcmp(arguments[0], "build")) {
        HANDLE build_error = NULL;
        if (DuplicateHandle(GetCurrentProcess(), output, GetCurrentProcess(), &build_error, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            if (error && error != INVALID_HANDLE_VALUE) CloseHandle(error);
            error = build_error;
        } else ok = false;
    }
    ok = ok && error != INVALID_HANDLE_VALUE && error != NULL;
    p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = ok && p->cancel;
    p->input.stop = p->output.stop = w->stop;
    p->input.cancel = p->output.cancel = p->cancel;
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = error;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    startup.lpAttributeList = win_alloc(size);
    bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) != 0;
    HANDLE inherited[] = {input, output, error};
    ok = ok && initialized &&
         UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), NULL, NULL);
    PROCESS_INFORMATION process = {0};
    if (ok)
        ok = CreateProcessW(system, wide, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                            NULL, NULL, &startup.StartupInfo, &process) != 0;
    if (ok) {
        p->process = process.hProcess;
        ok = AssignProcessToJobObject(w->job, process.hProcess) &&
             ResumeThread(process.hThread) != (DWORD)-1;
        CloseHandle(process.hThread);
    }
    if (initialized)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    free(wide);
    if (input)
        CloseHandle(input);
    if (output)
        CloseHandle(output);
    if (error && error != INVALID_HANDLE_VALUE)
        CloseHandle(error);
    if (!ok) {
        fprintf(stderr, "WSL process start failed: %lu\n", GetLastError());
        child_close(p);
    }
    return ok;
}
static bool child_start(Wsl *w, const char *const *arguments, Child *p) {
    return child_start_runtime(w, arguments, p, false);
}
static bool capture_runtime(Wsl *w, const char *const *arguments, StringBuilder *result, ContainerRuntime container) {
    Child p = {0};
    if (!child_start_runtime(w, arguments, &p, container))
        return false;
    ULONGLONG deadline = GetTickCount64() +
        ((container == RUNTIME_TAR || (container && (!strcmp(arguments[0], "build") || !strcmp(arguments[0], "run")))) ? 600000 : 30000);
    bool ok = true;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(p.output.pipe, NULL, 0, NULL, &available, NULL))
            break;
        if (available) {
            char buffer[8192];
            size_t count = available > sizeof(buffer) ? sizeof(buffer) : available;
            if (result->length + count > WSL_LIMIT ||
                !connection_read_some(&p.output, buffer, count, &count)) {
                ok = false;
                break;
            }
            bytes(result, buffer, count);
        } else if (WaitForSingleObject(w->stop, 10) == WAIT_OBJECT_0 ||
                   GetTickCount64() > deadline) {
            ok = false;
            break;
        } else if (WaitForSingleObject(p.process, 0) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(p.output.pipe, NULL, 0, NULL, &available, NULL) || !available)
                break;
        }
    }
    DWORD exit_code = 1;
    ok = ok && WaitForSingleObject(p.process, 5000) == WAIT_OBJECT_0 &&
         GetExitCodeProcess(p.process, &exit_code) && exit_code == 0;
    if (!ok)
        fprintf(stderr, "WSL command failed: exit %lu, output %zu bytes\n", exit_code,
                result->length);
    child_close(&p);
    return ok;
}
static bool capture(Wsl *w, const char *const *arguments, StringBuilder *result) {
    return capture_runtime(w, arguments, result, false);
}
static bool send_frame(Child *p, uint16_t type, const void *data, size_t length) {
    unsigned char header[16];
    write_uint32_le(header, 0x3142524f);
    write_uint16_le(header + 4, 1);
    write_uint16_le(header + 6, type);
    write_uint32_le(header + 8, 1);
    write_uint32_le(header + 12, (uint32_t)length);
    return connection_write(&p->input, header, sizeof(header)) &&
           (!length || connection_write(&p->input, data, length));
}
static bool receive_frame(Child *p, uint16_t *type, StringBuilder *data) {
    unsigned char header[16];
    if (!connection_read(&p->output, header, sizeof(header)) ||
        read_uint32_le(header) != 0x3142524f || read_uint16_le(header + 4) != 1)
        return false;
    uint32_t length = read_uint32_le(header + 12), stream = read_uint32_le(header + 8);
    *type = read_uint16_le(header + 6);
    if (length > 1024 * 1024 || stream > 1 ||
        ((*type == 3 || *type == 4 || *type == 5 || *type == 6 || *type == 7) && stream != 1))
        return false;
    data->length = 0;
    if (!sb_reserve(data, length + 1))
        return false;
    if (length && !connection_read(&p->output, data->data, length))
        return false;
    data->length = length;
    data->data[length] = 0;
    return true;
}
static bool bridge_start(Distribution *d, const char *socket, bool system, Child *p) {
    const char *args[12] = {"--distribution", d->name};
    size_t count = 2;
    if (system) {
        args[count++] = "--user";
        args[count++] = "root";
    }
    args[count++] = "--exec";
    args[count++] = WSL_BRIDGE;
    args[count++] = "bridge";
    args[count++] = "--socket";
    args[count++] = socket;
    if (d->container) {
        EnterCriticalSection(&d->owner->gate);
        char *runtime_id = win_strdup(d->runtime_id);
        LeaveCriticalSection(&d->owner->gate);
        const char *container_args[] = {"exec", "--interactive", "--user", "0",
            "--env", "HOME=/root", "--env", "XDG_RUNTIME_DIR=/run/user/0",
            runtime_id, WSL_BRIDGE, "bridge", "--socket", socket, NULL};
        bool started = child_start_runtime(d->owner, container_args, p, d->container);
        free(runtime_id);
        if (!started) return false;
    } else if (!child_start(d->owner, args, p))
        return false;
    StringBuilder data = {0};
    uint16_t type = 0;
    bool ok = receive_frame(p, &type, &data) && type == 1 && send_frame(p, 2, NULL, 0);
    while (ok && receive_frame(p, &type, &data)) {
        if (type == 10)
            continue;
        break;
    }
    ok = ok && type == 3;
    free(data.data);
    if (!ok)
        child_close(p);
    return ok;
}
static bool guest_http(Distribution *d, const char *method, const char *target,
                       StringBuilder *body) {
    char socket[160];
    snprintf(socket, sizeof(socket),
             d->uid ? "/run/user/%lu/org.outershell.OuterShell" : "/run/org.outershell.OuterShell",
             d->uid);
    Child p = {0};
    if (!bridge_start(d, socket, d->uid == 0, &p))
        return false;
    StringBuilder request = {0}, response = {0}, frame = {0};
    append(&request, method);
    append(&request, " ");
    append(&request, target);
    append(&request, " HTTP/1.1\r\nHost: localhost\r\nConnection: "
                     "close\r\nContent-Length: 0\r\n\r\n");
    bool ok = send_frame(&p, 5, request.data, request.length);
    uint16_t type = 0;
    while (ok && receive_frame(&p, &type, &frame)) {
        if (type == 6 || type == 7)
            break;
        if (type == 10)
            continue;
        if (type != 5 || response.length + frame.length > WSL_LIMIT) {
            ok = false;
            break;
        }
        bytes(&response, frame.data, frame.length);
    }
    ok = ok && (type == 6 || type == 7) && response.data &&
         (!strncmp(response.data, "HTTP/1.1 200 ", 13) ||
          !strncmp(response.data, "HTTP/1.0 200 ", 13));
    char *start = ok ? strstr(response.data, "\r\n\r\n") : NULL;
    if (start)
        bytes(body, start + 4, response.length - (size_t)(start + 4 - response.data));
    else
        ok = false;
    free(request.data);
    free(response.data);
    free(frame.data);
    child_close(&p);
    return ok;
}
static bool serve_guest_backends(Endpoint *endpoint, Connection *client);
typedef struct {
    Child *process;
    Connection *client;
} Relay;
static DWORD WINAPI relay_output(void *context) {
    Relay *relay = context;
    StringBuilder frame = {0};
    uint16_t type;
    while (receive_frame(relay->process, &type, &frame)) {
        if (type == 10)
            continue;
        if (type != 5 || !connection_write(relay->client, frame.data, frame.length))
            break;
    }
    free(frame.data);
    SetEvent(relay->process->cancel);
    return 0;
}
static void relay_client_impl(Connection *client, void *context) {
    Endpoint *endpoint = context;
    Wsl *w = endpoint->distribution->owner;
    EnterCriticalSection(&w->gate);
    bool enabled = endpoint->enabled && endpoint->distribution->present &&
                   (!endpoint->distribution->container || endpoint->distribution->running);
    bool system = endpoint->system;
    LeaveCriticalSection(&w->gate);
    if (!enabled || (!endpoint->distribution->container && serve_guest_backends(endpoint, client)))
        return;
    Child p = {0};
    if (!bridge_start(endpoint->distribution, endpoint->socket, system, &p))
        return;
    client->cancel = p.cancel;
    Relay relay = {&p, client};
    HANDLE output = CreateThread(NULL, 0, relay_output, &relay, 0, NULL);
    if (output) {
        char buffer[32768];
        size_t length;
        while (connection_read_some(client, buffer, sizeof(buffer), &length)) {
            if (!send_frame(&p, 5, buffer, length))
                break;
        }
        if (WaitForSingleObject(p.cancel, 0) != WAIT_OBJECT_0) {
            send_frame(&p, 6, NULL, 0);
            WaitForSingleObject(output, 1000);
        }
        SetEvent(p.cancel);
        WaitForSingleObject(output, INFINITE);
        CloseHandle(output);
    }
    client->cancel = NULL;
    child_close(&p);
}
static void relay_client(Connection *client, void *context) {
    Endpoint *endpoint = context;
    Wsl *w = endpoint->distribution->owner;
    if (InterlockedIncrement(&w->active_relays) <= 64)
        relay_client_impl(client, context);
    InterlockedDecrement(&w->active_relays);
}
static Endpoint *publish(Distribution *d, const char *socket, bool system) {
    Wsl *w = d->owner;
    for (Endpoint *e = w->endpoints; e; e = e->next)
        if (e->distribution == d && !strcmp(e->socket, socket)) {
            e->enabled = true;
            e->system = system;
            return e;
        }
    if (w->endpoint_count >= 256 || socket[0] != '/' || strlen(socket) >= 108)
        return NULL;
    unsigned char digest[32];
    StringBuilder identity = {0};
    bytes(&identity, d->id, strlen(d->id) + 1);
    bytes(&identity, &d->uid, sizeof(d->uid));
    bytes(&identity, socket, strlen(socket));
    BCRYPT_ALG_HANDLE algorithm = NULL;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
              BCryptHash(algorithm, NULL, 0, (PUCHAR)identity.data, (ULONG)identity.length, digest,
                         sizeof(digest)) == 0;
    free(identity.data);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        return NULL;
    char app[80], hash[33];
    for (size_t i = 0; i < 16; i++)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    snprintf(app, sizeof(app), "%s.%s", d->container == RUNTIME_DOCKER ? "docker" : d->container ? "wslc" : "wsl", hash);
    Endpoint *e = win_alloc(sizeof(*e));
    e->distribution = d;
    e->socket = win_strdup(socket);
    e->system = system;
    e->pipe = win_pipe(app);
    e->server = e->pipe && !w->inspect ? pipe_server_start(e->pipe, relay_client, e) : NULL;
    const char *paths[] = {e->pipe};
    StringBuilder task = {0};
    char *sid = win_sid();
    append(&task, "\\OuterShell-");
    if (sid) append(&task, sid);
    bool registered = w->inspect || (e->pipe && e->server && sid &&
        win_allow_pipes(paths, 1) && win_set_pipe_activation(e->pipe, task.data));
    free(sid);
    free(task.data);
    if (!e->pipe || !registered) {
        pipe_server_stop(e->server);
        free(e->socket);
        free(e->pipe);
        free(e);
        return NULL;
    }
    e->enabled = true;
    e->next = w->endpoints;
    w->endpoints = e;
    w->endpoint_count++;
    return e;
}
static StringBuilder reference(StringBuilder parent, size_t offset) {
    if (offset > parent.length || parent.length - offset < 8)
        return (StringBuilder){0};
    size_t start = read_uint32_le((unsigned char *)parent.data + offset);
    size_t length = read_uint32_le((unsigned char *)parent.data + offset + 4);
    if (start > parent.length || length > parent.length - start)
        return (StringBuilder){0};
    return (StringBuilder){parent.data + start, length, 0};
}
static char *string_reference(StringBuilder parent, size_t offset) {
    StringBuilder ref = reference(parent, offset);
    if (!ref.data || memchr(ref.data, 0, ref.length))
        return win_strdup("");
    char *text = win_alloc(ref.length + 1);
    memcpy(text, ref.data, ref.length);
    return text;
}
static StringBuilder copy_payload(StringBuilder source) {
    StringBuilder copy = payload(source.length);
    if (source.length)
        memcpy(copy.data, source.data, source.length);
    return copy;
}
static bool rewrite_backends(Distribution *d, StringBuilder *response) {
    if (response->length < 12)
        return false;
    uint32_t count = read_uint32_le((unsigned char *)response->data + 8);
    if (count > 256 || response->length < 12 + (size_t)count * 8)
        return false;
    StringBuilder result = copy_payload(*response);
    EnterCriticalSection(&d->owner->gate);
    for (uint32_t i = 0; i < count; i++) {
        StringBuilder source = reference(*response, 12 + i * 8);
        if (source.length < 84)
            continue;
        StringBuilder backend = copy_payload(source), apps = reference(source, 68);
        uint32_t n = apps.length >= 4 ? read_uint32_le((unsigned char *)apps.data) : 0;
        if (n > 256 || apps.length < 4 + (size_t)n * 8) {
            free(backend.data);
            continue;
        }
        char *scope = string_reference(source, 32);
        bool system = !strcmp(scope, "system");
        free(scope);
        StringBuilder updated = copy_payload(apps);
        for (uint32_t j = 0; j < n; j++) {
            StringBuilder original = reference(apps, 4 + j * 8);
            if (original.length < 64)
                continue;
            char *socket = string_reference(original, 16);
            Endpoint *endpoint = socket[0] == '/' ? publish(d, socket, system) : NULL;
            if (endpoint) {
                StringBuilder app = copy_payload(original);
                char *url = string_reference(original, 8);
                if (!strncmp(url, socket, strlen(socket)))
                    payload_text(&app, 8, url[strlen(socket)] ? url + strlen(socket) : "/");
                payload_text(&app, 16, endpoint->pipe);
                payload_data(&updated, 4 + j * 8, app.data, app.length);
                free(app.data);
                free(url);
            }
            free(socket);
        }
        payload_data(&backend, 68, updated.data, updated.length);
        payload_data(&result, 12 + i * 8, backend.data, backend.length);
        free(updated.data);
        free(backend.data);
    }
    LeaveCriticalSection(&d->owner->gate);
    free(response->data);
    *response = result;
    return true;
}
static bool container_ui(Distribution *d, uint16_t route, const char *form, StringBuilder *body) {
    Child p = {0};
    if (!bridge_start(d, "/run/user/0/outershelld-api", true, &p))
        return false;
    size_t form_length = strlen(form);
    StringBuilder message = payload(28 + form_length);
    unsigned char *b = (unsigned char *)message.data;
    write_uint32_le(b, (uint32_t)(24 + form_length));
    write_uint16_le(b + 4, 26);
    write_uint16_le(b + 6, route);
    write_uint32_le(b + 12, 24);
    write_uint32_le(b + 20, 24);
    write_uint32_le(b + 24, (uint32_t)form_length);
    memcpy(b + 28, form, form_length);
    bool ok = send_frame(&p, 5, b, message.length);
    StringBuilder response = {0}, frame = {0};
    uint16_t type;
    size_t expected = 0;
    while (ok && receive_frame(&p, &type, &frame)) {
        if (type == 10)
            continue;
        if (type != 5 || response.length + frame.length > WSL_LIMIT) {
            ok = false;
            break;
        }
        bytes(&response, frame.data, frame.length);
        if (response.length >= 4) {
            expected = (size_t)read_uint32_le((unsigned char *)response.data) + 4;
            if (expected < 28 || expected > WSL_LIMIT) { ok = false; break; }
            if (response.length >= expected)
                break;
        }
    }
    ok = ok && expected && response.length == expected;
    if (ok) {
        StringBuilder packet = {response.data + 4, response.length - 4, 0};
        ok = read_uint16_le((unsigned char *)packet.data) == 107 &&
             read_uint32_le((unsigned char *)packet.data + 2) == 200;
        StringBuilder value = reference(packet, 16);
        ok = ok && value.data;
        if (ok)
            bytes(body, value.data, value.length);
    }
    free(message.data);
    free(response.data);
    free(frame.data);
    child_close(&p);
    return ok;
}
static bool control_app(Distribution *d, const char *service, const char *verb, bool system) {
    StringBuilder response = {0};
    if (d->container) {
        StringBuilder form = {0};
        append(&form, "serviceID=");
        append(&form, service);
        append(&form, "&scope=user&operation=");
        append(&form, verb);
        bool ok = container_ui(d, 3, form.data, &response) && response.length >= 12 &&
                  (read_uint32_le((unsigned char *)response.data) & 1);
        free(form.data);
        free(response.data);
        return ok;
    }
    if (!guest_http(d, "GET", "/api/backends", &response) || response.length < 12) {
        free(response.data);
        return false;
    }
    uint32_t count = read_uint32_le((unsigned char *)response.data + 8);
    char *unit = NULL;
    if (count <= 256 && response.length >= 12 + (size_t)count * 8) {
        for (uint32_t i = 0; i < count; i++) {
            StringBuilder backend = reference(response, 12 + i * 8);
            char *id = string_reference(backend, 0), *scope = string_reference(backend, 32);
            bool match = !strcmp(id, service) && !strcmp(scope, system ? "system" : "user");
            free(id);
            free(scope);
            if (match) {
                unit = string_reference(backend, 16);
                break;
            }
        }
    }
    free(response.data);
    if (!unit)
        return false;
    size_t length = strlen(unit);
    bool valid = length > 8 && length < 256 && !strcmp(unit + length - 8, ".service");
    for (const char *p = unit; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
            valid = false;
    bool ok = false;
    if (valid) {
        char socket[256];
        snprintf(socket, sizeof(socket), "%.*s.socket", (int)(length - 8), unit);
        const char *args[14] = {"--distribution", d->name};
        size_t count = 2;
        if (system) {
            args[count++] = "--user";
            args[count++] = "root";
        }
        args[count++] = "--exec";
        args[count++] = "/usr/bin/systemctl";
        args[count++] = system ? "--system" : "--user";
        args[count++] = verb;
        args[count++] = unit;
        if (!strcmp(verb, "start"))
            args[count] = socket;
        StringBuilder output = {0};
        ok = capture(d->owner, args, &output);
        if (!ok && !strcmp(verb, "start")) {
            args[count] = NULL;
            output.length = 0;
            ok = capture(d->owner, args, &output);
        }
        free(output.data);
    }
    free(unit);
    return ok;
}

static bool serve_guest_backends(Endpoint *endpoint, Connection *client) {
    char shell_socket[160];
    Distribution *d = endpoint->distribution;
    snprintf(shell_socket, sizeof(shell_socket),
             d->uid ? "/run/user/%lu/org.outershell.OuterShell" : "/run/org.outershell.OuterShell",
             d->uid);
    if (strcmp(endpoint->socket, shell_socket))
        return false;
    ULONGLONG deadline = GetTickCount64() + 10000;
    char prefix[8193];
    for (;;) {
        DWORD count = 0;
        if (!PeekNamedPipe(client->pipe, prefix, sizeof(prefix) - 1, &count, NULL, NULL))
            return true;
        prefix[count] = 0;
        if (strchr(prefix, '\n') || count == sizeof(prefix) - 1)
            break;
        if (WaitForSingleObject(client->stop, 10) == WAIT_OBJECT_0 || GetTickCount64() > deadline)
            return true;
    }
    const char *target = "GET /api/backends";
    size_t length = strlen(target);
    if (strncmp(prefix, target, length) || (prefix[length] != ' ' && prefix[length] != '?'))
        return false;
    HttpRequest request = {0};
    if (http_read(client, &request) && http_authorize(client, &request)) {
        StringBuilder body = {0};
        if (guest_http(d, "GET", "/api/backends", &body) && rewrite_backends(d, &body))
            http_send(client, &request, 200, "application/octet-stream", body.data, body.length);
        else
            http_text(client, &request, 502, "The WSL app registry is unavailable.");
        free(body.data);
    }
    http_free(&request);
    return true;
}
static bool app_list(Distribution *d, StringBuilder *json) {
    StringBuilder response = {0};
    if (!(d->container ? container_ui(d, 1, "", &response) : guest_http(d, "GET", "/api/backends", &response)) || response.length < 12 ||
        reference(response, 0).length) {
        free(response.data);
        return false;
    }
    uint32_t count = read_uint32_le((unsigned char *)response.data + 8);
    if (count > 256 || response.length < 12 + (size_t)count * 8) {
        free(response.data);
        return false;
    }
    EnterCriticalSection(&d->owner->gate);
    for (Endpoint *e = d->owner->endpoints; e; e = e->next)
        if (e->distribution == d)
            e->enabled = false;
    append(json, "[");
    size_t added = 0;
    for (uint32_t i = 0; i < count; i++) {
        StringBuilder backend = reference(response, 12 + i * 8);
        if (backend.length < 84)
            continue;
        char *scope = string_reference(backend, 32);
        bool system = !strcmp(scope, "system");
        free(scope);
        char *service = string_reference(backend, 0);
        StringBuilder apps = reference(backend, 68);
        uint32_t n = apps.length >= 4 ? read_uint32_le((unsigned char *)apps.data) : 0;
        if (n > 256 || apps.length < 4 + (size_t)n * 8)
            n = 0;
        for (uint32_t j = 0; j < n; j++) {
            StringBuilder app = reference(apps, 4 + j * 8);
            if (app.length < 64)
                continue;
            char *socket = string_reference(app, 16), *name = string_reference(app, 0),
                 *url = string_reference(app, 8), *id = string_reference(app, 52);
            Endpoint *endpoint = socket[0] == '/' ? publish(d, socket, system) : NULL;
            if (endpoint) {
                if (added++)
                    append(json, ",");
                append(json, "{");
                field(json, "serviceID", service);
                append(json, ",");
                field(json, "frontendID", id);
                append(json, ",");
                field(json, "displayName", name);
                append(json, ",");
                field(json, "socketPath", socket);
                append(json, ",");
                field(json, "externalSocketPath", endpoint->pipe);
                append(json, ",");
                field(json, "url", url);
                append(json, ",");
                field(json, "userName", system ? "root" : d->user);
                append(json, ",");
                field(json, "serviceScope", system ? "system" : "user");
                StringBuilder icon = reference(app, 32);
                if (icon.length && icon.length <= 512 * 1024 &&
                    !(read_uint32_le((unsigned char *)app.data + 60) & 2)) {
                    append(json, ",\"iconData\":");
                    json_base64(json, icon);
                }
                append(json, ",\"iconPath\":\"\",\"iconObservationToken\":\"\",\"publishedPort\":0,\"isRunning\":");
                append(json, (read_uint32_le((unsigned char *)app.data + 60) & 1)
                                 ? "true"
                                 : "false");
                append(json, "}");
            }
            free(socket);
            free(name);
            free(url);
            free(id);
        }
        free(service);
    }
    append(json, "]");
    LeaveCriticalSection(&d->owner->gate);
    free(response.data);
    return true;
}
static char *registry_string(HKEY key, const wchar_t *name) {
    DWORD bytes_count = 0;
    if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &bytes_count) != ERROR_SUCCESS ||
        bytes_count > 65536)
        return NULL;
    wchar_t *wide = win_alloc(bytes_count + sizeof(wchar_t));
    char *value = NULL;
    if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, wide, &bytes_count) == ERROR_SUCCESS)
        value = win_utf8(wide);
    free(wide);
    return value;
}
static bool contains_line(const char *list, const char *name) {
    size_t length = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *end = strpbrk(p, "\r\n");
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && !memcmp(p, name, n))
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}
#include "WslContainers.inc"
#include "ContainerTransfer.inc"

static void discover_impl(Wsl *w) {
    StringBuilder running = {0};
    const char *args[] = {"--list", "--running", "--quiet", NULL};
    if (!capture(w, args, &running)) {
        fprintf(stderr, "WSL list failed: %lu (%zu bytes)\n", GetLastError(), running.length);
        free(running.data);
        running = (StringBuilder){0};
    }
    char *running_names;
    if (running.length >= 2 && running.data[1] == 0) {
        bytes(&running, "\0\0", 2);
        running_names = win_utf8((wchar_t *)running.data);
    } else
        running_names = win_strdup(running.data ? running.data : "");
    free(running.data);
    if (!running_names)
        return;
    HKEY root = NULL;
    LSTATUS registry_status =
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0,
                      KEY_READ, &root);
    if (registry_status != ERROR_SUCCESS && registry_status != ERROR_FILE_NOT_FOUND) {
        free(running_names);
        return;
    }
    EnterCriticalSection(&w->gate);
    for (Distribution *d = w->distributions; d; d = d->next)
        if (!d->container)
            d->present = false;
    LeaveCriticalSection(&w->gate);
    for (DWORD index = 0; root && index < 64; index++) {
        wchar_t key_name[80];
        DWORD length = 80;
        if (RegEnumKeyExW(root, index, key_name, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        HKEY key;
        if (RegOpenKeyExW(root, key_name, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        char *name = registry_string(key, L"DistributionName"), *id = win_utf8(key_name);
        DWORD uid = 0, size = sizeof(uid);
        bool valid = name && strncmp(name, "docker-desktop", 14) && id && strlen(id) == 38 && id[0] == '{' && id[37] == '}' &&
                     RegGetValueW(key, NULL, L"DefaultUid", RRF_RT_REG_DWORD, NULL, &uid, &size) ==
                         ERROR_SUCCESS;
        RegCloseKey(key);
        if (!valid) {
            free(name);
            free(id);
            continue;
        }
        id[37] = 0;
        memmove(id, id + 1, 37);
        EnterCriticalSection(&w->gate);
        Distribution *d;
        for (d = w->distributions; d; d = d->next)
            if (!strcmp(d->id, id) && !strcmp(d->name, name) && d->uid == uid)
                break;
        if (!d) {
            d = win_alloc(sizeof(*d));
            d->owner = w;
            d->id = win_strdup(id);
            d->name = win_strdup(name);
            d->user = win_strdup("workspace");
            d->uid = uid;
            d->next = w->distributions;
            w->distributions = d;
        }
        d->present = true;
        d->running = contains_line(running_names, name);
        LeaveCriticalSection(&w->gate);
        if (d->running) {
            StringBuilder user = {0};
            const char *identity[] = {"--distribution", d->name, "--exec",
                                      "/usr/bin/id",    "-un",   NULL};
            if (capture(w, identity, &user) && user.length) {
                user.data[strcspn(user.data, "\r\n")] = 0;
                free(d->user);
                d->user = win_strdup(user.data);
            }
            free(user.data);
            StringBuilder apps = {0};
            if (app_list(d, &apps)) {
                free(d->apps.data);
                d->apps = apps;
            } else
                free(apps.data);
        }
        free(name);
        free(id);
    }
    if (root)
        RegCloseKey(root);
    free(running_names);
    discover_containers(w, RUNTIME_WSLC);
    discover_containers(w, RUNTIME_DOCKER);
    publish_snapshot(w);
}
static void publish_snapshot(Wsl *w) {
    StringBuilder snapshot = {0};
    append(&snapshot, "{\"hostPlatform\":\"windows\",\"providers\":[");
    container_provider(w, &snapshot, RUNTIME_WSLC);
    append(&snapshot, ",");
    container_provider(w, &snapshot, RUNTIME_DOCKER);
    append(&snapshot, "],\"workspaces\":[");
    size_t count = 0;
    EnterCriticalSection(&w->gate);
    for (Distribution *d = w->distributions; d; d = d->next) {
        if (!d->present)
            continue;
        if (count++)
            append(&snapshot, ",");
        if (d->container) {
            container_snapshot(d, &snapshot);
            continue;
        }
        append(&snapshot, "{");
        field(&snapshot, "id", d->id);
        append(&snapshot, ",");
        field(&snapshot, "name", d->name);
        append(&snapshot, ",");
        field(&snapshot, "userName", d->user);
        append(&snapshot, ",");
        field(&snapshot, "state", d->running ? "running" : "stopped");
        append(&snapshot, ",\"runtimeKind\":\"wsl\",\"managementKind\":\"attached\","
                          "\"ownsContainer\":false,\"cpus\":0,\"memoryInGB\":0,\"shellCommand\":\"\","
                          "\"runtime\":{\"providerID\":\"wsl\",\"providerName\":\"WSL\","
                          "\"operatingSystemName\":\"Linux\",\"operatingSystemVersion\":\"\","
                          "\"architecture\":\"\",\"isolationKind\":\"wsl\",\"isolationName\":\"\"},"
                          "\"capabilities\":{\"supportsApps\":true,\"supportsShell\":false,"
                          "\"supportsRecipes\":false,\"supportsMounts\":false,\"supportsLiveMounts\":false},"
                          "\"outerShellSupport\":{");
        field(&snapshot, "status", d->apps.data ? "available" : "unknown");
        append(&snapshot, ",");
        field(&snapshot, "detail",
              d->apps.data ? ""
              : d->running ? "Outer Shell support is not responding in this distribution."
                           : "Start this distribution to discover its apps.");
        append(&snapshot, "},\"apps\":");
        append(&snapshot, d->apps.data ? d->apps.data : "[]");
        append(&snapshot, ",\"commands\":[],\"mounts\":[],\"persistentData\":[]}");
    }
    append(&snapshot, "]}");
    if (!w->snapshot.data || strcmp(w->snapshot.data, snapshot.data)) {
        free(w->snapshot.data);
        w->snapshot = snapshot;
        w->version++;
    } else
        free(snapshot.data);
    WakeAllConditionVariable(&w->ready);
    LeaveCriticalSection(&w->gate);
}
static void discover(Wsl *w) {
    EnterCriticalSection(&w->commands);
    discover_impl(w);
    LeaveCriticalSection(&w->commands);
}
static DWORD WINAPI discovery_thread(void *context) {
    Wsl *w = context;
    HANDLE events[] = {w->stop, w->refresh};
    while (WaitForSingleObject(w->stop, 0) != WAIT_OBJECT_0) {
        EnterCriticalSection(&w->gate);
        bool active = GetTickCount64() - w->last_read < 60000;
        LeaveCriticalSection(&w->gate);
        if (active)
            discover(w);
        if (WaitForMultipleObjects(2, events, FALSE, active ? 5000 : INFINITE) == WAIT_OBJECT_0)
            break;
    }
    return 0;
}
Wsl *wsl_start(HANDLE stop) {
    Wsl *w = win_alloc(sizeof(*w));
    w->stop = stop;
    w->version = 1;
    w->last_read = GetTickCount64() - 60000;
    InitializeCriticalSection(&w->gate);
    InitializeCriticalSection(&w->commands);
    InitializeConditionVariable(&w->ready);
    w->refresh = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (!w->refresh || !w->job ||
        !SetInformationJobObject(w->job, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        wsl_stop(w);
        return NULL;
    }
    w->thread = CreateThread(NULL, 0, discovery_thread, w, 0, NULL);
    if (!w->thread) {
        wsl_stop(w);
        return NULL;
    }
    return w;
}
void wsl_stop(Wsl *w) {
    if (!w)
        return;
    if (w->thread) {
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    }
    while (w->endpoints) {
        Endpoint *e = w->endpoints;
        w->endpoints = e->next;
        pipe_server_stop(e->server);
        free(e->socket);
        free(e->pipe);
        free(e);
    }
    while (w->distributions) {
        Distribution *d = w->distributions;
        w->distributions = d->next;
        if (d->build_thread) { WaitForSingleObject(d->build_thread, INFINITE); CloseHandle(d->build_thread); }
        free(d->build_error);
        child_close(&d->keepalive);
        free(d->runtime_id);
        free(d->id);
        free(d->name);
        free(d->user);
        free(d->apps.data);
        free(d);
    }
    if (w->job)
        CloseHandle(w->job);
    if (w->refresh)
        CloseHandle(w->refresh);
    DeleteCriticalSection(&w->gate);
    DeleteCriticalSection(&w->commands);
    free(w->snapshot.data);
    free(w);
}
uint64_t wsl_version(Wsl *w) {
    if (!w)
        return 0;
    EnterCriticalSection(&w->gate);
    bool stale = GetTickCount64() - w->last_read >= 60000;
    w->last_read = GetTickCount64();
    uint64_t version = w->version;
    if (stale || !w->snapshot.data)
        SetEvent(w->refresh);
    LeaveCriticalSection(&w->gate);
    return version;
}
static char *request_string(const HttpRequest *request, const char *name) {
    const unsigned char *cursor = (const unsigned char *)request->body.data;
    const unsigned char *end = cursor + request->body.length;
    layout_json_space(&cursor, end);
    if (cursor == end || *cursor++ != '{')
        return NULL;
    while (cursor < end) {
        layout_json_space(&cursor, end);
        const unsigned char *key = cursor;
        if (!layout_json_string(&cursor, end))
            return NULL;
        size_t length = (size_t)(cursor - key);
        bool match = length == strlen(name) + 2 && !memcmp(key + 1, name, length - 2);
        layout_json_space(&cursor, end);
        if (cursor == end || *cursor++ != ':')
            return NULL;
        layout_json_space(&cursor, end);
        const unsigned char *start = cursor;
        if (!layout_json_value(&cursor, end, 0))
            return NULL;
        if (match) {
            if (*start != '"')
                return NULL;
            StringBuilder result = {0};
            for (const unsigned char *p = start + 1; p < cursor - 1; p++) {
                unsigned char c = *p;
                if (c == '\\') {
                    c = *++p;
                    if (c == 'u') {
                        unsigned value = 0;
                        for (unsigned i = 0; i < 4; i++) {
                            c = *++p;
                            value =
                                value * 16 + (c <= '9' ? c - '0' : (unsigned)tolower(c) - 'a' + 10);
                        }
                        wchar_t wide[2] = {(wchar_t)value, 0};
                        int units = 1;
                        if (value >= 0xd800 && value <= 0xdbff && p + 6 < cursor - 1 && p[1] == '\\' && p[2] == 'u') {
                            unsigned low = 0; p += 2;
                            for (unsigned i = 0; i < 4; i++) { unsigned char digit = *++p; low = low * 16 + (digit <= '9' ? digit - '0' : (unsigned)tolower(digit) - 'a' + 10); }
                            if (low < 0xdc00 || low > 0xdfff) { free(result.data); return NULL; }
                            wide[1] = (wchar_t)low; units = 2;
                        }
                        char utf8[8];
                        int length = value ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, units, utf8, sizeof(utf8), NULL, NULL) : 0;
                        if (!length) { free(result.data); return NULL; }
                        bytes(&result, utf8, (size_t)length);
                        continue;
                    } else if (c == 'n') c = '\n';
                    else if (c == 'r') c = '\r';
                    else if (c == 't') c = '\t';
                    else if (c == 'b') c = '\b';
                    else if (c == 'f') c = '\f';
                    else if (c != '"' && c != '\\' && c != '/') {
                        free(result.data);
                        return NULL;
                    }
                }
                bytes(&result, &c, 1);
            }
            if (!result.data)
                append(&result, "");
            return result.data;
        }
        layout_json_space(&cursor, end);
        if (cursor == end || *cursor++ != ',')
            break;
    }
    return NULL;
}
static void send_snapshot(Wsl *w, Connection *c, HttpRequest *r) {
    wsl_version(w);
    EnterCriticalSection(&w->gate);
    const char *json = w->snapshot.data ? w->snapshot.data : "{\"hostPlatform\":\"windows\",\"providers\":[],\"workspaces\":[]}";
    char *copy = win_strdup(json);
    LeaveCriticalSection(&w->gate);
    char *request_id = r->body.length ? request_string(r, "requestID") : NULL;
    if (request_id) {
        StringBuilder response = {0};
        append(&response, "{");
        field(&response, "requestID", request_id);
        append(&response, ",");
        append(&response, copy + 1);
        http_send(c, r, 200, "application/json", response.data, response.length);
        free(response.data);
    } else {
        http_send(c, r, 200, "application/json", copy, strlen(copy));
    }
    free(request_id);
    free(copy);
}
static void serve_workspace_api(Wsl *w, Connection *c, HttpRequest *r) {
    const char *id = r->path + strlen("/api/wsl/");
    const char *route = strchr(id, '/');
    if (!route || route - id != 36 ||
        (strcmp(route, "/backends") && strcmp(route, "/control") &&
         strcmp(route, "/recipes") && strcmp(route, "/create") &&
         strcmp(route, "/file-picker") && strcmp(route, "/native-app-projects"))) {
        http_text(c, r, 404, "Unknown Linux API.");
        return;
    }
    bool read = !strcmp(route, "/backends") || !strcmp(route, "/recipes") || !strcmp(route, "/file-picker");
    if (strcmp(r->method, read ? "GET" : "POST")) {
        http_text(c, r, 405, "Unsupported method.");
        return;
    }
    EnterCriticalSection(&w->gate);
    Distribution *d;
    for (d = w->distributions; d; d = d->next)
        if (d->present && !_strnicmp(d->id, id, 36))
            break;
    bool running = d && d->running;
    LeaveCriticalSection(&w->gate);
    if (!d) {
        http_text(c, r, 409, "The Linux distribution is no longer registered.");
        return;
    }
    if (!running) {
        EnterCriticalSection(&w->commands);
        if (d->keepalive.process && WaitForSingleObject(d->keepalive.process, 0) == WAIT_OBJECT_0)
            child_close(&d->keepalive);
        const char *keepalive[] = {"--distribution", d->name, "--exec", "/bin/cat", NULL};
        const char *start[] = {"--distribution", d->name, "--exec", "/bin/true", NULL};
        StringBuilder output = {0};
        bool started = (d->keepalive.process || child_start(w, keepalive, &d->keepalive)) &&
                       capture(w, start, &output);
        free(output.data);
        LeaveCriticalSection(&w->commands);
        if (!started) {
            http_text(c, r, 502, "Could not start the Linux distribution.");
            return;
        }
        SetEvent(w->refresh);
    }
    char socket[160];
    snprintf(socket, sizeof(socket), d->uid ? "/run/user/%lu/org.outershell.OuterShell" : "/run/org.outershell.OuterShell", d->uid);
    Child child = {0};
    if (!bridge_start(d, socket, d->uid == 0, &child)) {
        http_text(c, r, 502, "Could not connect to the Linux app manager.");
        return;
    }
    StringBuilder request = {0}, frame = {0};
    append(&request, r->method);
    append(&request, " /api");
    append(&request, route);
    if (r->query && *r->query) {
        append(&request, "?");
        append(&request, r->query);
    }
    append(&request, " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: ");
    append(&request, !strcmp(route, "/native-app-projects")
                         ? "application/vnd.outershell.native-app-project"
                         : "application/x-www-form-urlencoded");
    char length[80];
    snprintf(length, sizeof(length), "\r\nContent-Length: %zu\r\n\r\n", r->body.length);
    append(&request, length);
    bool ok = send_frame(&child, 5, request.data, request.length);
    for (size_t offset = 0; ok && offset < r->body.length; offset += 32768) {
        size_t remaining = r->body.length - offset;
        ok = send_frame(&child, 5, r->body.data + offset, remaining < 32768 ? remaining : 32768);
    }
    uint16_t type = 0;
    while (ok && receive_frame(&child, &type, &frame)) {
        if (type == 6 || type == 7)
            break;
        if (type == 10)
            continue;
        if (type != 5)
            break;
        ok = connection_write(c, frame.data, frame.length);
    }
    free(request.data);
    free(frame.data);
    child_close(&child);
    if (!read)
        SetEvent(w->refresh);
}

void wsl_serve(Wsl *w, Connection *c, HttpRequest *r) {
    if (!w) {
        http_text(c, r, 503, "WSL discovery is unavailable.");
        return;
    }
    if (!strncmp(r->path, "/api/container-transfers/", strlen("/api/container-transfers/"))) {
        container_http_upload(w, c, r);
        return;
    }
    if (!strncmp(r->path, "/api/wsl/", 9)) {
        serve_workspace_api(w, c, r);
        return;
    }
    if (!strcmp(r->path, "/api/container-snapshot")) {
        send_snapshot(w, c, r);
        return;
    }
    if (r->body.length >= 4 && !memcmp(r->body.data, "OSCT", 4) && !strcmp(r->method, "POST")) {
        container_transfer(w, c, r);
        return;
    }
    const char *error = "This container operation is not supported.";
    char *operation = request_string(r, "operation"), *id = request_string(r, "workspaceID");
    bool ok = false, locked = false;
    if (!operation || strcmp(r->method, "POST"))
        goto done;
    if (!strcmp(operation, "list") || !strcmp(operation, "checkRuntimes")) {
        ok = true;
        goto done;
    }
    EnterCriticalSection(&w->commands);
    locked = true;
    if (!strcmp(operation, "beginImport") || !strcmp(operation, "cancelImport") || !strcmp(operation, "finishImport")) {
        container_transfer_operation(w, c, r, operation);
        LeaveCriticalSection(&w->commands); free(operation); free(id); return;
    }
    if (!strcmp(operation, "create")) {
        ok = create_container(w, r, &error);
        goto done;
    }
    EnterCriticalSection(&w->gate);
    Distribution *d;
    for (d = w->distributions; d; d = d->next)
        if (id && d->present && !_stricmp(d->id, id))
            break;
    LeaveCriticalSection(&w->gate);
    if (!d) {
        error = "The Linux environment is no longer registered.";
        goto done;
    }
    if (d->container && !strcmp(operation, "prepareShare")) {
        StringBuilder extra = {0};
        ok = container_share(d, r, &extra);
        if (ok) container_extra_response(w, c, r, extra.data);
        else http_text(c, r, 400, "Could not share the container. Linked folders and files cannot be included.");
        free(extra.data); LeaveCriticalSection(&w->commands); free(operation); free(id); return;
    }
    if (d->container && !strcmp(operation, "appLogs")) {
        container_logs(d, c, r);
        LeaveCriticalSection(&w->commands);
        free(operation);
        free(id);
        return;
    }
    if (d->container && (!strcmp(operation, "updateDockerfile") || !strcmp(operation, "updateContainerConfiguration"))) {
        ok = container_definition_update(d, r, !strcmp(operation, "updateContainerConfiguration"));
        error = "Could not save the definition. Check the Dockerfile, folder paths, environment, and ports.";
        if (ok) publish_snapshot(w);
    } else if (d->container && !strcmp(operation, "rebuildRecipe")) {
        ok = container_rebuild(d);
        error = "A rebuild is already running or this container is not managed by Outer Shell.";
    } else if (d->container && !strcmp(operation, "rename")) {
        char *name = request_string(r, "name");
        if (d->managed && valid_container_name(name)) {
            ok = save_container_metadata(d->id, name, d->cpus, d->memory_gb);
        }
        free(name);
        error = "The container name could not be saved.";
    } else if (d->container && !strcmp(operation, "delete")) {
        if (d->managed) {
            StringBuilder output = {0};
            const char *args[] = {d->container == RUNTIME_DOCKER ? "rm" : "remove", "--force", d->runtime_id, NULL};
            ok = !*d->runtime_id || capture_runtime(w, args, &output, d->container);
            free(output.data);
            if (ok) {
                EnterCriticalSection(&w->gate);
                for (Endpoint *endpoint = w->endpoints; endpoint; endpoint = endpoint->next) {
                    if (endpoint->distribution != d) continue;
                    endpoint->enabled = false;
                    win_set_pipe_activation(endpoint->pipe, NULL);
                    win_disallow_pipe(endpoint->pipe);
                }
                LeaveCriticalSection(&w->gate);
                char *path = container_metadata_path(d->id);
                wchar_t *wide = path ? win_wide(path) : NULL;
                if (wide) DeleteFileW(wide);
                free(wide);
                free(path);
                d->present = false;
            }
        }
        error = "Only containers created by Outer Shell can be deleted here.";
    } else if (d->container && (!strcmp(operation, "start") || !strcmp(operation, "stop") ||
                         !strcmp(operation, "restart"))) {
        StringBuilder output = {0};
        const char *args[] = {operation, d->runtime_id, NULL};
        ok = capture_runtime(w, args, &output, d->container);
        free(output.data);
        error = "The container runtime could not change the container state. See the Outer Shell log for details.";
    } else if (!strcmp(operation, "start") || !strcmp(operation, "stop")) {
        StringBuilder output = {0};
        const char *start[] = {"--distribution", d->name, "--exec", "/bin/true", NULL};
        const char *stop[] = {"--terminate", d->name, NULL};
        if (!strcmp(operation, "start")) {
            if (d->keepalive.process &&
                WaitForSingleObject(d->keepalive.process, 0) == WAIT_OBJECT_0)
                child_close(&d->keepalive);
            const char *keepalive[] = {"--distribution", d->name, "--exec", "/bin/cat", NULL};
            ok = (d->keepalive.process || child_start(w, keepalive, &d->keepalive)) &&
                 capture(w, start, &output);
            if (!ok)
                child_close(&d->keepalive);
        } else {
            child_close(&d->keepalive);
            ok = capture(w, stop, &output);
        }
        free(output.data);
        error = "WSL could not change the distribution state.";
    } else if (!strcmp(operation, "startApp") || !strcmp(operation, "stopApp") ||
               !strcmp(operation, "restartApp")) {
        char *scope = request_string(r, "serviceScope");
        char *service = request_string(r, "serviceID");
        bool valid = service && service[0] && strlen(service) < 256;
        if (service)
            for (const char *p = service; *p; p++)
                if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
                    valid = false;
        if (valid) {
            const char *verb = !strcmp(operation, "startApp")  ? "start"
                               : !strcmp(operation, "stopApp") ? "stop"
                                                               : "restart";
            ok = control_app(d, service, verb, scope && !strcmp(scope, "system"));
        }
        free(scope);
        free(service);
        error = "The Linux app could not complete this operation.";
    }
done:
    if (locked)
        LeaveCriticalSection(&w->commands);
    free(operation);
    free(id);
    if (ok) {
        SetEvent(w->refresh);
        send_snapshot(w, c, r);
    } else {
        StringBuilder json = {0};
        append(&json, "{");
        field(&json, "error", error);
        append(&json, "}");
        http_send(c, r, 400, "application/json", json.data, json.length);
        free(json.data);
    }
}

int wsl_inspect(void) {
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop)
        return 1;
    Wsl *w = wsl_start(stop);
    if (!w) {
        CloseHandle(stop);
        return 1;
    }
    w->inspect = true;
    discover(w);
    puts(w->snapshot.data ? w->snapshot.data : "WSL discovery failed.");
    int result = w->snapshot.data ? 0 : 1;
    SetEvent(stop);
    wsl_stop(w);
    CloseHandle(stop);
    return result;
}
