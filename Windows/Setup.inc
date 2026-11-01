static HRESULT setup_stop_commands(const char *directory) {
    ShellHost host = {0};
    host.state = win_strdup(directory);
    host.registry = win_join(directory, "registry.orwa");
    wchar_t *path = win_wide(host.registry);
    HRESULT result = S_OK;
    if (!path) result = E_OUTOFMEMORY;
    else if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) result = HRESULT_FROM_WIN32(error);
    } else if (!commands_load(&host)) result = E_FAIL;
    for (CommandApp *app = host.commands; app && SUCCEEDED(result); app = app->next) {
        result = command_task(app->task_name, "stop", NULL, NULL);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == SCHED_E_TASK_NOT_RUNNING) result = S_OK;
        if (SUCCEEDED(result) && !command_wait_stopped(&host, app)) result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    while (host.commands) {
        CommandApp *app = host.commands;
        host.commands = app->next;
        free(app->id); free(app->task_name); free(app);
    }
    free(path); free(host.state); free(host.registry);
    return result;
}


static int setup_tasks(const char *operation) {
    char *sid = win_sid(), *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *directory = local ? win_join(local, "OuterShell") : NULL; free(local);
    if (!sid || !directory) { free(sid); free(directory); return 1; }
    bool install = !strcmp(operation, "install"), remove = !strcmp(operation, "remove");
    HRESULT result = install ? S_OK : setup_stop_commands(directory);
    for (int index = 0; index < 1 + (int)(sizeof(bundled_apps) / sizeof(*bundled_apps)) && SUCCEEDED(result); index++) {
        StringBuilder name = {0}; sb_append(&name, index ? "\\OuterShell-" : "\\OuterShell-"); if (index) { sb_append(&name, bundled_apps[index - 1].name); sb_append(&name, "-"); } sb_append(&name, sid);
        if (install && index > 1) {
            DWORD state = 0;
            if (FAILED(command_task(name.data, "query", NULL, &state))) { free(name.data); continue; }
        }
        if (!install) {
            result = command_task(name.data, remove ? "uninstall" : "stop", NULL, NULL);
            if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == SCHED_E_TASK_NOT_RUNNING) result = S_OK;
        } else {
            char *path = win_join(directory, index ? bundled_apps[index - 1].executable : "OuterShell.exe");
            StringBuilder xml = {0};
            sb_append(&xml, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Principals><Principal id=\"User\"><UserId>");
            command_xml_text(&xml, sid);
            sb_append(&xml, "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowStartOnDemand>true</AllowStartOnDemand><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled><RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings><Actions Context=\"User\"><Exec><Command>");
            command_xml_text(&xml, path); sb_append(&xml, "</Command><WorkingDirectory>"); command_xml_text(&xml, directory);
            sb_append(&xml, "</WorkingDirectory></Exec></Actions></Task>");
            result = command_task(name.data, "register-core", xml.data, NULL);
            free(xml.data); free(path);
        }
        if (SUCCEEDED(result) && (install || remove)) {
            char *pipe = win_pipe(index ? bundled_apps[index - 1].id : shell_id);
            bool ok = pipe && win_set_pipe_activation(pipe, install ? name.data : NULL);
            if (remove && pipe) ok = win_disallow_pipe(pipe) && ok;
            if (!index) {
                char *api = win_pipe("org.outershell.outershelld");
                ok = api && win_set_pipe_activation(api, install ? name.data : NULL) && ok;
                if (remove && api) ok = win_disallow_pipe(api) && ok;
                free(api);
            }
            free(pipe);
            if (!ok) result = E_FAIL;
        }
        free(name.data);
    }
    if (SUCCEEDED(result) && install) {
        char *a = win_pipe(shell_id), *b = win_pipe(files_id);
        const char *paths[] = {a, b};
        if (!a || !b || !win_allow_pipes(paths, 2)) result = E_FAIL;
        free(a); free(b);
    }
    if (FAILED(result)) fprintf(stderr, "Outer Shell task setup failed (0x%08lx).\n", (unsigned long)result);
    free(sid); free(directory);
    return SUCCEEDED(result) ? 0 : 1;
}
