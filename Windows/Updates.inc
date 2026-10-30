#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <ctype.h>
#include <taskschd.h>

#ifndef OUTERSHELL_VERSION
#define OUTERSHELL_VERSION "0.0.0"
#endif
#ifndef OUTERSHELL_PUBLIC_BASE_URL
#define OUTERSHELL_PUBLIC_BASE_URL "https://outershell.org/outer-shell"
#endif
#define UPDATE_SOURCE OUTERSHELL_PUBLIC_BASE_URL
static CRITICAL_SECTION update_gate;
static HANDLE update_thread;
static char update_version[128];
static bool update_installing;

static bool update_normal_user(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation = {0}; DWORD size = 0;
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && !elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    return ok;
}
static bool update_newer(const char *a, const char *b) {
    for (;;) {
        char *ae, *be;
        unsigned long av = strtoul(a, &ae, 10), bv = strtoul(b, &be, 10);
        if (av != bv) return av > bv;
        if (*ae != '.' || *be != '.') return strcmp(ae, be) > 0;
        a = ae + 1; b = be + 1;
    }
}
static bool update_valid_version(const char *v) {
    unsigned a, b, c; char extra;
    return v && strlen(v) < 128 && isdigit((unsigned char)v[0]) &&
        sscanf(v, "%u.%u.%u%c", &a, &b, &c, &extra) == 3 && a <= 65535 && b <= 65535 && c <= 65535;
}
static bool update_source_parts(const char *source, char *host, size_t host_capacity, char *path, size_t path_capacity) {
    static const char scheme[] = "https://";
    if (strncmp(source, scheme, strlen(scheme))) return false;
    const char *start = source + strlen(scheme), *slash = strchr(start, '/');
    size_t host_length = slash ? (size_t)(slash - start) : strlen(start);
    if (!host_length || host_length >= host_capacity) return false;
    for (size_t i = 0; i < host_length; i++) {
        unsigned char c = (unsigned char)start[i];
        if (!isalnum(c) && c != '.' && c != '-') return false;
    }
    const char *rest = slash ? slash : "";
    size_t path_length = strlen(rest);
    while (path_length && rest[path_length - 1] == '/') path_length--;
    if (path_length >= path_capacity) return false;
    for (size_t i = 0; i < path_length; i++) {
        unsigned char c = (unsigned char)rest[i];
        if (!isalnum(c) && c != '/' && c != '.' && c != '-' && c != '_' && c != '~') return false;
    }
    memcpy(host, start, host_length); host[host_length] = '\0';
    memcpy(path, rest, path_length); path[path_length] = '\0';
    return true;
}
static bool update_fetch(const wchar_t *path, HANDLE file, StringBuilder *text, char *error, size_t capacity) {
    HINTERNET session = WinHttpOpen(L"Outer Shell Updates", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    HINTERNET connection = NULL, request = NULL;
    bool ok = false;
    char host[256], source_path[512];
    wchar_t *host_w = NULL;
    if (!update_source_parts(UPDATE_SOURCE, host, sizeof(host), source_path, sizeof(source_path))) {
        snprintf(error, capacity, "The update source is not a valid HTTPS URL."); goto done;
    }
    if (!session || !(host_w = win_wide(host))) goto done;
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
    connection = WinHttpConnect(session, host_w, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) goto done;
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) goto done;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL)) goto done;
    if (status != 200) { snprintf(error, capacity, "The update server returned HTTP %lu.", status); goto done; }
    size_t total = 0;
    ULONGLONG deadline = GetTickCount64() + 120000;
    for (;;) {
        if (GetTickCount64() > deadline) { snprintf(error, capacity, "The update download timed out."); goto done; }
        char bytes[16384]; DWORD count = 0, written = 0;
        if (!WinHttpReadData(request, bytes, sizeof(bytes), &count)) goto done;
        if (!count) { ok = true; break; }
        total += count;
        if (total > (text ? 127u : 256u * 1024u * 1024u)) { snprintf(error, capacity, "The update download is too large."); goto done; }
        if (text) {
            if (memchr(bytes, 0, count) || !sb_append_n(text, bytes, count)) goto done;
        } else if (!WriteFile(file, bytes, count, &written, NULL) || written != count) goto done;
    }
done:
    if (!ok && !error[0]) snprintf(error, capacity, "Could not download the update (Windows error %lu).", GetLastError());
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(host_w);
    return ok;
}
static bool update_query_value(StringBuilder *out, const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (!sb_append_n(out, (const char *)p, 1)) return false;
        } else {
            char encoded[4]; snprintf(encoded, sizeof(encoded), "%%%02X", *p);
            if (!sb_append(out, encoded)) return false;
        }
    }
    return true;
}
static bool update_query(StringBuilder *out, const char *heartbeat, const char *version,
                         const char *os_version, const char *arch) {
    char host[256], path[512];
    return update_source_parts(UPDATE_SOURCE, host, sizeof(host), path, sizeof(path)) &&
        sb_append(out, path) && sb_append(out, "/latest/windows-version.txt?heartbeat=") &&
        update_query_value(out, heartbeat) && sb_append(out, "&os=windows&osVersion=") &&
        update_query_value(out, os_version) && sb_append(out, "&arch=") &&
        update_query_value(out, arch) && sb_append(out, "&serviceManager=task-scheduler&appVersion=") &&
        update_query_value(out, version);
}
static wchar_t *update_check_path(const char *heartbeat) {
    USHORT process_machine = 0, native_machine = 0;
    const char *arch = "unknown";
    if (IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
        arch = native_machine == IMAGE_FILE_MACHINE_ARM64 ? "arm64" :
            native_machine == IMAGE_FILE_MACHINE_AMD64 ? "x86_64" :
            native_machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "unknown";
    }
    typedef LONG (WINAPI *VersionFunction)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    VersionFunction get_version = ntdll ? (VersionFunction)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW info = {0}; info.dwOSVersionInfoSize = sizeof(info);
    char os_version[64] = "unknown";
    if (get_version && get_version(&info) == 0)
        snprintf(os_version, sizeof(os_version), "%lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    StringBuilder path = {0};
    bool ok = update_query(&path, heartbeat, OUTERSHELL_VERSION, os_version, arch);
    wchar_t *wide = ok ? win_wide(path.data) : NULL;
    free(path.data); return wide;
}
static bool update_check(const char *heartbeat, char *error, size_t capacity) {
    StringBuilder text = {0};
    wchar_t *path = update_check_path(heartbeat);
    bool ok = path && update_fetch(path, INVALID_HANDLE_VALUE, &text, error, capacity);
    free(path);
    if (!ok && !error[0]) snprintf(error, capacity, "Could not prepare the update check.");
    if (ok) {
        while (text.length && isspace((unsigned char)text.data[text.length-1])) text.data[--text.length] = 0;
        ok = text.data && update_valid_version(text.data);
        if (ok) { EnterCriticalSection(&update_gate); snprintf(update_version, sizeof(update_version), "%s", text.data); LeaveCriticalSection(&update_gate); }
        else snprintf(error, capacity, "The update server returned an invalid version.");
    }
    free(text.data); return ok;
}
static bool update_verify(const wchar_t *path, HANDLE file) {
    WINTRUST_FILE_INFO info = {0}; info.cbStruct = sizeof(info); info.pcwszFilePath = path; info.hFile = file;
    WINTRUST_DATA trust = {0}; trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN; trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &info; trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(NULL, &policy, &trust);
    bool expected = false;
    if (status == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *data = WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR *signer = data ? WTHelperGetProvSignerFromChain(data, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT *certificate = signer ? WTHelperGetProvCertFromChain(signer, 0) : NULL;
        wchar_t name[256];
        if (certificate && certificate->pCert && CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256))
            expected = !wcscmp(name, L"Marcus Lewis");
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(NULL, &policy, &trust);
    return status == ERROR_SUCCESS && expected;
}
static bool update_installer_version(const wchar_t *path, const char *expected) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path, &ignored);
    void *data = size ? win_alloc(size) : NULL;
    VS_FIXEDFILEINFO *info = NULL; UINT length = 0;
    bool ok = data && GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\", (void **)&info, &length) && length >= sizeof(*info);
    unsigned a = 0, b = 0, c = 0; char extra = 0;
    ok = ok && sscanf(expected, "%u.%u.%u%c", &a, &b, &c, &extra) == 3 &&
        HIWORD(info->dwFileVersionMS) == a && LOWORD(info->dwFileVersionMS) == b &&
        HIWORD(info->dwFileVersionLS) == c && LOWORD(info->dwFileVersionLS) == 0;
    free(data); return ok;
}
static char *update_task_name(void) {
    char *sid = win_sid(); StringBuilder name = {0};
    if (sid) { sb_append(&name, "\\OuterShell-Update-"); sb_append(&name, sid); }
    free(sid); return name.data;
}
static int update_apply(void) {
    if (!update_normal_user()) return 1;
    char *directory = win_executable_directory(), *path = directory ? win_join(directory, "setup.exe") : NULL;
    wchar_t *wide = path ? win_wide(path) : NULL;
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    bool ok = file != INVALID_HANDLE_VALUE && update_verify(wide, file);
    DWORD code = 1;
    if (ok) {
        StringBuilder command = {0}; sb_append(&command, "\""); sb_append(&command, path);
        sb_append(&command, "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-");
        wchar_t *cmd = win_wide(command.data); STARTUPINFOW startup = {0}; startup.cb = sizeof(startup); PROCESS_INFORMATION process = {0};
        Sleep(1500);
        ok = cmd && CreateProcessW(wide, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
        if (ok) { CloseHandle(process.hThread); WaitForSingleObject(process.hProcess, INFINITE); GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess); ok = code == 0; }
        free(cmd); free(command.data);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    char *local = win_folder(CSIDL_LOCAL_APPDATA), *state = local ? win_join(local, "OuterShell") : NULL;
    char *result = state ? win_join(state, "update-result.txt") : NULL;
    char message[256]; snprintf(message, sizeof(message), ok ? "Update installed successfully." : "Update failed (installer exit code %lu). Try checking for updates again.", code);
    if (result) win_atomic_write(result, message, strlen(message));
    char *sid = win_sid(); StringBuilder task = {0};
    if (sid) { sb_append(&task, "\\OuterShell-"); sb_append(&task, sid); command_task(task.data, "start", NULL, NULL); }
    if (wide) DeleteFileW(wide);
    free(sid); free(task.data); free(result); free(state); free(local); free(wide); free(path); free(directory);
    return ok ? 0 : 1;
}
typedef bool (*UpdateFetch)(const wchar_t *, HANDLE, StringBuilder *, char *, size_t);
static bool update_stage(UpdateFetch fetch, char *error, size_t capacity) {
    if (!update_normal_user()) { snprintf(error, capacity, "Update Outer Shell from your ordinary Windows user session."); return false; }
    char *task = update_task_name(); DWORD state = 0;
    if (!task) return false;
    if (SUCCEEDED(command_task(task, "query", NULL, &state)) && state == TASK_STATE_RUNNING) {
        snprintf(error, capacity, "An Outer Shell update is already running."); free(task); return false;
    }
    GUID id; wchar_t guid[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, guid, 40)) { free(task); return false; }
    char *local = win_folder(CSIDL_LOCAL_APPDATA), *root = local ? win_join(local, "OuterShellUpdates") : NULL;
    char *name = win_utf8(guid), *directory = root && name ? win_join(root, name) : NULL;
    bool ok = root && directory && win_secure_directory(root) && win_secure_directory(directory);
    char *setup = directory ? win_join(directory, "setup.exe") : NULL, *runner = directory ? win_join(directory, "OuterShell.exe") : NULL;
    wchar_t *setup_w = setup ? win_wide(setup) : NULL, *runner_w = runner ? win_wide(runner) : NULL;
    HANDLE file = ok && setup_w ? CreateFileW(setup_w, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    SYSTEM_INFO system; GetNativeSystemInfo(&system);
    const wchar_t *arch = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"arm64" : system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64" : L"x86";
    char host[256], source_path[512];
    wchar_t url[640];
    ok = ok && update_source_parts(UPDATE_SOURCE, host, sizeof(host), source_path, sizeof(source_path)) &&
        swprintf(url, 640, L"%hs/latest/outer-shell-windows-%ls-setup.exe", source_path, arch) > 0;
    ok = ok && file != INVALID_HANDLE_VALUE && fetch(url, file, NULL, error, capacity);
    if (file != INVALID_HANDLE_VALUE) { FlushFileBuffers(file); CloseHandle(file); }
    file = ok ? CreateFileW(setup_w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    ok = ok && file != INVALID_HANDLE_VALUE && update_verify(setup_w, file);
    char expected[128];
    EnterCriticalSection(&update_gate); snprintf(expected, sizeof(expected), "%s", update_version); LeaveCriticalSection(&update_gate);
    if (ok && !update_installer_version(setup_w, expected)) { snprintf(error, capacity, "The signed installer version does not match the advertised Windows release."); ok = false; }
    wchar_t executable[32768];
    if (ok) ok = GetModuleFileNameW(NULL, executable, 32768) && runner_w && CopyFileW(executable, runner_w, TRUE);
    char *sid = win_sid(); StringBuilder xml = {0};
    if (ok && sid) {
        sb_append(&xml, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Principals><Principal id=\"User\"><UserId>");
        command_xml_text(&xml, sid);
        sb_append(&xml, "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT30M</ExecutionTimeLimit></Settings><Actions Context=\"User\"><Exec><Command>");
        command_xml_text(&xml, runner); sb_append(&xml, "</Command><Arguments>--apply-update</Arguments></Exec></Actions></Task>");
        ok = SUCCEEDED(command_task(task, "register-core", xml.data, NULL));
        if (ok) ok = SUCCEEDED(command_task(task, "start", NULL, NULL));
    } else ok = false;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) {
        if (!error[0]) snprintf(error, capacity, "Could not stage a trusted Outer Shell update (Windows error %lu).", GetLastError());
        if (setup_w) DeleteFileW(setup_w);
        if (runner_w) DeleteFileW(runner_w);
    }
    free(xml.data); free(sid); free(runner_w); free(setup_w); free(runner); free(setup); free(directory); free(name); free(root); free(local); free(task);
    return ok;
}
static StringBuilder update_control(const char *operation) {
    char error[512] = "", available[128] = "";
    bool check = !strcmp(operation, "checkUpdate") || !strcmp(operation, "checkOuterShellUpdate");
    bool install = !strcmp(operation, "update") || !strcmp(operation, "updateOuterShell");
    EnterCriticalSection(&update_gate);
    bool busy = update_installing;
    if (install && !busy) update_installing = true;
    LeaveCriticalSection(&update_gate);
    bool ok = false;
    if (check) ok = update_check("extra", error, sizeof(error));
    else if (install && !busy) ok = update_check("extra", error, sizeof(error));
    else snprintf(error, sizeof(error), busy ? "An update is already running." : "This update operation is not supported.");
    EnterCriticalSection(&update_gate); snprintf(available, sizeof(available), "%s", update_version); LeaveCriticalSection(&update_gate);
    bool newer = update_newer(available, OUTERSHELL_VERSION);
    if (ok && install && newer) ok = update_stage(update_fetch, error, sizeof(error));
    if (install) { EnterCriticalSection(&update_gate); if (!busy) update_installing = false; LeaveCriticalSection(&update_gate); }
    StringBuilder result = payload(28);
    payload_u32(&result, 0, (ok ? 1u : 0u) | (ok && newer ? 4u : 0u));
    payload_text(&result, 4, !ok ? error : !newer ? "Outer Shell is up to date." : install ? "Installing update. Outer Shell will reconnect shortly." : "A newer version of Outer Shell is available.");
    payload_text(&result, 12, OUTERSHELL_VERSION); payload_text(&result, 20, available);
    return result;
}
static const char *update_heartbeats[] = {"day", "week", "month"};
static const unsigned update_intervals[] = {1, 7, 30};
static ULONGLONG update_now(void) {
    FILETIME time; GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}
static char *update_marker(const char *state, unsigned index) {
    char name[64];
    snprintf(name, sizeof(name), "last-update-check-%s", update_heartbeats[index]);
    return win_join(state, name);
}
static int update_due_heartbeat(const char *state, ULONGLONG now) {
    for (int i = 2; i >= 0; i--) {
        char *path = update_marker(state, (unsigned)i);
        wchar_t *wide = path ? win_wide(path) : NULL;
        WIN32_FILE_ATTRIBUTE_DATA info;
        bool exists = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &info);
        ULARGE_INTEGER previous = {0};
        if (exists) { previous.LowPart = info.ftLastWriteTime.dwLowDateTime; previous.HighPart = info.ftLastWriteTime.dwHighDateTime; }
        free(wide); free(path);
        if (!exists || (now >= previous.QuadPart && now - previous.QuadPart >=
            (ULONGLONG)update_intervals[i] * 24 * 60 * 60 * 10000000)) return i;
    }
    return -1;
}
static bool update_mark_heartbeat(const char *state, unsigned index) {
    bool ok = true;
    for (unsigned i = 0; i <= index; i++) {
        char *path = update_marker(state, i);
        if (!path || !win_atomic_write(path, "", 0)) ok = false;
        free(path);
    }
    return ok;
}
typedef struct { ShellHost *host; unsigned heartbeat; } UpdateMonitor;
static DWORD WINAPI update_monitor(void *context) {
    UpdateMonitor *monitor = context;
    ShellHost *s = monitor->host;
    char error[512] = "";
    bool ok = update_mark_heartbeat(s->state, monitor->heartbeat) &&
        update_check(update_heartbeats[monitor->heartbeat], error, sizeof(error));
    free(monitor);
    if (ok) {
        EnterCriticalSection(&s->gate); changed(s); LeaveCriticalSection(&s->gate);
    }
    return 0;
}
static void update_interacted(ShellHost *s) {
    if (s->service_name[0]) return;
    EnterCriticalSection(&update_gate);
    if (update_thread && WaitForSingleObject(update_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(update_thread); update_thread = NULL;
    }
    int due = update_thread ? -1 : update_due_heartbeat(s->state, update_now());
    if (due >= 0) {
        UpdateMonitor *monitor = malloc(sizeof(*monitor));
        if (monitor) {
            monitor->host = s; monitor->heartbeat = (unsigned)due;
            update_thread = CreateThread(NULL, 0, update_monitor, monitor, 0, NULL);
            if (!update_thread) free(monitor);
        }
    }
    LeaveCriticalSection(&update_gate);
}
