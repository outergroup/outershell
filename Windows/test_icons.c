#include "Icons.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    char *fixture = win_utf8(argv[1]); StringBuilder png = {0};
    assert(win_read_file(fixture, 48 * 1024, &png)); free(fixture);

    char name[69];
    assert(icons_endpoint_file_name("\\\\.\\PIPE\\Example", NULL, 0, name));
    assert(!strcmp(name, "e28663b1c31ca2f3597a33e07b6912cf20a15c846dfddbcd8f30eaaa52283489.png"));
    assert(icons_endpoint_file_name(NULL, "127.0.0.1", 6006, name));
    assert(!strcmp(name, "57cadb02e51f395b58595fe590195d2f5b1a921b18f35ee60ea5fbfbb4cb19b5.png"));
    assert(icons_endpoint_file_name("", "LocalHost", 6006, name));
    assert(!strcmp(name, "57cadb02e51f395b58595fe590195d2f5b1a921b18f35ee60ea5fbfbb4cb19b5.png"));
    assert(!icons_endpoint_file_name("C:\\app.sock", NULL, 0, name));
    assert(!icons_endpoint_file_name(NULL, NULL, 0, name));
    assert(!icons_endpoint_file_name(NULL, "a/b", 80, name));

    char pipe[128];
    snprintf(pipe, sizeof(pipe), "\\\\.\\pipe\\outershell-icon-test-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    assert(icons_endpoint_file_name(pipe, NULL, 0, name));
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *outerssh = win_join(local, "OuterSSH");
    char *directory = win_join(outerssh, "endpoint-icons");
    char *path = win_join(directory, name);
    CreateDirectoryA(outerssh, NULL);
    CreateDirectoryA(directory, NULL);
    StringBuilder image = {0};
    assert(!icons_endpoint(pipe, NULL, 0, &image) && !image.length);
    assert(win_atomic_write(path, "not a png", 9));
    assert(!icons_endpoint(pipe, NULL, 0, &image) && !image.length);
    assert(win_atomic_write(path, png.data, png.length));
    assert(icons_endpoint(pipe, NULL, 0, &image));
    assert(image.length == png.length && !memcmp(image.data, png.data, png.length));
    assert(DeleteFileA(path));
    free(image.data); free(png.data); free(path); free(directory); free(outerssh); free(local);
    puts("PASS endpoint icon file names, missing and invalid cache entries, and cached icon reads");
    return 0;
}
