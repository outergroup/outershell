#include "Icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    char *fixture = win_utf8(argv[1]); StringBuilder png = {0};
    assert(win_read_file(fixture, 48 * 1024, &png)); free(fixture);
    char temp[MAX_PATH], directory[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp) && GetTempFileNameA(temp, "ico", 0, directory));
    assert(DeleteFileA(directory) && CreateDirectoryA(directory, NULL) && icons_init(directory));
    const char *first[] = {"native", "app", "http://localhost:6006/"};
    const char *other[] = {"guest", "app", "http://localhost:6006/"};
    const char *changed[] = {"native", "app", "http://localhost:6007/"};
    StringBuilder image = {0}; char token[33], again[33], second[33];
    icons_describe(first, 3, &image, token); assert(strlen(token) == 32 && !image.length);
    icons_describe(first, 3, &image, again); assert(!strcmp(token, again));
    icons_describe(other, 3, &image, second); assert(strlen(second) == 32 && strcmp(token, second));
    assert(icons_save("00000000000000000000000000000000", png.data, png.length) == 403);
    assert(icons_save(token, "bad", 3) == 400);
    assert(icons_save(token, png.data, 48 * 1024 + 1) == 400);
    assert(icons_save(token, png.data, 8) == 400);
    assert(icons_save(token, png.data, png.length) == 200);
    icons_describe(first, 3, &image, again);
    assert(!again[0] && image.length == png.length && !memcmp(image.data, png.data, png.length));
    free(image.data); image = (StringBuilder){0};
    icons_describe(changed, 3, &image, again); assert(!image.length && strcmp(token, again));
    assert(icons_init(directory));
    assert(icons_save(token, png.data, png.length) == 403);
    icons_describe(first, 3, &image, again); assert(!again[0] && image.length == png.length);
    free(image.data); image = (StringBuilder){0};
    icons_describe(other, 3, &image, again); assert(!image.length && again[0]);
    char *root = win_join(directory, "discovered-icons"), *pattern = win_join(root, "*.png");
    WIN32_FIND_DATAA found; HANDLE search = FindFirstFileA(pattern, &found);
    assert(search != INVALID_HANDLE_VALUE);
    do { char *path = win_join(root, found.cFileName); assert(DeleteFileA(path)); free(path); } while (FindNextFileA(search, &found));
    FindClose(search); assert(RemoveDirectoryA(root) && RemoveDirectoryA(directory));
    free(root); free(pattern); free(png.data);
    puts("PASS scoped tokens, malformed and oversized PNG rejection, endpoint changes, persisted icons, and token expiry after restart");
    return 0;
}
