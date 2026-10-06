#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool string_is(const unsigned char *payload, size_t offset, const char *value) {
    uint32_t start = read_u32(payload + offset);
    uint32_t length = read_u32(payload + offset + 4);
    return strlen(value) == length && memcmp(payload + start, value, length) == 0;
}

static void check_catalog(bool user_installed, bool root_installed) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    send_backends_response(sockets[0]);
    shutdown(sockets[0], SHUT_WR);
    unsigned char response[65536];
    size_t length = 0;
    ssize_t count;
    while ((count = read(sockets[1], response + length, sizeof(response) - length - 1)) > 0) length += count;
    response[length] = 0;
    close(sockets[0]);
    close(sockets[1]);
    char *header_end = strstr((char *)response, "\r\n\r\n");
    assert(header_end);
    const unsigned char *payload = (unsigned char *)header_end + 4;
    uint32_t records = read_u32(payload + 8);
    int user_count = 0, root_count = 0;
    for (uint32_t i = 0; i < records; i++) {
        const unsigned char *record = payload + read_u32(payload + 12 + i * 8);
        if (!string_is(record, 0, "org.outershell.Top")) continue;
        bool installed = (read_u32(record + 64) & BACKEND_FLAG_IS_INSTALLED) != 0;
        if (string_is(record, 32, "user")) {
            user_count++;
            assert(installed == user_installed);
        } else if (string_is(record, 32, "system")) {
            root_count++;
            assert(installed == root_installed);
        } else assert(false);
    }
    assert(user_count == 1);
    assert(root_count == 1);
}

static void register_app(const char *path, const char *plist) {
    RegistryStore database;
    char error[1024] = "";
    assert(registry_store_open_at(&database, path, true, error, sizeof(error)));
    assert(registry_store_upsert_backend(&database, "org.outershell.Top", "Top",
                                        "org.outershell.Top.service", plist, true));
    assert(registry_store_close(&database, true, error, sizeof(error)));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(geteuid() != 0);
    assert(setenv("HOME", argv[1], 1) == 0);
    assert(setenv("OUTERSHELL_HOME", argv[1], 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", argv[1], 1) == 0);
    assert(setenv("XDG_CONFIG_HOME", argv[1], 1) == 0);
    snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s/user/registry.orwa", argv[1]);
    snprintf(g_system_registry_database_path, sizeof(g_system_registry_database_path), "%s/system/registry.orwa", argv[1]);
    register_app(g_registry_database_path, "");
    char error[1024] = "";
    assert(unregister_backend_records("org.outershell.Top", error, sizeof(error)));
    check_catalog(false, false);
    register_app(g_system_registry_database_path, "/Library/LaunchDaemons/org.outershell.Top.plist");
    check_catalog(false, true);
#ifdef __APPLE__
    char user_plist[PATH_MAX];
    snprintf(user_plist, sizeof(user_plist), "%s/Library/LaunchAgents/org.outershell.Top.plist", argv[1]);
    register_app(g_registry_database_path, user_plist);
#else
    const BundledAppDefinition *app = bundled_app_for_service_id("org.outershell.Top");
    char stage[PATH_MAX], path[PATH_MAX], architecture[64];
    snprintf(stage, sizeof(stage), "%s/stage", argv[1]);
    assert(remote_machine_architecture(architecture, sizeof(architecture)));
    snprintf(path, sizeof(path), "%s/%s/%s/TopBackend", stage, remote_linux_binary_directory(), architecture);
    assert(write_text_file(path, "#!/bin/sh\nexit 0\n", error, sizeof(error)));
    assert(chmod(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/bundles/TopContent.bundle.macos-arm.aar", stage);
    assert(write_text_file(path, "arm", error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/bundles/TopContent.bundle.macos-x86.aar", stage);
    assert(write_text_file(path, "x86", error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/app-icon.png", stage);
    assert(write_text_file(path, "icon", error, sizeof(error)));
    bool install_needs_password = false;
    assert(install_bundled_app(app, "user", stage, NULL, &install_needs_password, error, sizeof(error)));
    assert(!install_needs_password);
    snprintf(path, sizeof(path), "%s/apps/org.outershell.Top/TopBackend", argv[1]);
    struct stat binary_info;
    assert(lstat(path, &binary_info) == 0 && S_ISREG(binary_info.st_mode));
#endif
    check_catalog(true, true);
    char user_payload[PATH_MAX], root_sentinel[PATH_MAX];
    default_user_outershell_app_root("org.outershell.Top", user_payload, sizeof(user_payload));
    assert(mkdir_p(user_payload));
    snprintf(root_sentinel, sizeof(root_sentinel), "%s/root-payload", argv[1]);
    assert(write_text_file(root_sentinel, "root remains installed", error, sizeof(error)));
    bool needs_password = false;
    assert(uninstall_backend("org.outershell.Top", "user", NULL, &needs_password, error, sizeof(error)));
    assert(!needs_password);
    assert(access(user_payload, F_OK) != 0);
    assert(access(root_sentinel, F_OK) == 0);
    check_catalog(false, true);
    char root_registry[PATH_MAX];
    snprintf(root_registry, sizeof(root_registry), "%s", g_system_registry_database_path);
    snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s", root_registry);
    assert(unregister_backend_records("org.outershell.Top", error, sizeof(error)));
    snprintf(g_registry_database_path, sizeof(g_registry_database_path), "%s/user/registry.orwa", argv[1]);
    #ifdef __APPLE__
    register_app(g_registry_database_path, user_plist);
#else
    register_app(g_registry_database_path, "");
#endif
    check_catalog(true, false);
    assert(uninstall_backend("org.outershell.Top", "system", NULL, &needs_password, error, sizeof(error)));
    check_catalog(true, false);
    puts("PASS: independent scope catalogs and scoped uninstall preserve the other installation");
    return 0;
}
