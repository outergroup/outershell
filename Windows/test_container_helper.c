// Runs an app helper in a Docker container through app_helper_start, as the
// Windows apps do: the first start uploads the helper, the second reuses it.
//
//   OuterShellContainerHelperTest.exe CONTAINER HELPER_NAME MAGIC
//
// The helpers folder beside the executable holds linux-<arch>/HELPER_NAME;
// MAGIC is the first 4 bytes the helper writes, such as FLH2.
#include "AppProcess.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(const char *name, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

static bool start(const char *container, const char *name, const char *magic, char *error, size_t capacity) {
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL), job = CreateJobObjectW(NULL, NULL);
    const char *args[32] = {"exec", "--interactive", container, NULL};
    const char *serve[] = {"serve", NULL};
    AppProcess process = {0};
    bool ok = app_helper_start(stop, job, "docker", args, 3, 32, "org.outershell.Test", name, serve, &process,
                               error, capacity);
    char hello[5] = {0};
    if (ok) ok = connection_read(&process.output, hello, 4) && !memcmp(hello, magic, 4);
    if (process.process) app_process_close(&process);
    CloseHandle(job);
    CloseHandle(stop);
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        fputs("usage: OuterShellContainerHelperTest CONTAINER HELPER_NAME MAGIC\n", stderr);
        return 2;
    }
    char *container = win_utf8(argv[1]), *name = win_utf8(argv[2]), *magic = win_utf8(argv[3]);
    char error[512] = "";
    check("uploads and starts the helper", start(container, name, magic, error, sizeof(error)));
    if (error[0]) printf("     %s\n", error);
    error[0] = 0;
    check("starts the helper again", start(container, name, magic, error, sizeof(error)));
    if (error[0]) printf("     %s\n", error);
    return failures ? 1 : 0;
}
