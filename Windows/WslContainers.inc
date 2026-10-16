static char *request_string(const HttpRequest *request, const char *name);

static char *container_string(const char *json, size_t length, const char *key) {
    HttpRequest request = {0};
    request.body = (StringBuilder){(char *)json, length, 0};
    return request_string(&request, key);
}

static const char *container_provider_id(ContainerRuntime runtime) {
    return runtime == RUNTIME_DOCKER ? "docker" : "wsl.container";
}

static const char *container_provider_name(ContainerRuntime runtime) {
    return runtime == RUNTIME_DOCKER ? "Docker" : "WSL containers";
}

static bool container_identity(ContainerRuntime runtime, const char *runtime_id, char id[37]) {
    unsigned char hash[32];
    BCRYPT_ALG_HANDLE algorithm = NULL;
    StringBuilder input = {0};
    append(&input, container_provider_id(runtime));
    append(&input, ":");
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
            if (p == start || p - start > 5) return 0;
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

#include "ContainerValue.inc"
#include "ContainerRecipes.inc"

static bool create_container(Wsl *w, HttpRequest *request, const char **error) {
    char *name = request_string(request, "name");
    char *provider = request_string(request, "runtimeProviderID");
    char *image = request_string(request, "baseImage");
    ContainerRuntime runtime = provider && !strcmp(provider, "docker") ? RUNTIME_DOCKER : RUNTIME_WSLC;
    unsigned cpus = container_number(request, "cpus", 4);
    unsigned memory = container_number(request, "memoryInGB", 8);
    bool ok = false;
    StringBuilder output = {0};
    *error = "Enter a container name, a valid image reference, and CPU/memory limits between 1 and 128.";
    if (!valid_container_name(name) || !provider || (strcmp(provider, "wsl.container") && strcmp(provider, "docker")) ||
        !image || !*image || strlen(image) > 512 ||
        strspn(image, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/:@") != strlen(image) ||
        !cpus || cpus > 128 || !memory || memory > 128)
        goto done;
    if (!strcmp(image, "outershell/container-base:17")) {
        const char *inspect[] = {"image", "inspect", image, NULL};
        if (!capture_runtime(w, inspect, &output, runtime)) {
            free(output.data);
            output = (StringBuilder){0};
            char *directory = win_executable_directory();
            char *context = directory ? win_join(directory, "container-bootstrap") : NULL;
            const char *build[] = {"build", "--progress", "plain", "--tag", image, context, NULL};
            ok = context && capture_runtime(w, build, &output, runtime);
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
    char runtime_name[80], cpu_text[12], memory_text[12], id[37];
    strcpy_s(id, sizeof(id), uuid + 1); CharLowerBuffA(id, 36);
    snprintf(runtime_name, sizeof(runtime_name), "outershell-%s", id);
    free(uuid);
    *error = "Could not save the container definition.";
    if (!save_container_metadata(id, name, cpus, memory) || !container_definition_create(id, image)) goto done;
    char *context = container_recipe_directory(id);
    StringBuilder mount = {0};
    if (context) { append(&mount, "type=bind,source="); append(&mount, context); append(&mount, ",target=/var/lib/outershell/project"); }
    snprintf(cpu_text, sizeof(cpu_text), "%u", cpus);
    snprintf(memory_text, sizeof(memory_text), "%uG", memory);
    const char *run[] = {"run", "--detach", "--name", runtime_name, "--cpus", cpu_text,
                        "--memory", memory_text, "--mount", mount.data, image, NULL};
    *error = "The container runtime could not create the container. Check the image name and the Outer Shell log.";
    ok = context && capture_runtime(w, run, &output, runtime);
    free(context); free(mount.data);

done:
    free(name);
    free(provider);
    free(image);
    free(output.data);
    return ok;
}

static void discover_containers(Wsl *w, ContainerRuntime runtime) {
    const char *args[] = {runtime == RUNTIME_DOCKER ? "ps" : "list", "--all", "--no-trunc", "--format", runtime == RUNTIME_DOCKER ? "{{json .}}" : "json", NULL};
    StringBuilder output = {0};
    bool available = capture_runtime(w, args, &output, runtime);
    if (available && runtime == RUNTIME_DOCKER) {
        StringBuilder system = {0};
        const char *info[] = {"info", "--format", "{{.OSType}}", NULL};
        available = capture_runtime(w, info, &system, runtime) && system.data && !strncmp(system.data, "linux", 5);
        free(system.data);
    }
    w->containers_available[runtime] = available;
    if (!available) {
        EnterCriticalSection(&w->gate);
        for (Distribution *d = w->distributions; d; d = d->next)
            if (d->container == runtime) d->running = false;
        LeaveCriticalSection(&w->gate);
        free(output.data);
        return;
    }
    EnterCriticalSection(&w->gate);
    for (Distribution *d = w->distributions; d; d = d->next)
        if (d->container == runtime && !d->building && *d->runtime_id)
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
        if (valid && container_identity(runtime, runtime_id, id)) {
            if (!strncmp(name, "outershell-", 11) && strlen(name) >= 47) {
                char owned_id[37]; memcpy(owned_id, name + 11, 36); owned_id[36] = 0;
                char *metadata_path = container_uuid(owned_id) ? container_metadata_path(owned_id) : NULL;
                wchar_t *wide = metadata_path ? win_wide(metadata_path) : NULL;
                if (wide && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) strcpy_s(id, sizeof(id), owned_id);
                free(wide); free(metadata_path);
            }
            EnterCriticalSection(&w->gate);
            Distribution *d;
            for (d = w->distributions; d; d = d->next)
                if (d->container && !strcmp(d->id, id))
                    break;
            if (!d) {
                d = win_alloc(sizeof(*d));
                d->owner = w;
                d->container = runtime;
                d->id = win_strdup(id);
                d->runtime_id = win_strdup(runtime_id);
                d->user = win_strdup("root");
                d->next = w->distributions;
                w->distributions = d;
            }
            free(d->runtime_id);
            d->runtime_id = win_strdup(runtime_id);
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
            if (d->managed) {
                char *definition = container_read(d->id, "Dockerfile", "");
                if (!*definition) {
                    char *image = container_string(line, length, "Image");
                    if (image && *image && !strpbrk(image, "\r\n")) container_definition_create(d->id, image);
                    free(image);
                }
                free(definition);
            }
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

static void container_provider(Wsl *w, StringBuilder *json, ContainerRuntime runtime) {
    append(json, "{");
    field(json, "id", container_provider_id(runtime));
    append(json, ",");
    field(json, "name", container_provider_name(runtime));
    append(json, ",\"isAvailable\":");
    append(json, w->containers_available[runtime] ? "true" : "false");
    append(json, ",\"isolationName\":\"Linux containers\",\"capabilities\":{"
                 "\"supportsApps\":true,\"supportsShell\":true,\"supportsRecipes\":true,"
                 "\"supportsMounts\":true,\"supportsLiveMounts\":false},\"status\":");
    json_string(json, w->containers_available[runtime] ? "ready" : "unavailable");
    append(json, ",\"detail\":");
    json_string(json, w->containers_available[runtime] ? "Ready." : runtime == RUNTIME_DOCKER ?
                "Install and start Docker Desktop with its Linux containers backend, then check again." :
                "Install or update WSL, then check again. WSL containers requires WSL 2.9.3 or later.");
    append(json, ",\"setupURL\":");
    json_string(json, runtime == RUNTIME_DOCKER ? "https://docs.docker.com/desktop/setup/install/windows-install/" :
                "https://learn.microsoft.com/windows/wsl/wsl-container");
    append(json, ",\"defaultBaseImage\":\"outershell/container-base:17\"}");
}

static void container_snapshot(Distribution *d, StringBuilder *json) {
    append(json, "{");
    field(json, "id", d->id);
    append(json, ",");
    field(json, "name", d->name);
    append(json, ",");
    field(json, "state", d->building ? "creating" : !d->owner->containers_available[d->container] ? "unavailable" : d->running ? "running" : "stopped");
    if (d->building || d->build_error) {
        append(json, ",\"buildProgress\":{");
        field(json, "phase", d->building ? "building" : "failed");
        append(json, ","); field(json, "detail", d->building ? "Building the container…" : d->build_error);
        append(json, ","); char *log = container_read(d->id, "build.log", d->build_error ? d->build_error : "");
        field(json, "log", log); free(log); append(json, "}");
    }
    append(json, ",\"runtimeKind\":");
    json_string(json, d->container == RUNTIME_DOCKER ? "docker" : "wslContainer");
    append(json, ",\"managementKind\":");
    json_string(json, d->managed ? "managed" : "attached");
    append(json, ",\"ownsContainer\":");
    append(json, d->managed ? "true" : "false");
    char limits[100];
    snprintf(limits, sizeof(limits), ",\"userName\":\"root\",\"cpus\":%u,\"memoryInGB\":%u,", d->cpus, d->memory_gb);
    append(json, limits);
    append(json, "\"runtime\":{");
    field(json, "providerID", container_provider_id(d->container));
    append(json, ",");
    field(json, "providerName", container_provider_name(d->container));
    append(json, ","
                 "\"operatingSystemName\":\"Linux\",\"operatingSystemVersion\":\"\","
                 "\"architecture\":\"\",\"isolationKind\":");
    json_string(json, d->container == RUNTIME_DOCKER ? "container" : "wslContainer");
    append(json, ",\"isolationName\":\"\"},\"capabilities\":{\"supportsApps\":true,\"supportsShell\":true,\"supportsRecipes\":");
    append(json, d->managed ? "true" : "false");
    append(json, ",\"supportsMounts\":"); append(json, d->managed ? "true" : "false");
    append(json, ",\"supportsLiveMounts\":false},\"shellCommand\":");
    StringBuilder command = {0};
    append(&command, d->container == RUNTIME_DOCKER ? "docker exec -it " : "wslc exec -it ");
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
    append(json, ",\"commands\":[]");
    if (d->managed) container_recipe_snapshot(d, json);
    else append(json, ",\"mounts\":[],\"persistentData\":[]");
    append(json, "}");
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
    bool ok = capture_runtime(d->owner, args, &output, d->container);
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
