#include "AppProcess.h"
#include "CommandTasks.h"
#include "../Resources/ContainerHelper.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void append(StringBuilder *b, const char *text) {
    if (!sb_append(b, text))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
static void bytes(StringBuilder *b, const void *data, size_t size) {
    if (!sb_append_n(b, data, size))
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
}
wchar_t *app_runtime_executable(const char *runtime) {
    wchar_t system[32768];
    UINT n = GetSystemDirectoryW(system, 32768);
    if (!n || n + 20 >= 32768)
        return NULL;
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64 &&
        sizeof(void *) == 4) {
        n = GetWindowsDirectoryW(system, 32768);
        if (!n || n + 20 >= 32768)
            return NULL;
        wcscat_s(system, 32768, L"\\Sysnative\\wsl.exe");
    } else
        wcscat_s(system, 32768, L"\\wsl.exe");
    if (!strcmp(runtime, "wslc")) {
        DWORD length = GetEnvironmentVariableW(L"ProgramW6432", system, 32768);
        if (!length)
            length = GetEnvironmentVariableW(L"ProgramFiles", system, 32768);
        if (!length || length + 32 >= 32768)
            return NULL;
        wcscat_s(system, 32768, L"\\WSL\\wslc.exe");
    }
    if (!strcmp(runtime, "docker")) {
        wchar_t *local = NULL;
        if (FAILED(
                SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)))
            return NULL;
        int written = swprintf_s(
            system, 32768,
            L"%s\\Programs\\DockerDesktop\\resources\\bin\\docker.exe", local);
        CoTaskMemFree(local);
        if (written < 0)
            return NULL;
        if (GetFileAttributesW(system) == INVALID_FILE_ATTRIBUTES) {
            DWORD length =
                GetEnvironmentVariableW(L"ProgramW6432", system, 32768);
            if (!length)
                length =
                    GetEnvironmentVariableW(L"ProgramFiles", system, 32768);
            if (!length || length + 60 >= 32768)
                return NULL;
            wcscat_s(system, 32768,
                     L"\\Docker\\Docker\\resources\\bin\\docker.exe");
        }
    }
    if (!strcmp(runtime, "tar")) {
        UINT length = GetSystemDirectoryW(system, 32768);
        if (!length || length + 10 >= 32768)
            return NULL;
        wcscat_s(system, 32768, L"\\tar.exe");
    }
    return _wcsdup(system);
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
    swprintf(name, 160, L"\\\\.\\pipe\\outer-wsl-io-%lu-%ld",
             GetCurrentProcessId(), InterlockedIncrement(&sequence));
    *parent = CreateNamedPipeW(
        name,
        (input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
            FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
        65536, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) {
        *parent = NULL;
        return false;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    *child = CreateFileW(name, input ? GENERIC_READ : GENERIC_WRITE, 0, &sa,
                         OPEN_EXISTING, 0, NULL);
    if (*child == INVALID_HANDLE_VALUE) {
        *child = NULL;
        CloseHandle(*parent);
        *parent = NULL;
        return false;
    }
    return true;
}
void app_process_close(AppProcess *p) {
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
bool app_process_start(HANDLE stop, HANDLE job, const char *runtime,
                       const char *const *arguments, AppProcess *p) {
    wchar_t *system = app_runtime_executable(runtime);
    if (!system)
        return false;
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
        error = CreateFileW(L"NUL", GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, NULL);
    ok = ok && error != INVALID_HANDLE_VALUE && error != NULL;
    p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = ok && p->cancel;
    p->input.stop = p->output.stop = stop;
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
    bool initialized = InitializeProcThreadAttributeList(
                           startup.lpAttributeList, 1, 0, &size) != 0;
    HANDLE inherited[] = {input, output, error};
    ok = ok && initialized &&
         UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), NULL, NULL);
    PROCESS_INFORMATION process = {0};
    if (ok)
        ok = CreateProcessW(system, wide, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                                CREATE_SUSPENDED,
                            NULL, NULL, &startup.StartupInfo, &process) != 0;
    if (ok) {
        p->process = process.hProcess;
        ok = AssignProcessToJobObject(job, process.hProcess) &&
             ResumeThread(process.hThread) != (DWORD)-1;
        CloseHandle(process.hThread);
    }
    if (initialized)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    free(wide);
    free(system);
    if (input)
        CloseHandle(input);
    if (output)
        CloseHandle(output);
    if (error && error != INVALID_HANDLE_VALUE)
        CloseHandle(error);
    if (!ok) {
        fprintf(stderr, "Container helper start failed: %lu\n", GetLastError());
        app_process_close(p);
    }
    return ok;
}

HANDLE app_open_registry(HANDLE stop) {
    char *pipe = win_pipe("org.outershell.outershelld");
    wchar_t *wide = pipe ? win_wide(pipe) : NULL; free(pipe);
    if (!wide) return INVALID_HANDLE_VALUE;
    bool started = false; ULONGLONG deadline = GetTickCount64() + 10000;
    HANDLE result = INVALID_HANDLE_VALUE;
    do {
        result = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (result != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) break;
        if (!started && error == ERROR_FILE_NOT_FOUND) {
            char *sid = win_sid(); StringBuilder task = {0};
            sb_append(&task, "\\OuterShell-"); if (sid) sb_append(&task, sid);
            HRESULT hr = command_task(task.data, "start", NULL, NULL); free(sid); free(task.data);
            if (FAILED(hr)) break; started = true;
        }
        if (WaitForSingleObject(stop, 50) != WAIT_TIMEOUT) break;
    } while (GetTickCount64() < deadline);
    free(wide); return result;
}
static ContainerHelper app_helper;
static bool app_helper_loaded;
static SRWLOCK app_helper_lock = SRWLOCK_INIT;
bool app_helper_start(HANDLE stop, HANDLE job, const char *runtime,
                      const char **args, size_t count, size_t capacity,
                      const char *id, const char *name,
                      const char *const *helper_args, AppProcess *p,
                      char *error, size_t error_capacity) {
    AcquireSRWLockExclusive(&app_helper_lock);
    if (!app_helper_loaded) {
        char *directory = win_executable_directory();
        char *helpers = directory ? win_join(directory, "helpers") : NULL;
        app_helper_loaded =
            helpers && container_helper_load(&app_helper, id, name, helpers);
        free(directory);
        free(helpers);
    }
    bool loaded = app_helper_loaded;
    ReleaseSRWLockExclusive(&app_helper_lock);
    if (!loaded) {
        snprintf(error, error_capacity,
                 "This app's container helpers are missing from its "
                 "installation.");
        return false;
    }
    if (!container_helper_command(&app_helper, args, count, capacity,
                                  helper_args) ||
        !app_process_start(stop, job, runtime, args, p)) {
        snprintf(error, error_capacity,
                 "Could not start the container helper (Windows error %lu).",
                 GetLastError());
        return false;
    }
    char line[512], message[512];
    size_t used = 0;
    for (;;) {
        if (used + 1 == sizeof(line) ||
            !connection_read(&p->output, line + used, 1)) {
            app_process_close(p);
            snprintf(error, error_capacity,
                     "The container helper could not start.");
            return false;
        }
        if (line[used] == '\n')
            break;
        used++;
    }
    line[used] = 0;
    int architecture = 0;
    int reply = container_helper_reply(line, &architecture, message,
                                       sizeof(message));
    if (reply == 2 &&
        !connection_write(&p->input, app_helper.data[architecture],
                          app_helper.length[architecture])) {
        reply = 0;
        snprintf(message, sizeof(message),
                 "Could not copy the helper into the container.");
    }
    if (!reply) {
        app_process_close(p);
        snprintf(error, error_capacity, "%s", message);
        return false;
    }
    return true;
}
