#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

static void check_status(LaunchdStatusEntry *entry, const char *fixture, const char *text, const char *expected) {
    FILE *file = fopen(fixture, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
    read_launchd_status(entry);
    if (strcmp(entry->status, expected)) {
        fprintf(stderr, "Expected %s, got %s\n", expected, entry->status);
        abort();
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    StringBuilder plist = {0};
    assert(make_bundled_launchd_plist("org.outershell.Test", "/tmp/backend", "", "",
        "/launchd/user/org.outershell.Test", 0600, "/tmp", "/tmp/api", "/tmp/log", "", &plist));
    assert(strstr(plist.data, "<key>SecureSocketWithKey</key>"));
    assert(strstr(plist.data, "OUTERSHELL_SOCKET_org.outershell.Test"));
    assert(strstr(plist.data, "/launchd/user/org.outershell.Test"));
    assert(!strstr(plist.data, "<key>SockPathName</key>"));
    free(plist.data);
    char fixture[PATH_MAX], socket_path[PATH_MAX], text[8192];
    snprintf(fixture, sizeof(fixture), "%s/fixture", argv[1]);
    snprintf(socket_path, sizeof(socket_path), "%s/socket", argv[1]);
    setenv("OUTERSHELL_TEST_LAUNCHD_OUTPUT", fixture, 1);
    LaunchdStatusEntry entry = {0};
    strcpy(entry.label, "org.outershell.Test");
    snprintf(entry.plist_path, sizeof(entry.plist_path), "%s/plist", argv[1]);
    fclose(fopen(entry.plist_path, "w"));
    check_status(&entry, fixture, "\tstate = not running\n\tlast exit code = 0\n", "stopped");
    check_status(&entry, fixture, "\tstate = spawn scheduled\n\tlast exit code = 1\n\t\t\tpassive = 1\n", "failed");
    check_status(&entry, fixture, "\tstate = running\n\tlast exit code = 1\n", "running");
    snprintf(text, sizeof(text), "\tstate = spawn scheduled\n\tlast exit code = 1\n\t\t\tpath = %s\n\t\t\tpassive = 1\n", socket_path);
    check_status(&entry, fixture, text, "socket unavailable");
    fclose(fopen(socket_path, "w"));
    check_status(&entry, fixture, text, "socket unavailable");
    unlink(socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    assert(strlen(socket_path) < sizeof(address.sun_path));
    strcpy(address.sun_path, socket_path);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    snprintf(text, sizeof(text), "\tstate = waiting\n\tlast exit code = 0\n\t\t\tpath = %s\n\t\t\tpassive = 1\n", socket_path);
    check_status(&entry, fixture, text, "available");
    close(fd);
    unlink(socket_path);
    check_status(&entry, fixture, text, "socket unavailable");
    g_launchd_status_entries = &entry;
    invalidate_launchd_status();
    assert(strcmp(entry.status, "unknown") == 0);
    g_launchd_status_entries = NULL;
    setenv("OUTERSHELL_HOME", argv[1], 1);
    entry.can_suspend = true;
    for (int run = 100; run <= 102; run++) {
        snprintf(text, sizeof(text), "\tstate = spawn scheduled\n\truns = %d\n\tlast exit code = 1\n", run);
        check_status(&entry, fixture, text, "failed");
        if (run == 100) {
            assert(entry.failed_launches == 1);
            check_status(&entry, fixture, text, "failed");
            assert(entry.failed_launches == 1);
            assert(!suspend_failing_launchd_service(&entry));
        }
    }
    assert(entry.failed_launches == 3);
    entry.can_suspend = false;
    assert(!suspend_failing_launchd_service(&entry));
    entry.can_suspend = true;
    LaunchdStatusEntry system = entry;
    strcpy(system.plist_path, "/Library/LaunchDaemons/test.plist");
    assert(!suspend_failing_launchd_service(&system));
    LaunchdStatusEntry shell = entry;
    strcpy(shell.label, "org.outershell.OuterShell");
    assert(!suspend_failing_launchd_service(&shell));
    assert(suspend_failing_launchd_service(&entry));
    assert(launchd_service_is_suspended(&entry));
    assert(strcmp(entry.status, "suspended after failures") == 0);
    uint64_t commands_token = file_state_token(getenv("OUTERSHELL_TEST_LAUNCHD_COMMANDS"));
    LaunchdStatusEntry restored = {0};
    strcpy(restored.label, entry.label);
    strcpy(restored.plist_path, entry.plist_path);
    restored.can_suspend = true;
    read_launchd_status(&restored);
    assert(strcmp(restored.status, "suspended after failures") == 0);
    assert(commands_token == file_state_token(getenv("OUTERSHELL_TEST_LAUNCHD_COMMANDS")));
    g_launchd_status_entries = &entry;
    char message[4096] = "";
    assert(run_launchd_operation(entry.label, entry.plist_path, "start", message, sizeof(message)));
    assert(!launchd_service_is_suspended(&entry));
    assert(entry.failed_launches == 0);
    entry.failed_launches = 3;
    strcpy(entry.status, "failed");
    setenv("OUTERSHELL_TEST_BOOTOUT_FAILURE", "1", 1);
    assert(!suspend_failing_launchd_service(&entry));
    assert(!launchd_service_is_suspended(&entry));
    commands_token = file_state_token(getenv("OUTERSHELL_TEST_LAUNCHD_COMMANDS"));
    assert(!suspend_failing_launchd_service(&entry));
    assert(commands_token == file_state_token(getenv("OUTERSHELL_TEST_LAUNCHD_COMMANDS")));
    unsetenv("OUTERSHELL_TEST_BOOTOUT_FAILURE");
    entry.last_suspend_attempt_ms = 0;
    assert(suspend_failing_launchd_service(&entry));
    setenv("OUTERSHELL_TEST_BOOTSTRAP_FAILURE", "1", 1);
    assert(!run_launchd_operation(entry.label, entry.plist_path, "start", message, sizeof(message)));
    assert(!launchd_service_is_suspended(&entry));
    g_launchd_status_entries = NULL;
    puts("PASS: launch failures, three-launch threshold, persistence, quiet monitoring, Retry, failed retries, scope limits and suspension failure backoff");
    return 0;
}
