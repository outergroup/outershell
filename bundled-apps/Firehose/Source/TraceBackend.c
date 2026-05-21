#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifdef __linux__
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

#define READ_BUFFER_SIZE 8192
#define EVENT_BINARY_MAGIC 0x45435254u
#define EVENT_BINARY_VERSION 4
#define EVENT_BINARY_HEADER_SIZE 72
#define EVENT_BINARY_RECORD_SIZE 64
#define EVENT_BINARY_MAX_COUNT 512
#define PROCESS_TIMELINE_BINARY_MAGIC 0x50435254u
#define PROCESS_TIMELINE_BINARY_VERSION 3
#define PROCESS_TIMELINE_BINARY_HEADER_SIZE 64
#define PROCESS_TIMELINE_BINARY_RECORD_SIZE 64
#define PROCESS_TIMELINE_DOT_RECORD_SIZE 20
#define PROCESS_TIMELINE_DOT_BUCKET_COUNT 512
#define PROCESS_TIMELINE_DOT_MAX_COUNT 16
#define EVENT_POSITION_BINARY_MAGIC 0x504a5254u
#define EVENT_POSITION_BINARY_VERSION 1
#define EVENT_POSITION_BINARY_HEADER_SIZE 40
#define CLEAR_LOG_BINARY_MAGIC 0x434c5254u
#define CLEAR_LOG_BINARY_VERSION 1
#define CLEAR_LOG_BINARY_HEADER_SIZE 16
#define CAPTURE_STATUS_BINARY_MAGIC 0x43535254u
#define CAPTURE_STATUS_BINARY_VERSION 1
#define CAPTURE_STATUS_BINARY_HEADER_SIZE 48
#define PROCESS_NAME_LEN 128
#define EVENT_PATH_LEN 512
#define EVENT_FILTER_MAX_CLAUSES 16
#define EVENT_FILTER_VALUE_LEN 256
#define DEFAULT_PORT 7352
#define SAMPLE_INTERVAL_MS 50
#define STORAGE_CHECK_INTERVAL_SECONDS 1.0
#define STORAGE_LOW_MIN_FREE_BYTES (1024ull * 1024ull * 1024ull)
#define STORAGE_LOW_MAX_FREE_BYTES (20ull * 1024ull * 1024ull * 1024ull)

#if defined(__GNUC__) || defined(__clang__)
#define TRACE_UNUSED __attribute__((unused))
#else
#define TRACE_UNUSED
#endif

static const char *kBundleUrlPath = "/bundles/TraceContent";
static const char *kBundleUrlPathMacosArm = "/bundles/TraceContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/TraceContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/TraceContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/TraceContent.bundle.macos-x86.aar";

static char g_bundle_file_path_macos_arm[PATH_MAX] = "";
static char g_bundle_file_path_macos_x86[PATH_MAX] = "";
static char g_listen_socket_path[PATH_MAX] = "";
static volatile sig_atomic_t g_shutdown_requested = 0;

typedef struct {
    int pid;
    int ppid;
    char name[PROCESS_NAME_LEN];
} ProcessInfo;

typedef struct {
    ProcessInfo *items;
    size_t count;
} ProcessSnapshot;

typedef struct {
    char column[16];
    char operation[16];
    char value[EVENT_FILTER_VALUE_LEN];
} EventFilterClause;

typedef struct {
    EventFilterClause clauses[EVENT_FILTER_MAX_CLAUSES];
    size_t count;
} EventFilter;

typedef struct {
    uint64_t id;
    double timestamp;
    int pid;
    int ppid;
    char type[32];
    char process[PROCESS_NAME_LEN];
    char path[EVENT_PATH_LEN];
    char detail[256];
} TraceEvent;

typedef struct {
    int pid;
    int ppid;
    char process[PROCESS_NAME_LEN];
    double first_timestamp;
    double start_timestamp;
    double end_timestamp;
    double last_timestamp;
    uint64_t event_count;
    uint8_t dot_counts[PROCESS_TIMELINE_DOT_BUCKET_COUNT];
    double dot_timestamp_sums[PROCESS_TIMELINE_DOT_BUCKET_COUNT];
    bool has_start;
    bool has_end;
    bool open_at_start;
} ProcessTimelineItem;

typedef struct {
    ProcessTimelineItem *items;
    size_t count;
} ProcessTimeline;

typedef struct {
    int pid;
    int fd;
    char path[EVENT_PATH_LEN];
} FdPathEntry;

typedef struct {
    FdPathEntry *items;
    size_t count;
} FdPathTable;

typedef struct {
    int pid;
    int tid;
    char path[EVENT_PATH_LEN];
} PendingOpenEntry;

typedef struct {
    PendingOpenEntry *items;
    size_t count;
} PendingOpenTable;

typedef struct {
    int pid;
    int ppid;
} ProcessParentEntry;

typedef struct {
    ProcessParentEntry *items;
    size_t count;
} ProcessParentTable;

typedef struct {
    size_t count;
    uint64_t next_id;
    int fd;
    bool reported_recursive_write_ignore;
    char path[PATH_MAX];
} EventStore;

typedef enum {
    CAPTURE_MODE_AUTO,
    CAPTURE_MODE_PROC,
    CAPTURE_MODE_EBPF
} CaptureMode;

typedef enum {
    CAPTURE_PAUSE_REASON_NONE = 0,
    CAPTURE_PAUSE_REASON_USER = 1,
    CAPTURE_PAUSE_REASON_LOW_STORAGE = 2
} CapturePauseReason;

typedef struct {
    bool valid;
    bool low;
    uint64_t available_bytes;
    uint64_t total_bytes;
    uint64_t threshold_bytes;
} StorageStatus;

static EventStore g_events = {0};
static ProcessSnapshot g_previous_snapshot = {0};
static bool g_has_previous_snapshot = false;
static FdPathTable g_fd_paths = {0};
static PendingOpenTable g_pending_opens = {0};
static ProcessParentTable g_process_parents = {0};
static bool g_capture_paused = false;
static CapturePauseReason g_capture_pause_reason = CAPTURE_PAUSE_REASON_NONE;
static bool g_capture_uses_ebpf = false;
static StorageStatus g_storage_status = {0};
static double g_last_storage_check_time = 0;

static void add_event(const char *type, const ProcessInfo *process, const char *path, const char *detail);
static void set_capture_paused(bool paused);

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
}

static double current_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int compare_process_info(const void *lhs, const void *rhs) {
    const ProcessInfo *a = (const ProcessInfo *)lhs;
    const ProcessInfo *b = (const ProcessInfo *)rhs;
    return (a->pid > b->pid) - (a->pid < b->pid);
}

static void free_process_snapshot(ProcessSnapshot *snapshot) {
    free(snapshot->items);
    snapshot->items = NULL;
    snapshot->count = 0;
}

static bool append_process(ProcessSnapshot *snapshot, ProcessInfo process) {
    ProcessInfo *next = realloc(snapshot->items, sizeof(ProcessInfo) * (snapshot->count + 1));
    if (!next) {
        return false;
    }
    snapshot->items = next;
    snapshot->items[snapshot->count++] = process;
    return true;
}

static void fd_path_table_free(FdPathTable *table) {
    free(table->items);
    table->items = NULL;
    table->count = 0;
}

static void pending_open_table_free(PendingOpenTable *table) {
    free(table->items);
    table->items = NULL;
    table->count = 0;
}

static void process_parent_table_free(ProcessParentTable *table) {
    free(table->items);
    table->items = NULL;
    table->count = 0;
}

static ProcessParentEntry *process_parent_find(int pid) {
    for (size_t i = 0; i < g_process_parents.count; ++i) {
        if (g_process_parents.items[i].pid == pid) {
            return &g_process_parents.items[i];
        }
    }
    return NULL;
}

static TRACE_UNUSED int process_parent_lookup(int pid) {
    ProcessParentEntry *entry = process_parent_find(pid);
    return entry ? entry->ppid : 0;
}

static TRACE_UNUSED void process_parent_set(int pid, int ppid) {
    if (pid <= 0 || ppid <= 0 || pid == ppid) {
        return;
    }
    ProcessParentEntry *entry = process_parent_find(pid);
    if (!entry) {
        ProcessParentEntry *next = realloc(g_process_parents.items,
                                           sizeof(ProcessParentEntry) * (g_process_parents.count + 1));
        if (!next) {
            return;
        }
        g_process_parents.items = next;
        entry = &g_process_parents.items[g_process_parents.count++];
    }
    entry->pid = pid;
    entry->ppid = ppid;
}

static TRACE_UNUSED void process_parent_remove(int pid) {
    for (size_t i = 0; i < g_process_parents.count; ++i) {
        if (g_process_parents.items[i].pid == pid) {
            g_process_parents.items[i] = g_process_parents.items[g_process_parents.count - 1];
            g_process_parents.count--;
            return;
        }
    }
}

static FdPathEntry *fd_path_find(int pid, int fd) {
    for (size_t i = 0; i < g_fd_paths.count; ++i) {
        if (g_fd_paths.items[i].pid == pid && g_fd_paths.items[i].fd == fd) {
            return &g_fd_paths.items[i];
        }
    }
    return NULL;
}

static void fd_path_set(int pid, int fd, const char *path) {
    if (pid <= 0 || fd < 0 || !path || path[0] == '\0') {
        return;
    }
    FdPathEntry *entry = fd_path_find(pid, fd);
    if (!entry) {
        FdPathEntry *next = realloc(g_fd_paths.items, sizeof(FdPathEntry) * (g_fd_paths.count + 1));
        if (!next) {
            return;
        }
        g_fd_paths.items = next;
        entry = &g_fd_paths.items[g_fd_paths.count++];
        memset(entry, 0, sizeof(*entry));
        entry->pid = pid;
        entry->fd = fd;
    }
    snprintf(entry->path, sizeof(entry->path), "%s", path);
}

static TRACE_UNUSED void fd_path_remove(int pid, int fd) {
    for (size_t i = 0; i < g_fd_paths.count; ++i) {
        if (g_fd_paths.items[i].pid == pid && g_fd_paths.items[i].fd == fd) {
            g_fd_paths.items[i] = g_fd_paths.items[g_fd_paths.count - 1];
            g_fd_paths.count--;
            return;
        }
    }
}

static void fd_path_remove_pid(int pid) {
    size_t out = 0;
    for (size_t i = 0; i < g_fd_paths.count; ++i) {
        if (g_fd_paths.items[i].pid != pid) {
            g_fd_paths.items[out++] = g_fd_paths.items[i];
        }
    }
    g_fd_paths.count = out;
}

static TRACE_UNUSED void fd_path_copy_pid(int parent_pid, int child_pid) {
    if (parent_pid <= 0 || child_pid <= 0 || parent_pid == child_pid) {
        return;
    }
    size_t original_count = g_fd_paths.count;
    for (size_t i = 0; i < original_count; ++i) {
        if (g_fd_paths.items[i].pid == parent_pid) {
            fd_path_set(child_pid, g_fd_paths.items[i].fd, g_fd_paths.items[i].path);
        }
    }
}

static PendingOpenEntry *pending_open_find(int pid, int tid) {
    for (size_t i = 0; i < g_pending_opens.count; ++i) {
        if (g_pending_opens.items[i].pid == pid && g_pending_opens.items[i].tid == tid) {
            return &g_pending_opens.items[i];
        }
    }
    return NULL;
}

static TRACE_UNUSED void pending_open_set(int pid, int tid, const char *path) {
    if (pid <= 0 || tid <= 0 || !path || path[0] == '\0') {
        return;
    }
    PendingOpenEntry *entry = pending_open_find(pid, tid);
    if (!entry) {
        PendingOpenEntry *next = realloc(g_pending_opens.items,
                                         sizeof(PendingOpenEntry) * (g_pending_opens.count + 1));
        if (!next) {
            return;
        }
        g_pending_opens.items = next;
        entry = &g_pending_opens.items[g_pending_opens.count++];
        memset(entry, 0, sizeof(*entry));
        entry->pid = pid;
        entry->tid = tid;
    }
    snprintf(entry->path, sizeof(entry->path), "%s", path);
}

static TRACE_UNUSED bool pending_open_take(int pid, int tid, char *out, size_t out_size) {
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';
    for (size_t i = 0; i < g_pending_opens.count; ++i) {
        if (g_pending_opens.items[i].pid == pid && g_pending_opens.items[i].tid == tid) {
            snprintf(out, out_size, "%s", g_pending_opens.items[i].path);
            g_pending_opens.items[i] = g_pending_opens.items[g_pending_opens.count - 1];
            g_pending_opens.count--;
            return out[0] != '\0';
        }
    }
    return false;
}

static TRACE_UNUSED void pending_open_remove_pid(int pid) {
    size_t out = 0;
    for (size_t i = 0; i < g_pending_opens.count; ++i) {
        if (g_pending_opens.items[i].pid != pid) {
            g_pending_opens.items[out++] = g_pending_opens.items[i];
        }
    }
    g_pending_opens.count = out;
}

static bool read_proc_fd_path(int pid, int fd, char *out, size_t out_size) {
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';
#ifdef __linux__
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%d", pid, fd);
    ssize_t len = readlink(link_path, out, out_size - 1);
    if (len < 0) {
        return false;
    }
    out[len] = '\0';
    return true;
#else
    (void)pid;
    (void)fd;
    return false;
#endif
}

static TRACE_UNUSED bool resolve_fd_path(int pid, int fd, char *out, size_t out_size) {
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';
    FdPathEntry *entry = fd_path_find(pid, fd);
    if (entry) {
        snprintf(out, out_size, "%s", entry->path);
        return out[0] != '\0';
    }
    if (read_proc_fd_path(pid, fd, out, out_size)) {
        fd_path_set(pid, fd, out);
        return true;
    }
    return false;
}

static TRACE_UNUSED bool resolve_cached_fd_path(int pid, int fd, char *out, size_t out_size) {
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';
    FdPathEntry *entry = fd_path_find(pid, fd);
    if (!entry) {
        return false;
    }
    snprintf(out, out_size, "%s", entry->path);
    return out[0] != '\0';
}

#ifdef __APPLE__
static bool capture_process_snapshot(ProcessSnapshot *snapshot) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t length = 0;
    if (sysctl(mib, 4, NULL, &length, NULL, 0) != 0 || length == 0) {
        return false;
    }

    struct kinfo_proc *processes = malloc(length);
    if (!processes) {
        return false;
    }

    if (sysctl(mib, 4, processes, &length, NULL, 0) != 0) {
        free(processes);
        return false;
    }

    size_t count = length / sizeof(struct kinfo_proc);
    for (size_t i = 0; i < count; ++i) {
        ProcessInfo info = {0};
        info.pid = processes[i].kp_proc.p_pid;
        info.ppid = processes[i].kp_eproc.e_ppid;
        snprintf(info.name, sizeof(info.name), "%s", processes[i].kp_proc.p_comm);
        if (info.pid > 0) {
            append_process(snapshot, info);
        }
    }
    free(processes);
    qsort(snapshot->items, snapshot->count, sizeof(ProcessInfo), compare_process_info);
    return true;
}

static TRACE_UNUSED int normalize_process_parent_pid(int ppid) {
    return ppid;
}
#else
static bool read_linux_process_stat(int pid, ProcessInfo *info) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char line[4096];
    bool ok = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!ok) {
        return false;
    }

    char *open = strchr(line, '(');
    char *close = strrchr(line, ')');
    if (!open || !close || close <= open) {
        return false;
    }

    size_t name_len = (size_t)(close - open - 1);
    if (name_len >= sizeof(info->name)) {
        name_len = sizeof(info->name) - 1;
    }
    memcpy(info->name, open + 1, name_len);
    info->name[name_len] = '\0';

    char state = '\0';
    int ppid = 0;
    if (sscanf(close + 2, "%c %d", &state, &ppid) != 2) {
        return false;
    }
    (void)state;
    info->pid = pid;
    info->ppid = ppid;
    return true;
}

static bool read_linux_process_tgid(int pid, int *tgid_out) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Tgid:", 5) != 0) {
            continue;
        }
        char *value = line + 5;
        while (*value && isspace((unsigned char)*value)) {
            value++;
        }
        char *end = NULL;
        long tgid = strtol(value, &end, 10);
        if (tgid > 0 && tgid <= INT_MAX) {
            *tgid_out = (int)tgid;
            found = true;
        }
        break;
    }
    fclose(file);
    return found;
}

static TRACE_UNUSED int normalize_process_parent_pid(int ppid) {
    if (ppid <= 0) {
        return ppid;
    }
    int tgid = 0;
    return read_linux_process_tgid(ppid, &tgid) ? tgid : ppid;
}

static bool capture_process_snapshot(ProcessSnapshot *snapshot) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }
        char *end = NULL;
        long pid_long = strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || pid_long <= 0 || pid_long > INT_MAX) {
            continue;
        }

        ProcessInfo info = {0};
        if (read_linux_process_stat((int)pid_long, &info)) {
            append_process(snapshot, info);
        }
    }
    closedir(dir);
    qsort(snapshot->items, snapshot->count, sizeof(ProcessInfo), compare_process_info);
    return true;
}
#endif

#ifdef __linux__
#define TRACE_BPF_EVENT_FORK 1
#define TRACE_BPF_EVENT_EXIT 2
#define TRACE_BPF_EVENT_OPENAT 3
#define TRACE_BPF_EVENT_OPENAT2 4
#define TRACE_BPF_EVENT_EXECVE 5
#define TRACE_BPF_EVENT_CLOSE 6
#define TRACE_BPF_EVENT_READ 7
#define TRACE_BPF_EVENT_WRITE 8
#define TRACE_BPF_EVENT_OPENAT_RET 9
#define TRACE_BPF_EVENT_OPENAT2_RET 10
#define TRACE_BPF_EVENT_SIZE 184
#define TRACE_BPF_STACK_OFF (-192)
#define TRACE_BPF_FORK_PARENT_PID_OFFSET 24
#define TRACE_BPF_FORK_CHILD_COMM_OFFSET 28
#define TRACE_BPF_FORK_CHILD_PID_OFFSET 44
#define TRACE_BPF_TEXT_LEN 128
#define TRACE_PERF_RING_PAGES 1024
#define TRACE_CAPTURE_RW_SYSCALLS 1

typedef struct {
    uint64_t timestamp_ns;
    uint32_t event_type;
    int32_t pid;
    int32_t ppid;
    int32_t tid;
    uint64_t arg0;
    uint64_t arg1;
    char comm[16];
    char text[TRACE_BPF_TEXT_LEN];
} BPFKernelEvent;

typedef struct {
    int fd;
    void *mapping;
    size_t mapping_size;
    size_t data_size;
} PerfRing;

typedef struct {
    bool active;
    int perf_map_fd;
    int *prog_fds;
    size_t prog_fd_count;
    int *tracepoint_fds;
    size_t tracepoint_fd_count;
    PerfRing *rings;
    int cpu_count;
    long page_size;
} EBPFCapture;

typedef struct {
    int parent_pid;
    int child_comm;
    int child_pid;
} ForkTracepointOffsets;

typedef struct {
    int arg0;
    int arg1;
    int text;
} SyscallTracepointOffsets;

static EBPFCapture g_ebpf_capture = {0};

_Static_assert(sizeof(BPFKernelEvent) == TRACE_BPF_EVENT_SIZE, "unexpected BPF event size");

#define TRACE_BPF_INSN(CODE, DST, SRC, OFF, IMM) \
    ((struct bpf_insn){.code = (CODE), .dst_reg = (DST), .src_reg = (SRC), .off = (OFF), .imm = (IMM)})
#define TRACE_BPF_ALU64_IMM(OP, DST, IMM) TRACE_BPF_INSN(BPF_ALU64 | BPF_K | (OP), DST, 0, 0, IMM)
#define TRACE_BPF_ALU64_REG(OP, DST, SRC) TRACE_BPF_INSN(BPF_ALU64 | BPF_X | (OP), DST, SRC, 0, 0)
#define TRACE_BPF_MOV64_IMM(DST, IMM) TRACE_BPF_ALU64_IMM(BPF_MOV, DST, IMM)
#define TRACE_BPF_MOV64_REG(DST, SRC) TRACE_BPF_ALU64_REG(BPF_MOV, DST, SRC)
#define TRACE_BPF_LDX_MEM(SIZE, DST, SRC, OFF) TRACE_BPF_INSN(BPF_LDX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define TRACE_BPF_ST_MEM(SIZE, DST, OFF, IMM) TRACE_BPF_INSN(BPF_ST | BPF_MEM | (SIZE), DST, 0, OFF, IMM)
#define TRACE_BPF_STX_MEM(SIZE, DST, SRC, OFF) TRACE_BPF_INSN(BPF_STX | BPF_MEM | (SIZE), DST, SRC, OFF, 0)
#define TRACE_BPF_EMIT_CALL(FUNC) TRACE_BPF_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, FUNC)
#define TRACE_BPF_EXIT_INSN() TRACE_BPF_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

static int trace_bpf(enum bpf_cmd command, union bpf_attr *attr, unsigned int size) {
    return (int)syscall(__NR_bpf, command, attr, size);
}

static int trace_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd,
                                 unsigned long flags) {
    return (int)syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int create_perf_event_array_map(int cpu_count, char *error, size_t error_size) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = BPF_MAP_TYPE_PERF_EVENT_ARRAY;
    attr.key_size = sizeof(uint32_t);
    attr.value_size = sizeof(uint32_t);
    attr.max_entries = (uint32_t)cpu_count;

    int fd = trace_bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
    if (fd < 0 && error) {
        snprintf(error, error_size, "BPF_MAP_CREATE failed: %s", strerror(errno));
    }
    return fd;
}

static bool update_perf_event_map(int map_fd, uint32_t cpu, int perf_fd, char *error, size_t error_size) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)map_fd;
    attr.key = (uint64_t)(uintptr_t)&cpu;
    attr.value = (uint64_t)(uintptr_t)&perf_fd;
    attr.flags = BPF_ANY;

    if (trace_bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr)) == 0) {
        return true;
    }
    if (error) {
        snprintf(error, error_size, "BPF_MAP_UPDATE_ELEM cpu %u failed: %s", cpu, strerror(errno));
    }
    return false;
}

static int load_bpf_program(const struct bpf_insn *insns, size_t insn_count, const char *name,
                            char *error, size_t error_size) {
    char log_buffer[65536];
    memset(log_buffer, 0, sizeof(log_buffer));

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = BPF_PROG_TYPE_TRACEPOINT;
    attr.insn_cnt = (uint32_t)insn_count;
    attr.insns = (uint64_t)(uintptr_t)insns;
    attr.license = (uint64_t)(uintptr_t)"GPL";
    attr.log_buf = (uint64_t)(uintptr_t)log_buffer;
    attr.log_size = sizeof(log_buffer);
    attr.log_level = 1;
    snprintf(attr.prog_name, sizeof(attr.prog_name), "%s", name);

    int fd = trace_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd >= 0) {
        return fd;
    }

    if (error) {
        if (log_buffer[0]) {
            snprintf(error, error_size, "BPF_PROG_LOAD %s failed: %s; verifier: %.512s",
                     name, strerror(errno), log_buffer);
        } else {
            snprintf(error, error_size, "BPF_PROG_LOAD %s failed: %s", name, strerror(errno));
        }
    }
    return -1;
}

static void append_zero_event_stack(struct bpf_insn *insns, size_t *count) {
    for (int offset = 0; offset < TRACE_BPF_EVENT_SIZE; offset += 8) {
        insns[(*count)++] = TRACE_BPF_ST_MEM(BPF_DW, BPF_REG_10, TRACE_BPF_STACK_OFF + offset, 0);
    }
}

static void append_ld_map_fd(struct bpf_insn *insns, size_t *count, uint8_t destination, int map_fd) {
    insns[(*count)++] = TRACE_BPF_INSN(BPF_LD | BPF_DW | BPF_IMM,
                                       destination, BPF_PSEUDO_MAP_FD, 0, map_fd);
    insns[(*count)++] = TRACE_BPF_INSN(0, 0, 0, 0, 0);
}

static void append_perf_submit(struct bpf_insn *insns, size_t *count, int map_fd, int output_cpu) {
    insns[(*count)++] = TRACE_BPF_MOV64_REG(BPF_REG_1, BPF_REG_6);
    append_ld_map_fd(insns, count, BPF_REG_2, map_fd);
    insns[(*count)++] = TRACE_BPF_MOV64_IMM(BPF_REG_3, output_cpu);
    insns[(*count)++] = TRACE_BPF_MOV64_REG(BPF_REG_4, BPF_REG_10);
    insns[(*count)++] = TRACE_BPF_ALU64_IMM(BPF_ADD, BPF_REG_4, TRACE_BPF_STACK_OFF);
    insns[(*count)++] = TRACE_BPF_MOV64_IMM(BPF_REG_5, TRACE_BPF_EVENT_SIZE);
    insns[(*count)++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_perf_event_output);
    insns[(*count)++] = TRACE_BPF_MOV64_IMM(BPF_REG_0, 0);
    insns[(*count)++] = TRACE_BPF_EXIT_INSN();
}

static int load_fork_bpf_program(int map_fd, int output_cpu, ForkTracepointOffsets offsets,
                                 char *error, size_t error_size) {
    struct bpf_insn insns[64];
    size_t count = 0;

    insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_6, BPF_REG_1);
    append_zero_event_stack(insns, &count);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_ktime_get_ns);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 0);
    insns[count++] = TRACE_BPF_ST_MEM(BPF_W, BPF_REG_10, TRACE_BPF_STACK_OFF + 8, TRACE_BPF_EVENT_FORK);
    insns[count++] = TRACE_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, offsets.child_pid);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 12);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 20);
    insns[count++] = TRACE_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, offsets.parent_pid);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 16);
    for (int i = 0; i < 16; i += 4) {
        insns[count++] = TRACE_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, offsets.child_comm + i);
        insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 40 + i);
    }
    append_perf_submit(insns, &count, map_fd, output_cpu);

    return load_bpf_program(insns, count, "trace_fork", error, error_size);
}

static int load_exit_bpf_program(int map_fd, int output_cpu, char *error, size_t error_size) {
    struct bpf_insn insns[64];
    size_t count = 0;

    insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_6, BPF_REG_1);
    append_zero_event_stack(insns, &count);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_ktime_get_ns);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 0);
    insns[count++] = TRACE_BPF_ST_MEM(BPF_W, BPF_REG_10, TRACE_BPF_STACK_OFF + 8, TRACE_BPF_EVENT_EXIT);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_get_current_pid_tgid);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 20);
    insns[count++] = TRACE_BPF_ALU64_IMM(BPF_RSH, BPF_REG_0, 32);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 12);
    insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_1, BPF_REG_10);
    insns[count++] = TRACE_BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, TRACE_BPF_STACK_OFF + 40);
    insns[count++] = TRACE_BPF_MOV64_IMM(BPF_REG_2, 16);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_get_current_comm);
    append_perf_submit(insns, &count, map_fd, output_cpu);

    return load_bpf_program(insns, count, "trace_exit", error, error_size);
}

static int load_syscall_bpf_program(int map_fd, int output_cpu, uint32_t event_type,
                                    SyscallTracepointOffsets offsets, const char *name,
                                    char *error, size_t error_size) {
    struct bpf_insn insns[128];
    size_t count = 0;

    insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_6, BPF_REG_1);
    append_zero_event_stack(insns, &count);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_ktime_get_ns);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 0);
    insns[count++] = TRACE_BPF_ST_MEM(BPF_W, BPF_REG_10, TRACE_BPF_STACK_OFF + 8, (int32_t)event_type);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_get_current_pid_tgid);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 20);
    insns[count++] = TRACE_BPF_ALU64_IMM(BPF_RSH, BPF_REG_0, 32);
    insns[count++] = TRACE_BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, TRACE_BPF_STACK_OFF + 12);
    insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_1, BPF_REG_10);
    insns[count++] = TRACE_BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, TRACE_BPF_STACK_OFF + 40);
    insns[count++] = TRACE_BPF_MOV64_IMM(BPF_REG_2, 16);
    insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_get_current_comm);
    if (offsets.arg0 >= 0) {
        insns[count++] = TRACE_BPF_LDX_MEM(BPF_DW, BPF_REG_2, BPF_REG_6, offsets.arg0);
        insns[count++] = TRACE_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 24);
    }
    if (offsets.arg1 >= 0) {
        insns[count++] = TRACE_BPF_LDX_MEM(BPF_DW, BPF_REG_2, BPF_REG_6, offsets.arg1);
        insns[count++] = TRACE_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_2, TRACE_BPF_STACK_OFF + 32);
    }
    if (offsets.text >= 0) {
        insns[count++] = TRACE_BPF_LDX_MEM(BPF_DW, BPF_REG_3, BPF_REG_6, offsets.text);
        insns[count++] = TRACE_BPF_MOV64_REG(BPF_REG_1, BPF_REG_10);
        insns[count++] = TRACE_BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, TRACE_BPF_STACK_OFF + 56);
        insns[count++] = TRACE_BPF_MOV64_IMM(BPF_REG_2, TRACE_BPF_TEXT_LEN);
        insns[count++] = TRACE_BPF_EMIT_CALL(BPF_FUNC_probe_read_user_str);
    }
    append_perf_submit(insns, &count, map_fd, output_cpu);

    return load_bpf_program(insns, count, name, error, error_size);
}

static bool read_text_file(const char *path, char *buffer, size_t buffer_size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    size_t n = fread(buffer, 1, buffer_size - 1, file);
    fclose(file);
    buffer[n] = '\0';
    return n > 0;
}

static bool read_tracepoint_file(const char *group_name, const char *event_name, const char *file_name,
                                 char *buffer, size_t buffer_size) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/kernel/tracing/events/%s/%s/%s", group_name, event_name, file_name);
    if (read_text_file(path, buffer, buffer_size)) {
        return true;
    }
    snprintf(path, sizeof(path), "/sys/kernel/debug/tracing/events/%s/%s/%s", group_name, event_name, file_name);
    return read_text_file(path, buffer, buffer_size);
}

static bool read_tracepoint_id(const char *group_name, const char *event_name, int *id_out) {
    char buffer[64];
    if (!read_tracepoint_file(group_name, event_name, "id", buffer, sizeof(buffer))) {
        return false;
    }
    char *end = NULL;
    long id = strtol(buffer, &end, 10);
    if (id <= 0 || id > INT_MAX) {
        return false;
    }
    *id_out = (int)id;
    return true;
}

static bool parse_tracepoint_field_offset(const char *format, const char *field, int *offset_out) {
    char field_with_semicolon[96];
    char field_with_array[96];
    snprintf(field_with_semicolon, sizeof(field_with_semicolon), " %s;", field);
    snprintf(field_with_array, sizeof(field_with_array), " %s[", field);

    const char *line = format;
    while (line && *line) {
        const char *line_end = strchr(line, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
        if ((memmem(line, line_len, field_with_semicolon, strlen(field_with_semicolon)) ||
             memmem(line, line_len, field_with_array, strlen(field_with_array)))) {
            const char *offset = memmem(line, line_len, "offset:", 7);
            if (offset) {
                char *end = NULL;
                long value = strtol(offset + 7, &end, 10);
                if (value >= 0 && value <= INT_MAX) {
                    *offset_out = (int)value;
                    return true;
                }
            }
        }
        line = line_end ? line_end + 1 : NULL;
    }
    return false;
}

static ForkTracepointOffsets read_fork_tracepoint_offsets(void) {
    ForkTracepointOffsets offsets = {
        .parent_pid = TRACE_BPF_FORK_PARENT_PID_OFFSET,
        .child_comm = TRACE_BPF_FORK_CHILD_COMM_OFFSET,
        .child_pid = TRACE_BPF_FORK_CHILD_PID_OFFSET
    };

    char format[8192];
    if (!read_tracepoint_file("sched", "sched_process_fork", "format", format, sizeof(format))) {
        return offsets;
    }
    parse_tracepoint_field_offset(format, "parent_pid", &offsets.parent_pid);
    parse_tracepoint_field_offset(format, "child_comm", &offsets.child_comm);
    parse_tracepoint_field_offset(format, "child_pid", &offsets.child_pid);
    return offsets;
}

static SyscallTracepointOffsets read_syscall_tracepoint_offsets(const char *event_name,
                                                                const char *arg0,
                                                                const char *arg1,
                                                                const char *text) {
    SyscallTracepointOffsets offsets = {.arg0 = -1, .arg1 = -1, .text = -1};
    char format[8192];
    if (!read_tracepoint_file("syscalls", event_name, "format", format, sizeof(format))) {
        return offsets;
    }
    if (arg0) {
        parse_tracepoint_field_offset(format, arg0, &offsets.arg0);
    }
    if (arg1) {
        parse_tracepoint_field_offset(format, arg1, &offsets.arg1);
    }
    if (text) {
        parse_tracepoint_field_offset(format, text, &offsets.text);
    }
    return offsets;
}

static int open_perf_output_ring(int cpu, long page_size, PerfRing *ring, char *error, size_t error_size) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_SOFTWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_SW_BPF_OUTPUT;
    attr.sample_type = PERF_SAMPLE_RAW;
    attr.wakeup_events = 1;

    int fd = trace_perf_event_open(&attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        snprintf(error, error_size, "perf_event_open output cpu %d failed: %s", cpu, strerror(errno));
        return -1;
    }

    size_t mapping_size = (size_t)page_size * (TRACE_PERF_RING_PAGES + 1);
    void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        snprintf(error, error_size, "mmap perf output cpu %d failed: %s", cpu, strerror(errno));
        close(fd);
        return -1;
    }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    ring->fd = fd;
    ring->mapping = mapping;
    ring->mapping_size = mapping_size;
    ring->data_size = mapping_size - (size_t)page_size;
    return fd;
}

static int open_tracepoint_attachment(const char *group_name, const char *event_name, int cpu, int prog_fd,
                                      char *error, size_t error_size) {
    int tracepoint_id = 0;
    if (!read_tracepoint_id(group_name, event_name, &tracepoint_id)) {
        snprintf(error, error_size, "could not read tracepoint id for %s/%s", group_name, event_name);
        return -1;
    }

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.size = sizeof(attr);
    attr.config = (uint64_t)tracepoint_id;
    attr.sample_period = 1;
    attr.wakeup_events = 1;

    int fd = trace_perf_event_open(&attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        snprintf(error, error_size, "perf_event_open tracepoint %s/%s cpu %d failed: %s",
                 group_name, event_name, cpu, strerror(errno));
        return -1;
    }
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
        snprintf(error, error_size, "PERF_EVENT_IOC_SET_BPF %s/%s cpu %d failed: %s",
                 group_name, event_name, cpu, strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        snprintf(error, error_size, "PERF_EVENT_IOC_ENABLE %s/%s cpu %d failed: %s",
                 group_name, event_name, cpu, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool append_tracepoint_fd(EBPFCapture *capture, int fd) {
    int *next = realloc(capture->tracepoint_fds, sizeof(int) * (capture->tracepoint_fd_count + 1));
    if (!next) {
        return false;
    }
    capture->tracepoint_fds = next;
    capture->tracepoint_fds[capture->tracepoint_fd_count++] = fd;
    return true;
}

static bool append_program_fd(EBPFCapture *capture, int fd) {
    int *next = realloc(capture->prog_fds, sizeof(int) * (capture->prog_fd_count + 1));
    if (!next) {
        return false;
    }
    capture->prog_fds = next;
    capture->prog_fds[capture->prog_fd_count++] = fd;
    return true;
}

static bool load_and_attach_program(EBPFCapture *capture, const char *group_name, const char *event_name,
                                    int cpu, int prog_fd, char *error, size_t error_size) {
    if (prog_fd < 0 || !append_program_fd(capture, prog_fd)) {
        if (prog_fd >= 0) {
            close(prog_fd);
        }
        return false;
    }
    int tracepoint_fd = open_tracepoint_attachment(group_name, event_name, cpu, prog_fd, error, error_size);
    if (tracepoint_fd < 0 || !append_tracepoint_fd(capture, tracepoint_fd)) {
        if (tracepoint_fd >= 0) {
            close(tracepoint_fd);
        }
        return false;
    }
    return true;
}

static void copy_from_perf_ring(void *destination, const char *data, size_t data_size,
                                uint64_t tail, size_t offset, size_t length) {
    size_t start = (size_t)((tail + offset) % data_size);
    size_t first = data_size - start;
    if (first > length) {
        first = length;
    }
    memcpy(destination, data + start, first);
    if (first < length) {
        memcpy((char *)destination + first, data, length - first);
    }
}

static void consume_perf_ring(PerfRing *ring) {
    if (ring->fd < 0 || !ring->mapping) {
        return;
    }

    struct perf_event_mmap_page *meta = (struct perf_event_mmap_page *)ring->mapping;
    char *data = (char *)ring->mapping + (size_t)sysconf(_SC_PAGESIZE);
    uint64_t head = meta->data_head;
    __sync_synchronize();
    uint64_t tail = meta->data_tail;

    while (tail < head) {
        struct perf_event_header header;
        copy_from_perf_ring(&header, data, ring->data_size, tail, 0, sizeof(header));
        if (header.size < sizeof(header) || tail + header.size > head) {
            break;
        }

        if (header.type == PERF_RECORD_SAMPLE) {
            uint32_t raw_size = 0;
            copy_from_perf_ring(&raw_size, data, ring->data_size, tail, sizeof(header), sizeof(raw_size));
            if (raw_size >= sizeof(BPFKernelEvent)) {
                BPFKernelEvent bpf_event;
                copy_from_perf_ring(&bpf_event, data, ring->data_size, tail,
                                     sizeof(header) + sizeof(raw_size), sizeof(bpf_event));

                ProcessInfo process = {
                    .pid = bpf_event.pid,
                    .ppid = bpf_event.ppid
                };
                snprintf(process.name, sizeof(process.name), "%.*s", (int)sizeof(bpf_event.comm), bpf_event.comm);
                if (process.name[0] == '\0') {
                    snprintf(process.name, sizeof(process.name), "pid-%d", process.pid);
                }

                char detail[256];
                char event_path[EVENT_PATH_LEN] = "";
                if (bpf_event.event_type == TRACE_BPF_EVENT_FORK) {
                    process.ppid = normalize_process_parent_pid(process.ppid);
                    process_parent_set(process.pid, process.ppid);
                    fd_path_copy_pid(process.ppid, process.pid);
                    snprintf(detail, sizeof(detail), "Forked from parent pid %d", process.ppid);
                    add_event("process.fork", &process, "", detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_EXIT) {
                    process.ppid = process_parent_lookup(process.pid);
                    pending_open_remove_pid(process.pid);
                    fd_path_remove_pid(process.pid);
                    process_parent_remove(process.pid);
                    snprintf(detail, sizeof(detail), "Exited %s", process.name);
                    add_event("process.exit", &process, "", detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_OPENAT) {
                    process.ppid = process_parent_lookup(process.pid);
                    snprintf(event_path, sizeof(event_path), "%s", bpf_event.text[0] ? bpf_event.text : "");
                    pending_open_set(process.pid, bpf_event.tid, event_path);
                    snprintf(detail, sizeof(detail), "dfd=%lld flags=0x%llx path=%s",
                             (long long)bpf_event.arg0,
                             (unsigned long long)bpf_event.arg1,
                             bpf_event.text[0] ? bpf_event.text : "(unavailable)");
                    add_event("file.openat", &process, event_path, detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_OPENAT2) {
                    process.ppid = process_parent_lookup(process.pid);
                    snprintf(event_path, sizeof(event_path), "%s", bpf_event.text[0] ? bpf_event.text : "");
                    pending_open_set(process.pid, bpf_event.tid, event_path);
                    snprintf(detail, sizeof(detail), "dfd=%lld path=%s",
                             (long long)bpf_event.arg0,
                             bpf_event.text[0] ? bpf_event.text : "(unavailable)");
                    add_event("file.openat2", &process, event_path, detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_OPENAT_RET ||
                           bpf_event.event_type == TRACE_BPF_EVENT_OPENAT2_RET) {
                    int fd = (int64_t)bpf_event.arg0 >= 0 ? (int)bpf_event.arg0 : -1;
                    if (pending_open_take(process.pid, bpf_event.tid, event_path, sizeof(event_path)) && fd >= 0) {
                        fd_path_set(process.pid, fd, event_path);
                    }
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_EXECVE) {
                    process.ppid = process_parent_lookup(process.pid);
                    snprintf(event_path, sizeof(event_path), "%s", bpf_event.text[0] ? bpf_event.text : "");
                    snprintf(detail, sizeof(detail), "path=%s",
                             bpf_event.text[0] ? bpf_event.text : "(unavailable)");
                    add_event("process.execve", &process, event_path, detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_CLOSE) {
                    process.ppid = process_parent_lookup(process.pid);
                    int fd = (int)bpf_event.arg0;
                    resolve_fd_path(process.pid, fd, event_path, sizeof(event_path));
                    snprintf(detail, sizeof(detail), "fd=%llu", (unsigned long long)bpf_event.arg0);
                    add_event("file.close", &process, event_path, detail);
                    fd_path_remove(process.pid, fd);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_READ) {
                    process.ppid = process_parent_lookup(process.pid);
                    int fd = (int)bpf_event.arg0;
                    resolve_fd_path(process.pid, fd, event_path, sizeof(event_path));
                    snprintf(detail, sizeof(detail), "fd=%llu count=%llu",
                             (unsigned long long)bpf_event.arg0,
                             (unsigned long long)bpf_event.arg1);
                    add_event("file.read", &process, event_path, detail);
                } else if (bpf_event.event_type == TRACE_BPF_EVENT_WRITE) {
                    process.ppid = process_parent_lookup(process.pid);
                    int fd = (int)bpf_event.arg0;
                    resolve_fd_path(process.pid, fd, event_path, sizeof(event_path));
                    if (process.pid == getpid() && fd == g_events.fd) {
                        if (event_path[0] == '\0') {
                            snprintf(event_path, sizeof(event_path), "%.*s",
                                     (int)sizeof(event_path) - 1,
                                     g_events.path);
                        }
                        if (!g_events.reported_recursive_write_ignore) {
                            g_events.reported_recursive_write_ignore = true;
                            snprintf(detail, sizeof(detail),
                                     "fd=%llu count=%llu; Ignoring subsequent writes, to avoid recursive explosion",
                                     (unsigned long long)bpf_event.arg0,
                                     (unsigned long long)bpf_event.arg1);
                            add_event("file.write", &process, event_path, detail);
                        }
                    } else {
                        snprintf(detail, sizeof(detail), "fd=%llu count=%llu",
                                 (unsigned long long)bpf_event.arg0,
                                 (unsigned long long)bpf_event.arg1);
                        add_event("file.write", &process, event_path, detail);
                    }
                }
            }
        } else if (header.type == PERF_RECORD_LOST) {
            uint64_t lost_count = 0;
            if (header.size >= sizeof(header) + sizeof(uint64_t) * 2) {
                copy_from_perf_ring(&lost_count, data, ring->data_size, tail,
                                     sizeof(header) + sizeof(uint64_t), sizeof(lost_count));
            }
            ProcessInfo self = {.pid = 0, .ppid = 0};
            snprintf(self.name, sizeof(self.name), "trace");
            char detail[256];
            if (lost_count > 0) {
                snprintf(detail, sizeof(detail),
                         "Kernel perf ring reported %" PRIu64 " lost eBPF events",
                         lost_count);
            } else {
                snprintf(detail, sizeof(detail), "Kernel perf ring reported lost eBPF events");
            }
            add_event("capture.lost", &self, "", detail);
        }

        tail += header.size;
    }

    __sync_synchronize();
    meta->data_tail = tail;
}

static void consume_ebpf_events(EBPFCapture *capture) {
    if (!capture->active) {
        return;
    }
    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        consume_perf_ring(&capture->rings[cpu]);
    }
}

static void discard_ebpf_ring_backlog(EBPFCapture *capture) {
    if (!capture || !capture->active) {
        return;
    }
    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        PerfRing *ring = &capture->rings[cpu];
        if (!ring->mapping) {
            continue;
        }
        struct perf_event_mmap_page *meta = (struct perf_event_mmap_page *)ring->mapping;
        __sync_synchronize();
        meta->data_tail = meta->data_head;
        __sync_synchronize();
    }
}

static void set_ebpf_capture_enabled(EBPFCapture *capture, bool enabled) {
    if (!capture || !capture->active) {
        return;
    }
    unsigned long request = enabled ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE;
    for (size_t i = 0; i < capture->tracepoint_fd_count; ++i) {
        if (capture->tracepoint_fds[i] >= 0) {
            ioctl(capture->tracepoint_fds[i], request, 0);
        }
    }
    if (!enabled) {
        discard_ebpf_ring_backlog(capture);
    }
}

static void stop_ebpf_capture(EBPFCapture *capture) {
    if (!capture) {
        return;
    }
    for (size_t i = 0; i < capture->tracepoint_fd_count; ++i) {
        if (capture->tracepoint_fds[i] >= 0) {
            close(capture->tracepoint_fds[i]);
        }
    }
    for (size_t i = 0; i < capture->prog_fd_count; ++i) {
        if (capture->prog_fds[i] >= 0) {
            close(capture->prog_fds[i]);
        }
    }
    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        if (capture->rings && capture->rings[cpu].mapping) {
            munmap(capture->rings[cpu].mapping, capture->rings[cpu].mapping_size);
        }
        if (capture->rings && capture->rings[cpu].fd >= 0) {
            close(capture->rings[cpu].fd);
        }
    }
    if (capture->perf_map_fd >= 0) {
        close(capture->perf_map_fd);
    }
    free(capture->prog_fds);
    free(capture->tracepoint_fds);
    free(capture->rings);
    memset(capture, 0, sizeof(*capture));
    capture->perf_map_fd = -1;
}

static bool start_ebpf_capture(EBPFCapture *capture, char *error, size_t error_size) {
    memset(capture, 0, sizeof(*capture));
    capture->perf_map_fd = -1;

    long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
    long page_size = sysconf(_SC_PAGESIZE);
    if (cpu_count <= 0 || cpu_count > 1024 || page_size <= 0) {
        snprintf(error, error_size, "could not determine CPU/page configuration");
        return false;
    }
    capture->cpu_count = (int)cpu_count;
    capture->page_size = page_size;
    capture->rings = calloc((size_t)capture->cpu_count, sizeof(PerfRing));
    if (!capture->rings) {
        snprintf(error, error_size, "out of memory allocating perf rings");
        return false;
    }
    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        capture->rings[cpu].fd = -1;
    }

    capture->perf_map_fd = create_perf_event_array_map(capture->cpu_count, error, error_size);
    if (capture->perf_map_fd < 0) {
        stop_ebpf_capture(capture);
        return false;
    }

    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        int fd = open_perf_output_ring(cpu, page_size, &capture->rings[cpu], error, error_size);
        if (fd < 0 || !update_perf_event_map(capture->perf_map_fd, (uint32_t)cpu, fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
    }

    ForkTracepointOffsets fork_offsets = read_fork_tracepoint_offsets();
    SyscallTracepointOffsets openat_offsets = read_syscall_tracepoint_offsets("sys_enter_openat", "dfd", "flags", "filename");
    SyscallTracepointOffsets openat2_offsets = read_syscall_tracepoint_offsets("sys_enter_openat2", "dfd", NULL, "filename");
    SyscallTracepointOffsets openat_ret_offsets = read_syscall_tracepoint_offsets("sys_exit_openat", "ret", NULL, NULL);
    SyscallTracepointOffsets openat2_ret_offsets = read_syscall_tracepoint_offsets("sys_exit_openat2", "ret", NULL, NULL);
    SyscallTracepointOffsets execve_offsets = read_syscall_tracepoint_offsets("sys_enter_execve", NULL, NULL, "filename");
    SyscallTracepointOffsets close_offsets = read_syscall_tracepoint_offsets("sys_enter_close", "fd", NULL, NULL);
#if TRACE_CAPTURE_RW_SYSCALLS
    SyscallTracepointOffsets read_offsets = read_syscall_tracepoint_offsets("sys_enter_read", "fd", "count", NULL);
    SyscallTracepointOffsets write_offsets = read_syscall_tracepoint_offsets("sys_enter_write", "fd", "count", NULL);
#endif
    if (openat_ret_offsets.arg0 < 0 || openat2_ret_offsets.arg0 < 0) {
        snprintf(error, error_size, "could not read openat/openat2 syscall return tracepoint offsets");
        stop_ebpf_capture(capture);
        return false;
    }

    for (int cpu = 0; cpu < capture->cpu_count; ++cpu) {
        int fork_prog_fd = load_fork_bpf_program(capture->perf_map_fd, cpu, fork_offsets, error, error_size);
        if (!load_and_attach_program(capture, "sched", "sched_process_fork", cpu, fork_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int exit_prog_fd = load_exit_bpf_program(capture->perf_map_fd, cpu, error, error_size);
        if (!load_and_attach_program(capture, "sched", "sched_process_exit", cpu, exit_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int openat_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_OPENAT,
                                                      openat_offsets, "trace_openat", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_openat", cpu, openat_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int openat2_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_OPENAT2,
                                                       openat2_offsets, "trace_openat2", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_openat2", cpu, openat2_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int openat_ret_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_OPENAT_RET,
                                                          openat_ret_offsets, "trace_openat_ret", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_exit_openat", cpu, openat_ret_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int openat2_ret_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_OPENAT2_RET,
                                                           openat2_ret_offsets, "trace_openat2_ret", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_exit_openat2", cpu, openat2_ret_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int execve_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_EXECVE,
                                                      execve_offsets, "trace_execve", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_execve", cpu, execve_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int close_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_CLOSE,
                                                     close_offsets, "trace_close", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_close", cpu, close_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
#if TRACE_CAPTURE_RW_SYSCALLS
        int read_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_READ,
                                                    read_offsets, "trace_read", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_read", cpu, read_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
        int write_prog_fd = load_syscall_bpf_program(capture->perf_map_fd, cpu, TRACE_BPF_EVENT_WRITE,
                                                     write_offsets, "trace_write", error, error_size);
        if (!load_and_attach_program(capture, "syscalls", "sys_enter_write", cpu, write_prog_fd, error, error_size)) {
            stop_ebpf_capture(capture);
            return false;
        }
#endif
    }

    capture->active = true;
    return true;
}
#else
typedef struct {
    bool active;
} EBPFCapture;

static EBPFCapture g_ebpf_capture = {0};

static bool start_ebpf_capture(EBPFCapture *capture, char *error, size_t error_size) {
    (void)capture;
    snprintf(error, error_size, "eBPF capture is only available on Linux");
    return false;
}

static void consume_ebpf_events(EBPFCapture *capture) {
    (void)capture;
}

static void discard_ebpf_ring_backlog(EBPFCapture *capture) {
    (void)capture;
}

static void set_ebpf_capture_enabled(EBPFCapture *capture, bool enabled) {
    (void)capture;
    (void)enabled;
}

static void stop_ebpf_capture(EBPFCapture *capture) {
    (void)capture;
}
#endif

static bool event_store_init(EventStore *store) {
    memset(store, 0, sizeof(*store));
    store->fd = -1;

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/trace-events-XXXXXX", tmpdir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return false;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        return false;
    }
    snprintf(store->path, sizeof(store->path), "%s", path);
    unlink(path);
    store->fd = fd;
    return true;
}

static void event_store_close(EventStore *store) {
    if (store->fd >= 0) {
        close(store->fd);
    }
    memset(store, 0, sizeof(*store));
    store->fd = -1;
}

static uint64_t saturating_mul_u64(uint64_t lhs, uint64_t rhs) {
    if (lhs != 0 && rhs > UINT64_MAX / lhs) {
        return UINT64_MAX;
    }
    return lhs * rhs;
}

static uint64_t storage_low_threshold_bytes(uint64_t total_bytes) {
    uint64_t threshold = total_bytes / 100;
    if (threshold < STORAGE_LOW_MIN_FREE_BYTES) {
        threshold = STORAGE_LOW_MIN_FREE_BYTES;
    }
    if (threshold > STORAGE_LOW_MAX_FREE_BYTES) {
        threshold = STORAGE_LOW_MAX_FREE_BYTES;
    }
    return threshold;
}

static bool refresh_storage_status(void) {
    if (g_events.fd < 0) {
        g_storage_status.valid = false;
        return false;
    }

    struct statvfs stats;
    if (fstatvfs(g_events.fd, &stats) != 0) {
        g_storage_status.valid = false;
        return false;
    }

    uint64_t block_size = stats.f_frsize ? (uint64_t)stats.f_frsize : (uint64_t)stats.f_bsize;
    uint64_t total_bytes = saturating_mul_u64((uint64_t)stats.f_blocks, block_size);
    uint64_t available_bytes = saturating_mul_u64((uint64_t)stats.f_bavail, block_size);
    uint64_t threshold_bytes = storage_low_threshold_bytes(total_bytes);

    g_storage_status.valid = true;
    g_storage_status.available_bytes = available_bytes;
    g_storage_status.total_bytes = total_bytes;
    g_storage_status.threshold_bytes = threshold_bytes;
    g_storage_status.low = available_bytes <= threshold_bytes;
    return true;
}

static bool event_store_clear(EventStore *store) {
    if (store->fd < 0) {
        return false;
    }
    if (ftruncate(store->fd, 0) != 0) {
        return false;
    }
    if (lseek(store->fd, 0, SEEK_SET) < 0) {
        return false;
    }
    store->count = 0;
    store->next_id = 0;
    store->reported_recursive_write_ignore = false;
    return true;
}

static bool write_all_fd(int fd, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            errno = ENOSPC;
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

static bool read_full_at(int fd, void *data, size_t len, off_t offset) {
    unsigned char *bytes = (unsigned char *)data;
    while (len > 0) {
        ssize_t bytes_read = pread(fd, bytes, len, offset);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_read == 0) {
            return false;
        }
        bytes += bytes_read;
        len -= (size_t)bytes_read;
        offset += bytes_read;
    }
    return true;
}

static bool event_file_offset(size_t index, off_t *offset_out) {
    if (index > (size_t)(LLONG_MAX / (off_t)sizeof(TraceEvent))) {
        errno = EOVERFLOW;
        return false;
    }
    *offset_out = (off_t)(index * sizeof(TraceEvent));
    return true;
}

static bool read_event_at_chronological_index(size_t index, TraceEvent *event) {
    if (index >= g_events.count || g_events.fd < 0) {
        return false;
    }
    off_t offset = 0;
    return event_file_offset(index, &offset) &&
           read_full_at(g_events.fd, event, sizeof(*event), offset);
}

static void add_event(const char *type, const ProcessInfo *process, const char *path, const char *detail) {
    if (g_events.fd < 0) {
        return;
    }

    TraceEvent event;
    memset(&event, 0, sizeof(event));
    event.id = ++g_events.next_id;
    event.timestamp = current_time_seconds();
    event.pid = process ? process->pid : 0;
    event.ppid = process ? process->ppid : 0;
    snprintf(event.type, sizeof(event.type), "%s", type);
    snprintf(event.process, sizeof(event.process), "%s", process ? process->name : "trace");
    snprintf(event.path, sizeof(event.path), "%s", path ? path : "");
    snprintf(event.detail, sizeof(event.detail), "%s", detail);

    if (write_all_fd(g_events.fd, &event, sizeof(event))) {
        g_events.count++;
    } else {
        int saved_errno = errno;
        fprintf(stderr, "TraceBackend failed to append event: %s\n", strerror(errno));
        if (saved_errno == ENOSPC) {
            g_capture_paused = true;
            g_capture_pause_reason = CAPTURE_PAUSE_REASON_LOW_STORAGE;
            if (g_capture_uses_ebpf) {
                set_ebpf_capture_enabled(&g_ebpf_capture, false);
            }
            refresh_storage_status();
        }
    }
}

static void update_process_events(void) {
    ProcessSnapshot current = {0};
    if (!capture_process_snapshot(&current)) {
        ProcessInfo self = {.pid = 0, .ppid = 0};
        snprintf(self.name, sizeof(self.name), "trace");
        add_event("capture.error", &self, "", "Unable to read the process table");
        return;
    }

    if (!g_has_previous_snapshot) {
        ProcessInfo self = {.pid = 0, .ppid = 0};
        snprintf(self.name, sizeof(self.name), "trace");
        add_event("capture.start", &self, "", "Started process event capture");
        for (size_t i = 0; i < current.count; ++i) {
            char detail[256];
            snprintf(detail, sizeof(detail), "Process present at capture start: %s", current.items[i].name);
            add_event("process.present", &current.items[i], "", detail);
        }
        g_previous_snapshot = current;
        g_has_previous_snapshot = true;
        return;
    }

    size_t old_index = 0;
    size_t new_index = 0;
    while (old_index < g_previous_snapshot.count || new_index < current.count) {
        if (old_index >= g_previous_snapshot.count) {
            char detail[256];
            snprintf(detail, sizeof(detail), "Started %s", current.items[new_index].name);
            add_event("process.start", &current.items[new_index], "", detail);
            new_index++;
            continue;
        }
        if (new_index >= current.count) {
            char detail[256];
            snprintf(detail, sizeof(detail), "Exited %s", g_previous_snapshot.items[old_index].name);
            add_event("process.exit", &g_previous_snapshot.items[old_index], "", detail);
            fd_path_remove_pid(g_previous_snapshot.items[old_index].pid);
            old_index++;
            continue;
        }

        int old_pid = g_previous_snapshot.items[old_index].pid;
        int new_pid = current.items[new_index].pid;
        if (old_pid == new_pid) {
            old_index++;
            new_index++;
        } else if (old_pid < new_pid) {
            char detail[256];
            snprintf(detail, sizeof(detail), "Exited %s", g_previous_snapshot.items[old_index].name);
            add_event("process.exit", &g_previous_snapshot.items[old_index], "", detail);
            fd_path_remove_pid(g_previous_snapshot.items[old_index].pid);
            old_index++;
        } else {
            char detail[256];
            snprintf(detail, sizeof(detail), "Started %s", current.items[new_index].name);
            add_event("process.start", &current.items[new_index], "", detail);
            new_index++;
        }
    }

    free_process_snapshot(&g_previous_snapshot);
    g_previous_snapshot = current;
}

static bool refresh_process_baseline(void) {
    ProcessSnapshot current = {0};
    if (!capture_process_snapshot(&current)) {
        return false;
    }
    free_process_snapshot(&g_previous_snapshot);
    g_previous_snapshot = current;
    g_has_previous_snapshot = true;
    return true;
}

static void format_bytes_compact(uint64_t bytes, char *buffer, size_t buffer_size) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit_index++;
    }
    if (unit_index == 0) {
        snprintf(buffer, buffer_size, "%" PRIu64 " %s", bytes, units[unit_index]);
    } else {
        snprintf(buffer, buffer_size, "%.1f %s", value, units[unit_index]);
    }
}

static void set_capture_paused_internal(bool paused,
                                        CapturePauseReason reason,
                                        const char *detail) {
    if (g_capture_paused == paused && g_capture_pause_reason == reason) {
        return;
    }

    ProcessInfo self = {.pid = 0, .ppid = 0};
    snprintf(self.name, sizeof(self.name), "trace");

    if (paused) {
        add_event("capture.pause", &self, "", detail ? detail : "Paused event capture");
        if (g_capture_uses_ebpf) {
            set_ebpf_capture_enabled(&g_ebpf_capture, false);
        }
        g_capture_paused = true;
        g_capture_pause_reason = reason;
        return;
    }

    if (g_capture_uses_ebpf) {
        discard_ebpf_ring_backlog(&g_ebpf_capture);
        set_ebpf_capture_enabled(&g_ebpf_capture, true);
    } else {
        refresh_process_baseline();
    }
    g_capture_paused = false;
    g_capture_pause_reason = CAPTURE_PAUSE_REASON_NONE;
    add_event("capture.resume", &self, "", "Resumed event capture");
}

static void pause_capture_for_low_storage(void) {
    char available[64];
    char threshold[64];
    format_bytes_compact(g_storage_status.available_bytes, available, sizeof(available));
    format_bytes_compact(g_storage_status.threshold_bytes, threshold, sizeof(threshold));
    char detail[256];
    snprintf(detail, sizeof(detail),
             "Paused event capture because available storage is low: %s available, threshold %s",
             available, threshold);
    set_capture_paused_internal(true, CAPTURE_PAUSE_REASON_LOW_STORAGE, detail);
}

static void update_storage_pause_state(bool force) {
    double now = current_time_seconds();
    if (!force && now - g_last_storage_check_time < STORAGE_CHECK_INTERVAL_SECONDS) {
        return;
    }
    g_last_storage_check_time = now;

    if (!refresh_storage_status()) {
        return;
    }
    if (g_storage_status.low) {
        pause_capture_for_low_storage();
    }
}

static void set_capture_paused(bool paused) {
    if (paused) {
        set_capture_paused_internal(true, CAPTURE_PAUSE_REASON_USER, "Paused event capture");
        return;
    }

    update_storage_pause_state(true);
    if (g_storage_status.valid && g_storage_status.low) {
        pause_capture_for_low_storage();
        return;
    }

    set_capture_paused_internal(false, CAPTURE_PAUSE_REASON_NONE, NULL);
}

static bool clear_capture_log(void) {
    if (g_capture_uses_ebpf) {
        discard_ebpf_ring_backlog(&g_ebpf_capture);
    }
    free(g_fd_paths.items);
    g_fd_paths.items = NULL;
    g_fd_paths.count = 0;
    pending_open_table_free(&g_pending_opens);
    process_parent_table_free(&g_process_parents);

    if (!event_store_clear(&g_events)) {
        return false;
    }
    if (!g_capture_uses_ebpf) {
        refresh_process_baseline();
    }
    return true;
}

static void format_event_time(double timestamp, char *buffer, size_t buffer_size) {
    time_t seconds = (time_t)timestamp;
    struct tm local_time;
    localtime_r(&seconds, &local_time);
    int millis = (int)((timestamp - (double)seconds) * 1000.0);
    if (millis < 0) {
        millis = 0;
    }
    snprintf(buffer, buffer_size, "%02d:%02d:%02d.%03d",
             local_time.tm_hour, local_time.tm_min, local_time.tm_sec, millis);
}

static void write_uint16_le(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
}

static void write_uint32_le(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
}

static void write_uint64_le(unsigned char *bytes, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes[i] = (unsigned char)((value >> (8 * i)) & 0xff);
    }
}

static void write_double_le(unsigned char *bytes, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    write_uint64_le(bytes, bits);
}

static bool append_string_ref(unsigned char *buffer, size_t cap, size_t *variable_offset,
                              size_t ref_offset, const char *string) {
    size_t length = strlen(string);
    if (*variable_offset > UINT32_MAX || length > UINT32_MAX ||
        length > cap - *variable_offset) {
        return false;
    }
    write_uint32_le(buffer + ref_offset, (uint32_t)*variable_offset);
    write_uint32_le(buffer + ref_offset + 4, (uint32_t)length);
    memcpy(buffer + *variable_offset, string, length);
    *variable_offset += length;
    return true;
}

static bool ascii_contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *cursor = haystack; *cursor; ++cursor) {
        size_t i = 0;
        while (i < needle_len &&
               cursor[i] &&
               tolower((unsigned char)cursor[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool ascii_equals_case_insensitive(const char *left, const char *right) {
    if (!left || !right) {
        return false;
    }
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static const char *event_value_for_filter_column(const TraceEvent *event,
                                                 const char *column,
                                                 char *scratch,
                                                 size_t scratch_size) {
    if (strcmp(column, "event") == 0) {
        return event->type;
    }
    if (strcmp(column, "pid") == 0) {
        snprintf(scratch, scratch_size, "%d", event->pid);
        return scratch;
    }
    if (strcmp(column, "ppid") == 0) {
        snprintf(scratch, scratch_size, "%d", event->ppid);
        return scratch;
    }
    if (strcmp(column, "process") == 0) {
        return event->process;
    }
    if (strcmp(column, "path") == 0) {
        return event->path;
    }
    if (strcmp(column, "detail") == 0) {
        return event->detail;
    }
    if (strcmp(column, "time") == 0) {
        format_event_time(event->timestamp, scratch, scratch_size);
        return scratch;
    }
    return event->detail;
}

static bool event_matches_filter_clause(const TraceEvent *event, const EventFilterClause *clause) {
    if (clause->value[0] == '\0') {
        return true;
    }

    char scratch[64];
    const char *value = event_value_for_filter_column(event, clause->column, scratch, sizeof(scratch));
    if (strcmp(clause->operation, "equals") == 0) {
        return ascii_equals_case_insensitive(value, clause->value);
    }
    if (strcmp(clause->operation, "notEquals") == 0) {
        return !ascii_equals_case_insensitive(value, clause->value);
    }
    if (strcmp(clause->operation, "excludes") == 0) {
        return !ascii_contains_case_insensitive(value, clause->value);
    }
    return ascii_contains_case_insensitive(value, clause->value);
}

static bool event_matches_filter(const TraceEvent *event, const EventFilter *filter) {
    for (size_t i = 0; i < filter->count; ++i) {
        if (!event_matches_filter_clause(event, &filter->clauses[i])) {
            return false;
        }
    }
    return true;
}

static void free_process_timeline(ProcessTimeline *timeline) {
    free(timeline->items);
    timeline->items = NULL;
    timeline->count = 0;
}

static ProcessTimelineItem *find_process_timeline_item(ProcessTimeline *timeline, int pid) {
    for (size_t i = 0; i < timeline->count; ++i) {
        if (timeline->items[i].pid == pid) {
            return &timeline->items[i];
        }
    }
    return NULL;
}

static bool is_known_process_name(const char *process);

static ProcessTimelineItem *append_process_timeline_item(ProcessTimeline *timeline, const TraceEvent *event) {
    ProcessTimelineItem *next = realloc(timeline->items, sizeof(ProcessTimelineItem) * (timeline->count + 1));
    if (!next) {
        return NULL;
    }
    timeline->items = next;

    ProcessTimelineItem *item = &timeline->items[timeline->count++];
    memset(item, 0, sizeof(*item));
    item->pid = event->pid;
    item->ppid = event->ppid;
    snprintf(item->process, sizeof(item->process), "%s", event->process);
    item->first_timestamp = event->timestamp;
    item->start_timestamp = event->timestamp;
    item->end_timestamp = event->timestamp;
    item->last_timestamp = event->timestamp;
    return item;
}

static ProcessTimelineItem *ensure_process_timeline_parent_item(ProcessTimeline *timeline,
                                                                const TraceEvent *fork_event) {
    if (fork_event->ppid <= 0) {
        return NULL;
    }
    ProcessTimelineItem *parent = find_process_timeline_item(timeline, fork_event->ppid);
    if (parent) {
        if (!is_known_process_name(parent->process) && is_known_process_name(fork_event->process)) {
            snprintf(parent->process, sizeof(parent->process), "%.*s",
                     (int)sizeof(parent->process) - 1,
                     fork_event->process);
        }
        return parent;
    }

    TraceEvent parent_event = *fork_event;
    parent_event.pid = fork_event->ppid;
    parent_event.ppid = 0;
    if (!is_known_process_name(parent_event.process)) {
        snprintf(parent_event.process, sizeof(parent_event.process), "pid-%d", parent_event.pid);
    }
    parent = append_process_timeline_item(timeline, &parent_event);
    if (!parent) {
        return NULL;
    }
    parent->event_count = 0;
    parent->open_at_start = true;
    return parent;
}

static bool is_process_start_event(const TraceEvent *event) {
    return strcmp(event->type, "process.fork") == 0 ||
           strcmp(event->type, "process.start") == 0 ||
           strcmp(event->type, "process.present") == 0;
}

static bool is_process_exit_event(const TraceEvent *event) {
    return strcmp(event->type, "process.exit") == 0;
}

static bool is_known_process_name(const char *process) {
    return process &&
           process[0] != '\0' &&
           strcmp(process, "unknown") != 0 &&
           strcmp(process, "trace") != 0;
}

static const char *path_basename(const char *path) {
    if (!path || path[0] == '\0') {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void update_process_timeline_name(ProcessTimelineItem *item, const TraceEvent *event) {
    if (strcmp(event->type, "process.execve") == 0 && event->path[0] != '\0') {
        snprintf(item->process, sizeof(item->process), "%.*s",
                 (int)sizeof(item->process) - 1,
                 path_basename(event->path));
        return;
    }

    if (strcmp(event->type, "process.fork") != 0 && is_known_process_name(event->process)) {
        snprintf(item->process, sizeof(item->process), "%.*s",
                 (int)sizeof(item->process) - 1,
                 event->process);
        return;
    }

    if (!is_known_process_name(item->process) && is_known_process_name(event->process)) {
        snprintf(item->process, sizeof(item->process), "%.*s",
                 (int)sizeof(item->process) - 1,
                 event->process);
    }
}

static int compare_process_timeline_items(const void *lhs, const void *rhs) {
    const ProcessTimelineItem *a = (const ProcessTimelineItem *)lhs;
    const ProcessTimelineItem *b = (const ProcessTimelineItem *)rhs;
    if (a->start_timestamp < b->start_timestamp) {
        return -1;
    }
    if (a->start_timestamp > b->start_timestamp) {
        return 1;
    }
    return (a->pid > b->pid) - (a->pid < b->pid);
}

static bool build_process_timeline(ProcessTimeline *timeline,
                                   double *capture_start_out,
                                   double *capture_end_out,
                                   uint64_t *event_count_out,
                                   const EventFilter *filter) {
    memset(timeline, 0, sizeof(*timeline));
    *capture_start_out = 0;
    *capture_end_out = 0;
    *event_count_out = 0;

    bool has_any_event = g_events.count > 0;
    if (has_any_event) {
        TraceEvent first_event;
        TraceEvent last_event;
        if (!read_event_at_chronological_index(0, &first_event) ||
            !read_event_at_chronological_index(g_events.count - 1, &last_event)) {
            return false;
        }
        *capture_start_out = first_event.timestamp;
        *capture_end_out = last_event.timestamp;
        if (*capture_end_out < *capture_start_out) {
            *capture_end_out = *capture_start_out;
        }
    }

    for (size_t i = 0; i < g_events.count; ++i) {
        TraceEvent event;
        if (!read_event_at_chronological_index(i, &event)) {
            free_process_timeline(timeline);
            return false;
        }

        if (event.timestamp > *capture_end_out) {
            *capture_end_out = event.timestamp;
        }
        if (event.pid <= 0) {
            continue;
        }
        bool matches_filter = event_matches_filter(&event, filter);
        if (strcmp(event.type, "process.fork") == 0 && event.ppid > 0) {
            if (!ensure_process_timeline_parent_item(timeline, &event)) {
                free_process_timeline(timeline);
                return false;
            }
        }

        ProcessTimelineItem *item = find_process_timeline_item(timeline, event.pid);
        if (!item) {
            item = append_process_timeline_item(timeline, &event);
            if (!item) {
                free_process_timeline(timeline);
                return false;
            }
        }

        if (matches_filter && has_any_event) {
            double duration = *capture_end_out - *capture_start_out;
            size_t bucket = 0;
            if (duration > 0 && PROCESS_TIMELINE_DOT_BUCKET_COUNT > 1) {
                double normalized = (event.timestamp - *capture_start_out) / duration;
                if (normalized < 0) {
                    normalized = 0;
                } else if (normalized > 1) {
                    normalized = 1;
                }
                bucket = (size_t)(normalized * (double)(PROCESS_TIMELINE_DOT_BUCKET_COUNT - 1));
                if (bucket >= PROCESS_TIMELINE_DOT_BUCKET_COUNT) {
                    bucket = PROCESS_TIMELINE_DOT_BUCKET_COUNT - 1;
                }
            }
            if (item->dot_counts[bucket] < PROCESS_TIMELINE_DOT_MAX_COUNT) {
                item->dot_timestamp_sums[bucket] += event.timestamp;
                item->dot_counts[bucket]++;
            }
        }

        if (matches_filter) {
            item->event_count++;
            (*event_count_out)++;
        }
        if (event.timestamp < item->first_timestamp) {
            item->first_timestamp = event.timestamp;
        }
        if (event.timestamp > item->last_timestamp) {
            item->last_timestamp = event.timestamp;
        }
        if (item->ppid <= 0 && event.ppid > 0) {
            item->ppid = event.ppid;
        }
        update_process_timeline_name(item, &event);

        if (is_process_start_event(&event)) {
            if (!item->has_start || event.timestamp < item->start_timestamp) {
                item->start_timestamp = event.timestamp;
            }
            item->has_start = true;
            if (strcmp(event.type, "process.present") == 0) {
                item->open_at_start = true;
            }
        }

        if (is_process_exit_event(&event)) {
            item->has_end = true;
            if (event.timestamp > item->end_timestamp) {
                item->end_timestamp = event.timestamp;
            }
        } else if (!item->has_end) {
            item->end_timestamp = item->last_timestamp;
        }
    }

    if (!has_any_event) {
        *capture_start_out = *capture_end_out;
    }

    for (size_t i = 0; i < timeline->count; ++i) {
        ProcessTimelineItem *item = &timeline->items[i];
        if (!item->has_start) {
            item->start_timestamp = *capture_start_out;
            item->open_at_start = true;
        } else if (item->first_timestamp < item->start_timestamp) {
            item->start_timestamp = item->first_timestamp;
        }
        if (!item->has_end) {
            item->end_timestamp = *capture_end_out;
        } else if (item->last_timestamp > item->end_timestamp) {
            item->end_timestamp = item->last_timestamp;
        }
        if (item->end_timestamp < item->start_timestamp) {
            item->end_timestamp = item->start_timestamp;
        }
    }

    size_t out = 0;
    for (size_t i = 0; i < timeline->count; ++i) {
        if (timeline->items[i].event_count > 0) {
            timeline->items[out++] = timeline->items[i];
        }
    }
    timeline->count = out;

    qsort(timeline->items, timeline->count, sizeof(ProcessTimelineItem), compare_process_timeline_items);
    return true;
}

static size_t build_process_timeline_binary(const EventFilter *filter, unsigned char **buffer_out) {
    ProcessTimeline timeline = {0};
    double capture_start = 0;
    double capture_end = 0;
    uint64_t event_count = 0;
    if (!build_process_timeline(&timeline, &capture_start, &capture_end, &event_count, filter)) {
        return 0;
    }

    size_t dot_count = 0;
    for (size_t item_index = 0; item_index < timeline.count; ++item_index) {
        ProcessTimelineItem *item = &timeline.items[item_index];
        for (size_t bucket = 0; bucket < PROCESS_TIMELINE_DOT_BUCKET_COUNT; ++bucket) {
            if (item->dot_counts[bucket] > 0) {
                dot_count++;
            }
        }
    }

    if (timeline.count > UINT32_MAX || dot_count > UINT32_MAX) {
        free_process_timeline(&timeline);
        return 0;
    }

    size_t fixed_size = PROCESS_TIMELINE_BINARY_HEADER_SIZE +
                        timeline.count * PROCESS_TIMELINE_BINARY_RECORD_SIZE +
                        dot_count * PROCESS_TIMELINE_DOT_RECORD_SIZE;
    size_t variable_size = 0;
    for (size_t i = 0; i < timeline.count; ++i) {
        variable_size += strlen(timeline.items[i].process);
    }
    if (fixed_size > SIZE_MAX - variable_size) {
        free_process_timeline(&timeline);
        return 0;
    }

    size_t total_size = fixed_size + variable_size;
    unsigned char *buffer = calloc(1, total_size == 0 ? 1 : total_size);
    if (!buffer) {
        free_process_timeline(&timeline);
        return 0;
    }

    write_uint32_le(buffer + 0, PROCESS_TIMELINE_BINARY_MAGIC);
    write_uint16_le(buffer + 4, PROCESS_TIMELINE_BINARY_VERSION);
    write_uint16_le(buffer + 6, PROCESS_TIMELINE_BINARY_HEADER_SIZE);
    write_double_le(buffer + 8, capture_start);
    write_double_le(buffer + 16, capture_end);
    write_uint32_le(buffer + 24, (uint32_t)timeline.count);
    write_uint32_le(buffer + 28, PROCESS_TIMELINE_BINARY_RECORD_SIZE);
    write_uint64_le(buffer + 32, event_count);
    write_uint64_le(buffer + 40, (uint64_t)timeline.count);
    write_uint32_le(buffer + 48, (uint32_t)dot_count);
    write_uint32_le(buffer + 52, PROCESS_TIMELINE_DOT_RECORD_SIZE);
    write_uint32_le(buffer + 56, PROCESS_TIMELINE_DOT_BUCKET_COUNT);
    write_uint32_le(buffer + 60, PROCESS_TIMELINE_DOT_MAX_COUNT);

    size_t variable_offset = fixed_size;
    for (size_t i = 0; i < timeline.count; ++i) {
        ProcessTimelineItem *item = &timeline.items[i];
        size_t record_offset = PROCESS_TIMELINE_BINARY_HEADER_SIZE +
                               i * PROCESS_TIMELINE_BINARY_RECORD_SIZE;
        uint32_t flags = 0;
        if (item->open_at_start) {
            flags |= 1u;
        }
        if (!item->has_end) {
            flags |= 2u;
        }

        write_uint32_le(buffer + record_offset + 0, (uint32_t)item->pid);
        write_uint32_le(buffer + record_offset + 4, (uint32_t)item->ppid);
        write_double_le(buffer + record_offset + 8, item->first_timestamp);
        write_double_le(buffer + record_offset + 16, item->start_timestamp);
        write_double_le(buffer + record_offset + 24, item->end_timestamp);
        write_double_le(buffer + record_offset + 32, item->last_timestamp);
        write_uint64_le(buffer + record_offset + 40, item->event_count);
        write_uint32_le(buffer + record_offset + 48, flags);
        if (!append_string_ref(buffer, total_size, &variable_offset, record_offset + 52, item->process)) {
            free(buffer);
            free_process_timeline(&timeline);
            return 0;
        }
    }

    size_t dot_index = 0;
    for (size_t item_index = 0; item_index < timeline.count; ++item_index) {
        ProcessTimelineItem *item = &timeline.items[item_index];
        for (size_t bucket = 0; bucket < PROCESS_TIMELINE_DOT_BUCKET_COUNT; ++bucket) {
            if (item->dot_counts[bucket] == 0) {
                continue;
            }
            size_t record_offset = PROCESS_TIMELINE_BINARY_HEADER_SIZE +
                                   timeline.count * PROCESS_TIMELINE_BINARY_RECORD_SIZE +
                                   dot_index * PROCESS_TIMELINE_DOT_RECORD_SIZE;
            write_uint32_le(buffer + record_offset + 0, (uint32_t)item->pid);
            write_uint32_le(buffer + record_offset + 4, (uint32_t)bucket);
            write_uint32_le(buffer + record_offset + 8, (uint32_t)item->dot_counts[bucket]);
            write_double_le(buffer + record_offset + 12,
                            item->dot_timestamp_sums[bucket] / (double)item->dot_counts[bucket]);
            dot_index++;
        }
    }

    free_process_timeline(&timeline);
    *buffer_out = buffer;
    return total_size;
}

static size_t build_events_binary(size_t start, size_t count, bool tail, const EventFilter *filter, unsigned char **buffer_out) {
    if (count > EVENT_BINARY_MAX_COUNT) {
        count = EVENT_BINARY_MAX_COUNT;
    }

    TraceEvent events[EVENT_BINARY_MAX_COUNT];
    size_t filtered_count = 0;
    size_t event_count = 0;
    size_t response_start = start;
    size_t tail_cursor = 0;

    if (filter->count == 0) {
        filtered_count = g_events.count;
        if (tail) {
            event_count = count < g_events.count ? count : g_events.count;
            response_start = g_events.count > event_count ? g_events.count - event_count : 0;
        } else {
            response_start = start > g_events.count ? g_events.count : start;
            size_t available = g_events.count - response_start;
            event_count = count < available ? count : available;
        }
        size_t output_count = 0;
        for (size_t i = 0; i < event_count; ++i) {
            TraceEvent event;
            if (!read_event_at_chronological_index(response_start + i, &event)) {
                return 0;
            }
            if (event.id != 0) {
                events[output_count++] = event;
            }
        }
        event_count = output_count;
    } else {
        for (size_t i = 0; i < g_events.count; ++i) {
            TraceEvent event;
            if (!read_event_at_chronological_index(i, &event)) {
                return 0;
            }
            if (event.id == 0) {
                continue;
            }
            if (!event_matches_filter(&event, filter)) {
                continue;
            }
            if (tail) {
                if (count > 0) {
                    if (event_count < count) {
                        events[event_count++] = event;
                    } else {
                        events[tail_cursor] = event;
                        tail_cursor = (tail_cursor + 1) % count;
                    }
                }
            } else if (filtered_count >= start && event_count < count) {
                events[event_count++] = event;
            }
            filtered_count++;
        }

        if (tail) {
            response_start = filtered_count > event_count ? filtered_count - event_count : 0;
            if (event_count == count && tail_cursor > 0) {
                TraceEvent ordered[EVENT_BINARY_MAX_COUNT];
                for (size_t i = 0; i < event_count; ++i) {
                    ordered[i] = events[(tail_cursor + i) % event_count];
                }
                memcpy(events, ordered, sizeof(TraceEvent) * event_count);
            }
        } else {
            response_start = start > filtered_count ? filtered_count : start;
        }
    }

    size_t fixed_size = EVENT_BINARY_HEADER_SIZE + event_count * EVENT_BINARY_RECORD_SIZE;
    size_t variable_size = 0;
    for (size_t i = 0; i < event_count; ++i) {
        char time_buffer[32];
        format_event_time(events[i].timestamp, time_buffer, sizeof(time_buffer));
        variable_size += strlen(time_buffer);
        variable_size += strlen(events[i].type);
        variable_size += strlen(events[i].process);
        variable_size += strlen(events[i].path);
        variable_size += strlen(events[i].detail);
    }
    if (fixed_size > SIZE_MAX - variable_size) {
        return 0;
    }
    size_t total_size = fixed_size + variable_size;
    unsigned char *buffer = calloc(1, total_size == 0 ? 1 : total_size);
    if (!buffer) {
        return 0;
    }

    write_uint32_le(buffer + 0, EVENT_BINARY_MAGIC);
    write_uint16_le(buffer + 4, EVENT_BINARY_VERSION);
    write_uint16_le(buffer + 6, EVENT_BINARY_HEADER_SIZE);
    write_uint64_le(buffer + 8, (uint64_t)filtered_count);
    write_uint64_le(buffer + 16, (uint64_t)response_start);
    write_uint32_le(buffer + 24, (uint32_t)event_count);
    write_uint32_le(buffer + 28, EVENT_BINARY_RECORD_SIZE);
    write_uint64_le(buffer + 32, (uint64_t)g_events.count);
    uint32_t capture_flags = 0;
    if (g_capture_paused) {
        capture_flags |= 1u << 0;
    }
    if (g_storage_status.valid) {
        capture_flags |= 1u << 1;
    }
    if (g_storage_status.valid && g_storage_status.low) {
        capture_flags |= 1u << 2;
    }
    write_uint32_le(buffer + 40, capture_flags);
    write_uint32_le(buffer + 44, (uint32_t)g_capture_pause_reason);
    write_uint64_le(buffer + 48, g_storage_status.available_bytes);
    write_uint64_le(buffer + 56, g_storage_status.threshold_bytes);
    write_uint64_le(buffer + 64, g_storage_status.total_bytes);

    size_t variable_offset = fixed_size;
    for (size_t i = 0; i < event_count; ++i) {
        TraceEvent *event = &events[i];
        size_t record_offset = EVENT_BINARY_HEADER_SIZE + i * EVENT_BINARY_RECORD_SIZE;
        char time_buffer[32];
        format_event_time(event->timestamp, time_buffer, sizeof(time_buffer));

        write_uint64_le(buffer + record_offset + 0, event->id);
        write_double_le(buffer + record_offset + 8, event->timestamp);
        write_uint32_le(buffer + record_offset + 16, (uint32_t)event->pid);
        write_uint32_le(buffer + record_offset + 20, (uint32_t)event->ppid);
        if (!append_string_ref(buffer, total_size, &variable_offset, record_offset + 24, time_buffer) ||
            !append_string_ref(buffer, total_size, &variable_offset, record_offset + 32, event->type) ||
            !append_string_ref(buffer, total_size, &variable_offset, record_offset + 40, event->process) ||
            !append_string_ref(buffer, total_size, &variable_offset, record_offset + 48, event->detail) ||
            !append_string_ref(buffer, total_size, &variable_offset, record_offset + 56, event->path)) {
            free(buffer);
            return 0;
        }
    }

    *buffer_out = buffer;
    return total_size;
}

static bool parse_size_query_value(const char *query, const char *name, size_t *out) {
    if (!query || !name || !out) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            errno = 0;
            char *end = NULL;
            unsigned long long value = strtoull(cursor + name_len + 1, &end, 10);
            if (errno == 0 && end && (*end == '\0' || *end == '&')) {
                *out = (size_t)value;
                return true;
            }
        }
        cursor = strchr(cursor, '&');
        if (!cursor) {
            break;
        }
        cursor++;
    }
    return false;
}

static bool parse_double_query_value(const char *query, const char *name, double *out) {
    if (!query || !name || !out) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            errno = 0;
            char *end = NULL;
            double value = strtod(cursor + name_len + 1, &end);
            if (errno == 0 && end && end != cursor + name_len + 1 && (*end == '\0' || *end == '&')) {
                *out = value;
                return true;
            }
        }
        cursor = strchr(cursor, '&');
        if (!cursor) {
            break;
        }
        cursor++;
    }
    return false;
}

static bool parse_bool_query_value(const char *query, const char *name) {
    if (!query || !name) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            const char *value = cursor + name_len + 1;
            size_t value_len = strcspn(value, "&");
            return (value_len == 1 && value[0] == '1') ||
                (value_len == 4 && strncmp(value, "true", 4) == 0) ||
                (value_len == 3 && strncmp(value, "yes", 3) == 0);
        }
        cursor = strchr(cursor, '&');
        if (!cursor) {
            break;
        }
        cursor++;
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode_component(const char *input, size_t input_len, char *output, size_t output_size) {
    if (output_size == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; i < input_len && out + 1 < output_size; ++i) {
        if (input[i] == '+') {
            output[out++] = ' ';
        } else if (input[i] == '%' && i + 2 < input_len) {
            int hi = hex_value(input[i + 1]);
            int lo = hex_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                output[out++] = input[i];
            }
        } else {
            output[out++] = input[i];
        }
    }
    output[out] = '\0';
}

static bool parse_string_query_value(const char *query, const char *name, char *out, size_t out_size) {
    if (!query || !name || !out || out_size == 0) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *part_end = strchr(cursor, '&');
        if (!part_end) {
            part_end = cursor + strlen(cursor);
        }
        if ((size_t)(part_end - cursor) > name_len &&
            strncmp(cursor, name, name_len) == 0 &&
            cursor[name_len] == '=') {
            const char *value = cursor + name_len + 1;
            url_decode_component(value, (size_t)(part_end - value), out, out_size);
            return true;
        }
        cursor = *part_end == '&' ? part_end + 1 : part_end;
    }
    return false;
}

static void append_filter_clause(EventFilter *filter,
                                 const char *column,
                                 const char *operation,
                                 const char *value) {
    if (filter->count >= EVENT_FILTER_MAX_CLAUSES || !value || value[0] == '\0') {
        return;
    }
    EventFilterClause *clause = &filter->clauses[filter->count++];
    snprintf(clause->column, sizeof(clause->column), "%s", column && column[0] ? column : "detail");
    snprintf(clause->operation, sizeof(clause->operation), "%s", operation && operation[0] ? operation : "contains");
    snprintf(clause->value, sizeof(clause->value), "%s", value);
}

static void parse_indexed_filter_clauses(const char *query, EventFilter *filter) {
    size_t count = 0;
    if (!parse_size_query_value(query, "filterCount", &count)) {
        return;
    }
    if (count > EVENT_FILTER_MAX_CLAUSES) {
        count = EVENT_FILTER_MAX_CLAUSES;
    }

    for (size_t i = 0; i < count; ++i) {
        char key[32];
        char column[16] = "detail";
        char operation[16] = "contains";
        char value[EVENT_FILTER_VALUE_LEN] = "";

        snprintf(key, sizeof(key), "f%zucolumn", i);
        parse_string_query_value(query, key, column, sizeof(column));
        snprintf(key, sizeof(key), "f%zuop", i);
        parse_string_query_value(query, key, operation, sizeof(operation));
        snprintf(key, sizeof(key), "f%zuvalue", i);
        parse_string_query_value(query, key, value, sizeof(value));
        append_filter_clause(filter, column, operation, value);
    }
}

static EventFilter parse_event_filter_query(const char *query) {
    EventFilter filter;
    memset(&filter, 0, sizeof(filter));

    parse_indexed_filter_clauses(query, &filter);
    if (filter.count == 0) {
        char value[EVENT_FILTER_VALUE_LEN];
        if (parse_string_query_value(query, "processContains", value, sizeof(value))) {
            append_filter_clause(&filter, "process", "contains", value);
        }
        if (parse_string_query_value(query, "processExclude", value, sizeof(value))) {
            append_filter_clause(&filter, "process", "excludes", value);
        }
        if (parse_string_query_value(query, "detailContains", value, sizeof(value))) {
            append_filter_clause(&filter, "detail", "contains", value);
        }
        if (parse_string_query_value(query, "detailExclude", value, sizeof(value))) {
            append_filter_clause(&filter, "detail", "excludes", value);
        }
    }
    return filter;
}

static bool queue_all(int fd, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (len > 0) {
        ssize_t written = send(fd, bytes, len, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        len -= (size_t)written;
    }
    return true;
}

static void send_response(int fd, int status, const char *status_text, const char *content_type,
                          const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n"
                              "\r\n",
                              status, status_text, content_type, body_len);
    if (header_len > 0 && (size_t)header_len < sizeof(header)) {
        queue_all(fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        queue_all(fd, body, body_len);
    }
}

static void send_text_response(int fd, int status, const char *message) {
    const char *status_text = status == 200 ? "OK" :
                              status == 400 ? "Bad Request" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "Error";
    send_response(fd, status, status_text, "text/plain; charset=utf-8", message, strlen(message));
}

static void send_outer_descriptor(int fd) {
    const char *plugin_json = "{\"eventsAPIPath\":\"/api/events\",\"captureAPIPath\":\"/api/capture\",\"pollIntervalSeconds\":0.75}";
    size_t path_len = strlen(kBundleUrlPath);
    size_t plugin_len = strlen(plugin_json);
    size_t header_len = 40;
    size_t data_offset = header_len + path_len;
    size_t total_len = data_offset + plugin_len;
    unsigned char *payload = malloc(total_len);
    if (!payload) {
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    payload[0] = 'O';
    payload[1] = 'U';
    payload[2] = 'T';
    payload[3] = 'R';
    write_uint32_le(payload + 4, 1);
    write_uint64_le(payload + 8, (uint64_t)header_len);
    write_uint64_le(payload + 16, (uint64_t)path_len);
    write_uint64_le(payload + 24, (uint64_t)data_offset);
    write_uint64_le(payload + 32, (uint64_t)plugin_len);
    memcpy(payload + header_len, kBundleUrlPath, path_len);
    memcpy(payload + data_offset, plugin_json, plugin_len);

    send_response(fd, 200, "OK", "application/vnd.outerframe", payload, total_len);
    free(payload);
}

static void send_bundle_file(int fd, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[PATH_MAX + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        send_text_response(fd, 404, message);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || st.st_size < 0) {
        close(file_fd);
        send_text_response(fd, 500, "failed to stat bundle\n");
        return;
    }

    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size);
    if (!data) {
        close(file_fd);
        send_text_response(fd, 500, "out of memory\n");
        return;
    }

    size_t total = 0;
    while (total < size) {
        ssize_t n = read(file_fd, data + total, size - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    close(file_fd);

    if (total != size) {
        free(data);
        send_text_response(fd, 500, "failed to read bundle\n");
        return;
    }
    send_response(fd, 200, "OK", "application/octet-stream", data, size);
    free(data);
}

static void send_events_response(int fd, const char *query) {
    size_t start = 0;
    size_t count = 80;
    EventFilter filter = parse_event_filter_query(query);
    parse_size_query_value(query, "start", &start);
    parse_size_query_value(query, "count", &count);
    bool tail = parse_bool_query_value(query, "tail");
    update_storage_pause_state(false);

    unsigned char *payload = NULL;
    size_t len = build_events_binary(start, count, tail, &filter, &payload);
    if (len == 0) {
        send_text_response(fd, 500, "failed to build events response\n");
        return;
    }
    send_response(fd, 200, "OK", "application/vnd.trace.events", payload, len);
    free(payload);
}

static size_t build_event_position_binary(int pid,
                                          bool has_target_time,
                                          double target_time,
                                          bool has_target_bucket,
                                          size_t target_bucket,
                                          size_t target_bucket_count,
                                          double target_range_start,
                                          double target_range_end,
                                          const EventFilter *filter,
                                          unsigned char **buffer_out) {
    unsigned char *buffer = calloc(1, EVENT_POSITION_BINARY_HEADER_SIZE);
    if (!buffer) {
        return 0;
    }

    uint64_t index = UINT64_MAX;
    uint64_t event_id = 0;
    double timestamp = 0;
    uint32_t found = 0;
    size_t filtered_index = 0;
    double best_distance = 0;

    if (pid > 0) {
        for (size_t i = 0; i < g_events.count; ++i) {
            TraceEvent event;
            if (!read_event_at_chronological_index(i, &event)) {
                free(buffer);
                return 0;
            }
            if (!event_matches_filter(&event, filter)) {
                continue;
            }
            if (event.pid == pid) {
                if (has_target_bucket) {
                    double duration = target_range_end - target_range_start;
                    size_t event_bucket = 0;
                    if (duration > 0 && target_bucket_count > 1) {
                        double normalized = (event.timestamp - target_range_start) / duration;
                        if (normalized < 0) {
                            normalized = 0;
                        } else if (normalized > 1) {
                            normalized = 1;
                        }
                        event_bucket = (size_t)(normalized * (double)(target_bucket_count - 1));
                        if (event_bucket >= target_bucket_count) {
                            event_bucket = target_bucket_count - 1;
                        }
                    }
                    if (event_bucket != target_bucket) {
                        filtered_index++;
                        continue;
                    }
                }
                double distance = event.timestamp >= target_time ?
                    event.timestamp - target_time :
                    target_time - event.timestamp;
                if (!has_target_time || !found || distance < best_distance) {
                    index = (uint64_t)filtered_index;
                    event_id = event.id;
                    timestamp = event.timestamp;
                    found = 1;
                    best_distance = distance;
                    if (!has_target_time || distance == 0) {
                        break;
                    }
                }
            }
            filtered_index++;
        }
    }

    write_uint32_le(buffer + 0, EVENT_POSITION_BINARY_MAGIC);
    write_uint16_le(buffer + 4, EVENT_POSITION_BINARY_VERSION);
    write_uint16_le(buffer + 6, EVENT_POSITION_BINARY_HEADER_SIZE);
    write_uint64_le(buffer + 8, index);
    write_uint64_le(buffer + 16, event_id);
    write_double_le(buffer + 24, timestamp);
    write_uint32_le(buffer + 32, found);
    write_uint32_le(buffer + 36, 0);

    *buffer_out = buffer;
    return EVENT_POSITION_BINARY_HEADER_SIZE;
}

static void send_event_position_response(int fd, const char *query) {
    size_t pid_value = 0;
    parse_size_query_value(query, "pid", &pid_value);
    int pid = pid_value <= (size_t)INT_MAX ? (int)pid_value : 0;
    double target_time = 0;
    bool has_target_time = parse_double_query_value(query, "time", &target_time);
    size_t target_bucket = 0;
    size_t target_bucket_count = 0;
    double target_range_start = 0;
    double target_range_end = 0;
    bool has_target_bucket = parse_size_query_value(query, "bucket", &target_bucket) &&
        parse_size_query_value(query, "bucketCount", &target_bucket_count) &&
        parse_double_query_value(query, "rangeStart", &target_range_start) &&
        parse_double_query_value(query, "rangeEnd", &target_range_end) &&
        target_bucket_count > 0 &&
        target_bucket < target_bucket_count &&
        target_range_end >= target_range_start;
    EventFilter filter = parse_event_filter_query(query);

    unsigned char *payload = NULL;
    size_t len = build_event_position_binary(pid,
                                             has_target_time,
                                             target_time,
                                             has_target_bucket,
                                             target_bucket,
                                             target_bucket_count,
                                             target_range_start,
                                             target_range_end,
                                             &filter,
                                             &payload);
    if (len == 0) {
        send_text_response(fd, 500, "failed to build event position response\n");
        return;
    }
    send_response(fd, 200, "OK", "application/vnd.trace.position", payload, len);
    free(payload);
}

static void send_process_timeline_response(int fd, const char *query) {
    EventFilter filter = parse_event_filter_query(query);
    unsigned char *payload = NULL;
    size_t len = build_process_timeline_binary(&filter, &payload);
    if (len == 0) {
        send_text_response(fd, 500, "failed to build process timeline response\n");
        return;
    }
    send_response(fd, 200, "OK", "application/vnd.trace.processes", payload, len);
    free(payload);
}

static size_t build_clear_log_binary(bool cleared, unsigned char **buffer_out) {
    unsigned char *buffer = calloc(1, CLEAR_LOG_BINARY_HEADER_SIZE);
    if (!buffer) {
        return 0;
    }
    write_uint32_le(buffer + 0, CLEAR_LOG_BINARY_MAGIC);
    write_uint16_le(buffer + 4, CLEAR_LOG_BINARY_VERSION);
    write_uint16_le(buffer + 6, CLEAR_LOG_BINARY_HEADER_SIZE);
    write_uint32_le(buffer + 8, cleared ? 1u : 0u);
    write_uint32_le(buffer + 12, 0);
    *buffer_out = buffer;
    return CLEAR_LOG_BINARY_HEADER_SIZE;
}

static void send_clear_log_response(int fd) {
    bool cleared = clear_capture_log();
    unsigned char *payload = NULL;
    size_t len = build_clear_log_binary(cleared, &payload);
    if (len == 0) {
        send_text_response(fd, 500, "failed to build clear response\n");
        return;
    }
    send_response(fd,
                  cleared ? 200 : 500,
                  cleared ? "OK" : "Internal Server Error",
                  "application/vnd.trace.clear",
                  payload,
                  len);
    free(payload);
}

static size_t build_capture_status_binary(unsigned char **buffer_out) {
    unsigned char *buffer = calloc(1, CAPTURE_STATUS_BINARY_HEADER_SIZE);
    if (!buffer) {
        return 0;
    }

    uint32_t capture_flags = 0;
    if (g_capture_paused) {
        capture_flags |= 1u << 0;
    }
    if (g_storage_status.valid) {
        capture_flags |= 1u << 1;
    }
    if (g_storage_status.valid && g_storage_status.low) {
        capture_flags |= 1u << 2;
    }

    write_uint32_le(buffer + 0, CAPTURE_STATUS_BINARY_MAGIC);
    write_uint16_le(buffer + 4, CAPTURE_STATUS_BINARY_VERSION);
    write_uint16_le(buffer + 6, CAPTURE_STATUS_BINARY_HEADER_SIZE);
    write_uint32_le(buffer + 8, capture_flags);
    write_uint32_le(buffer + 12, (uint32_t)g_capture_pause_reason);
    write_uint64_le(buffer + 16, g_storage_status.available_bytes);
    write_uint64_le(buffer + 24, g_storage_status.threshold_bytes);
    write_uint64_le(buffer + 32, g_storage_status.total_bytes);
    write_uint64_le(buffer + 40, 0);

    *buffer_out = buffer;
    return CAPTURE_STATUS_BINARY_HEADER_SIZE;
}

static void send_capture_response(int fd, const char *query) {
    char value[16];
    if (parse_string_query_value(query, "paused", value, sizeof(value))) {
        bool paused = strcmp(value, "1") == 0 ||
                      strcasecmp(value, "true") == 0 ||
                      strcasecmp(value, "yes") == 0;
        set_capture_paused(paused);
    }
    update_storage_pause_state(true);

    unsigned char *payload = NULL;
    size_t len = build_capture_status_binary(&payload);
    if (len == 0) {
        send_text_response(fd, 500, "failed to build capture status response\n");
        return;
    }
    send_response(fd, 200, "OK", "application/vnd.trace.capture", payload, len);
    free(payload);
}

static void handle_client(int fd) {
    struct pollfd client_poll = {.fd = fd, .events = POLLIN, .revents = 0};
    int ready = poll(&client_poll, 1, 1000);
    if (ready <= 0 || !(client_poll.revents & POLLIN)) {
        return;
    }

    char request[READ_BUFFER_SIZE];
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        return;
    }
    request[n] = '\0';

    char method[16];
    char target[1024];
    char version[16];
    if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3) {
        send_text_response(fd, 400, "bad request\n");
        return;
    }
    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "HEAD") != 0) {
        send_text_response(fd, 400, "unsupported method\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(target, "/") == 0 || strcmp(target, "/trace.outer") == 0) {
        send_outer_descriptor(fd);
    } else if (strcmp(target, kBundleUrlPath) == 0) {
        send_text_response(fd, 200, "macos-arm\nmacos-x86\n");
    } else if (strcmp(target, kBundleUrlPathMacosArm) == 0) {
        const char *path = g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
        send_bundle_file(fd, path);
    } else if (strcmp(target, kBundleUrlPathMacosX86) == 0) {
        const char *path = g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
        send_bundle_file(fd, path);
    } else if (strcmp(target, "/api/events") == 0) {
        send_events_response(fd, query);
    } else if (strcmp(target, "/api/event-position") == 0) {
        send_event_position_response(fd, query);
    } else if (strcmp(target, "/api/processes") == 0) {
        send_process_timeline_response(fd, query);
    } else if (strcmp(target, "/api/clear") == 0) {
        send_clear_log_response(fd);
    } else if (strcmp(target, "/api/capture") == 0) {
        send_capture_response(fd, query);
    } else {
        send_text_response(fd, 404, "not found\n");
    }
}

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int create_unix_listener(const char *socket_path) {
    if (!socket_path || !socket_path[0]) {
        fprintf(stderr, "socket path is required\n");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "socket path is too long: %s\n", socket_path);
        return -1;
    }

    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", socket_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (directory[0] && mkdir(directory, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "failed to create socket directory %s: %s\n", directory, strerror(errno));
            return -1;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    unlink(socket_path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (chmod(socket_path, 0600) != 0) {
        perror("chmod");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        perror("listen");
        close(fd);
        unlink(socket_path);
        return -1;
    }
    snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
    return fd;
}

static int systemd_activated_listener(void) {
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (!listen_pid || !listen_fds) {
        return -1;
    }
    char *end = NULL;
    long pid = strtol(listen_pid, &end, 10);
    if (!end || *end != '\0' || pid != (long)getpid()) {
        return -1;
    }
    end = NULL;
    long fds = strtol(listen_fds, &end, 10);
    if (!end || *end != '\0' || fds < 1) {
        return -1;
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return 3;
}

static bool parse_port(const char *value, int *port_out) {
    char *end = NULL;
    errno = 0;
    long port = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || port <= 0 || port > 65535) {
        return false;
    }
    *port_out = (int)port;
    return true;
}

static bool parse_capture_mode(const char *value, CaptureMode *mode_out) {
    if (strcmp(value, "auto") == 0) {
        *mode_out = CAPTURE_MODE_AUTO;
        return true;
    }
    if (strcmp(value, "proc") == 0) {
        *mode_out = CAPTURE_MODE_PROC;
        return true;
    }
    if (strcmp(value, "ebpf") == 0) {
        *mode_out = CAPTURE_MODE_EBPF;
        return true;
    }
    return false;
}

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--port PORT | --socket-path PATH] [--bundles-dir DIR] [--capture auto|proc|ebpf]\n", program);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    int port = DEFAULT_PORT;
    bool use_port = true;
    char socket_path[PATH_MAX] = "";
    CaptureMode capture_mode = CAPTURE_MODE_AUTO;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &port)) {
                print_usage(argv[0]);
                return 2;
            }
            use_port = true;
            socket_path[0] = '\0';
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            snprintf(socket_path, sizeof(socket_path), "%s", argv[++i]);
            use_port = false;
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            const char *dir = argv[++i];
            snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
                     "%s/TraceContent.bundle.macos-arm.aar", dir);
            snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
                     "%s/TraceContent.bundle.macos-x86.aar", dir);
        } else if ((strcmp(argv[i], "--label") == 0 || strcmp(argv[i], "--icon-file") == 0) && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            if (!parse_capture_mode(argv[++i], &capture_mode)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!event_store_init(&g_events)) {
        perror("event store");
        return 1;
    }

    bool using_ebpf = false;
    if (capture_mode != CAPTURE_MODE_PROC) {
        char ebpf_error[1024];
        ebpf_error[0] = '\0';
        using_ebpf = start_ebpf_capture(&g_ebpf_capture, ebpf_error, sizeof(ebpf_error));
        if (!using_ebpf) {
            if (capture_mode == CAPTURE_MODE_EBPF) {
                fprintf(stderr, "TraceBackend eBPF capture failed: %s\n", ebpf_error);
                stop_ebpf_capture(&g_ebpf_capture);
                event_store_close(&g_events);
                fd_path_table_free(&g_fd_paths);
                pending_open_table_free(&g_pending_opens);
                process_parent_table_free(&g_process_parents);
                return 1;
            }
            ProcessInfo self = {.pid = 0, .ppid = 0};
            snprintf(self.name, sizeof(self.name), "trace");
            char detail[512];
            snprintf(detail, sizeof(detail), "eBPF unavailable, using /proc polling: %s", ebpf_error);
            add_event("capture.warning", &self, "", detail);
        }
    }

    if (using_ebpf) {
        g_capture_uses_ebpf = true;
        ProcessInfo self = {.pid = 0, .ppid = 0};
        snprintf(self.name, sizeof(self.name), "trace");
        add_event("capture.start", &self, "", "Started eBPF process capture on sched_process_fork and sched_process_exit");
    } else {
        g_capture_uses_ebpf = false;
        update_process_events();
    }

    bool using_socket_activation = false;
    int listen_fd = !use_port ? systemd_activated_listener() : -1;
    if (listen_fd >= 0) {
        using_socket_activation = true;
        if (socket_path[0]) {
            snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", socket_path);
        }
    } else {
        listen_fd = use_port ? create_listener(port) : create_unix_listener(socket_path);
    }
    if (listen_fd < 0) {
        stop_ebpf_capture(&g_ebpf_capture);
        event_store_close(&g_events);
        fd_path_table_free(&g_fd_paths);
        pending_open_table_free(&g_pending_opens);
        process_parent_table_free(&g_process_parents);
        return 1;
    }

    if (use_port) {
        printf("FirehoseBackend listening on http://127.0.0.1:%d/ (%s capture)\n",
               port, using_ebpf ? "eBPF" : "proc");
    } else {
        printf("FirehoseBackend listening on %s/ (%s capture)\n",
               socket_path, using_ebpf ? "eBPF" : "proc");
    }
    fflush(stdout);

    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN, .revents = 0};
    while (!g_shutdown_requested) {
        int ready = poll(&pfd, 1, SAMPLE_INTERVAL_MS);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            int client = accept(listen_fd, NULL, NULL);
            if (client >= 0) {
                handle_client(client);
                close(client);
            }
        } else if (ready < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        update_storage_pause_state(false);
        if (g_capture_paused) {
            continue;
        }
        if (using_ebpf) {
            consume_ebpf_events(&g_ebpf_capture);
        } else {
            update_process_events();
        }
    }

    close(listen_fd);
    if (!use_port && g_listen_socket_path[0] && !using_socket_activation) {
        unlink(g_listen_socket_path);
    }
    stop_ebpf_capture(&g_ebpf_capture);
    event_store_close(&g_events);
    free_process_snapshot(&g_previous_snapshot);
    fd_path_table_free(&g_fd_paths);
    pending_open_table_free(&g_pending_opens);
    process_parent_table_free(&g_process_parents);
    return 0;
}
