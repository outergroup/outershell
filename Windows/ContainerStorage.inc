typedef struct StorageImage {
    struct StorageImage *next;
    char *reference, *digest;
    bool protected;
} StorageImage;

static void storage_images_free(StorageImage *images) {
    while (images) {
        StorageImage *next = images->next;
        free(images->reference); free(images->digest); free(images); images = next;
    }
}

static bool storage_owned_reference(const char *reference) {
    const char *prefix = "outershell/managed-";
    size_t length = strlen(prefix);
    if (!strncmp(reference, prefix, length) && strlen(reference) == length + 36 + strlen(":latest")) {
        char id[37]; memcpy(id, reference + length, 36); id[36] = 0;
        return container_uuid(id) && !strcmp(reference + length + 36, ":latest");
    }
    prefix = "outershell/container-base:"; length = strlen(prefix);
    if (strncmp(reference, prefix, length) || !reference[length] || strspn(reference + length, "0123456789") != strlen(reference + length)) return false;
    return strtoul(reference + length, NULL, 10) < 21;
}

static bool storage_next_object(const unsigned char **cursor, const unsigned char *end, StringBuilder *object) {
    layout_json_space(cursor, end);
    if (*cursor < end && (**cursor == '[' || **cursor == ',')) { (*cursor)++; layout_json_space(cursor, end); }
    if (*cursor == end || **cursor == ']') return false;
    const unsigned char *start = *cursor;
    if (**cursor != '{' || !layout_json_value(cursor, end, 0)) return false;
    bytes(object, start, (size_t)(*cursor - start));
    return true;
}

static bool storage_json_finished(const unsigned char *cursor, const unsigned char *end) {
    layout_json_space(&cursor, end);
    if (cursor < end && *cursor == ']') { cursor++; layout_json_space(&cursor, end); }
    return cursor == end;
}

static bool storage_saved_definitions(StringBuilder *definitions, const char *excluding) {
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *home = local ? win_join(local, "OuterShell") : NULL;
    char *root = home ? win_join(home, "containers") : NULL;
    char *pattern = root ? win_join(root, "*") : NULL;
    wchar_t *wide = pattern ? win_wide(pattern) : NULL;
    WIN32_FIND_DATAW entry;
    HANDLE search = wide ? FindFirstFileW(wide, &entry) : INVALID_HANDLE_VALUE;
    bool ok = wide && (search != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char *id = win_utf8(entry.cFileName);
            if (id && container_uuid(id) && (!excluding || _stricmp(id, excluding))) {
                char *metadata = container_metadata_path(id);
                if (metadata && container_path_exists(metadata)) {
                    append(definitions, "\noutershell/managed-"); append(definitions, id); append(definitions, ":latest\n");
                }
                free(metadata);
                const char *names[] = {"Dockerfile", "Dockerfile.realized"};
                for (size_t i = 0; ok && i < 2; i++) {
                    char *path = container_file(id, names[i]);
                    if (!path) { ok = false; break; }
                    if (container_path_exists(path)) {
                        StringBuilder file = {0};
                        ok = win_read_file(path, WSL_LIMIT, &file);
                        if (ok && file.data) { append(definitions, "\n"); bytes(definitions, file.data, file.length); }
                        free(file.data);
                    }
                    free(path);
                }
            }
            free(id);
        } while (ok && FindNextFileW(search, &entry));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
        FindClose(search);
    }
    free(wide); free(pattern); free(root); free(home); free(local);
    return ok;
}

static bool storage_inventory(Wsl *w, ContainerRuntime runtime, const char *excluding, StorageImage **images) {
    StringBuilder listing = {0}, definitions = {0}, containers = {0};
    const char *list[] = {"image", "list", "--no-trunc", "--format", runtime == RUNTIME_DOCKER ? "{{json .}}" : "json", NULL};
    bool ok = capture_runtime(w, list, &listing, runtime) && storage_saved_definitions(&definitions, excluding);
    const unsigned char *p = (const unsigned char *)(listing.data ? listing.data : ""), *end = p + listing.length;
    while (ok) {
        StringBuilder object = {0};
        if (!storage_next_object(&p, end, &object)) { ok = storage_json_finished(p, end); break; }
        char *repository = container_string(object.data, object.length, "Repository");
        char *tag = container_string(object.data, object.length, "Tag");
        char *digest = container_string(object.data, object.length, "ID");
        if (!digest) digest = container_string(object.data, object.length, "Id");
        ok = repository && tag && digest && *digest;
        if (ok && strcmp(repository, "<none>") && strcmp(tag, "<none>")) {
            StringBuilder reference = {0}; append(&reference, repository); append(&reference, ":"); append(&reference, tag);
            StorageImage *image = win_alloc(sizeof(*image));
            image->reference = reference.data; image->digest = digest; digest = NULL;
            image->protected = !strcmp(image->reference, "outershell/container-base:22") ||
                (definitions.data && (strstr(definitions.data, image->reference) || strstr(definitions.data, image->digest) || strchr(definitions.data, '$') || strstr(definitions.data, "@sha256:")));
            image->next = *images; *images = image;
        }
        free(repository); free(tag); free(digest); free(object.data);
    }
    const char *list_containers[] = {runtime == RUNTIME_DOCKER ? "ps" : "list", "--all", "--no-trunc", "--format", runtime == RUNTIME_DOCKER ? "{{json .}}" : "json", NULL};
    ok = ok && capture_runtime(w, list_containers, &containers, runtime);
    p = (const unsigned char *)(containers.data ? containers.data : ""); end = p + containers.length;
    while (ok) {
        StringBuilder object = {0}, inspection = {0};
        if (!storage_next_object(&p, end, &object)) { ok = storage_json_finished(p, end); break; }
        char *id = container_string(object.data, object.length, "ID");
        const char *inspect[] = {"inspect", id, NULL};
        ok = id && *id && capture_runtime(w, inspect, &inspection, runtime);
        if (ok) {
            const unsigned char *q = (const unsigned char *)(inspection.data ? inspection.data : ""), *limit = q + inspection.length;
            StringBuilder config = {0};
            ok = storage_next_object(&q, limit, &config);
            char *image = ok ? container_string(config.data, config.length, "Image") : NULL;
            ok = image && *image;
            for (StorageImage *item = *images; ok && item; item = item->next) {
                if (!strcmp(item->digest, image) || !strcmp(item->reference, image) ||
                    (!strncmp(image, "sha256:", 7) && !strcmp(item->digest, image + 7))) item->protected = true;
            }
            free(image); free(config.data);
        }
        free(id); free(object.data); free(inspection.data);
    }
    for (StorageImage *item = *images; item; item = item->next)
        for (StorageImage *other = *images; other; other = other->next)
            if (other->protected && !strcmp(other->digest, item->digest)) item->protected = true;
    free(listing.data); free(containers.data); free(definitions.data);
    if (!ok) { storage_images_free(*images); *images = NULL; }
    return ok;
}

static bool storage_candidate(StorageImage *images, const char *reference) {
    for (; images; images = images->next)
        if (!strcmp(images->reference, reference)) return !images->protected && storage_owned_reference(reference);
    return false;
}

static bool storage_remove_recipe(Distribution *d) {
    StorageImage *images = NULL;
    if (!storage_inventory(d->owner, d->container, d->id, &images)) return false;
    char reference[100]; snprintf(reference, sizeof(reference), "outershell/managed-%s:latest", d->id);
    bool remove = storage_candidate(images, reference);
    storage_images_free(images);
    if (!remove) return true;
    StringBuilder output = {0};
    const char *args[] = {"image", d->container == RUNTIME_DOCKER ? "rm" : "delete", reference, NULL};
    bool ok = capture_runtime(d->owner, args, &output, d->container);
    if (!ok && output.data) fprintf(stderr, "Image deletion failed: %s\n", output.data);
    free(output.data); return ok;
}

static bool storage_request(Wsl *w, HttpRequest *request, const char *operation, StringBuilder *extra) {
    char *provider = request_string(request, "runtimeProviderID");
    ContainerRuntime runtime = provider && !strcmp(provider, "docker") ? RUNTIME_DOCKER : RUNTIME_WSLC;
    bool ok = provider && (!strcmp(provider, "docker") || !strcmp(provider, "wsl.container"));
    free(provider);
    StorageImage *images = NULL;
    if (!ok || !storage_inventory(w, runtime, NULL, &images)) return false;
    StringBuilder deleted = {0}, failures = {0};
    if (!strcmp(operation, "cleanContainerBuildCache")) {
        const char *prune[] = {"builder", "prune", "--force", "--filter", "until=168h", "--keep-storage", "10GB", NULL};
        StringBuilder output = {0};
        ok = runtime == RUNTIME_DOCKER && capture_runtime(w, prune, &output, runtime);
        free(output.data);
    } else if (!strcmp(operation, "cleanContainerStorage")) {
        char *selected = container_json_field(request->body.data, "images");
        ContainerArguments references = {0};
        const unsigned char *p = (const unsigned char *)(selected ? selected : ""), *end = p + strlen((const char *)p);
        layout_json_space(&p, end); ok = p < end && *p++ == '[';
        while (ok) {
            layout_json_space(&p, end);
            if (p == end) { ok = false; break; }
            if (*p == ']') { p++; layout_json_space(&p, end); ok = p == end; break; }
            const unsigned char *start = p;
            if (*p != '"' || !layout_json_value(&p, end, 0)) { ok = false; break; }
            StringBuilder wrapped = {0}; append(&wrapped, "{\"v\":"); bytes(&wrapped, start, (size_t)(p - start)); append(&wrapped, "}");
            char *reference = container_string(wrapped.data, wrapped.length, "v"); free(wrapped.data);
            ok = reference && storage_candidate(images, reference) && container_argument(&references, reference);
            free(reference);
            layout_json_space(&p, end);
            if (p < end && *p == ',') p++;
        }
        for (size_t i = 0; ok && i < references.count; i++) {
            const char *reference = references.values[i];
            bool duplicate = false;
            for (size_t j = 0; j < i; j++) if (!strcmp(references.values[j], reference)) duplicate = true;
            if (duplicate) continue;
            storage_images_free(images); images = NULL;
            bool removed = storage_inventory(w, runtime, NULL, &images) && storage_candidate(images, reference);
            const char *remove[] = {"image", runtime == RUNTIME_DOCKER ? "rm" : "delete", reference, NULL};
            StringBuilder output = {0};
            removed = removed && capture_runtime(w, remove, &output, runtime);
            StringBuilder *destination = removed ? &deleted : &failures;
            if (destination->length) append(destination, ",");
            if (removed) json_string(destination, reference);
            else {
                StringBuilder message = {0}; append(&message, reference); append(&message, ": image is now in use or deletion failed. Refresh storage and retry.");
                json_string(destination, message.data); free(message.data);
            }
            free(output.data);
        }
        free(selected); container_arguments_free(&references);
    }
    storage_images_free(images); images = NULL;
    ok = ok && storage_inventory(w, runtime, NULL, &images);
    if (ok) {
        append(extra, "\"storage\":{\"images\":["); size_t count = 0;
        for (StorageImage *image = images; image; image = image->next) if (storage_candidate(images, image->reference)) {
            if (count++) append(extra, ",");
            append(extra, "{"); field(extra, "reference", image->reference); append(extra, "}");
        }
        append(extra, "],\"deleted\":["); if (deleted.data) append(extra, deleted.data);
        append(extra, "],\"failures\":["); if (failures.data) append(extra, failures.data);
        append(extra, "],\"supportsBuildCacheCleanup\":"); append(extra, runtime == RUNTIME_DOCKER ? "true}" : "false}");
    }
    storage_images_free(images); free(deleted.data); free(failures.data); return ok;
}
