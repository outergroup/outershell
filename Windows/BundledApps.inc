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
     "chart.bar.xaxis"}};
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
static bool bundled_available(ShellHost *s, const BundledApp *app) {
    char *path = win_join(s->directory, app->executable);
    wchar_t *wide = win_wide(path);
    DWORD a = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    free(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
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
    bool install = !strcmp(operation, "install"),
         remove = !strcmp(operation, "uninstall");
    if (install) {
        if (!bundled_available(s, app)) {
            snprintf(
                error, capacity,
                "%s is missing from this installation. Repair Outer Shell to "
                "install it.",
                app->name);
            goto done;
        }
        char *sid = win_sid(), *path = win_join(s->directory, app->executable);
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
        command_xml_text(&xml, s->directory);
        sb_append(&xml, "</WorkingDirectory></Exec></Actions></Task>");
        hr = command_task(task, "register-core", xml.data, NULL);
        free(xml.data);
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
        snprintf(error, capacity, "Windows could not %s %s (0x%08lx).",
                 operation, app->name, (unsigned long)hr);
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
        ok = ok && win_set_pipe_activation(pipe, install ? task : NULL);
        if (install) {
            const char *paths[] = {pipe};
            ok = ok && win_allow_pipes(paths, 1);
        } else
            ok = ok && win_disallow_pipe(pipe);
        if (!ok)
            snprintf(
                error, capacity,
                "Could not update the app registry or named-pipe activation "
                "mapping.");
    }
done:
    free(task);
    free(pipe);
    return ok;
}

static DWORD bundled_states[5];
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
