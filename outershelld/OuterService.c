#define _GNU_SOURCE

#include "OuterService.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { OUTER_SERVICE_MAX_LINE = 8192, OUTER_SERVICE_MAX_ITEMS = 128 };

typedef enum { START_MANUAL, START_EAGER, START_SOCKET } StartMode;
typedef enum { RESTART_NEVER, RESTART_ON_FAILURE, RESTART_ALWAYS } RestartMode;
typedef enum { SOCKET_UNIX, SOCKET_TCP } SocketType;

typedef struct {
    char *name;
    SocketType type;
    char *path;
    char *address;
    uint16_t port;
    mode_t mode;
    int backlog;
    int fd;
    bool saw_type;
    bool saw_path;
    bool saw_address;
    bool saw_port;
    bool saw_mode;
    bool saw_backlog;
} ServiceSocket;

typedef struct {
    char *id;
    char *source_path;
    char *name;
    char *executable;
    char *working_directory;
    char *log_path;
    char **arguments;
    size_t argument_count;
    char **environment;
    size_t environment_count;
    char **pass_environment;
    size_t pass_environment_count;
    bool inherit_environment;
    bool essential;
    StartMode start_mode;
    RestartMode restart_mode;
    unsigned restart_delay_ms;
    unsigned stop_timeout_ms;
    ServiceSocket *sockets;
    size_t socket_count;

    pid_t pid;
    bool desired_running;
    bool stopping;
    bool restart_after_stop;
    bool failed;
    int exit_status;
    uint64_t restart_at_ms;
    uint64_t stop_deadline_ms;
    const char *state;
} OuterService;

struct OuterServiceManager {
    char *services_directory;
    char *launcher_path;
    OuterServiceEventCallback event_callback;
    void *event_context;
    OuterService *services;
    size_t service_count;
    pthread_mutex_t mutex;
    pthread_t thread;
    bool thread_started;
    bool shutting_down;
    int exit_status;
    int wake_pipe[2];
    uint64_t configuration_version;
};

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static char *duplicate_string(const char *value) {
    char *copy = strdup(value ? value : "");
    return copy;
}

static char *trim(char *value) {
    while (*value && isspace((unsigned char)*value)) value++;
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return value;
}

static bool valid_identifier(const char *value) {
    if (!value || !value[0]) return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static bool append_string(char ***items, size_t *count, const char *value) {
    if (*count >= OUTER_SERVICE_MAX_ITEMS) return false;
    char **resized = realloc(*items, (*count + 1) * sizeof(char *));
    if (!resized) return false;
    resized[*count] = duplicate_string(value);
    if (!resized[*count]) {
        *items = resized;
        return false;
    }
    *items = resized;
    (*count)++;
    return true;
}

static void free_strings(char **items, size_t count) {
    for (size_t index = 0; index < count; index++) free(items[index]);
    free(items);
}

static void free_null_terminated_strings(char **items) {
    size_t count = 0;
    if (items) while (items[count]) count++;
    free_strings(items, count);
}

static void close_service_sockets(OuterService *service, bool unlink_paths) {
    for (size_t index = 0; index < service->socket_count; index++) {
        ServiceSocket *socket_definition = &service->sockets[index];
        if (socket_definition->fd >= 0) close(socket_definition->fd);
        socket_definition->fd = -1;
        if (unlink_paths && socket_definition->type == SOCKET_UNIX && socket_definition->path) {
            unlink(socket_definition->path);
        }
    }
}

static void free_service_definition(OuterService *service, bool unlink_paths) {
    if (!service) return;
    close_service_sockets(service, unlink_paths);
    free(service->id);
    free(service->source_path);
    free(service->name);
    free(service->executable);
    free(service->working_directory);
    free(service->log_path);
    free_strings(service->arguments, service->argument_count);
    free_strings(service->environment, service->environment_count);
    free_strings(service->pass_environment, service->pass_environment_count);
    for (size_t index = 0; index < service->socket_count; index++) {
        free(service->sockets[index].name);
        free(service->sockets[index].path);
        free(service->sockets[index].address);
    }
    free(service->sockets);
    memset(service, 0, sizeof(*service));
}

static void free_service(OuterService *service) {
    free_service_definition(service, true);
}

static bool parse_boolean(const char *value, bool *result) {
    if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *result = true;
        return true;
    }
    if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *result = false;
        return true;
    }
    return false;
}

static bool parse_unsigned(const char *value, unsigned maximum, unsigned *result) {
    if (!value || !value[0] || value[0] == '-') return false;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > maximum) return false;
    *result = (unsigned)parsed;
    return true;
}

static bool set_once(char **destination, const char *value) {
    if (*destination) return false;
    *destination = duplicate_string(value);
    return *destination != NULL;
}

static ServiceSocket *append_socket(OuterService *service, const char *name) {
    if (service->socket_count >= OUTER_SERVICE_MAX_ITEMS || !valid_identifier(name)) return NULL;
    for (size_t index = 0; index < service->socket_count; index++) {
        if (strcmp(service->sockets[index].name, name) == 0) return NULL;
    }
    ServiceSocket *resized = realloc(service->sockets,
                                     (service->socket_count + 1) * sizeof(ServiceSocket));
    if (!resized) return NULL;
    service->sockets = resized;
    ServiceSocket *result = &service->sockets[service->socket_count++];
    memset(result, 0, sizeof(*result));
    result->name = duplicate_string(name);
    result->type = SOCKET_UNIX;
    result->mode = 0600;
    result->backlog = 64;
    result->fd = -1;
    return result->name ? result : NULL;
}

static bool valid_environment_assignment(const char *value) {
    const char *equals = strchr(value, '=');
    if (!equals || equals == value) return false;
    for (const char *cursor = value; cursor < equals; cursor++) {
        if (cursor == value) {
            if (!(isalpha((unsigned char)*cursor) || *cursor == '_')) return false;
        } else if (!(isalnum((unsigned char)*cursor) || *cursor == '_')) {
            return false;
        }
    }
    return true;
}

static bool parse_service_file(const char *path,
                               const char *service_id,
                               OuterService *service,
                               char *error,
                               size_t error_size) {
    memset(service, 0, sizeof(*service));
    service->restart_delay_ms = 1000;
    service->stop_timeout_ms = 10000;
    service->start_mode = START_MANUAL;
    service->restart_mode = RESTART_ON_FAILURE;
    service->state = "stopped";
    service->id = duplicate_string(service_id);
    service->source_path = duplicate_string(path);
    if (!service->id || !service->source_path) goto allocation_error;

    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        free_service(service);
        return false;
    }

    enum { SECTION_NONE, SECTION_SERVICE, SECTION_SOCKET } section = SECTION_NONE;
    ServiceSocket *current_socket = NULL;
    bool saw_format = false;
    bool saw_environment_policy = false;
    bool saw_start = false;
    bool saw_restart = false;
    bool saw_restart_delay = false;
    bool saw_stop_timeout = false;
    bool saw_essential = false;
    char line[OUTER_SERVICE_MAX_LINE];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) {
            snprintf(error, error_size, "%s:%u: line is too long", path, line_number);
            goto parse_error;
        }
        char *text = trim(line);
        if (!text[0] || text[0] == '#' || text[0] == ';') continue;
        if (text[0] == '[') {
            size_t length = strlen(text);
            if (length < 3 || text[length - 1] != ']') {
                snprintf(error, error_size, "%s:%u: malformed section", path, line_number);
                goto parse_error;
            }
            text[length - 1] = '\0';
            const char *name = text + 1;
            if (strcmp(name, "Service") == 0) {
                section = SECTION_SERVICE;
                current_socket = NULL;
            } else if (strncmp(name, "Socket.", 7) == 0 && name[7]) {
                current_socket = append_socket(service, name + 7);
                if (!current_socket) {
                    snprintf(error, error_size, "%s:%u: invalid or duplicate socket section", path, line_number);
                    goto parse_error;
                }
                section = SECTION_SOCKET;
            } else {
                snprintf(error, error_size, "%s:%u: unknown section [%s]", path, line_number, name);
                goto parse_error;
            }
            continue;
        }

        char *equals = strchr(text, '=');
        if (!equals || section == SECTION_NONE) {
            snprintf(error, error_size, "%s:%u: expected Key=Value inside a section", path, line_number);
            goto parse_error;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        if (!key[0]) {
            snprintf(error, error_size, "%s:%u: empty key", path, line_number);
            goto parse_error;
        }

        bool ok = false;
        if (section == SECTION_SERVICE) {
            if (strcmp(key, "Format") == 0) {
                ok = !saw_format && strcmp(value, "1") == 0;
                saw_format = ok;
            } else if (strcmp(key, "Name") == 0) {
                ok = value[0] && set_once(&service->name, value);
            } else if (strcmp(key, "Executable") == 0) {
                ok = value[0] == '/' && set_once(&service->executable, value);
            } else if (strcmp(key, "Argument") == 0) {
                ok = append_string(&service->arguments, &service->argument_count, value);
            } else if (strcmp(key, "WorkingDirectory") == 0) {
                ok = value[0] == '/' && set_once(&service->working_directory, value);
            } else if (strcmp(key, "Environment") == 0) {
                ok = valid_environment_assignment(value) &&
                     append_string(&service->environment, &service->environment_count, value);
            } else if (strcmp(key, "PassEnvironment") == 0) {
                ok = valid_identifier(value) && !strchr(value, '.') && !strchr(value, '-') &&
                     append_string(&service->pass_environment, &service->pass_environment_count, value);
            } else if (strcmp(key, "EnvironmentPolicy") == 0) {
                if (!saw_environment_policy && strcmp(value, "clean") == 0) ok = true;
                else if (!saw_environment_policy && strcmp(value, "inherit") == 0) {
                    service->inherit_environment = true;
                    ok = true;
                }
                saw_environment_policy = ok;
            } else if (strcmp(key, "Start") == 0) {
                if (!saw_start && strcmp(value, "manual") == 0) {
                    service->start_mode = START_MANUAL;
                    ok = true;
                } else if (!saw_start && strcmp(value, "eager") == 0) {
                    service->start_mode = START_EAGER;
                    ok = true;
                } else if (!saw_start && strcmp(value, "socket") == 0) {
                    service->start_mode = START_SOCKET;
                    ok = true;
                }
                saw_start = ok;
            } else if (strcmp(key, "Restart") == 0) {
                if (!saw_restart && strcmp(value, "never") == 0) {
                    service->restart_mode = RESTART_NEVER;
                    ok = true;
                } else if (!saw_restart && strcmp(value, "on-failure") == 0) {
                    service->restart_mode = RESTART_ON_FAILURE;
                    ok = true;
                } else if (!saw_restart && strcmp(value, "always") == 0) {
                    service->restart_mode = RESTART_ALWAYS;
                    ok = true;
                }
                saw_restart = ok;
            } else if (strcmp(key, "RestartDelayMilliseconds") == 0) {
                ok = !saw_restart_delay && parse_unsigned(value, 3600000, &service->restart_delay_ms);
                saw_restart_delay = ok;
            } else if (strcmp(key, "StopTimeoutMilliseconds") == 0) {
                ok = !saw_stop_timeout && parse_unsigned(value, 3600000, &service->stop_timeout_ms);
                saw_stop_timeout = ok;
            } else if (strcmp(key, "LogPath") == 0) {
                ok = value[0] == '/' && set_once(&service->log_path, value);
            } else if (strcmp(key, "Essential") == 0) {
                ok = !saw_essential && parse_boolean(value, &service->essential);
                saw_essential = ok;
            }
        } else if (current_socket) {
            if (strcmp(key, "Type") == 0) {
                if (!current_socket->saw_type && strcmp(value, "unix") == 0) {
                    current_socket->type = SOCKET_UNIX;
                    ok = true;
                } else if (!current_socket->saw_type && strcmp(value, "tcp") == 0) {
                    current_socket->type = SOCKET_TCP;
                    ok = true;
                }
                current_socket->saw_type = ok;
            } else if (strcmp(key, "Path") == 0) {
                ok = !current_socket->saw_path && value[0] == '/' && set_once(&current_socket->path, value);
                current_socket->saw_path = ok;
            } else if (strcmp(key, "Address") == 0) {
                ok = !current_socket->saw_address && set_once(&current_socket->address, value);
                current_socket->saw_address = ok;
            } else if (strcmp(key, "Port") == 0) {
                unsigned port = 0;
                ok = !current_socket->saw_port && parse_unsigned(value, 65535, &port) && port > 0;
                current_socket->port = (uint16_t)port;
                current_socket->saw_port = ok;
            } else if (strcmp(key, "Mode") == 0) {
                char *end = NULL;
                errno = 0;
                unsigned long mode = strtoul(value, &end, 8);
                ok = !current_socket->saw_mode && !errno && end && !*end && mode <= 0777;
                current_socket->mode = (mode_t)mode;
                current_socket->saw_mode = ok;
            } else if (strcmp(key, "Backlog") == 0) {
                unsigned backlog = 0;
                ok = !current_socket->saw_backlog && parse_unsigned(value, 65535, &backlog) && backlog > 0;
                current_socket->backlog = (int)backlog;
                current_socket->saw_backlog = ok;
            }
        }
        if (!ok) {
            snprintf(error, error_size, "%s:%u: invalid, duplicate, or unknown key %s", path, line_number, key);
            goto parse_error;
        }
    }

    if (ferror(file)) {
        snprintf(error, error_size, "%s: read failed", path);
        goto parse_error;
    }
    fclose(file);
    if (!saw_format || !service->executable) {
        snprintf(error, error_size, "%s: [Service] requires Format=1 and Executable", path);
        free_service(service);
        return false;
    }
    if (service->start_mode == START_SOCKET && service->socket_count == 0) {
        snprintf(error, error_size, "%s: Start=socket requires at least one [Socket.name] section", path);
        free_service(service);
        return false;
    }
    for (size_t index = 0; index < service->socket_count; index++) {
        ServiceSocket *socket_definition = &service->sockets[index];
        if ((socket_definition->type == SOCKET_UNIX && !socket_definition->path) ||
            (socket_definition->type == SOCKET_TCP && !socket_definition->port)) {
            snprintf(error, error_size, "%s: socket %s is missing its Path or Port", path, socket_definition->name);
            free_service(service);
            return false;
        }
        if (socket_definition->type == SOCKET_TCP && !socket_definition->address) {
            socket_definition->address = duplicate_string("127.0.0.1");
            if (!socket_definition->address) goto allocation_error;
        }
    }
    if (!service->name) service->name = duplicate_string(service_id);
    if (!service->name) goto allocation_error;
    return true;

parse_error:
    fclose(file);
    free_service(service);
    return false;
allocation_error:
    snprintf(error, error_size, "%s: out of memory", path);
    free_service(service);
    return false;
}

static bool ensure_parent_directory(const char *path, char *error, size_t error_size) {
    char *copy = duplicate_string(path);
    if (!copy) return false;
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        free(copy);
        return true;
    }
    *slash = '\0';
    for (char *cursor = copy + 1; ; cursor++) {
        if (*cursor == '/' || *cursor == '\0') {
            char saved = *cursor;
            *cursor = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                snprintf(error, error_size, "mkdir %s: %s", copy, strerror(errno));
                free(copy);
                return false;
            }
            *cursor = saved;
            if (!saved) break;
        }
    }
    free(copy);
    return true;
}

static bool make_listener(ServiceSocket *definition, char *error, size_t error_size) {
    int fd = -1;
    if (definition->type == SOCKET_UNIX) {
        if (strlen(definition->path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
            snprintf(error, error_size, "Unix socket path is too long: %s", definition->path);
            return false;
        }
        if (!ensure_parent_directory(definition->path, error, error_size)) return false;
        struct stat existing;
        if (lstat(definition->path, &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) {
                snprintf(error, error_size, "refusing to replace non-socket path %s", definition->path);
                return false;
            }
            if (unlink(definition->path) != 0) {
                snprintf(error, error_size, "unlink %s: %s", definition->path, strerror(errno));
                return false;
            }
        } else if (errno != ENOENT) {
            snprintf(error, error_size, "stat %s: %s", definition->path, strerror(errno));
            return false;
        }
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un address;
            memset(&address, 0, sizeof(address));
            address.sun_family = AF_UNIX;
            snprintf(address.sun_path, sizeof(address.sun_path), "%s", definition->path);
            if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
                chmod(definition->path, definition->mode) != 0) {
                snprintf(error, error_size, "bind %s: %s", definition->path, strerror(errno));
                close(fd);
                unlink(definition->path);
                return false;
            }
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            int reuse = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            struct sockaddr_in address;
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons(definition->port);
            if (inet_pton(AF_INET, definition->address, &address.sin_addr) != 1) {
                snprintf(error, error_size, "invalid IPv4 socket address %s", definition->address);
                close(fd);
                return false;
            }
            if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
                snprintf(error, error_size, "bind %s:%u: %s", definition->address,
                         (unsigned)definition->port, strerror(errno));
                close(fd);
                return false;
            }
        }
    }
    if (fd < 0) {
        snprintf(error, error_size, "socket: %s", strerror(errno));
        return false;
    }
    if (listen(fd, definition->backlog) != 0) {
        snprintf(error, error_size, "listen: %s", strerror(errno));
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    definition->fd = fd;
    return true;
}

static void notify_service(OuterServiceManager *manager, OuterService *service) {
    if (manager->event_callback) manager->event_callback(manager->event_context, service->id);
}

static char **build_environment(const OuterService *service, size_t socket_count) {
    size_t inherited_count = 0;
    if (service->inherit_environment) while (environ[inherited_count]) inherited_count++;
    size_t capacity = inherited_count + service->pass_environment_count +
                      service->environment_count + 6;
    char **result = calloc(capacity, sizeof(char *));
    if (!result) return NULL;
    size_t count = 0;
    if (service->inherit_environment) {
        for (size_t index = 0; index < inherited_count; index++) {
            result[count++] = duplicate_string(environ[index]);
        }
    } else {
        const char *path = getenv("PATH");
        char buffer[4096];
        snprintf(buffer, sizeof(buffer), "PATH=%s", path ? path : "/usr/local/bin:/usr/bin:/bin");
        result[count++] = duplicate_string(buffer);
        for (size_t index = 0; index < service->pass_environment_count; index++) {
            const char *value = getenv(service->pass_environment[index]);
            if (!value) continue;
            size_t length = strlen(service->pass_environment[index]) + strlen(value) + 2;
            result[count] = malloc(length);
            if (result[count]) snprintf(result[count++], length, "%s=%s", service->pass_environment[index], value);
        }
    }
    for (size_t index = 0; index < service->environment_count; index++) {
        result[count++] = duplicate_string(service->environment[index]);
    }
    if (socket_count) {
        char fds[64];
        snprintf(fds, sizeof(fds), "LISTEN_FDS=%zu", socket_count);
        result[count++] = duplicate_string(fds);
        size_t names_length = strlen("LISTEN_FDNAMES=") + 1;
        for (size_t index = 0; index < service->socket_count; index++) {
            names_length += strlen(service->sockets[index].name) + 1;
        }
        char *names = malloc(names_length);
        if (names) {
            snprintf(names, names_length, "LISTEN_FDNAMES=");
            for (size_t index = 0; index < service->socket_count; index++) {
                if (index) strcat(names, ":");
                strcat(names, service->sockets[index].name);
            }
            result[count++] = names;
        }
    }
    for (size_t index = 0; index < count; index++) {
        if (!result[index]) {
            free_strings(result, count);
            return NULL;
        }
    }
    return result;
}

static bool spawn_service(OuterServiceManager *manager,
                          OuterService *service,
                          char *error,
                          size_t error_size) {
    size_t argc = 3 + service->argument_count;
    char **argv = calloc(argc + 1, sizeof(char *));
    if (!argv) goto allocation_error;
    argv[0] = manager->launcher_path;
    argv[1] = "--outerservice-exec";
    argv[2] = service->executable;
    for (size_t index = 0; index < service->argument_count; index++) argv[3 + index] = service->arguments[index];

    char **environment = build_environment(service, service->socket_count);
    if (!environment) {
        free(argv);
        goto allocation_error;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);

    int *copies = calloc(service->socket_count, sizeof(int));
    if (service->socket_count && !copies) {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        free_null_terminated_strings(environment);
        free(argv);
        goto allocation_error;
    }
    for (size_t index = 0; index < service->socket_count; index++) copies[index] = -1;
    bool actions_ok = true;
    int log_fd = -1;
    for (size_t index = 0; index < service->socket_count; index++) {
        copies[index] = fcntl(service->sockets[index].fd, F_DUPFD_CLOEXEC, 64);
        if (copies[index] < 0 || posix_spawn_file_actions_adddup2(&actions, copies[index], 3 + (int)index) != 0) {
            actions_ok = false;
            break;
        }
        posix_spawn_file_actions_addclose(&actions, copies[index]);
    }
    if (service->working_directory) {
        /* The child helper applies this portably before exec. */
        size_t length = strlen(service->working_directory) + 25;
        char *entry = malloc(length);
        if (!entry) actions_ok = false;
        else {
            snprintf(entry, length, "OUTERSERVICE_CHDIR=%s", service->working_directory);
            size_t count = 0;
            while (environment[count]) count++;
            environment[count] = entry;
        }
    }
    if (service->log_path && actions_ok) {
        if (!ensure_parent_directory(service->log_path, error, error_size)) actions_ok = false;
        else {
            log_fd = open(service->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (log_fd < 0) {
                snprintf(error, error_size, "open %s: %s", service->log_path, strerror(errno));
                actions_ok = false;
            } else {
                int action_result = posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO);
                if (action_result == 0) {
                    action_result = posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
                }
                if (action_result == 0) {
                    action_result = posix_spawn_file_actions_addclose(&actions, log_fd);
                }
                if (action_result != 0) {
                    snprintf(error, error_size, "redirect %s: %s",
                             service->log_path, strerror(action_result));
                    actions_ok = false;
                }
            }
        }
    }
    short flags = POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = 0;
    int spawn_result = actions_ok ? posix_spawn(&pid, manager->launcher_path, &actions,
                                                &attributes, argv, environment) : EINVAL;
    for (size_t index = 0; index < service->socket_count; index++) {
        if (copies[index] >= 0) close(copies[index]);
    }
    if (log_fd >= 0) close(log_fd);
    free(copies);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    free_null_terminated_strings(environment);
    free(argv);
    if (spawn_result != 0) {
        if (!error[0]) snprintf(error, error_size, "start %s: %s", service->id, strerror(spawn_result));
        return false;
    }
    service->pid = pid;
    service->stopping = false;
    service->failed = false;
    service->state = "running";
    notify_service(manager, service);
    return true;

allocation_error:
    snprintf(error, error_size, "start %s: out of memory", service->id);
    return false;
}

static void request_stop(OuterServiceManager *manager, OuterService *service, bool restart) {
    service->desired_running = restart;
    service->restart_after_stop = restart;
    if (service->pid > 0 && !service->stopping) {
        kill(-service->pid, SIGTERM);
        service->stopping = true;
        service->stop_deadline_ms = monotonic_ms() + service->stop_timeout_ms;
        service->state = "stopping";
        notify_service(manager, service);
    } else if (service->pid <= 0) {
        service->state = restart ? "starting" : "stopped";
        service->restart_at_ms = monotonic_ms();
    }
}

static void handle_exit(OuterServiceManager *manager, OuterService *service, int wait_status) {
    bool clean = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
    service->exit_status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) :
                           (WIFSIGNALED(wait_status) ? 128 + WTERMSIG(wait_status) : 1);
    service->pid = 0;
    bool explicitly_stopped = service->stopping && !service->restart_after_stop;
    bool should_restart = service->restart_after_stop ||
                          (!explicitly_stopped && service->restart_mode == RESTART_ALWAYS) ||
                          (!explicitly_stopped && service->restart_mode == RESTART_ON_FAILURE && !clean);
    bool activation_backoff = !explicitly_stopped && !clean && service->start_mode == START_SOCKET;
    service->stopping = false;
    service->restart_after_stop = false;
    service->desired_running = should_restart;
    service->failed = !clean && !should_restart;
    service->restart_at_ms = monotonic_ms() +
                             ((should_restart || activation_backoff) ? service->restart_delay_ms : 0);
    service->state = should_restart ? "waiting-to-restart" : (service->failed ? "failed" : "stopped");
    notify_service(manager, service);
    if (service->essential && !manager->shutting_down && !should_restart) {
        manager->exit_status = service->exit_status ? service->exit_status : 1;
        manager->shutting_down = true;
    }
}

static void *supervisor_thread(void *context) {
    OuterServiceManager *manager = context;
    for (;;) {
        pthread_mutex_lock(&manager->mutex);
        size_t socket_count = 0;
        bool any_children = false;
        for (size_t index = 0; index < manager->service_count; index++) {
            OuterService *service = &manager->services[index];
            if (service->pid > 0) any_children = true;
            if (!service->desired_running && service->pid == 0 && service->start_mode == START_SOCKET) {
                socket_count += service->socket_count;
            }
        }
        struct pollfd *poll_fds = calloc(socket_count + 1, sizeof(struct pollfd));
        size_t *owners = calloc(socket_count, sizeof(size_t));
        poll_fds[0].fd = manager->wake_pipe[0];
        poll_fds[0].events = POLLIN;
        size_t cursor = 1;
        for (size_t index = 0; index < manager->service_count; index++) {
            OuterService *service = &manager->services[index];
            if (service->desired_running || service->pid > 0 || service->start_mode != START_SOCKET) continue;
            for (size_t socket_index = 0; socket_index < service->socket_count; socket_index++) {
                poll_fds[cursor].fd = service->sockets[socket_index].fd;
                poll_fds[cursor].events = POLLIN;
                owners[cursor - 1] = index;
                cursor++;
            }
        }
        uint64_t configuration_version = manager->configuration_version;
        bool shutting_down = manager->shutting_down;
        if (shutting_down) {
            for (size_t index = 0; index < manager->service_count; index++) {
                OuterService *service = &manager->services[index];
                if (service->pid > 0 && !service->stopping) request_stop(manager, service, false);
            }
        }
        pthread_mutex_unlock(&manager->mutex);

        if (shutting_down && !any_children) {
            free(poll_fds);
            free(owners);
            break;
        }
        int poll_result = poll(poll_fds, (nfds_t)(socket_count + 1), 200);
        pthread_mutex_lock(&manager->mutex);
        if (poll_result > 0 && (poll_fds[0].revents & POLLIN)) {
            char discard[64];
            while (read(manager->wake_pipe[0], discard, sizeof(discard)) > 0) {}
        }
        if (poll_result > 0 && !manager->shutting_down &&
            configuration_version == manager->configuration_version) {
            for (size_t index = 1; index <= socket_count; index++) {
                size_t service_index = owners[index - 1];
                if (service_index < manager->service_count &&
                    (poll_fds[index].revents & POLLIN) &&
                    manager->services[service_index].pid == 0) {
                    OuterService *service = &manager->services[service_index];
                    uint64_t activation_time = monotonic_ms();
                    service->desired_running = true;
                    if (service->restart_at_ms <= activation_time) {
                        service->restart_at_ms = activation_time;
                        service->state = "starting";
                    } else {
                        service->state = "waiting-to-restart";
                    }
                }
            }
        }
        free(poll_fds);
        free(owners);

        uint64_t now = monotonic_ms();
        for (size_t index = 0; index < manager->service_count; index++) {
            OuterService *service = &manager->services[index];
            if (service->pid > 0) {
                int wait_status = 0;
                pid_t waited = waitpid(service->pid, &wait_status, WNOHANG);
                if (waited == service->pid) handle_exit(manager, service, wait_status);
                else if (service->stopping && now >= service->stop_deadline_ms) {
                    kill(-service->pid, SIGKILL);
                    service->stop_deadline_ms = UINT64_MAX;
                }
            }
            if (!manager->shutting_down && service->desired_running && service->pid == 0 && now >= service->restart_at_ms) {
                char error[512] = {0};
                if (!spawn_service(manager, service, error, sizeof(error))) {
                    fprintf(stderr, "outershelld: %s\n", error);
                    service->failed = true;
                    service->state = "failed";
                    service->exit_status = 127;
                    service->desired_running = service->restart_mode != RESTART_NEVER;
                    service->restart_at_ms = now + service->restart_delay_ms;
                    notify_service(manager, service);
                }
            }
        }
        pthread_mutex_unlock(&manager->mutex);
    }
    return NULL;
}

static int compare_names(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static bool load_services(OuterServiceManager *manager, char *error, size_t error_size) {
    DIR *directory = opendir(manager->services_directory);
    if (!directory) {
        if (errno == ENOENT) {
            if (mkdir(manager->services_directory, 0755) == 0) directory = opendir(manager->services_directory);
        }
        if (!directory) {
            snprintf(error, error_size, "open services directory %s: %s",
                     manager->services_directory, strerror(errno));
            return false;
        }
    }
    char **names = NULL;
    size_t name_count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        const char *suffix = ".outerservice";
        size_t suffix_length = strlen(suffix);
        if (length <= suffix_length || strcmp(entry->d_name + length - suffix_length, suffix) != 0) continue;
        char id[256];
        size_t id_length = length - suffix_length;
        if (id_length >= sizeof(id)) continue;
        memcpy(id, entry->d_name, id_length);
        id[id_length] = '\0';
        if (!valid_identifier(id) || !append_string(&names, &name_count, entry->d_name)) {
            snprintf(error, error_size, "invalid service filename %s", entry->d_name);
            closedir(directory);
            free_strings(names, name_count);
            return false;
        }
    }
    closedir(directory);
    qsort(names, name_count, sizeof(char *), compare_names);

    OuterService *services = calloc(name_count, sizeof(OuterService));
    if (name_count && !services) {
        free_strings(names, name_count);
        snprintf(error, error_size, "out of memory");
        return false;
    }
    for (size_t index = 0; index < name_count; index++) {
        size_t length = strlen(names[index]);
        char id[256];
        size_t id_length = length - strlen(".outerservice");
        memcpy(id, names[index], id_length);
        id[id_length] = '\0';
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", manager->services_directory, names[index]);
        if (!parse_service_file(path, id, &services[index], error, error_size)) {
            for (size_t free_index = 0; free_index < index; free_index++) free_service(&services[free_index]);
            free(services);
            free_strings(names, name_count);
            return false;
        }
        for (size_t socket_index = 0; socket_index < services[index].socket_count; socket_index++) {
            if (!make_listener(&services[index].sockets[socket_index], error, error_size)) {
                for (size_t free_index = 0; free_index <= index; free_index++) free_service(&services[free_index]);
                free(services);
                free_strings(names, name_count);
                return false;
            }
        }
        services[index].desired_running = services[index].start_mode == START_EAGER;
        services[index].state = services[index].desired_running ? "starting" : "stopped";
    }
    free_strings(names, name_count);
    manager->services = services;
    manager->service_count = name_count;
    return true;
}

OuterServiceManager *outer_service_manager_create(const OuterServiceManagerOptions *options,
                                                  char *error,
                                                  size_t error_size) {
    if (!options || !options->services_directory || !options->launcher_path) {
        snprintf(error, error_size, "internal service manager requires services_directory and launcher_path");
        return NULL;
    }
    OuterServiceManager *manager = calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    manager->services_directory = duplicate_string(options->services_directory);
    manager->launcher_path = duplicate_string(options->launcher_path);
    manager->event_callback = options->event_callback;
    manager->event_context = options->event_context;
    manager->wake_pipe[0] = manager->wake_pipe[1] = -1;
    pthread_mutex_init(&manager->mutex, NULL);
    if (!manager->services_directory || !manager->launcher_path ||
        pipe(manager->wake_pipe) != 0) {
        snprintf(error, error_size, "initialize internal service manager: %s", strerror(errno));
        outer_service_manager_destroy(manager);
        return NULL;
    }
    for (size_t index = 0; index < 2; index++) {
        fcntl(manager->wake_pipe[index], F_SETFD, fcntl(manager->wake_pipe[index], F_GETFD) | FD_CLOEXEC);
        fcntl(manager->wake_pipe[index], F_SETFL, fcntl(manager->wake_pipe[index], F_GETFL) | O_NONBLOCK);
    }
    if (!load_services(manager, error, error_size)) {
        outer_service_manager_destroy(manager);
        return NULL;
    }
    if (pthread_create(&manager->thread, NULL, supervisor_thread, manager) != 0) {
        snprintf(error, error_size, "start internal service supervisor thread");
        outer_service_manager_destroy(manager);
        return NULL;
    }
    manager->thread_started = true;
    return manager;
}

void outer_service_manager_shutdown(OuterServiceManager *manager) {
    if (!manager || !manager->thread_started) return;
    pthread_mutex_lock(&manager->mutex);
    manager->shutting_down = true;
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);
    pthread_join(manager->thread, NULL);
    manager->thread_started = false;
}

void outer_service_manager_destroy(OuterServiceManager *manager) {
    if (!manager) return;
    outer_service_manager_shutdown(manager);
    for (size_t index = 0; index < manager->service_count; index++) free_service(&manager->services[index]);
    free(manager->services);
    if (manager->wake_pipe[0] >= 0) close(manager->wake_pipe[0]);
    if (manager->wake_pipe[1] >= 0) close(manager->wake_pipe[1]);
    pthread_mutex_destroy(&manager->mutex);
    free(manager->services_directory);
    free(manager->launcher_path);
    free(manager);
}

bool outer_service_manager_exit_requested(OuterServiceManager *manager) {
    if (!manager) return false;
    pthread_mutex_lock(&manager->mutex);
    bool requested = manager->shutting_down && manager->exit_status != 0;
    pthread_mutex_unlock(&manager->mutex);
    return requested;
}

int outer_service_manager_exit_status(OuterServiceManager *manager) {
    if (!manager) return 0;
    pthread_mutex_lock(&manager->mutex);
    int status = manager->exit_status;
    pthread_mutex_unlock(&manager->mutex);
    return status;
}

static OuterService *find_service(OuterServiceManager *manager, const char *service_id);

bool outer_service_manager_reload(OuterServiceManager *manager, char *error, size_t error_size) {
    if (!manager) return false;
    pthread_mutex_lock(&manager->mutex);
    for (size_t index = 0; index < manager->service_count; index++) {
        if (manager->services[index].pid > 0) {
            snprintf(error, error_size, "cannot reload services while %s is running", manager->services[index].id);
            pthread_mutex_unlock(&manager->mutex);
            return false;
        }
    }
    manager->configuration_version++;
    for (size_t index = 0; index < manager->service_count; index++) free_service(&manager->services[index]);
    free(manager->services);
    manager->services = NULL;
    manager->service_count = 0;
    bool ok = load_services(manager, error, error_size);
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);
    return ok;
}

bool outer_service_manager_unload_service(OuterServiceManager *manager,
                                          const char *service_id,
                                          char *error,
                                          size_t error_size) {
    if (!manager || !valid_identifier(service_id)) {
        snprintf(error, error_size, "invalid service identifier");
        return false;
    }

    uint64_t deadline = 0;
    pthread_mutex_lock(&manager->mutex);
    OuterService *service = find_service(manager, service_id);
    if (!service) {
        pthread_mutex_unlock(&manager->mutex);
        return true;
    }
    if (service->pid > 0) {
        deadline = monotonic_ms() + service->stop_timeout_ms + 1000;
        request_stop(manager, service, false);
    }
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);

    while (deadline) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
        nanosleep(&delay, NULL);
        pthread_mutex_lock(&manager->mutex);
        service = find_service(manager, service_id);
        bool stopped = !service || service->pid == 0;
        pthread_mutex_unlock(&manager->mutex);
        if (stopped) break;
        if (monotonic_ms() >= deadline) {
            snprintf(error, error_size, "timed out stopping service %s", service_id);
            return false;
        }
    }

    pthread_mutex_lock(&manager->mutex);
    size_t index = 0;
    while (index < manager->service_count &&
           strcmp(manager->services[index].id, service_id) != 0) index++;
    if (index < manager->service_count) {
        manager->configuration_version++;
        notify_service(manager, &manager->services[index]);
        free_service(&manager->services[index]);
        if (index + 1 < manager->service_count) {
            memmove(&manager->services[index],
                    &manager->services[index + 1],
                    (manager->service_count - index - 1) * sizeof(OuterService));
        }
        manager->service_count--;
    }
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);
    return true;
}

bool outer_service_manager_load_service(OuterServiceManager *manager,
                                        const char *service_id,
                                        char *error,
                                        size_t error_size) {
    if (!manager || !valid_identifier(service_id)) {
        snprintf(error, error_size, "invalid service identifier");
        return false;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.outerservice", manager->services_directory, service_id);
    OuterService loaded;
    if (!parse_service_file(path, service_id, &loaded, error, error_size)) return false;

    if (outer_service_manager_has_service(manager, service_id) &&
        !outer_service_manager_unload_service(manager, service_id, error, error_size)) {
        free_service(&loaded);
        return false;
    }

    pthread_mutex_lock(&manager->mutex);
    for (size_t socket_index = 0; socket_index < loaded.socket_count; socket_index++) {
        if (!make_listener(&loaded.sockets[socket_index], error, error_size)) {
            free_service(&loaded);
            pthread_mutex_unlock(&manager->mutex);
            return false;
        }
    }
    loaded.desired_running = loaded.start_mode == START_EAGER;
    loaded.state = loaded.desired_running ? "starting" : "stopped";
    loaded.restart_at_ms = monotonic_ms();
    OuterService *services = realloc(manager->services,
                                     (manager->service_count + 1) * sizeof(OuterService));
    if (!services) {
        free_service(&loaded);
        pthread_mutex_unlock(&manager->mutex);
        snprintf(error, error_size, "out of memory");
        return false;
    }
    manager->services = services;
    manager->services[manager->service_count++] = loaded;
    manager->configuration_version++;
    notify_service(manager, &manager->services[manager->service_count - 1]);
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);
    return true;
}

static OuterService *find_service(OuterServiceManager *manager, const char *service_id) {
    for (size_t index = 0; index < manager->service_count; index++) {
        if (strcmp(manager->services[index].id, service_id) == 0) return &manager->services[index];
    }
    return NULL;
}

bool outer_service_manager_has_service(OuterServiceManager *manager, const char *service_id) {
    if (!manager || !service_id) return false;
    pthread_mutex_lock(&manager->mutex);
    bool found = find_service(manager, service_id) != NULL;
    pthread_mutex_unlock(&manager->mutex);
    return found;
}

bool outer_service_manager_status(OuterServiceManager *manager,
                                  const char *service_id,
                                  OuterServiceStatus *status) {
    if (!manager || !service_id || !status) return false;
    pthread_mutex_lock(&manager->mutex);
    OuterService *service = find_service(manager, service_id);
    if (service) {
        status->available = service->socket_count > 0;
        status->running = service->pid > 0 && !service->stopping;
        status->failed = service->failed;
        status->exit_status = service->exit_status;
        status->state = service->state;
    }
    pthread_mutex_unlock(&manager->mutex);
    return service != NULL;
}

bool outer_service_manager_operate(OuterServiceManager *manager,
                                   const char *service_id,
                                   const char *operation,
                                   char *error,
                                   size_t error_size) {
    if (!manager || !service_id || !operation) return false;
    pthread_mutex_lock(&manager->mutex);
    OuterService *service = find_service(manager, service_id);
    if (!service) {
        snprintf(error, error_size, "service %s is not loaded", service_id);
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    if (strcmp(operation, "start") == 0) {
        service->desired_running = true;
        service->failed = false;
        service->restart_at_ms = monotonic_ms();
        if (service->pid == 0) service->state = "starting";
    } else if (strcmp(operation, "stop") == 0) {
        request_stop(manager, service, false);
    } else if (strcmp(operation, "restart") == 0) {
        if (service->pid > 0) request_stop(manager, service, true);
        else {
            service->desired_running = true;
            service->failed = false;
            service->restart_at_ms = monotonic_ms();
            service->state = "starting";
        }
    } else {
        snprintf(error, error_size, "unsupported service operation %s", operation);
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    notify_service(manager, service);
    pthread_mutex_unlock(&manager->mutex);
    (void)write(manager->wake_pipe[1], "x", 1);
    return true;
}

int outer_service_exec_child(int argc, char **argv) {
    if (argc < 3 || !argv[2] || argv[2][0] != '/') {
        fprintf(stderr, "outershelld: --outerservice-exec requires an absolute executable path\n");
        return 126;
    }
    char pid[64];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    setenv("LISTEN_PID", pid, 1);
    const char *directory = getenv("OUTERSERVICE_CHDIR");
    if (directory && chdir(directory) != 0) {
        fprintf(stderr, "outershelld: chdir %s: %s\n", directory, strerror(errno));
        return 126;
    }
    unsetenv("OUTERSERVICE_CHDIR");
    execve(argv[2], &argv[2], environ);
    fprintf(stderr, "outershelld: exec %s: %s\n", argv[2], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
