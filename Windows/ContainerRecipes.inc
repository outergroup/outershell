static bool container_uuid(const char *id) {
    if (!id || strlen(id) != 36) return false;
    for (size_t i = 0; i < 36; i++)
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!isxdigit((unsigned char)id[i])) return false;
    return true;
}

static char *container_directory(const char *id) {
    if (!container_uuid(id)) return NULL;
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *home = local ? win_join(local, "OuterShell") : NULL;
    char *root = home ? win_join(home, "containers") : NULL;
    char *directory = root && win_secure_directory(root) ? win_join(root, id) : NULL;
    if (directory && !win_secure_directory(directory)) { free(directory); directory = NULL; }
    free(root); free(home); free(local);
    return directory;
}

static char *container_recipe_directory(const char *id) {
    char *directory = container_directory(id);
    char *recipe = directory ? win_join(directory, "Recipe") : NULL;
    if (recipe && !win_secure_directory(recipe)) { free(recipe); recipe = NULL; }
    free(directory); return recipe;
}

static char *container_file(const char *id, const char *name) {
    char *directory = !strcmp(name, "Dockerfile") ? container_recipe_directory(id) : container_directory(id);
    char *path = directory ? win_join(directory, name) : NULL;
    free(directory);
    return path;
}

static bool container_write(const char *id, const char *name, const void *data, size_t size) {
    char *path = container_file(id, name);
    StringBuilder encoded = {0};
    bool binary = !strncmp(name, "configuration", 13);
    bool ok = !binary || container_value_encode((const char *)data, &encoded);
    ok = ok && path && win_atomic_write(path, binary ? encoded.data : data, binary ? encoded.length : size);
    free(encoded.data);
    free(path);
    return ok;
}

static char *container_read(const char *id, const char *name, const char *fallback) {
    char *path = container_file(id, name);
    StringBuilder data = {0};
    bool ok = path && win_read_file(path, WSL_LIMIT, &data);
    free(path);
    if (ok && data.data) {
        if (strncmp(name, "configuration", 13)) return data.data;
        StringBuilder decoded = {0};
        bool valid = container_value_decode(&data, &decoded);
        free(data.data); data = (StringBuilder){0};
        if (valid) return decoded.data;
        free(decoded.data);
    }
    free(data.data);
    return win_strdup(fallback);
}

static char *container_json_field(const char *json, const char *key) {
    const unsigned char *p = (const unsigned char *)json, *end = p + strlen(json);
    layout_json_space(&p, end);
    if (p == end || *p++ != '{') return NULL;
    while (p < end) {
        layout_json_space(&p, end);
        const unsigned char *start = p;
        if (!layout_json_string(&p, end)) return NULL;
        bool match = (size_t)(p - start) == strlen(key) + 2 && !memcmp(start + 1, key, strlen(key));
        layout_json_space(&p, end);
        if (p == end || *p++ != ':') return NULL;
        layout_json_space(&p, end);
        start = p;
        if (!layout_json_value(&p, end, 0)) return NULL;
        if (match) { StringBuilder result = {0}; bytes(&result, start, (size_t)(p - start)); return result.data; }
        layout_json_space(&p, end);
        if (p == end || *p++ != ',') return NULL;
    }
    return NULL;
}

typedef struct { char **values; size_t count; } ContainerArguments;
static bool container_argument(ContainerArguments *args, const char *value) {
    if (args->count >= 1024) return false;
    char **values = realloc(args->values, (args->count + 2) * sizeof(*values));
    if (!values) return false;
    args->values = values;
    args->values[args->count++] = win_strdup(value);
    args->values[args->count] = NULL;
    return true;
}
static void container_arguments_free(ContainerArguments *args) {
    for (size_t i = 0; i < args->count; i++) free(args->values[i]);
    free(args->values);
}

static bool container_configuration_array(const char *array, const char *kind, ContainerArguments *args) {
    const unsigned char *p = (const unsigned char *)array, *end = p + strlen(array);
    layout_json_space(&p, end);
    if (p == end || *p++ != '[') return false;
    size_t count = 0;
    while (p < end) {
        layout_json_space(&p, end);
        if (*p == ']') { p++; layout_json_space(&p, end); return p == end; }
        const unsigned char *start = p;
        if (++count > 128 || !layout_json_value(&p, end, 0) || *start != '{') return false;
        StringBuilder object = {0}, option = {0};
        bytes(&object, start, (size_t)(p - start));
        HttpRequest request = {0}; request.body = object;
        bool ok = false;
        if (!strcmp(kind, "mounts")) {
            char *host = request_string(&request, "hostPath"), *guest = request_string(&request, "guestPath");
            char *id = request_string(&request, "id"), *name = request_string(&request, "name");
            char *read_only = container_json_field(object.data, "isReadOnly");
            wchar_t *wide = host ? win_wide(host) : NULL;
            DWORD attr = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
            ok = container_uuid(id) && name && host && strlen(host) > 3 && isalpha((unsigned char)host[0]) &&
                 host[1] == ':' && (host[2] == '\\' || host[2] == '/') && !strpbrk(host, ",\r\n\"") &&
                 guest && guest[0] == '/' && guest[1] && !strpbrk(guest, ",\r\n\"") &&
                 attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
                 read_only && (!strcmp(read_only, "true") || !strcmp(read_only, "false"));
            if (ok) {
                append(&option, "type=bind,source="); append(&option, host);
                append(&option, ",target="); append(&option, guest);
                if (!strcmp(read_only, "true")) append(&option, ",readonly");
                ok = container_argument(args, "--mount") && container_argument(args, option.data);
            }
            free(host); free(guest); free(id); free(name); free(wide); free(read_only);
        } else if (!strcmp(kind, "environment")) {
            char *name = request_string(&request, "name"), *value = request_string(&request, "value");
            ok = name && *name && !strpbrk(name, "=\r\n") && value;
            if (ok) {
                append(&option, name); append(&option, "="); append(&option, value);
                ok = container_argument(args, "--env") && container_argument(args, option.data);
            }
            free(name); free(value);
        } else {
            unsigned host = container_number(&request, "hostPort", 0), guest = container_number(&request, "containerPort", 0);
            ok = host && host <= 65535 && guest && guest <= 65535;
            if (ok) {
                char port[64]; snprintf(port, sizeof(port), "127.0.0.1:%u:%u", host, guest);
                ok = container_argument(args, "--publish") && container_argument(args, port);
            }
        }
        free(object.data); free(option.data);
        if (!ok) return false;
        layout_json_space(&p, end);
        if (p == end) return false;
        if (*p == ']') continue;
        if (*p++ != ',') return false;
        layout_json_space(&p, end);
        if (p == end || *p == ']') return false;
    }
    return false;
}

static bool container_configuration(const char *json, ContainerArguments *args) {
    const char *keys[] = {"mounts", "environment", "publishedPorts"};
    for (size_t i = 0; i < 3; i++) {
        char *array = container_json_field(json, keys[i]);
        bool ok = array && container_configuration_array(array, keys[i], args);
        free(array);
        if (!ok) return false;
    }
    return true;
}

static bool container_path_exists(const char *path) {
    wchar_t *wide = path ? win_wide(path) : NULL;
    bool exists = wide && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
    free(wide); return exists;
}

static char *container_volume_name(const char *id, const char *item) {
    if (!container_uuid(id) || !container_uuid(item)) return NULL;
    StringBuilder name = {0}; append(&name, "outershell-"); append(&name, id); append(&name, "-"); append(&name, item);
    CharLowerBuffA(name.data, (DWORD)name.length); return name.data;
}

static char *container_volume_mount(const char *id, const char *item) {
    char *directory = container_directory(id), *root = directory ? win_join(directory, "Persistent Data") : NULL;
    char *legacy = root && container_uuid(item) ? win_join(root, item) : NULL;
    char *name = container_volume_name(id, item);
    StringBuilder mount = {0};
    if (name) {
        append(&mount, container_path_exists(legacy) ? "type=bind,source=" : "type=volume,source=");
        append(&mount, container_path_exists(legacy) ? legacy : name);
    }
    free(directory); free(root); free(legacy); free(name); return mount.data;
}

static bool container_volume_transfer(Wsl *w, ContainerRuntime runtime, const char *id, const char *item,
                                      const char *directory, bool restore) {
    char *source = container_volume_mount(id, item);
    StringBuilder data = {0}, transfer = {0}, file = {0}, output = {0};
    if (source) append(&data, source);
    append(&data, restore ? ",target=/data" : ",target=/data,readonly");
    append(&transfer, "type=bind,source="); append(&transfer, directory);
    append(&transfer, restore ? ",target=/transfer,readonly" : ",target=/transfer");
    append(&file, "/transfer/"); append(&file, item); append(&file, ".tar");
    const char *args[] = {"run", "--rm", "--network", "none", "--user", "0", "--mount", data.data,
                          "--mount", transfer.data, "--entrypoint", "/bin/tar", "alpine:3.22",
                          restore ? "-xpf" : "-cpf", file.data, "-C", "/data", restore ? NULL : ".", NULL};
    bool ok = source && win_secure_directory(directory) && capture_runtime(w, args, &output, runtime);
    free(source); free(data.data); free(transfer.data); free(file.data); free(output.data); return ok;
}

static bool container_volume_prepare(Wsl *w, ContainerRuntime runtime, const char *id, const char *item) {
    char *mount = container_volume_mount(id, item);
    if (mount && !strncmp(mount, "type=bind,", 10)) { free(mount); return true; }
    free(mount);
    char *name = container_volume_name(id, item), *directory = container_directory(id);
    char *archives = directory ? win_join(directory, "Volume Archives") : NULL;
    StringBuilder filename = {0}; append(&filename, item); append(&filename, ".tar");
    char *archive = archives ? win_join(archives, filename.data) : NULL;
    StringBuilder output = {0};
    const char *inspect[] = {"volume", "inspect", name, NULL};
    bool exists = name && capture_runtime(w, inspect, &output, runtime);
    bool pending = container_path_exists(archive), ok = name && !(exists && pending);
    free(output.data); output = (StringBuilder){0};
    if (ok && !exists) {
        const char *create[] = {"volume", "create", name, NULL};
        ok = capture_runtime(w, create, &output, runtime);
        if (ok && pending) {
            ok = container_volume_transfer(w, runtime, id, item, archives, true);
            if (ok) {
                wchar_t *wide = win_wide(archive); ok = wide && DeleteFileW(wide); free(wide);
            } else {
                const char *remove[] = {"volume", runtime == RUNTIME_DOCKER ? "rm" : "remove", name, NULL};
                free(output.data); output = (StringBuilder){0}; capture_runtime(w, remove, &output, runtime);
            }
        }
    }
    free(name); free(directory); free(archives); free(filename.data); free(archive); free(output.data); return ok;
}

static bool container_volumes_remove(Wsl *w, ContainerRuntime runtime, const char *id, const char *config) {
    char *items = container_json_field(config, "persistentData");
    const unsigned char *p = (const unsigned char *)(items ? items : "[]") + 1, *end = p + strlen((const char *)p);
    bool ok = true;
    while (ok && p < end) {
        layout_json_space(&p, end); if (*p == ']') break;
        const unsigned char *start = p; ok = layout_json_value(&p, end, 0);
        char *item = ok ? container_string((const char *)start, (size_t)(p - start), "id") : NULL;
        char *name = container_volume_name(id, item);
        StringBuilder output = {0};
        const char *inspect[] = {"volume", "inspect", name, NULL};
        ok = name != NULL;
        if (ok && capture_runtime(w, inspect, &output, runtime)) {
            const char *remove[] = {"volume", runtime == RUNTIME_DOCKER ? "rm" : "remove", name, NULL};
            free(output.data); output = (StringBuilder){0};
            ok = capture_runtime(w, remove, &output, runtime);
        }
        free(item); free(name); free(output.data);
        layout_json_space(&p, end); if (p < end && *p == ',') p++;
    }
    free(items); return ok;
}

static bool container_persistent_arguments(Wsl *w, ContainerRuntime runtime, const char *id,
                                            const char *config, ContainerArguments *args) {
    char *array = container_json_field(config, "persistentData");
    if (!array) return true;
    bool ok = array[0] == '[';
    const unsigned char *p = (const unsigned char *)array + 1, *end = (const unsigned char *)array + strlen(array);
    size_t count = 0;
    while (ok && p < end) {
        layout_json_space(&p, end); if (*p == ']') break;
        const unsigned char *start = p; ok = ++count <= 128 && layout_json_value(&p, end, 0);
        if (!ok) break;
        char *item = container_string((const char *)start, (size_t)(p - start), "id");
        char *guest = container_string((const char *)start, (size_t)(p - start), "guestPath");
        if (item) CharLowerBuffA(item, (DWORD)strlen(item));
        ok = container_uuid(item) && guest && guest[0] == '/' && guest[1] && !strpbrk(guest, ",\r\n\"");
        StringBuilder mount = {0};
        if (ok) {
            ok = container_volume_prepare(w, runtime, id, item);
            char *source = container_volume_mount(id, item);
            if (source) append(&mount, source);
            append(&mount, ",target="); append(&mount, guest);
            ok = ok && source && container_argument(args, "--mount") && container_argument(args, mount.data);
            free(source);
        }
        free(item); free(guest); free(mount.data);
        layout_json_space(&p, end); if (p < end && *p == ',') p++;
    }
    free(array); return ok;
}

static const char *container_empty_configuration = "{\"mounts\":[],\"environment\":[],\"publishedPorts\":[]}";

static bool container_has_guest_path(const char *array, const char *guest) {
    if (!array || array[0] != '[') return false;
    const unsigned char *p = (const unsigned char *)array + 1, *end = p + strlen(array) - 1;
    while (p < end) {
        layout_json_space(&p, end); if (p == end || *p == ']') break;
        const unsigned char *start = p;
        if (!layout_json_value(&p, end, 0)) return false;
        char *path = container_string((const char *)start, (size_t)(p - start), "guestPath");
        bool match = path && !strcmp(path, guest); free(path);
        if (match) return true;
        layout_json_space(&p, end); if (p < end && *p == ',') p++;
    }
    return false;
}

static bool container_image_volumes(Wsl *w, ContainerRuntime runtime, const char *id, const char *image) {
    const char *inspect[] = {"image", "inspect", image, NULL};
    StringBuilder output = {0}, items = {0}, updated = {0};
    bool ok = capture_runtime(w, inspect, &output, runtime);
    char *object = output.data ? strchr(output.data, '{') : NULL;
    char *image_config = object ? container_json_field(object, "Config") : NULL;
    char *volumes = image_config ? container_json_field(image_config, "Volumes") : NULL;
    char *config = container_read(id, "configuration", container_empty_configuration);
    char *previous = container_json_field(config, "persistentData");
    char *mounts = container_json_field(config, "mounts");
    append(&items, "[");
    ok = ok && image_config != NULL;
    if (ok && volumes && volumes[0] == '{') {
        const unsigned char *p = (const unsigned char *)volumes + 1, *end = p + strlen(volumes) - 1;
        while (ok && p < end) {
            layout_json_space(&p, end); if (p == end || *p == '}') break;
            const unsigned char *start = p;
            ok = layout_json_string(&p, end);
            if (!ok) break;
            StringBuilder wrapper = {0}; append(&wrapper, "{\"path\":"); bytes(&wrapper, start, (size_t)(p - start)); append(&wrapper, "}");
            char *guest = container_string(wrapper.data, wrapper.length, "path"); free(wrapper.data);
            ok = guest && guest[0] == '/' && guest[1] && !strpbrk(guest, ",\r\n\"");
            if (ok && !container_has_guest_path(mounts, guest)) {
                char item_id[37];
                ok = container_identity(runtime, guest, item_id);
                if (ok && previous) {
                    const unsigned char *q = (const unsigned char *)previous + 1, *limit = q + strlen(previous) - 1;
                    while (q < limit) {
                        layout_json_space(&q, limit); if (q == limit || *q == ']') break;
                        const unsigned char *entry = q;
                        if (!layout_json_value(&q, limit, 0)) { ok = false; break; }
                        char *path = container_string((const char *)entry, (size_t)(q - entry), "guestPath");
                        char *old_id = container_string((const char *)entry, (size_t)(q - entry), "id");
                        if (path && !strcmp(path, guest) && container_uuid(old_id)) strcpy_s(item_id, sizeof(item_id), old_id);
                        free(path); free(old_id);
                        layout_json_space(&q, limit); if (q < limit && *q == ',') q++;
                    }
                }
                if (ok) {
                    if (items.length > 1) append(&items, ",");
                    append(&items, "{"); field(&items, "id", item_id); append(&items, ","); field(&items, "guestPath", guest); append(&items, "}");
                }
            }
            free(guest);
            layout_json_space(&p, end);
            ok = ok && p < end && *p++ == ':' && layout_json_value(&p, end, 0);
            layout_json_space(&p, end); if (p < end && *p == ',') p++;
        }
    }
    append(&items, "]"); append(&updated, "{");
    const char *fields[] = {"mounts", "environment", "publishedPorts"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        char *value = container_json_field(config, fields[i]);
        if (i) append(&updated, ","); json_string(&updated, fields[i]); append(&updated, ":"); append(&updated, value ? value : "[]"); free(value);
    }
    append(&updated, ",\"persistentData\":"); append(&updated, items.data); append(&updated, "}");
    ok = ok && container_write(id, "configuration", updated.data, updated.length);
    free(output.data); free(items.data); free(updated.data); free(image_config); free(volumes); free(config); free(previous); free(mounts);
    return ok;
}

static bool container_definition_create(const char *id, const char *image) {
    StringBuilder dockerfile = {0}; append(&dockerfile, "FROM "); append(&dockerfile, image); append(&dockerfile, "\n");
    bool ok = container_write(id, "Dockerfile", dockerfile.data, dockerfile.length) &&
              container_write(id, "Dockerfile.realized", dockerfile.data, dockerfile.length) &&
              container_write(id, "configuration", container_empty_configuration, strlen(container_empty_configuration)) &&
              container_write(id, "configuration.realized", container_empty_configuration, strlen(container_empty_configuration));
    free(dockerfile.data);
    return ok;
}

static bool container_definition_update(Distribution *d, HttpRequest *request, bool configuration) {
    if (!d->managed) return false;
    if (!configuration) {
        char *contents = request_string(request, "dockerfile");
        bool ok = contents && *contents && container_write(d->id, "Dockerfile", contents, strlen(contents));
        free(contents); return ok;
    }
    StringBuilder value = {0}; append(&value, "{");
    const char *keys[] = {"mounts", "environment", "publishedPorts"};
    for (size_t i = 0; i < 3; i++) {
        char *array = container_json_field(request->body.data, keys[i]);
        if (i) append(&value, ",");
        json_string(&value, keys[i]); append(&value, ":"); append(&value, array ? array : "[]"); free(array);
    }
    char *previous = container_read(d->id, "configuration", container_empty_configuration);
    char *persistent = container_json_field(previous, "persistentData");
    if (persistent) { append(&value, ",\"persistentData\":"); append(&value, persistent); }
    free(previous); free(persistent);
    append(&value, "}");
    ContainerArguments args = {0};
    bool ok = container_configuration(value.data, &args) && container_write(d->id, "configuration", value.data, value.length);
    container_arguments_free(&args); free(value.data); return ok;
}

static char *container_base_image(const char *dockerfile) {
    char *copy = win_strdup(dockerfile), *context = NULL, *result = NULL;
    for (char *line = strtok_s(copy, "\r\n", &context); line; line = strtok_s(NULL, "\r\n", &context)) {
        while (*line == ' ' || *line == '\t') line++;
        if (_strnicmp(line, "FROM", 4) || (line[4] != ' ' && line[4] != '\t')) continue;
        char *token_context = NULL;
        for (char *token = strtok_s(line + 4, " \t", &token_context); token; token = strtok_s(NULL, " \t", &token_context)) {
            if (!strncmp(token, "--", 2)) continue;
            result = win_strdup(token); break;
        }
        break;
    }
    free(copy); return result ? result : win_strdup("");
}

static void container_recipe_snapshot(Distribution *d, StringBuilder *json) {
    char *dockerfile = container_read(d->id, "Dockerfile", "");
    char *realized = container_read(d->id, "Dockerfile.realized", "");
    char *config = container_read(d->id, "configuration", container_empty_configuration);
    char *prior = container_read(d->id, "configuration.realized", "");
    char *path = container_file(d->id, "Dockerfile"), *directory = container_recipe_directory(d->id);
    char *mounts = container_json_field(config, "mounts"), *env = container_json_field(config, "environment"), *ports = container_json_field(config, "publishedPorts");
    append(json, ",\"mounts\":"); append(json, mounts ? mounts : "[]");
    append(json, ",\"persistentData\":[");
    char *persistent = container_json_field(config, "persistentData");
    if (persistent && persistent[0] == '[') {
        const unsigned char *p = (const unsigned char *)persistent + 1, *end = (const unsigned char *)persistent + strlen(persistent);
        size_t count = 0;
        while (p < end) {
            layout_json_space(&p, end); if (*p == ']') break;
            const unsigned char *start = p; if (!layout_json_value(&p, end, 0)) break;
            char *item = container_string((const char *)start, (size_t)(p - start), "id");
            char *guest = container_string((const char *)start, (size_t)(p - start), "guestPath");
            char *root = container_directory(d->id), *data = root ? win_join(root, "Persistent Data") : NULL;
            char *host = data && container_uuid(item) ? win_join(data, item) : NULL;
            if (host && !container_path_exists(host)) { free(host); host = container_volume_name(d->id, item); }
            if (host && guest) {
                if (count++) append(json, ","); append(json, "{"); field(json, "id", item);
                append(json, ","); field(json, "hostPath", host); append(json, ","); field(json, "guestPath", guest); append(json, "}");
            }
            free(item); free(guest); free(root); free(data); free(host);
            layout_json_space(&p, end); if (p < end && *p == ',') p++;
        }
    }
    free(persistent);
    append(json, "],\"recipe\":{");
    char *base_image = container_base_image(dockerfile);
    field(json, "baseImage", base_image); free(base_image); append(json, ","); field(json, "containerfile", dockerfile);
    append(json, ","); field(json, "dockerfileHostPath", path ? path : "");
    append(json, ","); field(json, "definitionDirectory", directory ? directory : "");
    append(json, ",\"dockerfileGuestPath\":\"/var/lib/outershell/project/Dockerfile\","
                 "\"installsOuterShellSupport\":false,\"outerShellSupportSnippet\":\"\","
                 "\"steps\":[],\"fragments\":[],\"catalog\":[],\"launchers\":[],\"scriptFiles\":[],"
                 "\"users\":[{\"id\":\"root\",\"name\":\"root\",\"homeDirectory\":\"/root\",\"workingDirectory\":\"/\",\"isRoot\":true}],"
                 "\"buildEngineAvailable\":true,\"transferItems\":[],\"hasUntrackedChanges\":false,\"workingDirectory\":\"/\",\"needsRebuild\":");
    append(json, strcmp(dockerfile, realized) || strcmp(config, prior) ? "true" : "false");
    append(json, ",\"environment\":"); append(json, env ? env : "[]");
    append(json, ",\"publishedPorts\":"); append(json, ports ? ports : "[]"); append(json, "}");
    free(dockerfile); free(realized); free(config); free(prior); free(path); free(directory); free(mounts); free(env); free(ports);
}

static void publish_snapshot(Wsl *w);
static DWORD WINAPI container_rebuild_thread(void *context) {
    Distribution *d = context;
    Wsl *w = d->owner;
    EnterCriticalSection(&w->commands);
    char *directory = container_recipe_directory(d->id);
    char *dockerfile = container_read(d->id, "Dockerfile", "");
    char *config = container_read(d->id, "configuration", container_empty_configuration);
    ContainerArguments args = {0};
    StringBuilder output = {0};
    char image[100], name[100], cpus[16], memory[16];
    snprintf(image, sizeof(image), "outershell/managed-%s:latest", d->id);
    snprintf(name, sizeof(name), "outershell-%s-%llu", d->id, (unsigned long long)GetTickCount64());
    snprintf(cpus, sizeof(cpus), "%u", d->cpus ? d->cpus : 4);
    snprintf(memory, sizeof(memory), "%uM", d->memory_mb ? d->memory_mb : 8192);
    const char *error = "The container configuration is invalid.";
    bool ok = directory && *dockerfile && container_configuration(config, &args);
    container_arguments_free(&args); args = (ContainerArguments){0};
    if (ok) {
        const char *build[] = {"build", "--progress", "plain", "--tag", image, directory, NULL};
        error = "The image build failed. The existing container was retained. See the container runtime log.";
        ok = capture_runtime(w, build, &output, d->container);
        if (output.data) container_write(d->id, "build.log", output.data, output.length);
    }
    if (ok) {
        error = "Could not prepare persistent image volumes.";
        ok = container_image_volumes(w, d->container, d->id, image);
        free(config); config = container_read(d->id, "configuration", container_empty_configuration);
    }
    bool created = false, stopped = false;
    char candidate[65] = {0};
    if (ok) {
        const char *prefix[] = {"create", "--name", name, "--cpus", cpus, "--memory", memory};
        for (size_t i = 0; i < sizeof(prefix) / sizeof(*prefix); i++) ok = container_argument(&args, prefix[i]) && ok;
        StringBuilder mount = {0}; append(&mount, "type=bind,source="); append(&mount, directory);
        append(&mount, ",target=/var/lib/outershell/project");
        ok = ok && container_argument(&args, "--mount") && container_argument(&args, mount.data) &&
             container_configuration(config, &args) && container_persistent_arguments(w, d->container, d->id, config, &args) && container_argument(&args, image);
        free(mount.data); free(output.data); output = (StringBuilder){0};
        error = "Could not create the replacement container. The existing container was retained.";
        ok = ok && capture_runtime(w, (const char *const *)args.values, &output, d->container);
        if (ok) {
            char *line = output.data;
            while (line && strspn(line, "0123456789abcdef") != 64) { line = strchr(line, '\n'); if (line) line++; }
            ok = line != NULL;
            if (ok) { memcpy(candidate, line, 64); created = true; }
        }
    }
    if (ok && d->running) {
        const char *stop[] = {"stop", d->runtime_id, NULL};
        free(output.data); output = (StringBuilder){0};
        ok = capture_runtime(w, stop, &output, d->container); stopped = ok;
        error = "Could not stop the existing container for replacement.";
    }
    if (ok) {
        const char *start[] = {"start", candidate, NULL};
        free(output.data); output = (StringBuilder){0};
        ok = capture_runtime(w, start, &output, d->container);
        error = "The replacement container could not start. The previous container was retained.";
    }
    if (ok) {
        const char *remove[] = {d->container == RUNTIME_DOCKER ? "rm" : "remove", "--force", d->runtime_id, NULL};
        free(output.data); output = (StringBuilder){0};
        bool removed = !*d->runtime_id || capture_runtime(w, remove, &output, d->container);
        if (!removed) fprintf(stderr, "Could not remove replaced container %s\n", d->runtime_id);
        EnterCriticalSection(&w->gate);
        free(d->runtime_id); d->runtime_id = win_strdup(candidate); d->running = true;
        LeaveCriticalSection(&w->gate);
        ok = container_write(d->id, "Dockerfile.realized", dockerfile, strlen(dockerfile)) &&
             container_write(d->id, "configuration.realized", config, strlen(config));
        error = "The rebuilt container is running, but its saved build state could not be updated.";
    } else {
        if (created) {
            const char *remove[] = {d->container == RUNTIME_DOCKER ? "rm" : "remove", "--force", candidate, NULL};
            free(output.data); output = (StringBuilder){0};
            capture_runtime(w, remove, &output, d->container);
        }
        if (stopped) {
            const char *start[] = {"start", d->runtime_id, NULL};
            free(output.data); output = (StringBuilder){0};
            if (!capture_runtime(w, start, &output, d->container))
                error = "The replacement failed, and the previous container could not restart. Its data was retained.";
        }
    }
    container_arguments_free(&args); free(directory); free(dockerfile); free(config); free(output.data);
    EnterCriticalSection(&w->gate);
    free(d->build_error); d->build_error = ok ? NULL : win_strdup(error); d->building = false;
    LeaveCriticalSection(&w->gate);
    publish_snapshot(w);
    LeaveCriticalSection(&w->commands);
    SetEvent(w->refresh);
    return 0;
}

static bool container_rebuild(Distribution *d) {
    if (!d->managed || d->building) return false;
    if (d->build_thread) {
        if (WaitForSingleObject(d->build_thread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(d->build_thread); d->build_thread = NULL;
    }
    d->building = true; free(d->build_error); d->build_error = NULL;
    d->build_thread = CreateThread(NULL, 0, container_rebuild_thread, d, 0, NULL);
    if (!d->build_thread) d->building = false;
    publish_snapshot(d->owner);
    return d->build_thread != NULL;
}
