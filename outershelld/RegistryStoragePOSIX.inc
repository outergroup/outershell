static int registry_binary_lock(const char *registry_path, int operation, char *error, size_t error_size) {
    char lock_path[PATH_MAX];
    if (!registry_binary_lock_path(registry_path, lock_path, sizeof(lock_path))) {
        snprintf(error, error_size, "Registry lock path is too long.");
        return -1;
    }
    int fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", lock_path, strerror(errno));
        return -1;
    }
    (void)fchmod(fd, 0666);
    if (flock(fd, operation) != 0) {
        snprintf(error, error_size, "Failed to lock %s: %s", lock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool registry_binary_write_file(const char *path, const void *data, size_t length, char *error, size_t error_size) {
    char temp_path[PATH_MAX];
    bool owns_lock = false;
    int lock_fd = -1;
    if (g_registry_write_lock_fd >= 0 && strcmp(g_registry_write_lock_path, path) == 0) {
        lock_fd = g_registry_write_lock_fd;
    } else {
        lock_fd = registry_binary_lock(path, LOCK_EX, error, error_size);
        if (lock_fd < 0) return false;
        owns_lock = true;
    }

    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        snprintf(error, error_size, "Registry binary path is too long.");
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        snprintf(error, error_size, "Failed to open %s: %s", temp_path, strerror(errno));
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    if (fchmod(fd, 0644) != 0) {
        snprintf(error, error_size, "Failed to chmod %s: %s", temp_path, strerror(errno));
        close(fd);
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    bool ok = queue_all(fd, data, length);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        snprintf(error, error_size, "Failed to write %s: %s", temp_path, strerror(errno));
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    if (rename(temp_path, path) != 0) {
        snprintf(error, error_size, "Failed to rename %s to %s: %s", temp_path, path, strerror(errno));
        unlink(temp_path);
        if (owns_lock) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return false;
    }
    char directory_path[PATH_MAX];
    snprintf(directory_path, sizeof(directory_path), "%s", path);
    char *slash = strrchr(directory_path, '/');
    if (slash) {
        if (slash == directory_path) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        int dir_fd = open(directory_path, O_RDONLY);
        if (dir_fd >= 0) {
            (void)fsync(dir_fd);
            close(dir_fd);
        }
    }
    if (owns_lock) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return true;
}

