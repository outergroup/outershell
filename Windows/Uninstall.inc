static bool uninstall_boundary(ULONGLONG ticks, char text[32]) {
    ULARGE_INTEGER value; value.QuadPart = ticks;
    FILETIME file = {value.LowPart, value.HighPart}; SYSTEMTIME time;
    if (!FileTimeToSystemTime(&file, &time)) return false;
    snprintf(text, 32, "%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    return true;
}
static StringBuilder uninstall_definition(const char *path, const char *sid, ULONGLONG now) {
    StringBuilder xml = {0}; char start[32], end[32];
    if (!uninstall_boundary(now + 5ULL * 10000000, start) ||
        !uninstall_boundary(now + 5ULL * 60 * 10000000, end)) return xml;
    sb_append(&xml, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><Triggers><TimeTrigger><StartBoundary>");
    sb_append(&xml, start); sb_append(&xml, "</StartBoundary><EndBoundary>");
    sb_append(&xml, end); sb_append(&xml, "</EndBoundary><Enabled>true</Enabled></TimeTrigger></Triggers><Principals><Principal id=\"User\"><UserId>");
    command_xml_text(&xml, sid);
    sb_append(&xml, "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT30M</ExecutionTimeLimit><DeleteExpiredTaskAfter>PT1H</DeleteExpiredTaskAfter></Settings><Actions Context=\"User\"><Exec><Command>");
    command_xml_text(&xml, path);
    sb_append(&xml, "</Command><Arguments>/VERYSILENT /SUPPRESSMSGBOXES /NORESTART</Arguments></Exec></Actions></Task>");
    return xml;
}
static StringBuilder uninstall_control(ShellHost *s) {
    char error[512] = "";
    bool ok = false;
    EnterCriticalSection(&update_gate);
    bool busy = update_installing;
    if (!busy) update_installing = true;
    LeaveCriticalSection(&update_gate);
    char *path = win_join(s->directory, "unins000.exe"), *sid = win_sid();
    wchar_t *wide = path ? win_wide(path) : NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    StringBuilder task = {0}, xml = {0};
    if (busy) snprintf(error, sizeof(error), "An update or uninstall is already in progress.");
    else if (!update_normal_user()) snprintf(error, sizeof(error), "Uninstall Outer Shell from your ordinary Windows user session.");
    else if (!wide || !sid) snprintf(error, sizeof(error), "Could not prepare Windows uninstall.");
    else {
        file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) snprintf(error, sizeof(error), "The Windows uninstaller is missing. Run the Outer Shell installer to repair this installation, then try again.");
        else if (!update_verify(wide, file)) snprintf(error, sizeof(error), "The Windows uninstaller does not have a trusted Outer Shell signature.");
        else {
            sb_append(&task, "\\OuterShell-Uninstall-"); sb_append(&task, sid);
            xml = uninstall_definition(path, sid, update_now());
            HRESULT result = task.data && xml.data ? command_task(task.data, "register-core", xml.data, NULL) : E_OUTOFMEMORY;
            ok = SUCCEEDED(result);
            if (!ok) snprintf(error, sizeof(error), "Could not schedule Windows uninstall (error 0x%08lx).", (unsigned long)result);
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(path); free(sid); free(wide); free(task.data); free(xml.data);
    if (!busy && !ok) { EnterCriticalSection(&update_gate); update_installing = false; LeaveCriticalSection(&update_gate); }
    StringBuilder response = payload(28);
    payload_u32(&response, 0, ok ? 1u : 0u);
    payload_text(&response, 4, ok ? "Uninstalling Outer Shell. This connection will close shortly." : error);
    return response;
}
