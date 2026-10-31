static uint64_t file_time_ms(FILETIME time) {
    ULARGE_INTEGER n;
    n.LowPart = time.dwLowDateTime;
    n.HighPart = time.dwHighDateTime;
    return n.QuadPart > 116444736000000000ULL
               ? (n.QuadPart - 116444736000000000ULL) / 10000
               : 0;
}
static void parent_path(const char *path, char out[PATH_MAX]) {
    snprintf(out, PATH_MAX, "%s", path);
    char *p = strrchr(out, '/');
    if (p && p > out + 2)
        *p = 0;
    else if (p)
        p[1] = 0;
}
static bool current_revision(const char *path, char revision[65]) {
    Bytes b = read_file(path, MAX_TEXT, true);
    if (!b.data) {
        if (errno == ENOENT) {
            failure[0] = 0;
            strcpy(revision, "missing");
            return true;
        }
        return false;
    }
    digest(b.data, b.size, revision);
    bytes_free(&b);
    return !failure[0];
}
static bool check_revision(const char *path, const char *expected) {
    char actual[65];
    if (!*expected) {
        fail("Open the file before saving or moving it.");
        return false;
    }
    if (!current_revision(path, actual))
        return false;
    if (strcmp(actual, expected)) {
        fail("The file changed since it was opened. Review changes before "
             "saving.");
        return false;
    }
    return true;
}
typedef struct {
    char *name, *path;
    WIN32_FIND_DATAW info;
} WindowsFileRow;
static int windows_file_compare(const void *left, const void *right) {
    const WindowsFileRow *a = left, *b = right;
    bool ad = (a->info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool bd = (b->info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (ad != bd) return ad ? -1 : 1;
    return _wcsicmp(a->info.cFileName, b->info.cFileName);
}
static BinaryReply windows_list(const char *path) {
    BinaryReply reply = {0};
    char *pattern = win_join(path, "*");
    wchar_t *wide = win_wide(pattern);
    free(pattern);
    WIN32_FIND_DATAW item;
    HANDLE search = wide ? FindFirstFileW(wide, &item) : INVALID_HANDLE_VALUE;
    free(wide);
    if (search == INVALID_HANDLE_VALUE) {
        fail("Cannot list folder (Windows error %lu).", GetLastError());
        return reply;
    }
    WindowsFileRow *rows = NULL;
    size_t count = 0;
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L".."))
            continue;
        if (count >= 100000) {
            fail("Folder contains too many entries.");
            break;
        }
        WindowsFileRow *next = realloc(rows, (count + 1) * sizeof(*rows));
        if (!next) {
            fail("Out of memory.");
            break;
        }
        rows = next;
        rows[count].name = win_utf8(item.cFileName);
        rows[count].path = win_join(path, rows[count].name);
        rows[count].info = item;
        count++;
    } while (FindNextFileW(search, &item));
    DWORD error = GetLastError();
    FindClose(search);
    if (!failure[0] && error != ERROR_NO_MORE_FILES)
        fail("Could not finish listing folder (%lu).", error);
    if (!failure[0]) {
        qsort(rows, count, sizeof(*rows), windows_file_compare);
        reply = response_create(OP_LIST, (uint32_t)count, FILE_ROW_SIZE);
        char parent[PATH_MAX];
        parent_path(path, parent);
        binary_string(&reply.metadata, 48, path);
        binary_string(&reply.metadata, 56, parent);
        for (size_t i = 0; i < count; i++) {
            size_t o = RESPONSE_SIZE + i * FILE_ROW_SIZE;
            WIN32_FIND_DATAW *f = &rows[i].info;
            bool dir = (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            binary_string(&reply.metadata, o, rows[i].name);
            binary_string(&reply.metadata, o + 8, rows[i].path);
            binary_string(&reply.metadata, o + 16,
                          dir ? "drwx------" : "-rw-------");
            put_u32(reply.metadata.data + o + 24, dir);
            put_u32(reply.metadata.data + o + 28,
                    (f->dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 3);
            put_u64(reply.metadata.data + o + 32,
                    ((uint64_t)f->nFileSizeHigh << 32) | f->nFileSizeLow);
            put_u64(reply.metadata.data + o + 40,
                    file_time_ms(f->ftLastWriteTime));
        }
    }
    for (size_t i = 0; i < count; i++) {
        free(rows[i].name);
        free(rows[i].path);
    }
    free(rows);
    return reply;
}
#ifdef APP_FILES
static char *favorites_path(void) {
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *directory = local ? win_join(local, "OuterShell") : NULL;
    free(local);
    if (!directory)
        return NULL;
    if (!mkdirs(directory)) {
        free(directory);
        return NULL;
    }
    char *path = win_join(directory, "files-favorites.tsv");
    free(directory);
    return path;
}
static BinaryReply windows_add_place(const char *path) {
    BinaryReply reply = {0};
    char resolved[PATH_MAX];
    if (!expand_path(path, resolved) || strpbrk(resolved, "\t\r\n"))
        return reply;
    wchar_t *wide = win_wide(resolved);
    DWORD attr = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        fail("Choose an existing folder.");
        return reply;
    }
    char *file = favorites_path();
    StringBuilder saved = {0};
    if (!file)
        return reply;
    win_read_file(file, MAX_DATA, &saved);
    bool found = false;
    char *copy = win_strdup(saved.data ? saved.data : "");
    char *context = NULL;
    for (char *line = strtok_s(copy, "\n", &context); line;
         line = strtok_s(NULL, "\n", &context))
        if (!_stricmp(line, resolved))
            found = true;
    free(copy);
    bool ok =
        found || (sb_append(&saved, resolved) && sb_append(&saved, "\n") &&
                  win_atomic_write(file, saved.data, saved.length));
    free(file);
    free(saved.data);
    if (!ok) {
        fail("Could not save the favorite folder.");
        return reply;
    }
    reply = response_create(OP_ADD_PLACE, 0, 0);
    binary_string(&reply.metadata, 48, resolved);
    return reply;
}
static BinaryReply windows_places(void) {
    const int folders[] = {CSIDL_PROFILE, CSIDL_DESKTOPDIRECTORY,
                           CSIDL_PERSONAL};
    const char *names[] = {"Home", "Desktop", "Documents"};
    char *paths[1024] = {0};
    char *labels[1024] = {0};
    size_t count = 0;
    for (size_t i = 0; i < 3; i++) {
        paths[count] = win_folder(folders[i]);
        labels[count] = win_strdup(names[i]);
        if (paths[count])
            count++;
        else
            free(labels[count]);
    }
    char *file = favorites_path();
    StringBuilder saved = {0};
    if (file)
        win_read_file(file, MAX_DATA, &saved);
    free(file);
    char *context = NULL;
    for (char *line = saved.data ? strtok_s(saved.data, "\n", &context) : NULL;
         line && count < 1024; line = strtok_s(NULL, "\n", &context)) {
        wchar_t *wide = win_wide(line);
        DWORD a = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
        free(wide);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        bool duplicate = false;
        for (size_t i = 0; i < count; i++)
            if (!_stricmp(paths[i], line))
                duplicate = true;
        if (duplicate)
            continue;
        paths[count] = win_strdup(line);
        char *name = strrchr(line, '/');
        labels[count] = win_strdup(name ? name + 1 : line);
        count++;
    }
    free(saved.data);
    BinaryReply reply =
        response_create(OP_PLACES, (uint32_t)count, FILE_ROW_SIZE);
    for (size_t i = 0; i < count; i++) {
        size_t o = RESPONSE_SIZE + i * FILE_ROW_SIZE;
        binary_string(&reply.metadata, o, labels[i]);
        binary_string(&reply.metadata, o + 8, paths[i]);
        binary_string(&reply.metadata, o + 16, "");
        put_u32(reply.metadata.data + o + 24, 1);
        put_u32(reply.metadata.data + o + 28, 3);
        free(paths[i]);
        free(labels[i]);
    }
    return reply;
}
#endif
BinaryReply local_binary_operation(const Request *request, Bytes body) {
    (void)body;
    BinaryReply reply = {0};
    char path[PATH_MAX];
#ifdef APP_FILES
    if (request->operation == OP_PLACES)
        return windows_places();
    if (request->operation == OP_ADD_PLACE)
        return windows_add_place(request->path);
#endif
    if (!expand_path(request->path, path))
        return reply;
    if (request->operation == OP_LIST)
        return windows_list(path);
    if (request->operation == OP_READ) {
#ifdef APP_PLAINTEXT
        bool missing = false;
#endif
        reply.body =
            read_file(path,
                      request->limit ? ((size_t)request->limit < MAX_DATA
                                            ? (size_t)request->limit
                                            : MAX_DATA)
                                     : MAX_TEXT,
                      true);
#ifdef APP_PLAINTEXT
        if (!reply.body.data && errno == ENOENT) {
            failure[0] = 0;
            reply.body.data = allocate(1);
            missing = true;
        }
        if (reply.body.data && !utf8(reply.body.data, reply.body.size))
            fail("This file is not UTF-8 text.");
#endif
        if (!failure[0]) {
            Bytes data = reply.body;
            reply = response_create(OP_READ, 0, 0);
            reply.body = data;
            char revision[65];
#ifdef APP_PLAINTEXT
            if (missing)
                strcpy(revision, "missing");
            else
                digest(reply.body.data, reply.body.size, revision);
#else
            digest(reply.body.data, reply.body.size, revision);
#endif
            binary_string(&reply.metadata, 48, path);
            binary_string(&reply.metadata, 56, revision);
        }
        return reply;
    }
#ifdef APP_PLAINTEXT
    if (request->operation == OP_SAVE || request->operation == OP_MOVE) {
        if (!check_revision(path, request->revision))
            return reply;
        wchar_t *wide = win_wide(path);
        char revision[65];
        if (request->operation == OP_SAVE) {
            if (body.size > MAX_TEXT || !utf8(body.data, body.size)) {
                free(wide);
                fail("Document must be UTF-8 and at most 4 MiB.");
                return reply;
            }
            char *temp = format("%s.XXXXXX", path);
            int fd = mkstemp(temp);
            bool ok = fd >= 0;
            if (!ok)
                fail("Could not create a temporary file beside the document.");
            if (ok)
                ok = write_all(fd, body.data, body.size) && _commit(fd) == 0;
            if (fd >= 0)
                _close(fd);
            if (ok)
                ok = check_revision(path, request->revision);
            wchar_t *temporary = win_wide(temp);
            if (ok) {
                DWORD attrs = GetFileAttributesW(wide);
                ok = attrs != INVALID_FILE_ATTRIBUTES
                         ? ReplaceFileW(wide, temporary, NULL, 0, NULL, NULL) !=
                               0
                         : MoveFileExW(temporary, wide,
                                       MOVEFILE_WRITE_THROUGH) != 0;
                if (!ok)
                    fail("Could not save the document (Windows error %lu).",
                         GetLastError());
            }
            win_unlink(temp);
            free(temporary);
            free(temp);
            if (ok)
                digest(body.data, body.size, revision);
        } else {
            char destination[PATH_MAX];
            if (expand_path(request->destination, destination)) {
                wchar_t *to = win_wide(destination);
                if (!MoveFileExW(wide, to, MOVEFILE_WRITE_THROUGH))
                    fail("Could not move the document (Windows error %lu).",
                         GetLastError());
                else
                    strcpy(path, destination);
                free(to);
            }
            if (!failure[0])
                current_revision(path, revision);
        }
        free(wide);
        if (!failure[0]) {
            reply = response_create(request->operation, 0, 0);
            binary_string(&reply.metadata, 48, path);
            binary_string(&reply.metadata, 56, revision);
        }
        return reply;
    }
#endif
    fail("Unsupported local file operation.");
    return reply;
}
