#define COBJMACROS
#include "Runtime.h"
#include "CommandTasks.h"
#include <taskschd.h>
#include <oleauto.h>
#include <string.h>
#include <stdlib.h>

static BSTR task_string(const char *value) {
    wchar_t *wide = win_wide(value);
    BSTR result = wide ? SysAllocString(wide) : NULL;
    free(wide);
    return result;
}
HRESULT command_task(const char *name, const char *operation, const char *xml, DWORD *state) {
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return initialized;
    ITaskService *service = NULL;
    ITaskFolder *folder = NULL;
    IRegisteredTask *task = NULL;
    IRunningTask *running = NULL;
    VARIANT empty; VariantInit(&empty);
    BSTR path = task_string(name), root = SysAllocString(L"\\"), definition = xml ? task_string(xml) : NULL;
    HRESULT hr = path && root && (!xml || definition) ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&service);
    if (SUCCEEDED(hr)) hr = ITaskService_Connect(service, empty, empty, empty, empty);
    if (SUCCEEDED(hr)) hr = ITaskService_GetFolder(service, root, &folder);
    if (SUCCEEDED(hr)) {
        if (xml) hr = ITaskFolder_RegisterTask(folder, path, definition, TASK_CREATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &task);
        else hr = ITaskFolder_GetTask(folder, path, &task);
    }
    if (SUCCEEDED(hr) && !strcmp(operation, "query")) {
        TASK_STATE value = TASK_STATE_UNKNOWN;
        hr = IRegisteredTask_get_State(task, &value);
        if (SUCCEEDED(hr) && state) *state = (DWORD)value;
    } else if (SUCCEEDED(hr)) {
        bool stop = !strcmp(operation, "stop") || !strcmp(operation, "restart") || !strcmp(operation, "uninstall");
        bool start = !strcmp(operation, "start") || !strcmp(operation, "restart");
        if (stop) {
            hr = IRegisteredTask_put_Enabled(task, VARIANT_FALSE);
            if (SUCCEEDED(hr)) hr = IRegisteredTask_Stop(task, 0);
            if (hr == SCHED_E_TASK_NOT_RUNNING) hr = S_OK;
            else if (hr == S_FALSE) {
                TASK_STATE remaining = TASK_STATE_UNKNOWN;
                hr = IRegisteredTask_get_State(task, &remaining);
                if (SUCCEEDED(hr) && (remaining == TASK_STATE_RUNNING || remaining == TASK_STATE_QUEUED)) hr = E_ACCESSDENIED;
            }
            ULONGLONG deadline = GetTickCount64() + 10000;
            TASK_STATE value = TASK_STATE_RUNNING;
            while (SUCCEEDED(hr) && GetTickCount64() < deadline) {
                hr = IRegisteredTask_get_State(task, &value);
                if (value != TASK_STATE_RUNNING && value != TASK_STATE_QUEUED) break;
                Sleep(50);
            }
            if (SUCCEEDED(hr) && (value == TASK_STATE_RUNNING || value == TASK_STATE_QUEUED)) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (SUCCEEDED(hr) && start) {
            hr = IRegisteredTask_put_Enabled(task, VARIANT_TRUE);
            if (SUCCEEDED(hr)) hr = IRegisteredTask_Run(task, empty, &running);
        }
        if (SUCCEEDED(hr) && !strcmp(operation, "uninstall")) hr = ITaskFolder_DeleteTask(folder, path, 0);
    }
    if (running) IRunningTask_Release(running);
    if (task) IRegisteredTask_Release(task);
    if (folder) ITaskFolder_Release(folder);
    if (service) ITaskService_Release(service);
    SysFreeString(path); SysFreeString(root); SysFreeString(definition);
    CoUninitialize();
    return hr;
}
