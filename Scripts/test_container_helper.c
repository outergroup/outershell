// Tests Resources/ContainerHelper.h: its SHA-256, and running a helper in a
// Docker container, uploading it on first use.
//
//   cc -std=gnu17 -Wall -Wextra -o test_container_helper Scripts/test_container_helper.c
//   ./test_container_helper HELPER_DIRECTORY HELPER_NAME CONTAINER [--baked]
//
// With --baked, the container's image was built with the helper, so the first
// run must find it rather than upload it.
// HELPER_DIRECTORY contains linux-<arch>/HELPER_NAME. The helper must write
// "FLH2", "PTH2" or similar as the first 4 bytes after starting with `serve`.
#include "../Resources/ContainerHelper.h"
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int failures;
static void check(const char *name, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

static bool read_line(int fd, char *line, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        if (read(fd, &c, 1) != 1) return false;
        if (c == '\n') { line[used] = 0; return true; }
        line[used++] = c;
    }
    return false;
}

static bool write_all(int fd, const unsigned char *data, size_t length) {
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n <= 0) return false;
        data += n;
        length -= (size_t)n;
    }
    return true;
}

// Runs the helper once and returns the first reply character, or 0.
static char run(const ContainerHelper *h, const char *container, const char *user, char *magic) {
    const char *args[32] = {"docker", "exec", "-i"};
    size_t n = 3;
    if (user) { args[n++] = "--user"; args[n++] = user; }
    args[n++] = container;
    const char *serve[] = {"serve", NULL};
    n = container_helper_command(h, args, n, 32, serve);
    int input[2], output[2];
    if (!n || pipe(input) || pipe(output)) return 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], 1);
    posix_spawn_file_actions_addclose(&actions, input[1]);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    pid_t pid;
    if (posix_spawnp(&pid, "docker", &actions, NULL, (char **)args, environ)) return 0;
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    close(output[1]);
    char line[512], message[512], result = 0;
    int architecture = 0;
    if (read_line(output[0], line, sizeof(line))) {
        int reply = container_helper_reply(line, &architecture, message, sizeof(message));
        if (reply == 2 && !write_all(input[1], h->data[architecture], h->length[architecture])) reply = 0;
        if (reply && read(output[0], magic, 4) == 4) result = line[0];
        if (!reply) printf("     %s\n", message);
    }
    close(input[1]);
    close(output[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return result;
}

int main(int argc, char **argv) {
    char hex[65];
    container_helper_sha256((const unsigned char *)"", 0, hex);
    check("SHA-256 of nothing", !strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    container_helper_sha256((const unsigned char *)"abc", 3, hex);
    check("SHA-256 of abc", !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    unsigned char block[1000];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (unsigned char)i;
    container_helper_sha256(block, 56, hex);
    check("SHA-256 across a padding boundary", !strcmp(hex, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"));
    container_helper_sha256(block, sizeof(block), hex);
    check("SHA-256 of several blocks", !strcmp(hex, "a8af099bf2e878609558dbf69d8f88f4a31040a8cf84b549a0cfa912f12ffc3f"));
    if (argc != 4 && argc != 5) return failures ? 1 : 0;
    bool baked = argc == 5 && !strcmp(argv[4], "--baked");

    ContainerHelper h;
    check("loads the helpers", container_helper_load(&h, "org.outershell.Test", argv[2], argv[1]));
    char magic[5] = {0};
    if (baked) {
        check("finds the helper built into the image", run(&h, argv[3], NULL, magic) == 'R');
        return failures ? 1 : 0;
    }
    check("uploads on first use", run(&h, argv[3], NULL, magic) == 'U');
    printf("     helper said %.4s\n", magic);
    memset(magic, 0, sizeof(magic));
    check("reuses the uploaded helper", run(&h, argv[3], NULL, magic) == 'R' && magic[0]);
    check("uploads for another user", run(&h, argv[3], "nobody", magic) == 'U');
    return failures ? 1 : 0;
}
