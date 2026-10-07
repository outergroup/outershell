#define main outershelld_main
#include "../outershelld/outershelld.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    snprintf(g_bundled_apps_directory, sizeof(g_bundled_apps_directory), "%s/bundled-apps", argv[1]);
    for (size_t i = 0; i < sizeof(kBundledApps) / sizeof(kBundledApps[0]); i++) {
        const BundledAppDefinition *app = &kBundledApps[i];
        for (int installed = 0; installed < 2; installed++) {
            StringBuilder frontends = {0}, logs = {0}, payload = {0};
            assert(build_empty_array_payload(&frontends));
            assert(build_empty_array_payload(&logs));
            assert(build_backend_payload(app->service_id, app->display_name, "", "", "user",
                installed ? "stopped" : "available",
                BACKEND_FLAG_IS_BUNDLED | (installed ? BACKEND_FLAG_IS_INSTALLED : 0),
                "", "", "", "", "", "", &frontends, &logs, &payload));
            const unsigned char *bytes = (const unsigned char *)payload.data;
            uint32_t offset = read_uint32_le(bytes + 116);
            uint32_t length = read_uint32_le(bytes + 120);
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/web/icons/%s.png", argv[1], app->service_id);
            size_t expected_length = 0;
            char *expected = read_text_file_alloc(path, &expected_length);
            assert(expected && length == expected_length && length > 0);
            assert(offset + length <= payload.length);
            assert(memcmp(bytes + offset, expected, length) == 0);
            snprintf(path, sizeof(path), "%s/%s-%d.bin", argv[2], app->service_id, installed);
            FILE *out = fopen(path, "wb"); assert(out);
            assert(fwrite(payload.data, 1, payload.length, out) == payload.length);
            fclose(out);
            free(expected); free(frontends.data); free(logs.data); free(payload.data);
        }
    }
    puts("PASS: canonical catalog PNGs in installed and uninstalled backend records");
    return 0;
}
