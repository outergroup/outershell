// cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/test_endpoint_icons Scripts/test_endpoint_icons.c && /tmp/test_endpoint_icons
#include "../outershelld/EndpointIcons.h"
#undef NDEBUG
#include <assert.h>

static void write_file(const char *path, const void *bytes, size_t length, mode_t mode) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    assert(fd >= 0 && write(fd, bytes, length) == (ssize_t)length && close(fd) == 0);
    assert(chmod(path, mode) == 0);
}

int main(void) {
    char line[256], name[65], path[PATH_MAX], directory[PATH_MAX], other[PATH_MAX + 16];
    assert(endpoint_icon_record_line("/run/user/1000/app", NULL, 0, line, sizeof(line)));
    endpoint_icon_sha256_hex(line, name);
    assert(!strcmp(name, "476840428b67c48d6ee75d7335f2c8e0454c264c96b3541c76e7d310628fa072"));
    const char *loopbacks[] = {NULL, "", "LocalHost", "127.0.0.1", "::1", "[::1]"};
    for (size_t i = 0; i < sizeof(loopbacks) / sizeof(*loopbacks); i++) {
        assert(endpoint_icon_record_line("", loopbacks[i], 6006, line, sizeof(line)));
        endpoint_icon_sha256_hex(line, name);
        assert(!strcmp(name, "57cadb02e51f395b58595fe590195d2f5b1a921b18f35ee60ea5fbfbb4cb19b5"));
    }
    assert(endpoint_icon_record_line(NULL, "Box.Example", 80, line, sizeof(line)) && !strcmp(line, "tcp box.example 80"));
    assert(!endpoint_icon_record_line("relative.sock", NULL, 0, line, sizeof(line)));
    assert(!endpoint_icon_record_line(NULL, NULL, 0, line, sizeof(line)));
    assert(!endpoint_icon_record_line(NULL, "a/b", 80, line, sizeof(line)));
    assert(!endpoint_icon_record_line(NULL, NULL, 65536, line, sizeof(line)));

    char root[] = "/tmp/outershell-endpoint-icons-XXXXXX";
    assert(mkdtemp(root));
    setenv("HOME", root, 1);
    setenv("XDG_CONFIG_HOME", root, 1);
    assert(endpoint_icon_directory(directory, sizeof(directory)));
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    char command[PATH_MAX + 32];
    snprintf(command, sizeof(command), "mkdir -p '%s'", directory);
    assert(system(command) == 0 && chmod(directory, 0700) == 0);

    static const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
    assert(endpoint_icon_record_line("/app.sock", NULL, 0, line, sizeof(line)));
    endpoint_icon_sha256_hex(line, name);
    char expected[PATH_MAX + 80];
    snprintf(expected, sizeof(expected), "%s/%s.png", directory, name);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    write_file(expected, png, sizeof(png), 0600);
    assert(endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)) && !strcmp(path, expected));
    assert(!endpoint_icon_cached_path("/other.sock", NULL, 0, path, sizeof(path)));

    assert(chmod(expected, 0620) == 0);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    assert(chmod(expected, 0600) == 0 && chmod(directory, 0770) == 0);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    assert(chmod(directory, 0700) == 0);
    write_file(expected, "GIF89a-not-png", 14, 0600);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    static unsigned char large[ENDPOINT_ICON_MAXIMUM_BYTES + 1];
    memcpy(large, png, sizeof(png));
    write_file(expected, large, sizeof(large), 0600);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));
    snprintf(other, sizeof(other), "%s/target.png", root);
    write_file(other, png, sizeof(png), 0600);
    assert(unlink(expected) == 0 && symlink(other, expected) == 0);
    assert(!endpoint_icon_cached_path("/app.sock", NULL, 0, path, sizeof(path)));

    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    assert(system(command) == 0);
    puts("PASS endpoint icon cache names, loopback aliases, ownership and permission checks, PNG and size validation, symlink rejection");
    return 0;
}
