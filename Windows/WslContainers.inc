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

static char *container_legacy_names_path(const char *folder, const char *id) {
    char *local = win_folder(CSIDL_LOCAL_APPDATA);
    char *root = local ? win_join(local, "OuterShell") : NULL;
    char *directory = root ? win_join(root, folder) : NULL;
    char *path = directory && (!id || win_secure_directory(directory)) ? (id ? win_join(directory, id) : win_strdup(directory)) : NULL;
    free(local);
    free(root);
    free(directory);
    return path;
}

static bool valid_container_name(const char *name) {
    return name && *name && strlen(name) <= 200 && !strpbrk(name, "\r\n\t");
}

#include "ContainerRegistry.inc"

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
#include "ContainerDefinitions.inc"

static bool create_container(Wsl *w, HttpRequest *request, const char **error) {
    char *name = request_string(request, "name");
    char *provider = request_string(request, "runtimeProviderID");
    char *image = request_string(request, "baseImage");
    char *dockerfile = request_string(request, "dockerfile");
    ContainerRuntime runtime = provider && !strcmp(provider, "docker") ? RUNTIME_DOCKER : RUNTIME_WSLC;
    unsigned cpus = container_number(request, "cpus", 4);
    unsigned memory = container_number(request, "memoryInMB", 8192);
    bool ok = false;
    StringBuilder output = {0};
    *error = "Enter a container name, a valid image reference, 1 to 128 CPUs, and 128 MB to 1 TB of memory.";
    if (dockerfile && (!*dockerfile || strlen(dockerfile) > 1000000)) goto done;
    if (!valid_container_name(name) || !provider || (strcmp(provider, "wsl.container") && strcmp(provider, "docker")) ||
        !image || !*image || strlen(image) > 512 ||
        strspn(image, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/:@") != strlen(image) ||
        !cpus || cpus > 128 || memory < 128 || memory > 1048576)
        goto done;
    if (!dockerfile) dockerfile = container_default_dockerfile(image);
    if (!dockerfile) goto done;
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
    if (!container_registry_upsert(id, name, runtime, runtime_name, cpus, memory, CONTAINER_MANAGED_FLAGS, CONTAINER_SET_ALL) ||
        !container_definition_create(id, image)) goto done;
    char *context = container_build_context_directory(id);
    char run_image[100];
    if (!context || !container_write(id, "Dockerfile", dockerfile, strlen(dockerfile))) { free(context); goto done; }
    snprintf(run_image, sizeof(run_image), "outershell/managed-%s:latest", id);
    *error = "Could not build the container. Check the container build log.";
    bool built = container_build(w, runtime, id, dockerfile, run_image, context, &output);
    if (output.data) container_write(id, "build.log", output.data, output.length);
    free(output.data); output = (StringBuilder){0};
    if (!built) { free(context); goto done; }
    if (!container_write(id, "Dockerfile.realized", dockerfile, strlen(dockerfile))) { free(context); goto done; }
    StringBuilder mount = {0};
    if (context) { append(&mount, "type=bind,source="); append(&mount, context); append(&mount, ",target=/var/lib/outershell/project"); }
    snprintf(cpu_text, sizeof(cpu_text), "%u", cpus);
    snprintf(memory_text, sizeof(memory_text), "%uM", memory);
    ContainerArguments args = {0};
    const char *prefix[] = {"run", "--detach", "--name", runtime_name, "--cpus", cpu_text,
                           "--memory", memory_text, "--mount", mount.data};
    ok = context && container_image_volumes(w, runtime, id, run_image);
    char *config = container_read(id, "configuration", container_empty_configuration);
    for (size_t i = 0; ok && i < sizeof(prefix) / sizeof(*prefix); i++) ok = container_argument(&args, prefix[i]);
    for (size_t i = 0; ok && i < sizeof(container_runtime_arguments) / sizeof(*container_runtime_arguments); i++)
        ok = container_argument(&args, container_runtime_arguments[i]);
    ok = ok && container_persistent_arguments(w, runtime, id, config, &args) && container_argument(&args, run_image);
    *error = "The container runtime could not create the container. Check the image name and the Outer Shell log.";
    ok = ok && capture_runtime(w, (const char *const *)args.values, &output, runtime);
    container_arguments_free(&args); free(config);
    free(context); free(mount.data);

done:
    free(name);
    free(provider);
    free(image);
    free(dockerfile);
    free(output.data);
    return ok;
}

static Distribution *container_distribution_for_row(Wsl *w, ContainerRuntime runtime, const ContainerRow *row) {
    Distribution *d;
    for (d = w->distributions; d; d = d->next)
        if (d->container && !strcmp(d->id, row->id))
            break;
    if (!d) {
        d = win_alloc(sizeof(*d));
        d->owner = w;
        d->container = runtime;
        d->id = win_strdup(row->id);
        d->runtime_id = win_strdup("");
        d->user = win_strdup("root");
        d->next = w->distributions;
        w->distributions = d;
    }
    free(d->name);
    d->name = win_strdup(row->name);
    d->managed = (row->flags & CONTAINER_OWNS_RUNTIME) != 0;
    d->owns_build_context = (row->flags & CONTAINER_OWNS_BUILD_CONTEXT) != 0;
    d->self_modifying = (row->flags & CONTAINER_SELF_MODIFYING) != 0;
    free(d->build_context);
    d->build_context = win_strdup(row->build_context);
    d->cpus = row->cpus ? row->cpus : 4;
    d->memory_mb = row->memory_mb ? row->memory_mb : 8192;
    return d;
}

static bool container_move_legacy_file(const char *from, const char *id, const char *suffix) {
    char *folder = container_legacy_names_path("container-names.migrated", NULL);
    bool ok = folder && win_secure_directory(folder);
    StringBuilder name = {0};
    append(&name, id);
    append(&name, suffix);
    char *to = ok ? win_join(folder, name.data) : NULL;
    wchar_t *wide_from = win_wide(from), *wide_to = to ? win_wide(to) : NULL;
    ok = wide_from && wide_to && MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING);
    free(folder); free(name.data); free(to); free(wide_from); free(wide_to);
    return ok;
}

static bool container_migrate_legacy(ContainerRuntime runtime, const char *runtime_name) {
    if (strncmp(runtime_name, "outershell-", 11) || strlen(runtime_name) != 47) return false;
    char id[37];
    memcpy(id, runtime_name + 11, 36);
    id[36] = 0;
    if (!container_uuid(id)) return false;
    char *path = container_legacy_names_path("container-names", id);
    StringBuilder metadata = {0};
    bool ok = path && container_path_exists(path) && win_read_file(path, 1024, &metadata) && metadata.data;
    char *newline = ok ? strchr(metadata.data, '\n') : NULL;
    ok = newline != NULL;
    unsigned cpus = 4, memory = 8192;
    if (ok) {
        *newline = 0;
        char unit[3] = {0};
        sscanf(newline + 1, "%u\n%u\n%2s", &cpus, &memory, unit);
        if (strcmp(unit, "mb") != 0) memory *= 1024;
        ok = valid_container_name(metadata.data) &&
             container_registry_upsert(id, metadata.data, runtime, runtime_name, cpus, memory,
                                       CONTAINER_MANAGED_FLAGS, CONTAINER_SET_ALL);
    }
    char *directory = ok ? container_directory(id) : NULL;
    const char *names[] = {"configuration", "configuration.realized"};
    for (size_t i = 0; ok && directory && i < 2; i++) {
        char *file = win_join(directory, names[i]);
        StringBuilder value = {0};
        if (file && container_path_exists(file)) {
            OrwvValue *decoded = win_read_file(file, WSL_LIMIT, &value) && value.data
                ? orwv_decode(value.data, value.length) : NULL;
            unsigned char *encoded = NULL;
            size_t encoded_length = 0;
            ok = decoded && orwv_encode(decoded, &encoded, &encoded_length) &&
                 container_resource_set(id, names[i], encoded, encoded_length);
            orwv_free(decoded);
            free(encoded);
            char suffix[40];
            snprintf(suffix, sizeof(suffix), ".%s", names[i]);
            if (ok) container_move_legacy_file(file, id, suffix);
        }
        free(value.data);
        free(file);
    }
    if (ok) container_move_legacy_file(path, id, "");
    free(directory);
    free(metadata.data);
    free(path);
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
    ContainerRow *rows = NULL;
    size_t row_count = 0;
    if (!container_registry_rows(NULL, &rows, &row_count)) {
        rows = NULL;
        row_count = 0;
    }
    char **seen = NULL;
    size_t seen_count = 0;
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
        char *labels = runtime == RUNTIME_DOCKER ? container_string(line, length, "Labels") : NULL;
        bool internal = false;
        for (const char *label = labels; label && *label;) {
            if (!strncmp(label, "org.outershell.tracing-worker=", sizeof("org.outershell.tracing-worker=") - 1)) internal = true;
            const char *comma = strchr(label, ',');
            label = comma ? comma + 1 : NULL;
        }
        free(labels);
        bool valid = !internal && runtime_id && strlen(runtime_id) == 64 &&
                     strspn(runtime_id, "0123456789abcdef") == 64 && name && *name && state;
        const ContainerRow *row = NULL;
        if (valid) {
            for (size_t i = 0; i < row_count && !row; i++)
                if (!strcmp(rows[i].provider, container_provider_id(runtime)) && !strcmp(rows[i].runtime_name, name))
                    row = &rows[i];
            if (!row && container_migrate_legacy(runtime, name)) {
                container_rows_free(rows, row_count);
                if (!container_registry_rows(NULL, &rows, &row_count)) { rows = NULL; row_count = 0; }
                for (size_t i = 0; i < row_count && !row; i++)
                    if (!strcmp(rows[i].provider, container_provider_id(runtime)) && !strcmp(rows[i].runtime_name, name))
                        row = &rows[i];
            }
        }
        if (row) {
            char **grown = realloc(seen, (seen_count + 1) * sizeof(*seen));
            if (grown) {
                seen = grown;
                seen[seen_count++] = win_strdup(row->id);
            }
            EnterCriticalSection(&w->gate);
            Distribution *d = container_distribution_for_row(w, runtime, row);
            free(d->runtime_id);
            d->runtime_id = win_strdup(runtime_id);
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
    EnterCriticalSection(&w->gate);
    for (size_t i = 0; i < row_count; i++) {
        if (strcmp(rows[i].provider, container_provider_id(runtime))) continue;
        bool found = false;
        for (size_t j = 0; j < seen_count && !found; j++) found = !strcmp(seen[j], rows[i].id);
        if (found) continue;
        Distribution *d = container_distribution_for_row(w, runtime, &rows[i]);
        if (d->building) continue;
        free(d->runtime_id);
        d->runtime_id = win_strdup("");
        d->present = true;
        d->running = false;
    }
    for (Distribution *d = w->distributions; d; d = d->next) {
        if (d->container != runtime || d->building) continue;
        bool registered = false;
        for (size_t i = 0; i < row_count && !registered; i++) registered = !strcmp(rows[i].id, d->id);
        if (!registered) d->present = false;
    }
    LeaveCriticalSection(&w->gate);
    for (size_t j = 0; j < seen_count; j++) free(seen[j]);
    free(seen);
    container_rows_free(rows, row_count);
}

static void container_provider(Wsl *w, StringBuilder *json, ContainerRuntime runtime) {
    append(json, "{");
    field(json, "id", container_provider_id(runtime));
    append(json, ",");
    field(json, "name", container_provider_name(runtime));
    append(json, ",\"isAvailable\":");
    append(json, w->containers_available[runtime] ? "true" : "false");
    append(json, ",\"isolationName\":\"Linux containers\",\"capabilities\":{"
                 "\"supportsApps\":true,\"supportsShell\":true,\"supportsDefinitions\":true,"
                 "\"supportsMounts\":true,\"supportsLiveMounts\":false,\"supportsStorageCleanup\":true},\"status\":");
    json_string(json, w->containers_available[runtime] ? "ready" : "unavailable");
    append(json, ",\"detail\":");
    json_string(json, w->containers_available[runtime] ? "Ready." : runtime == RUNTIME_DOCKER ?
                "Install and start Docker Desktop with its Linux containers backend, then check again." :
                "Install or update WSL, then check again. WSL containers requires WSL 2.9.3 or later.");
    append(json, ",\"setupURL\":");
    json_string(json, runtime == RUNTIME_DOCKER ? "https://docs.docker.com/desktop/setup/install/windows-install/" :
                "https://learn.microsoft.com/windows/wsl/wsl-container");
    append(json, ",\"defaultBaseImage\":\"debian:bookworm\"}");
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
    append(json, ",\"ownsBuildContext\":");
    append(json, d->managed && d->owns_build_context ? "true" : "false");
    append(json, ",\"selfModifying\":");
    append(json, d->managed && d->self_modifying ? "true" : "false");
    if (d->managed) {
        char *context = d->build_context && *d->build_context ? win_strdup(d->build_context) : container_build_context_directory(d->id);
        append(json, ",");
        field(json, "buildContextPath", context ? context : "");
        free(context);
    }
    char limits[100];
    snprintf(limits, sizeof(limits), ",\"userName\":\"root\",\"cpus\":%u,\"memoryInMB\":%u,", d->cpus, d->memory_mb);
    append(json, limits);
    append(json, "\"runtime\":{");
    field(json, "providerID", container_provider_id(d->container));
    append(json, ",");
    field(json, "providerName", container_provider_name(d->container));
    append(json, ","
                 "\"operatingSystemName\":\"Linux\",\"operatingSystemVersion\":\"\","
                 "\"architecture\":\"\",\"isolationKind\":");
    json_string(json, d->container == RUNTIME_DOCKER ? "container" : "wslContainer");
    append(json, ",\"isolationName\":\"\"},\"capabilities\":{\"supportsApps\":true,\"supportsShell\":true,\"supportsDefinitions\":");
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
    if (d->managed) container_definition_snapshot(d, json);
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
        container_send_reply(d->owner, client, request, "Invalid app identifier.", NULL, false);
        return;
    }
    StringBuilder path = {0}, output = {0}, json = {0};
    append(&path, "/var/lib/outershell/apps/");
    append(&path, service);
    append(&path, "/backend.log");
    const char *args[] = {"exec", "--user", "0", d->runtime_id,
                          "/usr/bin/tail", "-c", "1048576", path.data, NULL};
    bool ok = capture_runtime(d->owner, args, &output, d->container);
    append(&json, "\"appLog\":{");
    field(&json, "path", path.data);
    append(&json, ",");
    field(&json, "contents", output.data ? output.data : "");
    append(&json, ",");
    field(&json, "error", ok ? "" : "The app log is not available yet.");
    char numbers[120];
    snprintf(numbers, sizeof(numbers), ",\"isTruncated\":%s,\"fileSize\":%zu,\"modified\":0}",
             output.length >= 1048576 ? "true" : "false", output.length);
    append(&json, numbers);
    container_send_reply(d->owner, client, request, NULL, json.data, true);
    free(service);
    free(path.data);
    free(output.data);
    free(json.data);
}
