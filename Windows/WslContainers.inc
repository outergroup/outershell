static char *request_string(const HttpRequest *request, const char *name);

static char *container_string(const char *json, size_t length, const char *key) {
    HttpRequest request = {0};
    request.body = (StringBuilder){(char *)json, length, 0};
    return request_string(&request, key);
}

static bool container_identity(const char *runtime_id, char id[37]) {
    unsigned char hash[32];
    BCRYPT_ALG_HANDLE algorithm = NULL;
    StringBuilder input = {0};
    append(&input, "wsl.container:");
    append(&input, runtime_id);
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
              BCryptHash(algorithm, NULL, 0, (PUCHAR)input.data, (ULONG)input.length, hash,
                         sizeof(hash)) == 0;
    free(input.data);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        return false;
    snprintf(id, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
             hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return true;
}

static char *container_metadata_path(const char *id) {
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *root = local ? win_join(local, "OuterShell") : NULL;
    char *directory = root ? win_join(root, "container-names") : NULL;
    bool ok = directory && win_secure_directory(directory);
    char *path = ok ? win_join(directory, id) : NULL;
    free(local);
    free(root);
    free(directory);
    return path;
}

static bool valid_container_name(const char *name) {
    return name && *name && strlen(name) <= 200 && !strpbrk(name, "\r\n\t");
}

static bool save_container_metadata(const char *id, const char *name, unsigned cpus, unsigned memory) {
    char *path = container_metadata_path(id);
    StringBuilder data = {0};
    append(&data, name);
    char limits[64];
    snprintf(limits, sizeof(limits), "\n%u\n%u\n", cpus, memory);
    append(&data, limits);
    bool ok = path && win_atomic_write(path, data.data, data.length);
    free(path);
    free(data.data);
    return ok;
}

static unsigned container_number(const HttpRequest *request, const char *key, unsigned fallback) {
    const unsigned char *p = (const unsigned char *)request->body.data;
    const unsigned char *end = p + request->body.length;
    layout_json_space(&p, end);
    if (p == end || *p++ != '{') return fallback;
    while (p < end) {
        layout_json_space(&p, end);
        const unsigned char *start = p;
        if (!layout_json_string(&p, end)) break;
        bool match = (size_t)(p - start) == strlen(key) + 2 && !memcmp(start + 1, key, strlen(key));
        layout_json_space(&p, end);
        if (p == end || *p++ != ':') break;
        layout_json_space(&p, end);
        start = p;
        if (!layout_json_value(&p, end, 0)) break;
        if (match) {
            unsigned value = 0;
            if (p == start || p - start > 4) return 0;
            for (; start < p; start++) {
                if (*start < '0' || *start > '9') return 0;
                value = value * 10 + (*start - '0');
            }
            return value;
        }
        layout_json_space(&p, end);
        if (p == end || *p++ != ',') break;
    }
    return fallback;
}

static bool create_container(Wsl *w, HttpRequest *request, const char **error) {
    char *name = request_string(request, "name");
    char *provider = request_string(request, "runtimeProviderID");
    char *image = request_string(request, "baseImage");
    unsigned cpus = container_number(request, "cpus", 4);
    unsigned memory = container_number(request, "memoryInGB", 8);
    bool ok = false;
    StringBuilder output = {0};
    *error = "Enter a container name, a valid image reference, and CPU/memory limits between 1 and 128.";
    if (!valid_container_name(name) || !provider || strcmp(provider, "wsl.container") ||
        !image || !*image || strlen(image) > 512 ||
        strspn(image, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/:@") != strlen(image) ||
        !cpus || cpus > 128 || !memory || memory > 128)
        goto done;
    if (!strcmp(image, "outershell/container-base:17")) {
        const char *inspect[] = {"image", "inspect", image, NULL};
        if (!capture_runtime(w, inspect, &output, true)) {
            free(output.data);
            output = (StringBuilder){0};
            char *directory = win_executable_directory();
            char *context = directory ? win_join(directory, "container-bootstrap") : NULL;
            const char *build[] = {"build", "--progress", "plain", "--tag", image, context, NULL};
            ok = context && capture_runtime(w, build, &output, true);
            free(context);
            free(directory);
            if (!ok) {
                *error = "Could not prepare the Outer Shell container image. Check the Outer Shell log and the installed container bootstrap.";
                goto done;
            }
        }
    }
    free(output.data);
    output = (StringBuilder){0};
    ok = false;
    GUID guid;
    wchar_t guid_text[40];
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, guid_text, 40)) goto done;
    char *uuid = win_utf8(guid_text);
    if (!uuid) goto done;
    uuid[37] = 0;
    char runtime_name[80], cpu_text[12], memory_text[12];
    snprintf(runtime_name, sizeof(runtime_name), "outershell-%s", uuid + 1);
    free(uuid);
    snprintf(cpu_text, sizeof(cpu_text), "%u", cpus);
    snprintf(memory_text, sizeof(memory_text), "%uG", memory);
    const char *run[] = {"run", "--detach", "--name", runtime_name, "--cpus", cpu_text,
                        "--memory", memory_text, image, NULL};
    *error = "WSL containers could not create the container. Check the image name and the Outer Shell log.";
    if (!capture_runtime(w, run, &output, true)) goto done;
    char *last = output.data;
    for (char *line = output.data; line && *line;) {
        char *end = strchr(line, '\n');
        if (strspn(line, "0123456789abcdef") == 64) { last = line; break; }
        line = end ? end + 1 : NULL;
    }
    char id[37], runtime_id[65];
    if (!last || strspn(last, "0123456789abcdef") != 64) goto done;
    memcpy(runtime_id, last, 64);
    runtime_id[64] = 0;
    ok = container_identity(runtime_id, id) && save_container_metadata(id, name, cpus, memory);
    if (!ok) *error = "The container was created, but its display name could not be saved. It remains available as an attached container.";
done:
    free(name);
    free(provider);
    free(image);
    free(output.data);
    return ok;
}

static void discover_containers(Wsl *w) {
    const char *args[] = {"list", "--all", "--no-trunc", "--format", "json", NULL};
    StringBuilder output = {0};
    bool available = capture_runtime(w, args, &output, true);
    w->containers_available = available;
    if (!available) {
        free(output.data);
        return;
    }
    EnterCriticalSection(&w->gate);
    for (Distribution *d = w->distributions; d; d = d->next)
        if (d->container)
            d->present = false;
    LeaveCriticalSection(&w->gate);
    size_t count = 0;
    for (char *line = output.data; line && *line && count < 64;) {
        char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        char *runtime_id = container_string(line, length, "ID");
        char *name = container_string(line, length, "Names");
        char *state = container_string(line, length, "State");
        char id[37];
        bool valid = runtime_id && strlen(runtime_id) == 64 &&
                     strspn(runtime_id, "0123456789abcdef") == 64 && name && *name && state;
        if (valid && container_identity(runtime_id, id)) {
            EnterCriticalSection(&w->gate);
            Distribution *d;
            for (d = w->distributions; d; d = d->next)
                if (d->container && !strcmp(d->id, id))
                    break;
            if (!d) {
                d = win_alloc(sizeof(*d));
                d->owner = w;
                d->container = true;
                d->id = win_strdup(id);
                d->runtime_id = win_strdup(runtime_id);
                d->user = win_strdup("root");
                d->next = w->distributions;
                w->distributions = d;
            }
            free(d->name);
            d->name = win_strdup(name);
            char *path = container_metadata_path(id);
            StringBuilder metadata = {0};
            d->managed = path && win_read_file(path, 1024, &metadata) && metadata.data &&
                         strchr(metadata.data, '\n');
            if (d->managed) {
                char *newline = strchr(metadata.data, '\n');
                *newline = 0;
                free(d->name);
                d->name = win_strdup(metadata.data);
                sscanf(newline + 1, "%u\n%u", &d->cpus, &d->memory_gb);
            }
            free(metadata.data);
            free(path);
            d->present = true;
            d->running = !strcmp(state, "running");
            LeaveCriticalSection(&w->gate);
            if (d->running) {
                StringBuilder apps = {0};
                if (app_list(d, &apps)) {
                    free(d->apps.data);
                    d->apps = apps;
                } else
                    free(apps.data);
            }
            count++;
        }
        free(runtime_id);
        free(name);
        free(state);
        line = end ? end + 1 : NULL;
    }
    free(output.data);
}

static void container_provider(Wsl *w, StringBuilder *json) {
    append(json, "{\"id\":\"wsl.container\",\"name\":\"WSL containers\",\"isAvailable\":");
    append(json, w->containers_available ? "true" : "false");
    append(json, ",\"isolationName\":\"Linux containers\",\"capabilities\":{"
                 "\"supportsApps\":true,\"supportsShell\":true,\"supportsRecipes\":false,"
                 "\"supportsMounts\":false,\"supportsLiveMounts\":false},\"status\":");
    json_string(json, w->containers_available ? "ready" : "unavailable");
    append(json, ",\"detail\":");
    json_string(json, w->containers_available ? "WSL containers is ready." :
                "Install or update WSL, then check again. WSL containers requires WSL 2.9.3 or later.");
    append(json, ",\"setupURL\":\"https://learn.microsoft.com/windows/wsl/wsl-container\","
                 "\"defaultBaseImage\":\"outershell/container-base:17\"}");
}

static void container_snapshot(Distribution *d, StringBuilder *json) {
    append(json, "{");
    field(json, "id", d->id);
    append(json, ",");
    field(json, "name", d->name);
    append(json, ",");
    field(json, "state", d->running ? "running" : "stopped");
    append(json, ",\"runtimeKind\":\"wslContainer\",\"managementKind\":");
    json_string(json, d->managed ? "managed" : "attached");
    append(json, ",\"ownsContainer\":");
    append(json, d->managed ? "true" : "false");
    char limits[100];
    snprintf(limits, sizeof(limits), ",\"userName\":\"root\",\"cpus\":%u,\"memoryInGB\":%u,", d->cpus, d->memory_gb);
    append(json, limits);
    append(json,
                 "\"runtime\":{\"providerID\":\"wsl.container\",\"providerName\":\"WSL containers\","
                 "\"operatingSystemName\":\"Linux\",\"operatingSystemVersion\":\"\","
                 "\"architecture\":\"\",\"isolationKind\":\"wslContainer\",\"isolationName\":\"\"},"
                 "\"capabilities\":{\"supportsApps\":true,\"supportsShell\":true,"
                 "\"supportsRecipes\":false,\"supportsMounts\":false,\"supportsLiveMounts\":false},"
                 "\"shellCommand\":");
    StringBuilder command = {0};
    append(&command, "wslc exec -it ");
    append(&command, d->runtime_id);
    append(&command, " /bin/sh");
    json_string(json, command.data);
    free(command.data);
    append(json, ",\"outerShellSupport\":{");
    field(json, "status", d->apps.data ? "available" : d->running ? "missing" : "unknown");
    append(json, ",");
    field(json, "detail", d->apps.data ? "" : d->running ?
          "This image does not have a running Outer Shell service manager." :
          "Start this container to discover its apps.");
    append(json, "},\"apps\":");
    const char *apps = d->apps.data ? d->apps.data : "[]";
    const char *running_flag = "\"isRunning\":true";
    const char *flag;
    while (!d->running && (flag = strstr(apps, running_flag)) != NULL) {
        bytes(json, apps, (size_t)(flag - apps));
        append(json, "\"isRunning\":false");
        apps = flag + strlen(running_flag);
    }
    append(json, apps);
    append(json, ",\"commands\":[],\"mounts\":[],\"persistentData\":[]}");
}

static bool container_service_id(const char *service) {
    return service && isalnum((unsigned char)service[0]) && strlen(service) < 256 &&
           strspn(service, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == strlen(service);
}

static void container_logs(Distribution *d, Connection *client, HttpRequest *request) {
    char *service = request_string(request, "serviceID");
    if (!container_service_id(service)) {
        free(service);
        http_text(client, request, 400, "Invalid app identifier.");
        return;
    }
    StringBuilder path = {0}, output = {0}, json = {0};
    append(&path, "/var/lib/outershell/apps/");
    append(&path, service);
    append(&path, "/backend.log");
    const char *args[] = {"exec", "--user", "0", d->runtime_id,
                          "/usr/bin/tail", "-c", "1048576", path.data, NULL};
    bool ok = capture_runtime(d->owner, args, &output, true);
    append(&json, "{");
    char *request_id = request_string(request, "requestID");
    if (request_id) {
        field(&json, "requestID", request_id);
        append(&json, ",");
    }
    append(&json, "\"appLog\":{");
    field(&json, "path", path.data);
    append(&json, ",");
    field(&json, "contents", output.data ? output.data : "");
    append(&json, ",");
    field(&json, "error", ok ? "" : "The app log is not available yet.");
    char numbers[120];
    snprintf(numbers, sizeof(numbers), ",\"isTruncated\":%s,\"fileSize\":%zu,\"modified\":0},",
             output.length >= 1048576 ? "true" : "false", output.length);
    append(&json, numbers);
    EnterCriticalSection(&d->owner->gate);
    append(&json, d->owner->snapshot.data ? d->owner->snapshot.data + 1 : "\"workspaces\":[]}");
    LeaveCriticalSection(&d->owner->gate);
    http_send(client, request, 200, "application/json", json.data, json.length);
    free(request_id);
    free(service);
    free(path.data);
    free(output.data);
    free(json.data);
}
