typedef struct {
    char *identifier;
    char *display_name;
    char *provider_id;
    char *runtime_name;
    char *build_context_path;
    uint64_t created_at_milliseconds;
    uint32_t cpus;
    uint32_t memory_in_mb;
    uint32_t flags;
    uint32_t set_mask;
} ContainerRegistrationRequest;

enum {
    CONTAINER_REGISTRATION_MESSAGE_SIZE = 68,
    CONTAINER_LIST_ROW_SIZE = 64
};

static void container_registration_request_free(ContainerRegistrationRequest *request) {
    if (!request) return;
    free(request->identifier);
    free(request->display_name);
    free(request->provider_id);
    free(request->runtime_name);
    free(request->build_context_path);
    memset(request, 0, sizeof(*request));
}

static bool container_registration_read_string(const unsigned char *message,
                                               size_t length,
                                               size_t ref_offset,
                                               char **out) {
    uint32_t offset = read_uint32_le(message + ref_offset);
    uint32_t count = read_uint32_le(message + ref_offset + 4);
    if (offset > length || count > length - offset || (count && memchr(message + offset, 0, count))) return false;
    char *copy = malloc((size_t)count + 1);
    if (!copy) return false;
    if (count) memcpy(copy, message + offset, count);
    copy[count] = 0;
    *out = copy;
    return true;
}

static bool container_registration_decode(const unsigned char *message,
                                          size_t length,
                                          ContainerRegistrationRequest *request) {
    memset(request, 0, sizeof(*request));
    if (!message || length < CONTAINER_REGISTRATION_MESSAGE_SIZE) return false;
    request->flags = read_uint32_le(message + 4);
    request->set_mask = read_uint32_le(message + 8);
    request->cpus = read_uint32_le(message + 12);
    request->memory_in_mb = read_uint32_le(message + 16);
    request->created_at_milliseconds = read_uint64_le(message + 20);
    if (container_registration_read_string(message, length, 28, &request->identifier) &&
        container_registration_read_string(message, length, 36, &request->display_name) &&
        container_registration_read_string(message, length, 44, &request->provider_id) &&
        container_registration_read_string(message, length, 52, &request->runtime_name) &&
        container_registration_read_string(message, length, 60, &request->build_context_path)) {
        return true;
    }
    container_registration_request_free(request);
    return false;
}

static bool container_identifier_valid(const char *identifier) {
    if (!identifier || strlen(identifier) != 36) return false;
    for (size_t index = 0; index < 36; index++) {
        char character = identifier[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (character != '-') return false;
        } else if (!isxdigit((unsigned char)character)) {
            return false;
        }
    }
    return true;
}

static bool container_path_separator(char character) {
#ifdef _WIN32
    return character == '\\' || character == '/';
#else
    return character == '/';
#endif
}

static bool container_path_prefix_equal(const char *left, const char *right, size_t length) {
#ifdef _WIN32
    for (size_t index = 0; index < length; index++) {
        char a = left[index], b = right[index];
        if (container_path_separator(a) && container_path_separator(b)) continue;
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return false;
    }
    return true;
#else
    return strncmp(left, right, length) == 0;
#endif
}

static size_t container_path_trimmed_length(const char *path) {
    size_t length = strlen(path);
    while (length > 1 && container_path_separator(path[length - 1])) length--;
    return length;
}

static bool container_path_within(const char *path, const char *ancestor) {
    size_t path_length = container_path_trimmed_length(path);
    size_t ancestor_length = container_path_trimmed_length(ancestor);
    if (ancestor_length == 0 || ancestor_length > path_length ||
        !container_path_prefix_equal(path, ancestor, ancestor_length)) {
        return false;
    }
    return ancestor_length == path_length ||
           container_path_separator(path[ancestor_length]) ||
           container_path_separator(ancestor[ancestor_length - 1]);
}

static bool container_paths_overlap(const char *path, const char *other) {
    return other && other[0] && (container_path_within(path, other) || container_path_within(other, path));
}

#ifdef _WIN32
static bool container_build_context_directory_valid(const char *path) {
    bool absolute = (isalpha((unsigned char)path[0]) && path[1] == ':' && container_path_separator(path[2])) ||
                    (path[0] == '\\' && path[1] == '\\');
    if (!absolute) return false;
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_length <= 0) return false;
    wchar_t *wide = malloc((size_t)wide_length * sizeof(wchar_t));
    if (!wide) return false;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, wide_length);
    DWORD attributes = GetFileAttributesW(wide);
    wchar_t full[MAX_PATH * 4];
    DWORD full_length = GetFullPathNameW(wide, (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL);
    bool canonical = full_length > 0 && full_length < sizeof(full) / sizeof(full[0]) && _wcsicmp(full, wide) == 0;
    free(wide);
    return canonical && attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
#else
static bool container_build_context_directory_valid(const char *path) {
    if (path[0] != '/') return false;
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode)) return false;
    char resolved[PATH_MAX];
    return realpath(path, resolved) && strcmp(resolved, path) == 0;
}
#endif

static bool container_registration_check(const RegistryContainerRecord *record,
                                         const char *const *protected_paths,
                                         size_t protected_count,
                                         char *error,
                                         size_t error_size) {
    bool owns_build_context = (record->flags & REGISTRY_CONTAINER_OWNS_BUILD_CONTEXT) != 0;
    bool self_modifying = (record->flags & REGISTRY_CONTAINER_SELF_MODIFYING) != 0;
    const char *path = record->build_context_path ? record->build_context_path : "";
    const char *problem = NULL;
    if (!record->display_name || !record->display_name[0]) {
        problem = "A container needs a name.";
    } else if (!record->provider_id || !record->provider_id[0]) {
        problem = "A container needs a provider.";
    } else if ((!record->runtime_name || !record->runtime_name[0]) &&
               !(record->flags & REGISTRY_CONTAINER_OWNS_RUNTIME)) {
        problem = "An attached container needs a runtime name.";
    } else if (owns_build_context && path[0]) {
        problem = "A build context owned by Outer Shell cannot name a folder; Outer Shell chooses it.";
    } else if (self_modifying && !owns_build_context && !path[0]) {
        problem = "A self-modifying container needs a build context.";
    } else if (path[0] && !container_build_context_directory_valid(path)) {
        problem = "The build context must be an existing folder, named by its absolute, canonical path, and not a link.";
    } else if (self_modifying && path[0]) {
        for (size_t index = 0; index < protected_count; index++) {
            if (container_paths_overlap(path, protected_paths[index])) {
                problem = "A self-modifying build context cannot include or be inside Outer Shell's own folders.";
                break;
            }
        }
    }
    if (problem && error && error_size) snprintf(error, error_size, "%s", problem);
    return problem == NULL;
}

static bool container_registration_apply(RegistryStore *store,
                                         const ContainerRegistrationRequest *request,
                                         uint64_t now_milliseconds,
                                         const char *const *protected_paths,
                                         size_t protected_count,
                                         char *error,
                                         size_t error_size) {
    if (!container_identifier_valid(request->identifier)) {
        snprintf(error, error_size, "A container identifier must be a UUID.");
        return false;
    }
    uint32_t mask = request->set_mask;
    if (mask & ~(uint32_t)OUTERSHELL_CONTAINER_SET_ALL) {
        snprintf(error, error_size, "The container registration has unknown fields.");
        return false;
    }
    const RegistryContainerRecord *existing = registry_store_find_container(store, request->identifier);
    if (!existing && !(mask & OUTERSHELL_CONTAINER_SET_RUNTIME)) {
        snprintf(error, error_size, "A new container needs a provider and runtime.");
        return false;
    }
    RegistryContainerRecord merged = {
        .identifier = request->identifier,
        .display_name = existing ? existing->display_name : "",
        .provider_id = existing ? existing->provider_id : "",
        .runtime_name = existing ? existing->runtime_name : "",
        .build_context_path = existing ? existing->build_context_path : "",
        .created_at_milliseconds = existing ? existing->created_at_milliseconds : now_milliseconds,
        .cpus = existing ? existing->cpus : 0,
        .memory_in_mb = existing ? existing->memory_in_mb : 0,
        .flags = existing ? existing->flags : 0,
    };
    if (mask & OUTERSHELL_CONTAINER_SET_NAME) merged.display_name = request->display_name;
    if (mask & OUTERSHELL_CONTAINER_SET_RUNTIME) {
        merged.provider_id = request->provider_id;
        merged.runtime_name = request->runtime_name;
    }
    if (mask & OUTERSHELL_CONTAINER_SET_BUILD_CONTEXT) {
        uint32_t build_flags = REGISTRY_CONTAINER_OWNS_BUILD_CONTEXT | REGISTRY_CONTAINER_SELF_MODIFYING;
        merged.build_context_path = request->build_context_path;
        merged.flags = (merged.flags & ~build_flags) | (request->flags & build_flags);
    }
    if (mask & OUTERSHELL_CONTAINER_SET_OWNS_RUNTIME) {
        merged.flags = (merged.flags & ~(uint32_t)REGISTRY_CONTAINER_OWNS_RUNTIME) |
                       (request->flags & REGISTRY_CONTAINER_OWNS_RUNTIME);
    }
    if (mask & OUTERSHELL_CONTAINER_SET_CPUS) merged.cpus = request->cpus;
    if (mask & OUTERSHELL_CONTAINER_SET_MEMORY) merged.memory_in_mb = request->memory_in_mb;
    if (mask & OUTERSHELL_CONTAINER_SET_CREATED_AT) merged.created_at_milliseconds = request->created_at_milliseconds;
    if (!container_registration_check(&merged, protected_paths, protected_count, error, error_size)) return false;
    if (!registry_store_upsert_container(store,
                                         merged.identifier,
                                         merged.display_name,
                                         merged.provider_id,
                                         merged.runtime_name,
                                         merged.build_context_path,
                                         merged.created_at_milliseconds,
                                         merged.cpus,
                                         merged.memory_in_mb,
                                         merged.flags)) {
        snprintf(error, error_size, "Out of memory.");
        return false;
    }
    return true;
}
