#include "CommandTasks.h"

struct CommandApp {
    struct CommandApp *next;
    char *id;
    bool owns_allowlist_entry;
    char *task_name;
    DWORD task_state;
    ULONGLONG queried_at;
    HANDLE process, job;
};

static CommandApp *command_find(ShellHost *s, const char *id) {
    for (CommandApp *a = s->commands; a; a = a->next)
        if (!strcmp(a->id, id)) return a;
    return NULL;
}
static char *command_path(ShellHost *s, const char *id, const char *filename) {
    char *root = win_join(s->state, "commands"), *dir = win_join(root, id);
    free(root);
    if (!filename) return dir;
    char *path = win_join(dir, filename);
    free(dir);
    return path;
}
static bool command_stop(CommandApp *a) {
    if (a->job) {
        if (!TerminateJobObject(a->job, 0)) return false;
        CloseHandle(a->job);
        a->job = NULL;
    }
    if (a->process) {
        WaitForSingleObject(a->process, 5000);
        CloseHandle(a->process);
        a->process = NULL;
    }
    return true;
}
static bool command_process_running(ShellHost *s, CommandApp *a) {
    if (!a->process) return false;
    if (WaitForSingleObject(a->process, 0) == WAIT_TIMEOUT) return true;
    command_stop(a);
    changed(s);
    return false;
}
static bool command_spawn(ShellHost *s, CommandApp *a) {
    if (command_process_running(s, a)) return true;
    wchar_t system[MAX_PATH];
    UINT n = GetSystemDirectoryW(system, MAX_PATH);
    if (!n || n + 9 >= MAX_PATH) return false;
    wcscat_s(system, MAX_PATH, L"\\cmd.exe");
    char *script = command_path(s, a->id, "run.cmd"), *log = command_path(s, a->id, "output.log");
    wchar_t *script_w = win_wide(script), *log_w = win_wide(log);
    char *home = win_folder(CSIDL_PROFILE);
    wchar_t *cwd = home ? win_wide(home) : NULL;
    free(home); free(script); free(log);
    if (!script_w || !log_w || !cwd) { free(script_w); free(log_w); free(cwd); return false; }
    size_t size = wcslen(script_w) + MAX_PATH + 32;
    wchar_t *line = win_alloc(size * sizeof(wchar_t));
    swprintf(line, size, L"\"%s\" /d /s /c \"\"%s\"\"", system, script_w);
    free(script_w);
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE output = CreateFileW(log_w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(log_w);
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    startup.lpAttributeList = win_alloc(bytes);
    bool attributes = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != 0;
    HANDLE handles[] = {input, output};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    a->job = CreateJobObjectW(NULL, NULL);
    PROCESS_INFORMATION process = {0};
    bool ok = input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE && attributes && a->job &&
        SetInformationJobObject(a->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL) &&
        CreateProcessW(system, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, cwd, &startup.StartupInfo, &process);
    if (ok) {
        ok = AssignProcessToJobObject(a->job, process.hProcess) && ResumeThread(process.hThread) != (DWORD)-1;
        if (ok) a->process = process.hProcess;
        else { TerminateProcess(process.hProcess, 1); CloseHandle(process.hProcess); }
        CloseHandle(process.hThread);
    }
    if (attributes) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(line); free(cwd);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!ok) command_stop(a);
    changed(s);
    return ok;
}
static void command_key(char *key, size_t size, const char *id) {
    snprintf(key, size, "windows.command.%s", id);
}
static char *command_task_name(const char *id) {
    char *sid = win_sid();
    if (!sid) return NULL;
    StringBuilder name = {0}; sb_append(&name, "\\OuterShell-App-"); sb_append(&name, sid); sb_append(&name, "-"); sb_append(&name, id);
    free(sid); return name.data;
}
static bool command_running(ShellHost *s, CommandApp *a) {
    ULONGLONG now = GetTickCount64();
    if (!a->queried_at || now - a->queried_at >= 1000) {
        DWORD state = 0;
        command_task(a->task_name, "query", NULL, &state);
        if (state != a->task_state) { a->task_state = state; changed(s); }
        a->queried_at = now;
    }
    return a->task_state == 4;
}
static void command_xml_text(StringBuilder *xml, const char *text) {
    for (; *text; text++) {
        switch (*text) {
        case '&': sb_append(xml, "&amp;"); break;
        case '<': sb_append(xml, "&lt;"); break;
        case '>': sb_append(xml, "&gt;"); break;
        case '"': sb_append(xml, "&quot;"); break;
        default: sb_append_n(xml, text, 1); break;
        }
    }
}
static HRESULT command_register(ShellHost *s, CommandApp *a) {
    char *runner = command_path(s, a->id, "runner.exe");
    wchar_t executable[32768];
    wchar_t *target = win_wide(runner);
    DWORD length = GetModuleFileNameW(NULL, executable, 32768);
    if (!target || !length || length >= 32768 || !CopyFileW(executable, target, TRUE)) {
        DWORD error = GetLastError(); free(runner); free(target); return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
    }
    free(target);
    char *sid = win_sid(), *home = win_folder(CSIDL_PROFILE);
    if (!sid || !home) { free(sid); free(home); free(runner); return E_FAIL; }
    StringBuilder xml = {0};
    sb_append(&xml, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Principals><Principal id=\"Author\"><UserId>");
    command_xml_text(&xml, sid);
    sb_append(&xml, "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>");
    sb_append(&xml, "LeastPrivilege");
    sb_append(&xml, "</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled><RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings><Actions Context=\"Author\"><Exec><Command>");
    command_xml_text(&xml, runner);
    sb_append(&xml, "</Command><Arguments>--run-command "); command_xml_text(&xml, a->id);
    sb_append(&xml, " --state &quot;"); command_xml_text(&xml, s->state); sb_append(&xml, "&quot;</Arguments><WorkingDirectory>");
    command_xml_text(&xml, home); sb_append(&xml, "</WorkingDirectory></Exec></Actions></Task>");
    HRESULT hr = command_task(a->task_name, "register", xml.data, NULL);
    free(runner); free(sid); free(home); free(xml.data); return hr;
}
static bool commands_load(ShellHost *s) {
    RegistryStore store;
    if (!store_open(s, &store, false)) return false;
    for (size_t i = 0; i < store.backend_count; i++) {
        const char *id = store.backends[i].service_id;
        if (strpbrk(id, "/\\:") || strlen(id) > 160) continue;
        char key[256]; command_key(key, sizeof(key), id);
        RegistryResourceRecord *r = registry_store_find_resource(&store, key);
        if (!r || r->data_length != 1) continue;
        CommandApp *a = win_alloc(sizeof(*a));
        a->owns_allowlist_entry = (r->data[0] & 2) != 0;
        a->id = win_strdup(id); a->task_name = command_task_name(id); a->next = s->commands; s->commands = a;

    }
    return store_close(s, &store, false);
}
static bool command_wait_stopped(ShellHost *s, CommandApp *a) {
    char *path = command_path(s, a->id, "runner.lock"); wchar_t *wide = win_wide(path); free(path);
    if (!wide) return false;
    ULONGLONG deadline = GetTickCount64() + 10000;
    bool ok = false;
    do {
        HANDLE file = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); ok = true; break; }
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { ok = true; break; }
        if (error != ERROR_SHARING_VIOLATION) break;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    free(wide); return ok;
}
static bool command_delete_file(const char *path) {
    wchar_t *wide = win_wide(path);
    if (!wide) return false;
    ULONGLONG deadline = GetTickCount64() + 5000;
    bool ok = false;
    do {
        if (DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND) { ok = true; break; }
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    free(wide); return ok;
}
static bool command_control(ShellHost *s, CommandApp *a, const char *operation, char *error, size_t error_size) {
    snprintf(error, error_size, "Could not update the app registry or activation mapping.");
    if (strcmp(operation, "start") && strcmp(operation, "stop") && strcmp(operation, "restart") && strcmp(operation, "uninstall")) return false;
    bool restart = !strcmp(operation, "restart");
    HRESULT hr = command_task(a->task_name, restart ? "stop" : operation, NULL, NULL);
    if (SUCCEEDED(hr) && strcmp(operation, "start") && !command_wait_stopped(s, a)) { snprintf(error, error_size, "The command runner did not stop in time."); return false; }
    if (SUCCEEDED(hr) && restart) hr = command_task(a->task_name, "start", NULL, NULL);
    a->queried_at = 0;
    bool removed = !strcmp(operation, "uninstall");
    if (FAILED(hr) && !(removed && hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) { snprintf(error, error_size, "Windows could not %s the task (0x%08lx).", operation, (unsigned long)hr); return false; }
    if (!removed) return true;
    RegistryStore store;
    if (!store_open(s, &store, true)) return false;
    char key[256]; command_key(key, sizeof(key), a->id);
    bool allow_removed = true;
    for (size_t i = 0; i < store.frontend_count; i++)
        if (!strcmp(store.frontends[i].service_id, a->id) && store.frontends[i].endpoint_kind == 3)
            allow_removed = win_set_pipe_activation(store.frontends[i].socket_path, NULL) && allow_removed;
    if (a->owns_allowlist_entry) {
        for (size_t i = 0; i < store.frontend_count; i++)
            if (!strcmp(store.frontends[i].service_id, a->id) && store.frontends[i].endpoint_kind == 3)
                allow_removed = win_disallow_pipe(store.frontends[i].socket_path) && allow_removed;
    }
    if (!allow_removed) { store_close(s, &store, false); return false; }
    bool ok = registry_store_remove_backend_and_owned_records(&store, a->id) && registry_store_remove_resource(&store, key);
    if (!store_close(s, &store, ok) || !ok) return false;
    const char *files[] = {"run.cmd", "output.log", "runner.exe", "runner.lock"};
    char *dir = command_path(s, a->id, NULL); wchar_t *wide = win_wide(dir);
    bool cleaned = false;
    ULONGLONG cleanup_deadline = GetTickCount64() + 10000;
    do {
        for (size_t i = 0; i < 4; i++) {
            char *path = command_path(s, a->id, files[i]);
            if (!command_delete_file(path)) snprintf(error, error_size, "Could not delete %s (Windows error %lu).", files[i], (unsigned long)GetLastError());
            free(path);
        }
        if (wide && (RemoveDirectoryW(wide) || GetLastError() == ERROR_PATH_NOT_FOUND)) { cleaned = true; break; }
        DWORD code = GetLastError();
        snprintf(error, error_size, "Could not remove the command directory (Windows error %lu).", (unsigned long)code);
        if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED) break;
        Sleep(25);
    } while (GetTickCount64() < cleanup_deadline);
    free(wide); free(dir);
    for (CommandApp **p = &s->commands; *p; p = &(*p)->next) if (*p == a) { *p = a->next; break; }
    free(a->id); free(a->task_name); free(a);
    if (!cleaned) { log_error(s, error); return false; }
    return update_allowlist(s);
}
static char *command_parameter(HttpRequest *r, const char *key) {
    char *value = http_form(r, key);
    return value ? value : http_query(r, key);
}
static StringBuilder command_create(ShellHost *s, HttpRequest *r) {
    char *name = command_parameter(r, "name"), *id = command_parameter(r, "identifier"),
         *text = command_parameter(r, "command"), *pipe = command_parameter(r, "socketPath"),
         *port_text = command_parameter(r, "port"),
         *transport = command_parameter(r, "frontendTransport"), *scope = command_parameter(r, "scope");
    bool ok = false;
    const char *message = "Enter a name, commands, and a port or local named pipe path.";
    char task_error[180];
    char service[200] = "", key[256] = "", app_id[220] = "";
    char *dir = NULL, *script = NULL;
    bool named_pipe = transport && !strcmp(transport, "namedPipe");
    unsigned long port = 0;
    if (scope && strcmp(scope, "user")) { message = "Windows commands currently run as the Outer Shell user, not as administrator."; goto done; }
    if (!name || !*name || strlen(name) > 256 || !text || !*text || strlen(text) > 256 * 1024 || !id || !*id || strlen(id) > 100) goto done;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) { message = "Use lowercase letters, numbers, and hyphens for the ID."; goto done; }
    if (named_pipe) {
        if (!pipe || _strnicmp(pipe, "\\\\.\\pipe\\", 9) || !pipe[9] || strlen(pipe) > 110 || strpbrk(pipe + 9, "/\\:*?\"<>|\t\r\n")) goto done;
        if (strspn(pipe + 9, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != strlen(pipe + 9)) {
            message = "Pipe names may contain letters, numbers, dots, hyphens, and underscores."; goto done;
        }
    } else {
        if (transport && strcmp(transport, "port")) { message = "Choose Port or Named pipe."; goto done; }
        if (!port_text || !*port_text || strlen(port_text) > 5 || strspn(port_text, "0123456789") != strlen(port_text)) {
            message = "Enter a port from 1 to 65535."; goto done;
        }
        port = strtoul(port_text, NULL, 10);
        if (!port || port > 65535) { message = "Enter a port from 1 to 65535."; goto done; }
    }
    snprintf(service, sizeof(service), "org.outershell.command.%s", id);
    snprintf(app_id, sizeof(app_id), "%s:main", service);
    command_key(key, sizeof(key), service);
    RegistryStore store;
    if (!store_open(s, &store, true)) { message = "Could not open the app registry."; goto done; }
    bool duplicate = registry_store_find_backend(&store, service) != NULL;
    for (size_t i = 0; i < store.frontend_count; i++)
        if (named_pipe && store.frontends[i].endpoint_kind == 3 && !_stricmp(store.frontends[i].socket_path, pipe)) duplicate = true;
    if (duplicate) { store_close(s, &store, false); message = "That ID or named pipe is already registered."; goto done; }
    char *root = win_join(s->state, "commands");
    ok = win_secure_directory(root); free(root);
    dir = command_path(s, service, NULL); script = command_path(s, service, "run.cmd");
    StringBuilder batch = {0};
    ok = ok && win_secure_directory(dir) && sb_append(&batch, "@echo off\r\nchcp 65001 >nul\r\n");
    for (const char *p = text; ok && *p; p++) {
        if (*p == '\r') continue;
        ok = sb_append_n(&batch, *p == '\n' ? "\r\n" : p, *p == '\n' ? 2 : 1);
    }
    ok = ok && sb_append(&batch, "\r\n") && win_atomic_write(script, batch.data, batch.length);
    free(batch.data);

    bool owns_allowlist_entry = named_pipe && !win_pipe_is_allowed(pipe);
    unsigned char enabled = 1 | (owns_allowlist_entry ? 2 : 0);
    ok = ok && registry_store_upsert_backend(&store, service, name, "", "", false) &&
        registry_store_upsert_frontend_endpoint(&store, app_id, service, name, named_pipe ? 3 : 1, 1, 0, named_pipe ? "" : "127.0.0.1", (uint16_t)port, "/", named_pipe ? pipe : "", "", true) &&
        registry_store_upsert_resource(&store, key, &enabled, 1);
    ok = store_close(s, &store, ok) && ok;

    if (!ok) { message = "Could not save the Windows command."; goto done; }
    CommandApp *a = win_alloc(sizeof(*a)); a->owns_allowlist_entry = owns_allowlist_entry; a->id = win_strdup(service); a->task_name = command_task_name(service); a->next = s->commands; s->commands = a;
    HRESULT task_result = command_register(s, a);
    if (FAILED(task_result)) {
        RegistryStore rollback;
        if (store_open(s, &rollback, true)) {
            registry_store_remove_backend_and_owned_records(&rollback, service);
            registry_store_remove_resource(&rollback, key);
            store_close(s, &rollback, true);
        }
        s->commands = a->next;
        const char *files[] = {"run.cmd", "runner.exe"};
        for (size_t i = 0; i < 2; i++) {
            char *path = command_path(s, a->id, files[i]); wchar_t *wide = win_wide(path);
            if (wide) DeleteFileW(wide);
            free(path); free(wide);
        }
        wchar_t *wide = win_wide(dir); if (wide) RemoveDirectoryW(wide); free(wide);
        free(a->task_name); free(a->id); free(a);
        snprintf(task_error, sizeof(task_error), "Windows could not register the app task (0x%08lx).", (unsigned long)task_result);
        message = task_error; ok = false; goto done;
    }
    ok = update_allowlist(s) && SUCCEEDED(command_task(a->task_name, "start", NULL, NULL));
    message = ok ? "Created Windows app." : "The app was saved but could not start. Check its command script and try Start.";
    changed(s);
done:
    free(name); free(id); free(text); free(pipe); free(port_text); free(transport); free(scope); free(dir); free(script);
    StringBuilder response = payload(28); payload_u32(&response, 0, ok ? 1 : 0); payload_text(&response, 4, message);
    return response;
}

static StringBuilder command_logs(ShellHost *s, HttpRequest *r) {
    char *id = http_query(r, "serviceID");
    StringBuilder result = payload(52), contents = {0};
    const char *error = "This app has no command output log.";
    if (id && command_find(s, id)) {
        char *path = command_path(s, id, "output.log");
        wchar_t *wide = win_wide(path);
        HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        LARGE_INTEGER size = {0}, offset = {0};
        bool ok = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size);
        offset.QuadPart = size.QuadPart > 262144 ? size.QuadPart - 262144 : 0;
        ok = ok && SetFilePointerEx(file, offset, NULL, FILE_BEGIN);
        char buffer[8192]; DWORD count = 0;
        while (ok && contents.length < 262144) {
            ok = ReadFile(file, buffer, (DWORD)(262144 - contents.length < sizeof(buffer) ? 262144 - contents.length : sizeof(buffer)), &count, NULL) != 0;
            if (!ok || !count) break;
            ok = sb_append_n(&contents, buffer, count);
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        payload_text(&result, 0, id); payload_text(&result, 8, path);
        payload_data(&result, 16, contents.data, contents.length);
        payload_u32(&result, 24, offset.QuadPart ? 1 : 0); payload_u64(&result, 28, (uint64_t)size.QuadPart);
        error = ok ? "" : "Could not read the command output log.";
        free(path); free(wide);
    }
    payload_text(&result, 44, error);
    free(id); free(contents.data); return result;
}
