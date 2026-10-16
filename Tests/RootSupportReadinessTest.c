#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[1], "bootstrap")) {
        if (geteuid() == 0) return 99;
        bool needs_password = false;
        char message[4096] = "";
        bool ok = ensure_root_helper_installed(NULL, &needs_password, message, sizeof(message));
        fputs(message, stdout);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[1], "paths")) {
        char path[PATH_MAX];
        default_system_outershelld_path(path, sizeof(path));
        assert(!strcmp(path, "/opt/outershell/libexec/outershelld"));
        default_system_outerctl_path(path, sizeof(path));
        assert(!strcmp(path, "/opt/outershell/bin/outerctl"));
        system_binary_users_dir(path, sizeof(path));
        assert(!strcmp(path, "/var/lib/outershell/system-binary-users"));
        return 0;
    }
    if (!strcmp(argv[1], "cleanup")) {
        write_system_binary_cleanup_shell(stdout);
        fputs("remove_system_binaries_if_unused\n", stdout);
        return 0;
    }
    if (!strcmp(argv[1], "cleanup-apps")) {
        write_root_apps_marker_cleanup_shell(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "selinux")) {
        write_linux_selinux_functions(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "emit")) {
        char quoted[PATH_MAX * 2 + 16];
        shell_quote(argv[2], quoted, sizeof(quoted));
        fputs("set -eu\n", stdout);
        write_linux_root_support_readiness_check(stdout, quoted);
        fputs("printf 'APP_OPERATION_STARTED\\n'\n", stdout);
        return 0;
    }
    assert(!strcmp(argv[1], "run"));
    bool needs_password = false;
    char message[4096] = "";
    bool ok = run_root_script(argv[2], NULL, &needs_password, message, sizeof(message));
    fputs(message, stdout);
    return ok ? 0 : needs_password ? 2 : 1;
}
