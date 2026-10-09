#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>

static bool admin_secure_file(const wchar_t *path) {
    PSECURITY_DESCRIPTOR sd = NULL;
    char *sid = win_sid();
    if (!sid) return false;
    char descriptor[256];
    snprintf(descriptor, sizeof(descriptor), "O:BAG:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;%s)", sid);
    free(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(descriptor, SDDL_REVISION_1, &sd, NULL)) return false;
    bool ok = SetFileSecurityW(path, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd) != 0;
    LocalFree(sd); return ok;
}
static bool admin_secure_directory(const char *path) {
    PSECURITY_DESCRIPTOR sd = NULL;
    char *sid = win_sid();
    if (!sid) return false;
    char descriptor[256];
    snprintf(descriptor, sizeof(descriptor), "O:BAG:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;GRGX;;;%s)", sid);
    free(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(descriptor, SDDL_REVISION_1, &sd, NULL)) return false;
    wchar_t *wide = win_wide(path);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    bool ok = wide && (CreateDirectoryW(wide, &sa) || GetLastError() == ERROR_ALREADY_EXISTS);
    HANDLE handle = ok ? CreateFileW(wide, READ_CONTROL | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info;
    PACL acl = NULL; PSID owner = NULL; BOOL present, defaulted;
    ok = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
        SetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            owner, NULL, acl, NULL) == ERROR_SUCCESS;
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    LocalFree(sd); free(wide); return ok;
}
static void admin_host_free(ShellHost *s) {
    while (s->commands) {
        CommandApp *a = s->commands; s->commands = a->next;
        free(a->id); free(a->task_name); free(a);
    }
    free(s->state); free(s->registry); free(s->web);
}
static bool admin_host(ShellHost *s, bool create) {
    char *programs = win_folder(CSIDL_PROGRAM_FILES), *sid = win_sid();
    if (!programs || !sid) { free(programs); free(sid); return false; }
    char *root = win_join(programs, "OuterShell Commands");
    s->state = win_join(root, sid);
    s->registry = win_join(s->state, "registry.orwa");
    s->web = win_strdup("");
    s->administrator = true;
    InitializeConditionVariable(&s->changed);
    bool ok = true;
    if (create) ok = admin_secure_directory(root) && admin_secure_directory(s->state);
    else {
        wchar_t *path = win_wide(s->registry);
        ok = path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        free(path);
    }
    free(root); free(programs); free(sid);
    if (!ok) admin_host_free(s);
    return ok;
}
static void admin_refresh(ShellHost *s) {
    ULONGLONG now = GetTickCount64();
    if (now - s->admin_queried < 1000) return;
    s->admin_queried = now;
    ShellHost admin = {0};
    ULONGLONG revision = 0;
    if (admin_host(&admin, false)) {
        wchar_t *path = win_wide(admin.registry);
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (path && GetFileAttributesExW(path, GetFileExInfoStandard, &info))
            revision = ((ULONGLONG)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
        free(path);
        commands_load(&admin);
        for (CommandApp *a = admin.commands; a; a = a->next) {
            DWORD state = 0;
            command_task(a->task_name, "query", NULL, &state);
            revision = revision * 33 + state;
        }
        admin_host_free(&admin);
    }
    if (revision != s->admin_revision) { s->admin_revision = revision; changed(s); }
}
static bool admin_local_client(Connection *c) {
    ULONG client_session = 0;
    DWORD own_session = 0;
    return GetNamedPipeClientSessionId(c->pipe, &client_session) && client_session != 0 &&
        ProcessIdToSessionId(GetCurrentProcessId(), &own_session) && client_session == own_session;
}

static bool admin_transfer(HANDLE pipe, void *buffer, DWORD length, bool write) {
    unsigned char *cursor = buffer;
    while (length) {
        OVERLAPPED operation = {0};
        operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!operation.hEvent) return false;
        DWORD count = 0;
        BOOL ok = write ? WriteFile(pipe, cursor, length, &count, &operation) : ReadFile(pipe, cursor, length, &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, 120000) != WAIT_OBJECT_0) CancelIoEx(pipe, &operation);
            ok = GetOverlappedResult(pipe, &operation, &count, TRUE);
        }
        CloseHandle(operation.hEvent);
        if (!ok || !count) return false;
        cursor += count; length -= count;
    }
    return true;
}
static int admin_apply_command(const wchar_t *path, const wchar_t *expected_sid) {
    HANDLE token = NULL; TOKEN_ELEVATION elevation = {0}; DWORD size = 0, session = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated &&
        ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0;
    if (token) CloseHandle(token);
    char *sid = win_sid(); wchar_t *wide_sid = sid ? win_wide(sid) : NULL;
    bool same_user = wide_sid && !wcscmp(wide_sid, expected_sid);
    free(sid); free(wide_sid);
    if (!elevated || !same_user) return ERROR_ACCESS_DENIED;
    if (wcsncmp(path, L"\\\\.\\pipe\\org.outershell.admin-apply-", 36)) return ERROR_INVALID_PARAMETER;
    FreeConsole();
    HANDLE pipe = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return (int)GetLastError();
    ULONG server_session = 0;
    DWORD length = 0;
    bool ok = GetNamedPipeServerSessionId(pipe, &server_session) && server_session == session &&
        admin_transfer(pipe, &length, sizeof(length), false) && length && length <= 2 * 1024 * 1024;
    HttpRequest request = {0};
    if (ok) {
        request.body.data = win_alloc((size_t)length + 1); request.body.length = length;
        ok = admin_transfer(pipe, request.body.data, length, false);
        request.body.data[length] = 0;
    }
    if (ok) {
        ShellHost host = {0};
        if (admin_host(&host, true)) {
            StringBuilder result = command_create(&host, &request);
            length = (DWORD)result.length;
            ok = admin_transfer(pipe, &length, sizeof(length), true) && admin_transfer(pipe, result.data, length, true);
            free(result.data); admin_host_free(&host);
        } else ok = false;
    }
    free(request.body.data); CloseHandle(pipe);
    return ok ? 0 : ERROR_INVALID_DATA;
}
static bool admin_commands_request(ShellHost *s, Connection *c, HttpRequest *r) {
    if (strcmp(r->path, "/api/admin-commands")) return false;
    bool local = admin_local_client(c);
    if (!strcmp(r->method, "GET")) {
        const char *body = local ? "{\"localSetup\":true}" : "{\"localSetup\":false}";
        http_send(c, r, 200, "application/json", body, strlen(body));
        return true;
    }
    if (strcmp(r->method, "POST")) { http_text(c, r, 405, "Unsupported method."); return true; }
    if (!local) { http_text(c, r, 403, "Set up administrator commands locally on this Windows machine."); return true; }
    if (!r->body.length || r->body.length > 2 * 1024 * 1024 || (r->query && *r->query)) {
        http_text(c, r, 400, "Submit the administrator command form."); return true;
    }
    GUID guid;
    wchar_t unique[40], path[128], executable[32768], parameters[512];
    char *sid = win_sid(); wchar_t *wide_sid = sid ? win_wide(sid) : NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    char descriptor[256];
    snprintf(descriptor, sizeof(descriptor), "D:P(A;;GA;;;BA)(A;;GA;;;%s)", sid ? sid : "");
    bool ready = wide_sid && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(&guid, unique, 40) &&
        ConvertStringSecurityDescriptorToSecurityDescriptorA(descriptor, SDDL_REVISION_1, &sd, NULL);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (ready) {
        swprintf(path, 128, L"\\\\.\\pipe\\org.outershell.admin-apply-%ls", unique);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
        pipe = CreateNamedPipeW(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
    }
    if (sd) LocalFree(sd);
    OVERLAPPED connection = {0}; connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    bool pending = false;
    if (pipe != INVALID_HANDLE_VALUE && connection.hEvent) {
        pending = !ConnectNamedPipe(pipe, &connection) && GetLastError() == ERROR_IO_PENDING;
    }
    const char *error = "Could not prepare administrator setup.";
    StringBuilder result = {0};
    if (pending) {
        DWORD count = GetModuleFileNameW(NULL, executable, 32768);
        swprintf(parameters, 512, L"--apply-admin-command \"%ls\" %ls", path, wide_sid);
        SHELLEXECUTEINFOW launch = {0}; launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.lpVerb = L"runas"; launch.lpFile = executable;
        launch.lpParameters = parameters; launch.nShow = SW_HIDE;
        if (count && count < 32768 && ShellExecuteExW(&launch) && launch.hProcess) {
            HANDLE waits[2] = {connection.hEvent, launch.hProcess};
            DWORD bytes = 0; ULONG client = 0;
            bool connected = WaitForMultipleObjects(2, waits, FALSE, 120000) == WAIT_OBJECT_0 &&
                GetOverlappedResult(pipe, &connection, &bytes, FALSE) &&
                GetNamedPipeClientProcessId(pipe, &client) && client == GetProcessId(launch.hProcess);
            DWORD length = (DWORD)r->body.length;
            if (connected && admin_transfer(pipe, &length, sizeof(length), true) &&
                admin_transfer(pipe, r->body.data, length, true) && admin_transfer(pipe, &length, sizeof(length), false) &&
                length >= 28 && length <= 1024 * 1024) {
                result.data = win_alloc(length); result.length = length;
                if (!admin_transfer(pipe, result.data, length, false)) { free(result.data); result = (StringBuilder){0}; }
            }
            error = "Administrator setup did not complete. Check that UAC used the same Windows account, then refresh before retrying.";
            CloseHandle(launch.hProcess);
        } else error = GetLastError() == ERROR_CANCELLED ? "Administrator setup was cancelled. No app was created." : "Could not open administrator setup.";
        CancelIoEx(pipe, &connection);
        DWORD bytes = 0; GetOverlappedResult(pipe, &connection, &bytes, TRUE);
    }
    if (connection.hEvent) CloseHandle(connection.hEvent);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    free(sid); free(wide_sid);
    if (result.length) {
        http_send(c, r, 200, "application/octet-stream", result.data, result.length);
        EnterCriticalSection(&s->gate); changed(s); LeaveCriticalSection(&s->gate);
    } else http_text(c, r, 409, error);
    free(result.data);
    return true;
}
