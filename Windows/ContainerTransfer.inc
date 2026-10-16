static size_t container_array_count(const char *array) {
    if (!array || *array != '[') return 0;
    const unsigned char *p = (const unsigned char *)array + 1, *end = (const unsigned char *)array + strlen(array);
    size_t count = 0;
    while (p < end) {
        layout_json_space(&p, end); if (*p == ']') break;
        if (!layout_json_value(&p, end, 0)) return 0;
        count++; layout_json_space(&p, end); if (p < end && *p == ',') p++;
    }
    return count;
}

static char *container_new_id(void) {
    GUID guid; wchar_t text[40];
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, text, 40)) return NULL;
    char *id = win_utf8(text + 1);
    if (id) { id[36] = 0; CharLowerBuffA(id, 36); }
    return id;
}

static char *container_transfer_directory(const char *id, bool create) {
    if (!container_uuid(id)) return NULL;
    char *local = win_folder(CSIDL_LOCAL_APPDATA), *home = local ? win_join(local, "OuterShell") : NULL;
    char *root = home ? win_join(home, "container-transfers") : NULL;
    char *path = root && win_secure_directory(root) ? win_join(root, id) : NULL;
    if (path && create && !win_secure_directory(path)) { free(path); path = NULL; }
    free(local); free(home); free(root); return path;
}
static bool container_tree(const char *source, const char *destination, unsigned depth) {
    if (depth > 64) return false;
    wchar_t *wide = win_wide(source);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    bool ok = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    if (ok && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t *target = destination ? win_wide(destination) : NULL;
        ok = destination ? target && CopyFileW(wide, target, TRUE) : DeleteFileW(wide);
        free(target); free(wide); return ok;
    }
    if (!ok || (destination && !win_secure_directory(destination))) { free(wide); return false; }
    char *pattern = win_join(source, "*"); wchar_t *search = win_wide(pattern); free(pattern);
    WIN32_FIND_DATAW entry; HANDLE iterator = search ? FindFirstFileW(search, &entry) : INVALID_HANDLE_VALUE; free(search);
    if (iterator == INVALID_HANDLE_VALUE) { free(wide); return false; }
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        char *name = win_utf8(entry.cFileName), *from = name ? win_join(source, name) : NULL;
        char *to = destination && name ? win_join(destination, name) : NULL;
        ok = from && (!destination || to) && container_tree(from, to, depth + 1);
        free(name); free(from); free(to);
    } while (ok && FindNextFileW(iterator, &entry));
    DWORD error = GetLastError(); FindClose(iterator);
    ok = ok && error == ERROR_NO_MORE_FILES;
    if (ok && !destination) ok = RemoveDirectoryW(wide);
    free(wide); return ok;
}

static bool container_archive_path(const char *name) {
    if (!*name || *name == '/' || strchr(name, '\\') || strchr(name, ':') || strlen(name) > 2000) return false;
    for (const char *p = name; *p; p++) if ((unsigned char)*p < 32 || strchr("<>|?*\"", *p)) return false;
    for (const char *p = name; *p;) {
        const char *slash = strchr(p, '/'); size_t length = slash ? (size_t)(slash - p) : strlen(p);
        if (!length || p[length - 1] == '.' || p[length - 1] == ' ') return false;
        char part[256]; if (length >= sizeof(part)) return false;
        memcpy(part, p, length); part[length] = 0;
        char *dot = strchr(part, '.'); if (dot) *dot = 0;
        if (!_stricmp(part, "CON") || !_stricmp(part, "PRN") || !_stricmp(part, "AUX") || !_stricmp(part, "NUL") ||
            ((!_strnicmp(part, "COM", 3) || !_strnicmp(part, "LPT", 3)) && strlen(part) == 4 && part[3] >= '0' && part[3] <= '9')) return false;
        p += length; if (*p == '/') p++;
    }
    return true;
}

static bool container_extract(Wsl *w, const char *archive, const char *destination) {
    StringBuilder names = {0}, types = {0}, output = {0};
    const char *list[] = {"-tzf", archive, NULL}, *verbose[] = {"-tvzf", archive, NULL};
    bool ok = capture_runtime(w, list, &names, RUNTIME_TAR) && capture_runtime(w, verbose, &types, RUNTIME_TAR);
    for (char *p = names.data; ok && p && *p;) {
        char *end = strchr(p, '\n'); if (end) *end = 0;
        size_t length = strlen(p); if (length && p[length - 1] == '\r') p[--length] = 0;
        ok = container_archive_path(p);
        p = end ? end + 1 : NULL;
    }
    for (char *p = types.data; ok && p && *p;) {
        ok = *p == '-' || *p == 'd';
        p = strchr(p, '\n'); if (p) p++;
    }
    if (ok) {
        const char *extract[] = {"-xzf", archive, "--no-same-owner", "--no-same-permissions", "-C", destination, NULL};
        ok = win_secure_directory(destination) && capture_runtime(w, extract, &output, RUNTIME_TAR);
    }
    free(names.data); free(types.data); free(output.data); return ok;
}

static void container_extra_response(Wsl *w, Connection *client, HttpRequest *request, const char *extra) {
    StringBuilder json = {0}; append(&json, "{");
    char *request_id = request_string(request, "requestID");
    if (request_id) { field(&json, "requestID", request_id); append(&json, ","); }
    if (extra && *extra) { append(&json, extra); append(&json, ","); }
    EnterCriticalSection(&w->gate);
    append(&json, w->snapshot.data ? w->snapshot.data + 1 : "\"workspaces\":[]}");
    LeaveCriticalSection(&w->gate);
    http_send(client, request, 200, "application/json", json.data, json.length);
    free(request_id); free(json.data);
}

static bool container_write_value_file(const char *directory, const char *name, const char *json) {
    StringBuilder value = {0}; char *path = win_join(directory, name);
    bool ok = path && container_value_encode(json, &value) && win_atomic_write(path, value.data, value.length);
    free(path); free(value.data); return ok;
}
static char *container_read_value_file(const char *directory, const char *name) {
    StringBuilder value = {0}, json = {0}; char *path = win_join(directory, name);
    bool ok = path && win_read_file(path, WSL_LIMIT, &value) && container_value_decode(&value, &json);
    free(path); free(value.data);
    if (!ok) { free(json.data); return NULL; }
    return json.data;
}

static bool container_share(Distribution *d, HttpRequest *request, StringBuilder *extra) {
    if (!d->managed || d->building) return false;
    char *id = container_new_id(), *root = id ? container_transfer_directory(id, true) : NULL;
    char *context = container_recipe_directory(d->id), *recipe = root ? win_join(root, "Recipe") : NULL;
    char *config = container_read(d->id, "configuration", container_empty_configuration);
    char *mounts = container_json_field(config, "mounts");
    char *include = container_json_field(request->body.data, "includeMountedFolders");
    bool included = include && !strcmp(include, "true");
    char *include_data = container_json_field(request->body.data, "includePersistentData");
    bool persistent_included = include_data && !strcmp(include_data, "true");
    free(include_data);
    char *dockerfile = container_read(d->id, "Dockerfile", ""), *base_image = container_base_image(dockerfile);
    StringBuilder shared_recipe = {0}; append(&shared_recipe, "{\"version\":1,\"steps\":[],\"realizedStepIDs\":[],");
    field(&shared_recipe, "baseImage", base_image); append(&shared_recipe, ","); append(&shared_recipe, config + 1);
    bool ok = root && context && recipe && container_tree(context, recipe, 0) &&
              container_write_value_file(recipe, "recipe.orwv", shared_recipe.data);
    free(dockerfile); free(base_image); free(shared_recipe.data);
    size_t count = 0;
    if (ok && included && mounts) {
        char *mount_root = win_join(root, "Mounted Folders"); ok = win_secure_directory(mount_root);
        const unsigned char *p = (const unsigned char *)mounts + 1, *end = (const unsigned char *)mounts + strlen(mounts);
        while (ok && p < end) {
            layout_json_space(&p, end); if (*p == ']') break;
            const unsigned char *start = p; ok = layout_json_value(&p, end, 0);
            char *host = ok ? container_string((const char *)start, (size_t)(p - start), "hostPath") : NULL;
            char *mount_id = ok ? container_string((const char *)start, (size_t)(p - start), "id") : NULL;
            if (mount_id) CharLowerBuffA(mount_id, (DWORD)strlen(mount_id));
            char *target = container_uuid(mount_id) ? win_join(mount_root, mount_id) : NULL;
            ok = host && target && container_tree(host, target, 0); count += ok;
            free(host); free(mount_id); free(target);
            layout_json_space(&p, end); if (p < end && *p == ',') p++;
        }
        free(mount_root);
    }
    if (ok && persistent_included) {
        char *directory = container_directory(d->id), *from = directory ? win_join(directory, "Persistent Data") : NULL;
        char *to = win_join(root, "Persistent Data");
        wchar_t *wide = from ? win_wide(from) : NULL;
        ok = wide && (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES ? win_secure_directory(to) : container_tree(from, to, 0));
        free(directory); free(from); free(to); free(wide);
    }
    StringBuilder manifest = {0}; append(&manifest, "{\"version\":1,"); field(&manifest, "name", d->name);
    char limits[100]; snprintf(limits, sizeof(limits), ",\"cpus\":%u,\"memoryInGB\":%u,", d->cpus, d->memory_gb); append(&manifest, limits);
    field(&manifest, "sourceRuntimeProviderID", container_provider_id(d->container));
    append(&manifest, ",\"includesPersistentData\":"); append(&manifest, persistent_included ? "true" : "false"); append(&manifest, ",\"includedMounts\":"); append(&manifest, included && mounts ? mounts : "[]");
    append(&manifest, ",\"omittedMounts\":"); append(&manifest, !included && mounts ? mounts : "[]"); append(&manifest, "}");
    char *archive = root ? win_join(root, "container.outershell-container") : NULL;
    StringBuilder output = {0};
    if (ok) {
        ok = container_write_value_file(root, "manifest.orwv", manifest.data);
        const char *args[10] = {"-czf", archive, "-C", root, "manifest.orwv", "Recipe", NULL};
        size_t argc = 6;
        if (included) args[argc++] = "Mounted Folders";
        if (persistent_included) args[argc++] = "Persistent Data";
        ok = ok && capture_runtime(d->owner, args, &output, RUNTIME_TAR);
    }
    if (ok) {
        wchar_t *wide = win_wide(archive); WIN32_FILE_ATTRIBUTE_DATA info;
        ok = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &info); free(wide);
        if (ok) {
            field(extra, "transferID", id); append(extra, ",\"fileName\":\"Container.outershell-container\"");
            char size[160]; snprintf(size, sizeof(size), ",\"byteCount\":%llu,\"includedMountCount\":%zu,\"omittedMountCount\":%zu",
                                    ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow, count, included ? 0 : container_array_count(mounts)); append(extra, size);
        }
    }
    if (!ok && root) container_tree(root, NULL, 0);
    free(id); free(root); free(context); free(recipe); free(config); free(mounts); free(include); free(manifest.data); free(archive); free(output.data);
    return ok;
}

static void container_transfer(Wsl *w, Connection *client, HttpRequest *request) {
    (void)w;
    const unsigned char *data = (const unsigned char *)request->body.data;
    StringBuilder result = {0}; unsigned char header[40] = {'O','S','C','T',1}; bytes(&result, header, sizeof(header));
    const char *error = "Invalid container transfer request.";
    HANDLE file = INVALID_HANDLE_VALUE;
    char *root = NULL, *path = NULL;
    if (request->body.length < 48 || memcmp(data, "OSCT\1\0", 6)) goto done;
    char id[37]; snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         data[8],data[9],data[10],data[11],data[12],data[13],data[14],data[15],data[16],data[17],data[18],data[19],data[20],data[21],data[22],data[23]);
    unsigned operation = read_uint16_le(data + 6);
    uint64_t offset = payload_read64(data + 24), length = payload_read64(data + 32);
    if ((operation != 1 && operation != 2) || offset > INT64_MAX || length > 16000000) goto done;
    root = container_transfer_directory(id, false);
    path = root ? win_join(root, operation == 1 ? "container.outershell-container" : "upload.outershell-container") : NULL;
    wchar_t *wide = path ? win_wide(path) : NULL;
    file = wide ? CreateFileW(wide, operation == 1 ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE; free(wide);
    LARGE_INTEGER size, position; position.QuadPart = (LONGLONG)offset;
    BY_HANDLE_FILE_INFORMATION info;
    if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileSizeEx(file, &size) || !SetFilePointerEx(file, position, NULL, FILE_BEGIN)) goto done;
    if (operation == 1) {
        if (offset > (uint64_t)size.QuadPart) goto done;
        DWORD count = (DWORD)(length ? length : 1), read = 0;
        void *buffer = win_alloc(count);
        bool ok = ReadFile(file, buffer, count, &read, NULL);
        if (ok) {
            container_write64((unsigned char *)result.data + 8, offset + read); container_write64((unsigned char *)result.data + 16, (uint64_t)size.QuadPart);
            write_uint32_le((unsigned char *)result.data + 24, 40); write_uint32_le((unsigned char *)result.data + 28, read); bytes(&result, buffer, read); error = NULL;
        }
        free(buffer);
    } else {
        uint32_t start = read_uint32_le(data + 40), count = read_uint32_le(data + 44); DWORD written = 0;
        if (start < 48 || start > request->body.length || count > request->body.length - start || length != count || offset + count > 8ull * 1024 * 1024 * 1024) goto done;
        bool ok = false;
        if ((uint64_t)size.QuadPart == offset) ok = WriteFile(file, data + start, count, &written, NULL) && written == count;
        else if ((uint64_t)size.QuadPart == offset + count) {
            void *prior = win_alloc(count); ok = ReadFile(file, prior, count, &written, NULL) && written == count && !memcmp(prior, data + start, count); free(prior);
        }
        if (ok) { container_write64((unsigned char *)result.data + 8, offset + count); container_write64((unsigned char *)result.data + 16, offset + count); error = NULL; }
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (error) {
        write_uint16_le((unsigned char *)result.data + 6, 1); write_uint32_le((unsigned char *)result.data + 32, (uint32_t)result.length);
        write_uint32_le((unsigned char *)result.data + 36, (uint32_t)strlen(error)); append(&result, error);
    }
    http_send(client, request, 200, "application/vnd.outershell.container-transfer", result.data, result.length);
    free(root); free(path); free(result.data);
}

static bool container_import_destination(const char *path) {
    wchar_t *wide = win_wide(path);
    if (!wide) return false;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES && CreateDirectoryW(wide, NULL)) attributes = GetFileAttributesW(wide);
    bool ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    free(wide); return ok;
}

static bool container_import(Wsl *w, HttpRequest *request, StringBuilder *extra) {
    char *transfer_id = request_string(request, "transferID"), *provider = request_string(request, "runtimeProviderID");
    char *destination = request_string(request, "mountDestinationRoot");
    char *root = container_transfer_directory(transfer_id, false);
    char *archive = root ? win_join(root, "upload.outershell-container") : NULL;
    char *extracted = root ? win_join(root, "Extracted") : NULL;
    char *recipe = extracted ? win_join(extracted, "Recipe") : NULL;
    char *manifest = NULL, *config = NULL, *name = NULL, *id = NULL, *target = NULL;
    char *mounts = NULL, *environment = NULL, *ports = NULL;
    bool ok = provider && (!strcmp(provider, "docker") || !strcmp(provider, "wsl.container")) && root;
    wchar_t *wide = extracted ? win_wide(extracted) : NULL;
    if (ok && (!wide || GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES)) {
        ok = container_extract(w, archive, extracted);
        if (!ok && extracted) container_tree(extracted, NULL, 0);
    }
    free(wide);
    if (ok) {
        manifest = container_read_value_file(extracted, "manifest.orwv");
        config = container_read_value_file(recipe, "recipe.orwv");
        ok = manifest && config;
    }
    HttpRequest record = {0};
    if (ok) {
        record.body = (StringBuilder){manifest, strlen(manifest), 0};
        ok = container_number(&record, "version", 0) == 1;
        name = request_string(&record, "name"); mounts = container_json_field(manifest, "includedMounts");
        environment = container_json_field(config, "environment"); ports = container_json_field(config, "publishedPorts");
        ok = ok && valid_container_name(name) && mounts && mounts[0] == '[';
    }
    StringBuilder imported = {0}; append(&imported, "[");
    if (ok && strcmp(mounts, "[]") && (!destination || !*destination)) {
        append(extra, "\"needsMountDestination\":true,"); field(extra, "importName", name);
        char *home = win_folder(CSIDL_PROFILE), *suggested = home ? win_join(home, "Outer Shell Containers") : NULL;
        append(extra, ","); field(extra, "suggestedMountRoot", suggested ? suggested : ""); append(extra, ",\"importMounts\":[");
        const unsigned char *p = (const unsigned char *)mounts + 1, *end = p + strlen(mounts) - 1;
        size_t count = 0;
        while (p < end) {
            layout_json_space(&p, end); if (p == end || *p == ']') break;
            const unsigned char *start = p; if (!layout_json_value(&p, end, 0)) { ok = false; break; }
            char *mount_id = container_string((const char *)start, (size_t)(p - start), "id");
            char *mount_name = container_string((const char *)start, (size_t)(p - start), "name");
            char *host = container_string((const char *)start, (size_t)(p - start), "hostPath");
            char *guest = container_string((const char *)start, (size_t)(p - start), "guestPath");
            StringBuilder mount_json = {0}; bytes(&mount_json, start, (size_t)(p - start));
            char *read_only = container_json_field(mount_json.data, "isReadOnly");
            if (!container_uuid(mount_id) || !mount_name || !host || !guest) ok = false;
            if (ok) {
                if (count++) append(extra, ","); append(extra, "{"); field(extra, "id", mount_id);
                append(extra, ","); field(extra, "name", mount_name); append(extra, ","); field(extra, "sourceHostPath", host);
                append(extra, ","); field(extra, "guestPath", guest); append(extra, ","); field(extra, "directoryName", mount_id);
                append(extra, ",\"isReadOnly\":"); append(extra, read_only && !strcmp(read_only, "true") ? "true" : "false"); append(extra, "}");
            }
            free(mount_id); free(mount_name); free(host); free(guest); free(read_only); free(mount_json.data);
            if (!ok) break;
            layout_json_space(&p, end); if (p < end && *p == ',') p++;
        }
        append(extra, "]"); free(home); free(suggested); goto done;
    }
    if (ok && strcmp(mounts, "[]")) {
        ok = destination && strlen(destination) > 3 && isalpha((unsigned char)destination[0]) && destination[1] == ':' &&
             (destination[2] == '\\' || destination[2] == '/') && container_import_destination(destination);
        const unsigned char *p = (const unsigned char *)mounts + 1, *end = p + strlen(mounts) - 1;
        size_t count = 0;
        while (ok && p < end) {
            layout_json_space(&p, end); if (p == end || *p == ']') break;
            const unsigned char *start = p; ok = layout_json_value(&p, end, 0); if (!ok) break;
            StringBuilder item = {0}; bytes(&item, start, (size_t)(p - start));
            char *mount_id = container_string(item.data, item.length, "id"), *mount_name = container_string(item.data, item.length, "name");
            char *guest = container_string(item.data, item.length, "guestPath"), *read_only = container_json_field(item.data, "isReadOnly");
            char *from_root = win_join(extracted, "Mounted Folders");
            char *from = container_uuid(mount_id) ? win_join(from_root, mount_id) : NULL;
            char *to = from ? win_join(destination, mount_id) : NULL;
            wchar_t *existing = to ? win_wide(to) : NULL;
            ok = from && to && mount_name && guest && read_only && existing && GetFileAttributesW(existing) == INVALID_FILE_ATTRIBUTES && container_tree(from, to, 0);
            if (ok) {
                if (count++) append(&imported, ","); append(&imported, "{"); field(&imported, "id", mount_id);
                append(&imported, ","); field(&imported, "name", mount_name); append(&imported, ","); field(&imported, "hostPath", to);
                append(&imported, ","); field(&imported, "guestPath", guest); append(&imported, ",\"isReadOnly\":"); append(&imported, read_only); append(&imported, "}");
            }
            free(item.data); free(mount_id); free(mount_name); free(guest); free(read_only); free(from_root); free(from); free(to); free(existing);
            layout_json_space(&p, end); if (p < end && *p == ',') p++;
        }
    }
    append(&imported, "]");
    if (ok) {
        id = container_new_id(); target = id ? container_recipe_directory(id) : NULL;
        ok = target && container_tree(recipe, target, 0);
        if (ok) {
            char *metadata = win_join(target, "recipe.orwv");
            wchar_t *wide_metadata = metadata ? win_wide(metadata) : NULL;
            ok = wide_metadata && DeleteFileW(wide_metadata);
            free(metadata); free(wide_metadata);
        }
    }
    if (ok) {
        StringBuilder value = {0}; append(&value, "{\"mounts\":"); append(&value, imported.data);
        append(&value, ",\"environment\":"); append(&value, environment ? environment : "[]");
        append(&value, ",\"publishedPorts\":"); append(&value, ports ? ports : "[]");
        char *persistent = container_json_field(config, "persistentData");
        if (persistent) { append(&value, ",\"persistentData\":"); append(&value, persistent); }
        free(persistent); append(&value, "}");
        ContainerArguments args = {0};
        ok = container_configuration(value.data, &args) && container_write(id, "configuration", value.data, value.length) &&
             container_write(id, "configuration.realized", "{}", 2) && container_write(id, "Dockerfile.realized", "", 0);
        free(value.data); container_arguments_free(&args);
    }
    if (ok) {
        char *directory = container_directory(id), *from = win_join(extracted, "Persistent Data");
        char *to = directory ? win_join(directory, "Persistent Data") : NULL;
        wchar_t *wide = win_wide(from);
        if (wide && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) ok = to && container_tree(from, to, 0);
        free(directory); free(from); free(to); free(wide);
    }
    if (ok) {
        unsigned cpus = container_number(&record, "cpus", 4), memory = container_number(&record, "memoryInGB", 8);
        ok = cpus && cpus <= 128 && memory && memory <= 128 && save_container_metadata(id, name, cpus, memory);
        if (ok) {
            Distribution *d = win_alloc(sizeof(*d)); d->owner = w; d->id = win_strdup(id); d->name = win_strdup(name);
            d->user = win_strdup("root"); d->runtime_id = win_strdup(""); d->managed = true; d->present = true;
            d->container = !strcmp(provider, "docker") ? RUNTIME_DOCKER : RUNTIME_WSLC; d->cpus = cpus; d->memory_gb = memory;
            EnterCriticalSection(&w->gate); d->next = w->distributions; w->distributions = d; LeaveCriticalSection(&w->gate);
            ok = container_rebuild(d);
            if (ok) {
                field(extra, "importedWorkspaceID", id);
                char *omitted = container_json_field(manifest, "omittedMounts");
                char count[64]; snprintf(count, sizeof(count), ",\"omittedMountCount\":%zu", container_array_count(omitted));
                append(extra, count); free(omitted); container_tree(root, NULL, 0);
            }
        }
    }
done:
    free(transfer_id); free(provider); free(destination); free(root); free(archive); free(extracted); free(recipe);
    free(manifest); free(config); free(name); free(id); free(target); free(mounts); free(environment); free(ports); free(imported.data);
    return ok;
}

static bool container_transfer_operation(Wsl *w, Connection *client, HttpRequest *request, const char *operation) {
    StringBuilder extra = {0}; bool ok = false;
    if (!strcmp(operation, "beginImport")) {
        char *id = container_new_id(), *root = id ? container_transfer_directory(id, true) : NULL;
        char *path = root ? win_join(root, "upload.outershell-container") : NULL;
        ok = path && win_atomic_write(path, "", 0);
        if (ok) field(&extra, "transferID", id);
        free(id); free(root); free(path);
    } else if (!strcmp(operation, "cancelImport")) {
        char *id = request_string(request, "transferID"), *root = container_transfer_directory(id, false);
        ok = root && container_tree(root, NULL, 0); free(root); free(id);
    } else if (!strcmp(operation, "finishImport")) ok = container_import(w, request, &extra);
    if (ok) container_extra_response(w, client, request, extra.data);
    else http_text(client, request, 400, "Could not import the container. Check the archive, destination folders, and runtime log.");
    free(extra.data); return ok;
}

static void container_http_upload(Wsl *w, Connection *client, HttpRequest *request) {
    (void)w;
    const char *id = request->path + strlen("/api/container-transfers/");
    if ((!strcmp(request->method, "HEAD") || !strcmp(request->method, "PATCH")) &&
        container_uuid(id) && request->upload_protocol && !strcmp(request->upload_protocol, "outershell-resumable-v1")) {
        char *root = container_transfer_directory(id, false);
        char *path = root ? win_join(root, "upload.outershell-container") : NULL;
        wchar_t *wide = path ? win_wide(path) : NULL;
        bool patch = !strcmp(request->method, "PATCH");
        HANDLE file = wide ? CreateFileW(wide, GENERIC_READ | (patch ? GENERIC_WRITE : 0), 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
        free(wide); free(root); free(path);
        LARGE_INTEGER size; BY_HANDLE_FILE_INFORMATION info;
        if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !GetFileSizeEx(file, &size)) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            http_text(client, request, 404, "The container transfer does not exist."); return;
        }
        int status = 200;
        if (patch) {
            const char *text = request->upload_offset;
            uint64_t offset = 0;
            bool valid = text && *text;
            for (const char *p = text; valid && *p; p++) {
                if (*p < '0' || *p > '9' || offset > (UINT64_MAX - 9) / 10) valid = false;
                else offset = offset * 10 + (unsigned)(*p - '0');
            }
            if (!valid) status = 400;
            else if (offset != (uint64_t)size.QuadPart) status = 409;
            else if (offset > 8ull * 1024 * 1024 * 1024 || request->body.length > 8ull * 1024 * 1024 * 1024 - offset) status = 413;
            else {
                DWORD written = 0;
                if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) ||
                    !WriteFile(file, request->body.data, (DWORD)request->body.length, &written, NULL) || written != request->body.length || !FlushFileBuffers(file)) status = 500;
                if (!GetFileSizeEx(file, &size)) status = 500;
            }
        }
        CloseHandle(file);
        char response[384];
        int length = snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nUpload-Protocol: outershell-resumable-v1\r\nUpload-Offset: %llu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
            status, status == 200 ? "OK" : status == 409 ? "Conflict" : "Upload Error", (unsigned long long)size.QuadPart);
        if (length > 0 && (size_t)length < sizeof(response)) connection_write(client, response, (size_t)length);
        return;
    }
    http_text(client, request, 400, "Invalid container upload request.");
}
