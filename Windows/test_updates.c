#define wmain outer_shell_entry
#include "OuterShell.c"
#undef wmain
#undef NDEBUG
#include <assert.h>
static const wchar_t *fixture;
static bool fixture_download(const wchar_t *url, HANDLE file, StringBuilder *text, char *error, size_t size) {
    (void)url; (void)text; (void)error; (void)size;
    HANDLE source = CreateFileW(fixture, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE) return false;
    char bytes[16384]; DWORD count, written; bool ok = true;
    while (ReadFile(source, bytes, sizeof(bytes), &count, NULL) && count) {
        if (!WriteFile(file, bytes, count, &written, NULL) || written != count) { ok = false; break; }
    }
    CloseHandle(source); return ok;
}
static void test_reporting(void) {
    StringBuilder query = {0};
    assert(update_query(&query, "day bucket", "0.2.7+dev", "10.0.26200", "arm64"));
    assert(!strcmp(query.data, "/outer-shell/latest/windows-version.txt?heartbeat=day%20bucket&os=windows&osVersion=10.0.26200&arch=arm64&serviceManager=task-scheduler&appVersion=0.2.7%2Bdev"));
    free(query.data);
    wchar_t *native = update_check_path("extra");
    assert(native && wcsstr(native, L"heartbeat=extra&os=windows&osVersion=10.0."));
    assert(!wcsstr(native, L"unknown"));
    USHORT process_machine = 0, native_machine = 0;
    assert(IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine));
    if (native_machine == IMAGE_FILE_MACHINE_ARM64) assert(wcsstr(native, L"&arch=arm64&"));
    if (native_machine == IMAGE_FILE_MACHINE_AMD64) assert(wcsstr(native, L"&arch=x86_64&"));
    if (native_machine == IMAGE_FILE_MACHINE_I386) assert(wcsstr(native, L"&arch=x86&"));
    wprintf(L"%ls\n", native); free(native);
    char temp[MAX_PATH], directory[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    assert(GetTempFileNameA(temp, "ous", 0, directory));
    assert(DeleteFileA(directory) && CreateDirectoryA(directory, NULL));
    assert(update_due_heartbeat(directory, update_now()) == 2);
    update_mark_heartbeat(directory, 2);
    ULONGLONG now = update_now(), day = 24ULL * 60 * 60 * 10000000;
    assert(update_due_heartbeat(directory, now) == -1);
    assert(update_due_heartbeat(directory, now + day) == 0);
    assert(update_due_heartbeat(directory, now + 7 * day) == 1);
    assert(update_due_heartbeat(directory, now + 30 * day) == 2);
    assert(update_due_heartbeat(directory, now - day) == -1);
    for (unsigned i = 0; i < 3; i++) {
        char *path = update_marker(directory, i); assert(path && DeleteFileA(path)); free(path);
    }
    update_mark_heartbeat(directory, 1);
    assert(update_due_heartbeat(directory, update_now()) == 2);
    for (unsigned i = 0; i < 2; i++) {
        char *path = update_marker(directory, i); assert(path && DeleteFileA(path)); free(path);
    }
    assert(RemoveDirectoryA(directory));
    puts("PASS update query encoding, native platform metadata, and persisted heartbeat scheduling");
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--apply-update")) return update_apply();
    InitializeCriticalSection(&update_gate);
    assert(update_newer("0.2.10", "0.2.9"));
    assert(!update_newer("0.2.7", "0.2.7"));
    assert(!update_newer("0.1.9", "0.2.0"));
    assert(update_valid_version("0.2.7"));
    assert(!update_valid_version("../../setup.exe"));
    if (argc == 2 && !wcscmp(argv[1], L"reporting")) { test_reporting(); return 0; }
    if (argc != 3) return 2;
    fixture = argv[2];
    HANDLE file = CreateFileW(fixture, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 3;
    bool trusted = update_verify(fixture, file);
    CloseHandle(file);
    if (!wcscmp(argv[1], L"reject")) return trusted ? 4 : 0;
    if (!trusted || !update_installer_version(fixture, OUTERSHELL_VERSION) || update_installer_version(fixture, "65535.65535.65535")) return 5;
    if (!wcscmp(argv[1], L"verify")) { puts("PASS version comparison, publisher verification and installer version binding"); return 0; }
    snprintf(update_version, sizeof(update_version), "%s", OUTERSHELL_VERSION);
    char error[512] = "";
    if (!update_stage(fixture_download, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 6; }
    puts("PASS staged signed update and launched independent task");
    return 0;
}
