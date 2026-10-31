#include "environment.h"
static HANDLE environment_stop, environment_job;
static SRWLOCK environment_lock = SRWLOCK_INIT;
static void environment_snapshot_free_fields(EnvironmentSnapshot *s) {
    for (size_t i = 0; i < s->target_count; i++) {
        EnvironmentTarget *t = &s->targets[i];
        free(t->id);
        free(t->runtime);
        free(t->container);
        free(t->name);
        free(t->state);
        free(t->image);
        free(t->user);
        free(t->executable);
        free(t->socket);
    }
    for (size_t i = 0; i < s->error_count; i++) {
        free(s->errors[i].runtime);
        free(s->errors[i].message);
    }
    free(s->targets);
    free(s->errors);
}
void environment_snapshot_free(EnvironmentSnapshot *s) {
    environment_snapshot_free_fields(s);
#ifdef APP_TOP
    processes_free(&s->processes);
#endif
    *s = (EnvironmentSnapshot){0};
}
static void environment_error(EnvironmentSnapshot *s, const char *runtime,
                              const char *message) {
    void *p = realloc(s->errors, (s->error_count + 1) * sizeof(*s->errors));
    if (!p)
        return;
    s->errors = p;
    s->errors[s->error_count++] =
        (EnvironmentError){strdup(runtime), strdup(message)};
}
EnvironmentSnapshot discover_environments(void) {
    EnvironmentSnapshot s = {0};
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    s.time_ms = (value.QuadPart - 116444736000000000ULL) / 10000;
    Connection c = {0};
    c.stop = environment_stop;
    c.pipe = app_open_registry(environment_stop);
    Bytes b = {0};
    unsigned char request[] = {2, 0, 0, 0, 34, 0}, header[4];
    bool ok = c.pipe != INVALID_HANDLE_VALUE &&
              connection_write(&c, request, 6) &&
              connection_read(&c, header, 4);
    size_t length = ok ? get_u32(header) : 0;
    if (ok && length >= 48 && length <= MAX_DATA) {
        b.data = allocate(length);
        b.size = length;
        ok = connection_read(&c, b.data, length);
    } else
        ok = false;
    if (c.pipe != INVALID_HANDLE_VALUE)
        CloseHandle(c.pipe);
    if (!ok) {
        environment_error(&s, "Outer Shell",
                          "Container discovery is unavailable. Open Outer "
                          "Shell and try again.");
        goto done;
    }
    size_t count = get_u32(b.data + 12), variable = get_u32(b.data + 24);
    if (memcmp(b.data, "OSED", 4) || get_u32(b.data + 4) != 1 ||
        get_u32(b.data + 8) != length || get_u32(b.data + 16) != 72 ||
        get_u32(b.data + 20) != 48 || count > (length - 48) / 72 ||
        variable != 48 + count * 72 || get_u32(b.data + 28)) {
        environment_error(&s, "Outer Shell",
                          "Invalid container discovery response.");
        goto done;
    }
    s.targets = allocate(count * sizeof(*s.targets));
    for (size_t i = 0; i < count; i++) {
        char *f[9] = {0};
        for (size_t j = 0; j < 9; j++)
            f[j] = binary_read_string(b, 48 + i * 72 + j * 8, variable, 4096);
        if (!failure[0] && (!strcmp(f[2], "docker") || !strcmp(f[2], "wslc")) &&
            *f[3] && strspn(f[3], "0123456789abcdef") == strlen(f[3]) &&
            strlen(f[3]) == 64) {
            EnvironmentTarget *t = &s.targets[s.target_count++];
            t->id = format("%s:%s", f[2], f[3]);
            t->runtime = strdup(f[2]);
            t->container = strdup(f[3]);
            t->name = strdup(f[1]);
            t->state = strdup(f[4]);
            t->image = strdup(f[5]);
            t->user = strdup("");
            t->socket = strdup("");
            t->executable = strdup("");
        }
        for (size_t j = 0; j < 9; j++)
            free(f[j]);
        if (failure[0]) {
            environment_error(&s, "Outer Shell", failure);
            failure[0] = 0;
            break;
        }
    }
done:
    bytes_free(&b);
#ifndef APP_TOP
    EnvironmentTarget *targets =
        allocate((s.target_count + 1) * sizeof(*targets));
    targets[0] = (EnvironmentTarget){.id = strdup("host"),
                                     .runtime = strdup("host"),
                                     .container = strdup(""),
                                     .name = strdup("Windows"),
                                     .state = strdup("running"),
                                     .image = strdup(""),
                                     .user = strdup(""),
                                     .executable = strdup(""),
                                     .socket = strdup("")};
    if (s.target_count)
        memcpy(targets + 1, s.targets, s.target_count * sizeof(*targets));
    free(s.targets);
    s.targets = targets;
    s.target_count++;
#endif
    return s;
}
static EnvironmentSnapshot environment_cached;
static const EnvironmentSnapshot *environment_registry(bool refresh) {
    if (refresh || !environment_cached.time_ms) {
        environment_snapshot_free(&environment_cached);
        environment_cached = discover_environments();
    }
    return &environment_cached;
}
typedef struct {
    char *id, *user;
    AppProcess child;
    uint32_t sequence;
} EnvironmentSession;
static EnvironmentSession environment_sessions[32];
static void session_close(EnvironmentSession *s) {
    app_process_close(&s->child);
    free(s->id);
    free(s->user);
    memset(s, 0, sizeof(*s));
}
void sessions_close(void) {
    for (size_t i = 0; i < 32; i++)
        session_close(&environment_sessions[i]);
    environment_snapshot_free(&environment_cached);
    if (environment_job)
        CloseHandle(environment_job);
    if (environment_stop)
        CloseHandle(environment_stop);
    environment_job = environment_stop = NULL;
}
static bool helper_frame_write(EnvironmentSession *s, uint32_t kind,
                               const void *data, size_t length) {
    unsigned char h[16];
    memcpy(h, APP_FRAME_MAGIC, 4);
    put_u32(h + 4, kind);
    put_u32(h + 8, s->sequence);
    put_u32(h + 12, (uint32_t)length);
    return connection_write(&s->child.input, h, 16) &&
           (!length || connection_write(&s->child.input, data, length));
}
static bool helper_chunks(EnvironmentSession *s, uint32_t kind, Bytes b) {
    for (size_t offset = 0; offset < b.size;) {
        size_t n = b.size - offset;
        if (n > MAX_FRAME)
            n = MAX_FRAME;
        if (!helper_frame_write(s, kind, b.data + offset, n))
            return false;
        offset += n;
    }
    return true;
}
static bool helper_frame_read(EnvironmentSession *s, uint32_t *kind,
                              uint32_t *id, Bytes *b) {
    unsigned char h[16];
    if (!connection_read(&s->child.output, h, 16) ||
        memcmp(h, APP_FRAME_MAGIC, 4))
        return false;
    size_t n = get_u32(h + 12);
    if (n > MAX_FRAME)
        return false;
    *kind = get_u32(h + 4);
    *id = get_u32(h + 8);
    b->data = allocate(n + 1);
    b->size = n;
    if (n && !connection_read(&s->child.output, b->data, n)) {
        bytes_free(b);
        return false;
    }
    return true;
}
BinaryReply environment_binary_operation(const char *target, const char *user,
                                         const Request *request, Bytes body) {
    if (!strcmp(target, "host")) {
        BinaryReply local = local_binary_operation(request, body);
        response_finish(&local);
        return local;
    }
    BinaryReply reply = {0};
    EnvironmentSnapshot snapshot = discover_environments();
    EnvironmentTarget *selected = NULL;
    for (size_t i = 0; i < snapshot.target_count; i++)
        if (!strcmp(snapshot.targets[i].id, target))
            selected = &snapshot.targets[i];
    if (!selected || strcmp(selected->state, "running")) {
        fail("The selected container is not running or no longer registered.");
        goto done;
    }
    if (*user &&
        (strlen(user) > 128 ||
         strspn(user, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                      "XYZ0123456789_.-") != strlen(user))) {
        fail("Invalid container user.");
        goto done;
    }
    EnvironmentSession *session = NULL;
    for (size_t i = 0; i < 32; i++)
        if (environment_sessions[i].id &&
            !strcmp(environment_sessions[i].id, target) &&
            !strcmp(environment_sessions[i].user, user))
            session = &environment_sessions[i];
    if (!session)
        for (size_t i = 0; i < 32; i++)
            if (!environment_sessions[i].id) {
                session = &environment_sessions[i];
                break;
            }
    if (!session) {
        session = &environment_sessions[0];
        session_close(session);
    }
    if (!session->id) {
        const char *args[10] = {"exec", "--interactive", NULL};
        size_t n = 2;
        if (*user) {
            args[n++] = "--user";
            args[n++] = user;
        }
        args[n++] = selected->container;
        args[n++] = HELPER_PATH;
        args[n++] = "serve";
        args[n] = NULL;
        if (!app_process_start(environment_stop, environment_job,
                               selected->runtime, args, &session->child)) {
            fail("Could not start the container helper (Windows error %lu).",
                 GetLastError());
            goto done;
        }
        session->id = strdup(target);
        session->user = strdup(user);
        Bytes hello = {0};
        uint32_t kind, id;
        bool ok = helper_frame_read(session, &kind, &id, &hello) && kind == 1 &&
                  id == 0 && hello.size == 16 && get_u32(hello.data) == 2 &&
                  get_u32(hello.data + 8) == MAX_DATA &&
                  get_u32(hello.data + 12) == MAX_FRAME;
        bytes_free(&hello);
        if (!ok) {
            fail("Container helper is missing or incompatible. Rebuild the "
                 "container "
                 "with current app helpers.");
            session_close(session);
            goto done;
        }
    }
    session->sequence++;
    Bytes metadata = request_encode(request);
    bool ok = metadata.data && helper_chunks(session, 2, metadata) &&
              helper_chunks(session, 3, body) &&
              helper_frame_write(session, 4, NULL, 0);
    bytes_free(&metadata);
    bool body_started = false, complete = false;
    while (ok && !complete) {
        uint32_t kind, id;
        Bytes frame = {0};
        ok = helper_frame_read(session, &kind, &id, &frame) &&
             id == session->sequence;
        if (ok && kind == 2 && !body_started)
            ok = append(&reply.metadata, frame.data, frame.size, MAX_DATA);
        else if (ok && kind == 3) {
            body_started = true;
            ok = append(&reply.body, frame.data, frame.size, MAX_DATA);
        } else if (ok && kind == 4 && !frame.size && reply.metadata.size)
            complete = true;
        else
            ok = false;
        bytes_free(&frame);
    }
    if (!ok) {
        binary_reply_free(&reply);
        session_close(session);
        fail("Container helper connection failed. The operation was not "
             "retried.");
    } else
        response_validate(reply, request->operation);
done:
    environment_snapshot_free(&snapshot);
    return reply;
}
static bool environment_initialize(void) {
    if (environment_stop)
        return true;
    environment_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    environment_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!environment_stop || !environment_job ||
        !SetInformationJobObject(environment_job,
                                 JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        fail("Could not initialize container runtime.");
        return false;
    }
    atexit(sessions_close);
    return true;
}
