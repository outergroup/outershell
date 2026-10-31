#ifndef OUTER_APP_ENVIRONMENT_H
#define OUTER_APP_ENVIRONMENT_H
#define ENVIRONMENT_HELPER_H
#include "AppProcess.h"
#include "Runtime.h"
#include <bcrypt.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define MAX_FRAME 65536u
#define MAX_DATA (16u * 1024u * 1024u)
#define MAX_TEXT (4u * 1024u * 1024u)
#define PATH_MAX 32768
#define strcasecmp _stricmp
#define strdup _strdup
#define sysconf(x) 100
#define _SC_CLK_TCK 0
#define O_CLOEXEC 0
#define LOCK_EX 1
typedef unsigned uid_t;
typedef struct {
    unsigned char *data;
    size_t size;
} Bytes;
#include "binary.h"
static _Thread_local char failure[1024];
static double deadline;
static void fail(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(failure, sizeof(failure), fmt, a);
    va_end(a);
}
static void *allocate(size_t size) { return win_alloc(size ? size : 1); }
static char *format(const char *fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = _vscprintf(fmt, a);
    va_end(a);
    if (n < 0) {
        va_end(b);
        return NULL;
    }
    char *p = allocate((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, b);
    va_end(b);
    return p;
}
static bool append(Bytes *b, const void *p, size_t n, size_t limit) {
    if (n > limit || b->size > limit - n) {
        fail("Size limit exceeded.");
        return false;
    }
    void *next = realloc(b->data, b->size + n + 1);
    if (!next) {
        fail("Out of memory.");
        return false;
    }
    b->data = next;
    if (n)
        memcpy(b->data + b->size, p, n);
    b->size += n;
    b->data[b->size] = 0;
    return true;
}
static void bytes_free(Bytes *b) {
    free(b->data);
    *b = (Bytes){0};
}
static double now(void) { return (double)GetTickCount64() / 1000; }
long clock_ticks(void) { return 100; }
static bool utf8(const unsigned char *p, size_t n) {
    return !n || (n <= INT_MAX && !memchr(p, 0, n) &&
                  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      (const char *)p, (int)n, NULL, 0) > 0);
}
static void digest(const void *p, size_t n, char out[65]) {
    unsigned char hash[32];
    NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)p,
                                 (ULONG)n, hash, 32);
    if (status < 0) {
        fail("Could not hash document.");
        out[0] = 0;
        return;
    }
    for (size_t i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
}
static bool expand_path(const char *path, char result[PATH_MAX]) {
    char *home = NULL, *expanded = NULL;
    if (!*path || !strcmp(path, "~") || !strncmp(path, "~/", 2) ||
        !strncmp(path, "~\\", 2)) {
        home = win_folder(CSIDL_PROFILE);
        expanded =
            home ? win_join(home, strlen(path) > 1 ? path + 2 : "") : NULL;
        path = expanded;
    }
    bool absolute =
        path && ((strlen(path) >= 3 && path[1] == ':' &&
                  (path[2] == '/' || path[2] == '\\')) ||
                 (!strncmp(path, "\\\\", 2) || !strncmp(path, "//", 2)));
    if (path && (!strncmp(path, "\\\\.\\", 4) || !strncmp(path, "\\\\?\\", 4)))
        absolute = false;
    if (path) {
        for (const unsigned char *p = (const unsigned char *)path; *p; p++)
            if (*p < 32 || strchr("*?\"<>|", *p) ||
                (*p == ':' && p != (const unsigned char *)path + 1))
                absolute = false;
    }
    wchar_t *wide = absolute ? win_wide(path) : NULL;
    wchar_t full[PATH_MAX];
    DWORD n = wide ? GetFullPathNameW(wide, PATH_MAX, full, NULL) : 0;
    free(wide);
    free(home);
    free(expanded);
    char *value = n && n < PATH_MAX ? win_utf8(full) : NULL;
    if (!value || strlen(value) >= PATH_MAX) {
        free(value);
        fail("Choose an absolute Windows path.");
        return false;
    }
    if (!strncmp(value, "\\\\.\\", 4) || !strncmp(value, "\\\\?\\", 4)) {
        free(value);
        fail("Choose a file or folder, not a Windows device path.");
        return false;
    }
    strcpy(result, value);
    free(value);
    for (char *p = result; *p; p++)
        if (*p == '\\')
            *p = '/';
    return true;
}
static Bytes read_file(const char *path, size_t limit, bool regular) {
    Bytes b = {0};
    wchar_t *wide = win_wide(path);
    HANDLE f = wide
                   ? CreateFileW(
                         wide, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                   : INVALID_HANDLE_VALUE;
    free(wide);
    DWORD error = GetLastError();
    if (f == INVALID_HANDLE_VALUE) {
        errno = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    ? ENOENT
                    : EACCES;
        fail("Cannot read file (Windows error %lu).", error);
        return b;
    }
    LARGE_INTEGER size;
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileType(f) == FILE_TYPE_DISK && GetFileSizeEx(f, &size) &&
              size.QuadPart >= 0 && (uint64_t)size.QuadPart <= limit &&
              GetFileInformationByHandle(f, &info) &&
              (!regular || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    if (ok) {
        b.data = allocate((size_t)size.QuadPart + 1);
        DWORD n = 0;
        ok = ReadFile(f, b.data, (DWORD)size.QuadPart, &n, NULL) &&
             n == size.QuadPart;
        b.size = n;
    }
    CloseHandle(f);
    if (!ok) {
        bytes_free(&b);
        fail("Cannot read file or file exceeds size limit.");
    }
    return b;
}
static bool mkdirs(const char *path) {
    wchar_t *wide = win_wide(path);
    int n = wide ? SHCreateDirectoryExW(NULL, wide, NULL) : ERROR_INVALID_NAME;
    free(wide);
    if (n != ERROR_SUCCESS && n != ERROR_ALREADY_EXISTS) {
        fail("Cannot create state directory (%d).", n);
        return false;
    }
    return true;
}
static bool write_all(int fd, const void *p, size_t n) {
    while (n) {
        int k = _write(fd, p, (unsigned)(n > 65536 ? 65536 : n));
        if (k <= 0) {
            fail("Could not write file.");
            return false;
        }
        p = (const char *)p + k;
        n -= (size_t)k;
    }
    return true;
}
static int win_open(const char *path, int flags, ...) {
    wchar_t *w = win_wide(path);
    int fd =
        w ? _wopen(w, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE)
          : -1;
    free(w);
    return fd;
}
static int win_unlink(const char *path) {
    wchar_t *w = win_wide(path);
    int n = w ? _wunlink(w) : -1;
    free(w);
    return n;
}
static int win_rename(const char *from, const char *to) {
    wchar_t *a = win_wide(from), *b = win_wide(to);
    bool ok =
        a && b &&
        MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok)
        errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
    free(a);
    free(b);
    return ok ? 0 : -1;
}
static int mkstemp(char *path) {
    size_t n = strlen(path);
    if (n < 6)
        return -1;
    for (int attempt = 0; attempt < 32; attempt++) {
        unsigned char random[6];
        if (BCryptGenRandom(NULL, random, 6, BCRYPT_USE_SYSTEM_PREFERRED_RNG) <
            0)
            return -1;
        for (size_t i = 0; i < 6; i++)
            path[n - 6 + i] =
                "abcdefghijklmnopqrstuvwxyz0123456789"[random[i] % 36];
        int fd = win_open(path, _O_CREAT | _O_EXCL | _O_RDWR);
        if (fd >= 0)
            return fd;
    }
    return -1;
}
static int flock(int fd, int operation) {
    (void)operation;
    OVERLAPPED o = {0};
    return LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK, 0,
                      MAXDWORD, MAXDWORD, &o)
               ? 0
               : -1;
}
#define open win_open
#define unlink win_unlink
#define rename win_rename
#define close _close
#define fsync _commit
#endif
