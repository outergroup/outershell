static int registry_binary_lock(const char *path, int operation, char *error, size_t error_size) {
    char name[PATH_MAX];
    if (!registry_binary_lock_path(path, name, sizeof(name))) return -1;
    wchar_t *wide = windows_path(name);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        snprintf(error, error_size, "Could not open registry lock (Windows error %lu).", GetLastError()); return -1;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (fd < 0) { CloseHandle(handle); return -1; }
    if (flock(fd, operation)) { snprintf(error, error_size, "Could not lock registry."); _close(fd); return -1; }
    return fd;
}

static bool registry_binary_write_file(const char *path, const void *data, size_t length, char *error, size_t error_size) {
    if (length > 32 * 1024 * 1024) { snprintf(error, error_size, "Registry exceeds the 32 MiB size limit."); return false; }
    bool own_lock = g_registry_write_lock_fd < 0 || strcmp(g_registry_write_lock_path, path);
    int lock = own_lock ? registry_binary_lock(path, LOCK_EX, error, error_size) : g_registry_write_lock_fd;
    if (lock < 0) return false;
    unsigned char random[16];
    bool ok = BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
    char temporary[PATH_MAX];
    int prefix = snprintf(temporary, sizeof(temporary), "%s.tmp.", path);
    if (prefix < 0 || (size_t)prefix + 33 > sizeof(temporary)) ok = false;
    if (ok) {
        for (size_t i = 0; i < sizeof(random); i++) snprintf(temporary + prefix + i * 2, 3, "%02x", random[i]);
    }
    wchar_t *target = ok ? windows_path(path) : NULL;
    wchar_t *staging = ok ? windows_path(temporary) : NULL;
    HANDLE file = staging ? CreateFileW(staging, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (!target || file == INVALID_HANDLE_VALUE) ok = false;
    size_t offset = 0;
    while (ok && offset < length) {
        DWORD count = 0, wanted = (DWORD)(length - offset > 65536 ? 65536 : length - offset);
        ok = WriteFile(file, (const char *)data + offset, wanted, &count, NULL) && count != 0;
        offset += count;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    DWORD saved_error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (ok && !MoveFileExW(staging, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ok = false; saved_error = GetLastError(); }
    if (!ok) {
        if (staging) DeleteFileW(staging);
        snprintf(error, error_size, "Could not commit registry (Windows error %lu).", saved_error);
    }
    free(target); free(staging);
    if (own_lock) { flock(lock, LOCK_UN); _close(lock); }
    return ok;
}
