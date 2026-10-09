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
int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--apply-update")) return update_apply();
    InitializeCriticalSection(&update_gate);
    assert(update_newer("0.2.10", "0.2.9"));
    assert(!update_newer("0.2.7", "0.2.7"));
    assert(!update_newer("0.1.9", "0.2.0"));
    assert(update_valid_version("0.2.7"));
    assert(!update_valid_version("../../setup.exe"));
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
