#include <shellapi.h>
typedef struct {
    const char *id, *name, *executable, *symbol;
} BundledApp;
static const BundledApp bundled_apps[] = {
    {"org.outershell.Files", "Files", "FilesBackend.exe", "folder"},
    {"org.outershell.Plaintext", "Plaintext", "PlaintextBackend.exe",
     "doc.plaintext"},
    {"org.outershell.Top", "Top", "TopBackend.exe", "waveform.path.ecg"},
    {"org.outershell.Firehose", "Firehose", "FirehoseBackend.exe", "flame"},
    {"org.outershell.Profile", "Profile", "ProfileBackend.exe",
     "chart.bar.xaxis"},
    {"org.outershell.Terminal", "Terminal", "TerminalBackend.exe",
     "apple.terminal"}};
static const BundledApp *bundled_find(const char *id) {
    for (size_t i = 0; i < sizeof(bundled_apps) / sizeof(*bundled_apps); i++)
        if (!strcmp(id, bundled_apps[i].id))
            return &bundled_apps[i];
    return NULL;
}
static char *bundled_task_name(const BundledApp *app) {
    char *sid = win_sid();
    StringBuilder name = {0};
    sb_append(&name, "\\OuterShell-");
    sb_append(&name, app->name);
    sb_append(&name, "-");
    if (sid)
        sb_append(&name, sid);
    free(sid);
    return name.data;
}
#ifndef OUTERSHELL_ARCHITECTURE
#define OUTERSHELL_ARCHITECTURE "x64"
#endif
// Apps are downloaded from Outer Shell's release source, as on macOS and
// Linux. Each download goes in its own folder under apps\<id>, so programs
// still running from an older copy, such as Terminal sessions, never block an
// install. apps\<id>\current names the folder in use.
static char *bundled_root(ShellHost *s, const BundledApp *app) {
    char *apps = win_join(s->state, "apps");
    char *root = apps ? win_join(apps, app->id) : NULL;
    free(apps);
    return root;
}
static char *bundled_executable(ShellHost *s, const BundledApp *app) {
    char *root = bundled_root(s, app), *current = root ? win_join(root, "current") : NULL;
    StringBuilder name = {0};
    char *path = NULL;
    if (current && win_read_file(current, 64, &name) && name.data && name.length &&
        !strpbrk(name.data, "\\/.:")) {
        char *folder = win_join(root, name.data);
        path = folder ? win_join(folder, app->executable) : NULL;
        free(folder);
        wchar_t *wide = path ? win_wide(path) : NULL;
        DWORD a = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        free(wide);
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
            free(path);
            path = NULL;
        }
    }
    free(name.data); free(current); free(root);
    return path;
}
static bool bundled_remove_tree(const char *path) {
    wchar_t *wide = win_wide(path);
    if (!wide) return false;
    size_t length = wcslen(wide);
    wchar_t *list = win_alloc((length + 2) * sizeof(wchar_t));
    memcpy(list, wide, length * sizeof(wchar_t));
    list[length] = list[length + 1] = 0;
    SHFILEOPSTRUCTW operation = {0};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list;
    operation.fFlags = FOF_NO_UI;
    bool ok = SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
    free(list); free(wide);
    return ok;
}
static void bundled_remove_other_copies(const char *root, const char *keep) {
    char *pattern = win_join(root, "*");
    wchar_t *wide = pattern ? win_wide(pattern) : NULL;
    WIN32_FIND_DATAW entry;
    HANDLE find = wide ? FindFirstFileW(wide, &entry) : INVALID_HANDLE_VALUE;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char *name = win_utf8(entry.cFileName);
            if (name && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, "current") && strcmp(name, keep)) {
                char *path = win_join(root, name);
                if (path) bundled_remove_tree(path);
                free(path);
            }
            free(name);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    free(wide); free(pattern);
}
static bool bundled_pipe_exists(const char *pipe) {
    const char *name = strrchr(pipe, '\\');
    wchar_t *wide = name ? win_wide(name + 1) : NULL;
    WIN32_FIND_DATAW entry;
    HANDLE find = wide ? FindFirstFileW(L"\\\\.\\pipe\\*", &entry) : INVALID_HANDLE_VALUE;
    bool found = false;
    if (find != INVALID_HANDLE_VALUE) {
        do found = !_wcsicmp(entry.cFileName, wide);
        while (!found && FindNextFileW(find, &entry));
        FindClose(find);
    }
    free(wide);
    return found;
}
// Task Scheduler reports a stopped task before its program has exited, and the
// program holds the app's pipe until it does.
static void bundled_wait_pipe_closed(const char *pipe) {
    ULONGLONG deadline = GetTickCount64() + 10000;
    while (bundled_pipe_exists(pipe) && GetTickCount64() < deadline)
        Sleep(100);
}
static bool bundled_extract(const char *archive, const char *directory) {
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    wchar_t *archive_w = win_wide(archive), *directory_w = win_wide(directory);
    bool ok = length && length < MAX_PATH - 9 && archive_w && directory_w;
    if (ok) {
        wcscat(system, L"\\tar.exe");
        size_t count = wcslen(system) + wcslen(archive_w) + wcslen(directory_w) + 32;
        wchar_t *command = win_alloc(count * sizeof(wchar_t));
        swprintf(command, count, L"\"%s\" -xzf \"%s\" -C \"%s\"", system, archive_w, directory_w);
        STARTUPINFOW startup = {0};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {0};
        DWORD code = 1;
        ok = CreateProcessW(system, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process) != 0;
        if (ok) {
            ok = WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0 &&
                 GetExitCodeProcess(process.hProcess, &code) && code == 0;
            if (!ok) TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        free(command);
    }
    free(archive_w); free(directory_w);
    return ok;
}
static bool bundled_download(ShellHost *s, const BundledApp *app, char *error, size_t capacity) {
    char host[256], source[512], stamp[32];
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    snprintf(stamp, sizeof(stamp), "%08lx%08lx", now.dwHighDateTime, now.dwLowDateTime);
    char *apps = win_join(s->state, "apps"), *root = bundled_root(s, app);
    char *archive = NULL, *staging = NULL, *extracted = NULL, *folder = NULL, *executable = NULL;
    wchar_t *apps_w = apps ? win_wide(apps) : NULL, *root_w = root ? win_wide(root) : NULL;
    wchar_t *archive_w = NULL, *staging_w = NULL, *url_w = NULL;
    StringBuilder url = {0};
    bool ok = false;
    if (!apps_w || !root_w || !update_source_parts(UPDATE_SOURCE, host, sizeof(host), source, sizeof(source))) {
        snprintf(error, capacity, "Outer Shell has no valid app download source.");
        goto done;
    }
    CreateDirectoryW(apps_w, NULL);
    CreateDirectoryW(root_w, NULL);
    sb_append(&url, source); sb_append(&url, "/apps/"); sb_append(&url, app->name);
    sb_append(&url, "/windows-" OUTERSHELL_ARCHITECTURE ".tar.gz");
    char name[48];
    snprintf(name, sizeof(name), "%s.tar.gz", stamp);
    archive = win_join(root, name);
    snprintf(name, sizeof(name), "%s.partial", stamp);
    staging = win_join(root, name);
    url_w = url.data ? win_wide(url.data) : NULL;
    archive_w = archive ? win_wide(archive) : NULL;
    staging_w = staging ? win_wide(staging) : NULL;
    if (!url_w || !archive_w || !staging_w) goto done;
    HANDLE file = CreateFileW(archive_w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { snprintf(error, capacity, "Could not save the %s download.", app->name); goto done; }
    char fetch_error[256] = "";
    ok = update_fetch(url_w, file, NULL, fetch_error, sizeof(fetch_error));
    CloseHandle(file);
    if (!ok) {
        snprintf(error, capacity, "Failed to download %s from https://%s%s: %s", app->name, host, url.data, fetch_error);
        goto done;
    }
    ok = CreateDirectoryW(staging_w, NULL) && bundled_extract(archive, staging);
    if (!ok) { snprintf(error, capacity, "Could not extract %s.", app->name); goto done; }
    extracted = win_join(staging, app->name);
    executable = extracted ? win_join(extracted, app->executable) : NULL;
    wchar_t *executable_w = executable ? win_wide(executable) : NULL;
    HANDLE image = executable_w ? CreateFileW(executable_w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    ok = image != INVALID_HANDLE_VALUE && update_verify(executable_w, image);
    if (image != INVALID_HANDLE_VALUE) CloseHandle(image);
    free(executable_w);
    if (!ok) { snprintf(error, capacity, "The %s download does not contain a program with a trusted Outer Shell signature.", app->name); goto done; }
    folder = win_join(root, stamp);
    wchar_t *extracted_w = extracted ? win_wide(extracted) : NULL, *folder_w = folder ? win_wide(folder) : NULL;
    char *current = win_join(root, "current");
    ok = extracted_w && folder_w && current && MoveFileW(extracted_w, folder_w) &&
         win_atomic_write(current, stamp, strlen(stamp));
    free(extracted_w); free(folder_w); free(current);
    if (!ok) { snprintf(error, capacity, "Could not install %s.", app->name); goto done; }
    bundled_remove_other_copies(root, stamp);
done:
    if (archive_w) DeleteFileW(archive_w);
    if (staging) bundled_remove_tree(staging);
    free(url.data); free(url_w); free(archive_w); free(staging_w); free(apps_w); free(root_w);
    free(apps); free(root); free(archive); free(staging); free(extracted); free(folder); free(executable);
    return ok;
}
static bool bundled_registered(RegistryStore *store, const BundledApp *app) {
    char *pipe = win_pipe(app->id);
    char frontend[160];
    snprintf(frontend, sizeof(frontend), "%s:main", app->id);
    bool ok = pipe &&
              registry_store_upsert_backend(store, app->id, app->name, "", "",
                                            false) &&
              registry_store_upsert_frontend_endpoint(store, frontend, app->id,
                                                      app->name, 3, 1, 0, "", 0,
                                                      "/", pipe, "", true);
    free(pipe);
    return ok;
}
static bool bundled_control(ShellHost *s, const BundledApp *app,
                            const char *operation, char *error,
                            size_t capacity) {
    char *task = bundled_task_name(app), *pipe = win_pipe(app->id);
    HRESULT hr = E_INVALIDARG;
    bool ok = false;
    bool install = !strcmp(operation, "install") || !strcmp(operation, "installUser") ||
                   !strcmp(operation, "run") || !strcmp(operation, "runUser"),
         remove = !strcmp(operation, "uninstall");
    if (!strcmp(operation, "installRoot") || !strcmp(operation, "runRoot")) {
        snprintf(error, capacity, "%s cannot be installed with administrator privileges on Windows.", app->name);
        goto done;
    }
    if (install) {
        if (!bundled_download(s, app, error, capacity))
            goto done;
        command_task(task, "stop", NULL, NULL);
        if (pipe)
            bundled_wait_pipe_closed(pipe);
        char *sid = win_sid(), *path = bundled_executable(s, app);
        char *directory = path ? win_strdup(path) : NULL;
        char *separator = directory ? strrchr(directory, '\\') : NULL;
        if (!sid || !separator) {
            free(sid); free(path); free(directory);
            snprintf(error, capacity, "Could not install %s.", app->name);
            goto done;
        }
        *separator = '\0';
        StringBuilder xml = {0};
        sb_append(&xml,
                  "<Task version=\"1.2\" "
                  "xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/"
                  "task\"><Principals><Principal id=\"User\"><UserId>");
        command_xml_text(&xml, sid);
        sb_append(
            &xml,
            "</UserId><LogonType>InteractiveToken</"
            "LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></"
            "Principals><Settings><MultipleInstancesPolicy>IgnoreNew</"
            "MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</"
            "DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</"
            "StopIfGoingOnBatteries><AllowStartOnDemand>true</"
            "AllowStartOnDemand><ExecutionTimeLimit>PT0S</"
            "ExecutionTimeLimit><Enabled>true</Enabled></Settings><Actions "
            "Context=\"User\"><Exec><Command>");
        command_xml_text(&xml, path);
        sb_append(&xml, "</Command><WorkingDirectory>");
        command_xml_text(&xml, directory);
        sb_append(&xml, "</WorkingDirectory></Exec></Actions></Task>");
        hr = command_task(task, "register-core", xml.data, NULL);
        if (SUCCEEDED(hr) && (!strcmp(operation, "run") || !strcmp(operation, "runUser")))
            hr = command_task(task, "start", NULL, NULL);
        free(xml.data);
        free(directory);
        free(path);
        free(sid);
    } else if (!strcmp(operation, "restart")) {
        hr = command_task(task, "stop", NULL, NULL);
        if (SUCCEEDED(hr))
            hr = command_task(task, "start", NULL, NULL);
    } else if (remove || !strcmp(operation, "start") ||
               !strcmp(operation, "stop"))
        hr = command_task(task, operation, NULL, NULL);
    else if (!strcmp(operation, "enable"))
        hr = command_task(task, "start", NULL, NULL);
    else if (!strcmp(operation, "disable"))
        hr = command_task(task, "stop", NULL, NULL);
    if (remove && hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        hr = S_OK;
    if (FAILED(hr)) {
        char reason[512];
        win_hresult_text(hr, reason, sizeof(reason));
        snprintf(error, capacity, "Windows could not %s %s: %s", operation, app->name, reason);
        goto done;
    }
    ok = true;
    if (install || remove) {
        RegistryStore store;
        ok = store_open(s, &store, true);
        if (ok) {
            ok = install ? bundled_registered(&store, app)
                         : registry_store_remove_backend_and_owned_records(
                               &store, app->id);
            ok = store_close(s, &store, ok) && ok;
        }
        if (!ok)
            snprintf(error, capacity, "Could not update the app registry.");
        else if (!win_set_pipe_activation(pipe, install ? task : NULL)) {
            ok = false;
            snprintf(error, capacity, "Could not update the named-pipe activation mapping.");
        } else if (install) {
            const char *paths[] = {pipe};
            ok = win_allow_pipes(paths, 1);
            if (!ok)
                snprintf(error, capacity, "Could not add %s to the named-pipe allowlist.", app->name);
        } else {
            ok = win_disallow_pipe(pipe);
            if (!ok)
                snprintf(error, capacity, "Could not remove %s from the named-pipe allowlist.", app->name);
            char *root = bundled_root(s, app);
            if (root) bundled_remove_tree(root);
            free(root);
        }
    }
done:
    free(task);
    free(pipe);
    return ok;
}

// Download apps that are installed but whose program is missing: installs from
// before apps were downloaded, and reinstalls after an uninstall, which removes
// the apps' programs and tasks but keeps the app registry.
static DWORD WINAPI bundled_migrate(void *context) {
    ShellHost *s = context;
    bool registered[sizeof(bundled_apps) / sizeof(*bundled_apps)] = {0};
    RegistryStore store;
    if (store_open(s, &store, false)) {
        for (size_t i = 0; i < sizeof(bundled_apps) / sizeof(*bundled_apps); i++)
            registered[i] = registry_store_find_backend(&store, bundled_apps[i].id) != NULL;
        store_close(s, &store, false);
    }
    for (size_t i = 0; i < sizeof(bundled_apps) / sizeof(*bundled_apps); i++) {
        const BundledApp *app = &bundled_apps[i];
        char *task = bundled_task_name(app), *path = bundled_executable(s, app);
        DWORD state = 0;
        if (task && !path && (registered[i] || SUCCEEDED(command_task(task, "query", NULL, &state)))) {
            char error[512] = "";
            if (!bundled_control(s, app, "install", error, sizeof(error)))
                log_error(s, error);
        }
        free(task); free(path);
    }
    return 0;
}

static DWORD bundled_states[sizeof(bundled_apps) / sizeof(*bundled_apps)];
static ULONGLONG bundled_queried;
static void bundled_poll(ShellHost *s) {
    ULONGLONG now = GetTickCount64();
    if (bundled_queried && now - bundled_queried < 1000)
        return;
    for (size_t i = 0; i < sizeof(bundled_apps) / sizeof(*bundled_apps); i++) {
        char *task = bundled_task_name(&bundled_apps[i]);
        DWORD state = 0;
        if (FAILED(command_task(task, "query", NULL, &state)))
            state = 0;
        free(task);
        if (state != bundled_states[i]) {
            bundled_states[i] = state;
            changed(s);
        }
    }
    bundled_queried = now;
}
