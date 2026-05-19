#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <os/log.h>
#endif

#ifdef __linux__
#include <linux/limits.h>
#endif

#define MAX_REQUEST_SIZE 8192
#define JSON_BUFFER_SIZE (512 * 1024)
#define STREAM_INTERVAL_SEC 1
#define STREAM_BUFFER_SIZE (1024 * 1024)
#define MAX_DESCRIPTOR_LEN 32
#define MAX_TYPE_LEN 32
#define MAX_OPEN_NAME_LEN 1024
#define MAX_CLIENT_ID_LEN 64
#define MAX_SEARCH_TERM_LEN 256
#define MAX_QUERY_VALUE_LEN 768
#define MAX_PROCESS_USER_LEN 64
#define MAX_PROCESS_COMMAND_LEN 256
#define DEFAULT_WINDOW_SIZE 40
#define DEFAULT_TIME_WINDOW_SEC 10.0
#define MIN_TIME_WINDOW_SEC 0.5
#define MAX_TIME_WINDOW_SEC 120.0
#define MAX_SNAPSHOT_HISTORY 180
#define SELECTION_FLAG_HAS_WINDOW 0x1
#define SELECTION_FLAG_TRAILING 0x2
#define LINUX_PF_KTHREAD 0x00200000u

static const char *kBundleUrlPath = "/bundles/TopContent";
static const char *kBundleUrlPathMacosArm = "/bundles/TopContent/macos-arm";
static const char *kBundleUrlPathMacosX86 = "/bundles/TopContent/macos-x86";
static const char *kBundleFilePathMacosArm = "bundles/TopContent.bundle.macos-arm.aar";
static const char *kBundleFilePathMacosX86 = "bundles/TopContent.bundle.macos-x86.aar";
static char g_bundle_file_path_macos_arm[4096] = "";
static char g_bundle_file_path_macos_x86[4096] = "";

typedef struct ProcessIdentity {
    int pid;
    size_t ref_count;
    bool is_kernel_thread;
    char user[MAX_PROCESS_USER_LEN];
    char command[MAX_PROCESS_COMMAND_LEN];
} ProcessIdentity;

typedef struct {
    long memory_kb;
    uint64_t total_cpu_time_ticks;
    double sample_cpu_percent;
    long sample_cpu_time_ms;
    int slot;
    uint32_t slot_generation;
    ProcessIdentity *identity;
} ProcessRecord;

// This struct exists mainly because we compute all of the aggregate numbers,
// then use them for sorting. Without sorting, we could just compute these
// numbers immediately before putting them in the HTTP response.
typedef struct {
    const ProcessRecord *record;
    double display_cpu_percent;
    long display_cpu_time_ms;
} ProcessViewRow;

typedef struct {
    int pid;
    int parent_pid;
    double cpu_percent;
    long rss_kb;
    long vsz_kb;
    long cpu_time_ms;
    bool is_kernel_thread;
    char user[64];
    char command[256];
    char short_command[128];
    int thread_count;
    char launch_time[64];
    char parent_short_command[128];
} ProcessDetail;

typedef struct {
    char descriptor[MAX_DESCRIPTOR_LEN];
    char type[MAX_TYPE_LEN];
    char name[MAX_OPEN_NAME_LEN];
} OpenFileEntry;

static bool append_open_file_entry(OpenFileEntry **entries,
                                   size_t *count,
                                   size_t *capacity,
                                   const OpenFileEntry *entry) {
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 32 : *capacity;
        if (*capacity != 0) {
            if (new_capacity > SIZE_MAX / 2) {
                return false;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(OpenFileEntry)) {
        return false;
    }
    OpenFileEntry *new_entries = realloc(*entries, sizeof(OpenFileEntry) * new_capacity);
    if (!new_entries) {
        return false;
        }
        *entries = new_entries;
        *capacity = new_capacity;
    }
    (*entries)[(*count)++] = *entry;
    return true;
}

typedef struct {
    bool has_cpu;
    double cpu_user;
    double cpu_system;
    double cpu_idle;
    int process_count;
    int visible_process_count;
    int thread_count;
    int logical_cpu_count;
} SystemMetrics;

typedef enum {
    PROCESS_SORT_NONE = 0,
    PROCESS_SORT_PID,
    PROCESS_SORT_COMMAND,
    PROCESS_SORT_USER,
    PROCESS_SORT_CPU,
    PROCESS_SORT_CPU_TIME,
    PROCESS_SORT_MEMORY
} ProcessSortColumn;

typedef enum {
    TIME_SELECTION_NONE = 0,
    TIME_SELECTION_TRAILING,
    TIME_SELECTION_FIXED
} TimeSelectionMode;

typedef struct {
    TimeSelectionMode mode;
    double start_time;
    double end_time;
    double duration;
} ProcessTimeSelection;

typedef struct {
    ProcessSortColumn sort_column;
    bool has_sort;
    bool sort_ascending;
    size_t window_start;
    size_t window_end;
    bool has_window;
    char search_term[MAX_SEARCH_TERM_LEN];
    uint64_t requested_snapshot;
    bool has_snapshot;
    bool has_time_selection;
    ProcessTimeSelection time_selection;
} ProcessWindowRequest;

typedef struct {
    ProcessSortColumn column;
    bool ascending;
} ProcessSortContext;

typedef struct {
    ProcessRecord *records;
    size_t record_count;
    int total_process_count;
    SystemMetrics metrics;
    double timestamp;
    uint64_t tick_timestamp;
    uint64_t snapshot_index;
    bool valid;
} ProcessSnapshot;

typedef struct {
    ProcessViewRow *entries;
    size_t entry_count;
    int total_process_count;
    SystemMetrics metrics;
    double timestamp;
    uint64_t tick_timestamp;
    uint64_t snapshot_index;
    double selection_start;
    double selection_end;
    bool has_time_window;
    bool selection_is_trailing;
} ProcessSnapshotView;

typedef struct {
    ProcessSortColumn sort_column;
    bool sort_ascending;
    bool has_time_selection;
    ProcessTimeSelection time_selection;
    char search_term[MAX_SEARCH_TERM_LEN];
} ProcessOrderContext;

typedef struct {
    int slot;
    uint32_t generation;
    int index;
} ProcessOrderCacheEntry;

typedef struct {
    bool valid;
    ProcessOrderContext context;
    int *slot_indices;
    uint32_t *slot_generations;
    size_t slot_capacity;
    ProcessOrderCacheEntry *overflow_entries;
    size_t overflow_count;
    size_t overflow_capacity;
} ProcessOrderCache;

typedef struct {
    double timestamp;
    bool has_cpu;
    double cpu_user;
    double cpu_system;
    double cpu_idle;
} CpuMetricHistoryEntry;

static bool has_previous_cpu_sample = false;
static ProcessSnapshot current_snapshot = {0};
static ProcessSnapshot snapshot_history[MAX_SNAPSHOT_HISTORY] = {0};
static size_t snapshot_history_start = 0;
static size_t snapshot_history_count = 0;

typedef struct {
    int slot;
    uint32_t generation;
    int pid;
    uint64_t total_time_ticks;
} CpuHistorySample;

typedef struct {
    bool active;
    int pid;
    uint32_t generation;
    ProcessIdentity *identity;
} ProcessSlot;

static CpuHistorySample *cpu_history = NULL;
static size_t cpu_history_count = 0;
static uint64_t cpu_history_ticks = 0;
static bool timebase_initialized = false;
#ifdef __APPLE__
static mach_timebase_info_data_t timebase_info = {0};
static os_log_t g_os_log = NULL;
#endif
static int logical_cpu_count_cached = 0;
static ProcessSlot *process_slots = NULL;
static size_t process_slot_count = 0;
static size_t process_slot_capacity = 0;

typedef enum {
    CONN_STATE_READING_REQUEST,
    CONN_STATE_STREAMING,
    CONN_STATE_WRITING_RESPONSE,
    CONN_STATE_CLOSING
} ConnectionState;

#define WRITE_BUFFER_INITIAL_SIZE 4096

typedef struct StreamClient {
    int fd;
    ConnectionState state;
    char client_id[MAX_CLIENT_ID_LEN];

    ProcessWindowRequest request;
    ProcessOrderCache order_cache;

    bool *sent_rows;
    size_t sent_rows_capacity;
    size_t sent_rows_count;
    bool sent_rows_valid;
    uint64_t sent_snapshot_index;
    ProcessOrderContext last_sent_context;
    bool has_last_sent_context;

    char read_buffer[MAX_REQUEST_SIZE];
    size_t read_buffer_used;
    unsigned char *write_buffer;
    size_t write_buffer_capacity;
    size_t write_buffer_used;
    size_t write_buffer_sent;
    bool close_after_write;

    unsigned char *frame_buffer;

    bool needs_immediate_frame;
} StreamClient;

typedef struct {
    bool exists;
    StreamClient client;
} ClientSlot;

typedef struct {
    ClientSlot *slots;
    size_t count;
    size_t capacity;
} ClientRegistry;

static ClientRegistry g_clients = {0};

typedef struct {
    struct pollfd *fds;
    size_t *client_indices;
    size_t capacity;
    size_t count;
    int listen_fd_index;
} PollState;

static PollState g_poll = {0};

static ProcessSortContext current_sort_context = {
    .column = PROCESS_SORT_CPU,
    .ascending = false
};

static bool refresh_process_snapshot(void);
static void free_process_snapshot(ProcessSnapshotView *view);
static size_t encode_process_stream_frame(unsigned char *buffer,
                                          size_t cap,
                                          const ProcessSnapshotView *view,
                                          const ProcessWindowRequest *request,
                                          ProcessOrderCache *order_cache);
static bool read_process_short_command(int pid, char *out, size_t out_size);
static void resolve_username(uid_t uid, char *out, size_t out_size);
static CpuHistorySample *copy_cpu_history(size_t *count_out, uint64_t *ticks_out);
static void update_cpu_history(const ProcessRecord *records, size_t count, uint64_t timestamp_ticks);
static double compute_cpu_percent(int slot,
                                  uint32_t generation,
                                  pid_t pid,
                                  uint64_t total_time_units,
                                  const CpuHistorySample *history,
                                  size_t history_count,
                                  uint64_t elapsed_ticks);
static ProcessSnapshot *append_snapshot_history(const ProcessSnapshot *snapshot);
static ProcessSnapshot *history_entry_at(size_t order_index);
static size_t copy_cpu_metric_history(CpuMetricHistoryEntry **out_entries);
static size_t build_cpu_history_json(char *buffer, size_t cap);
static bool build_process_snapshot_view(ProcessSnapshotView *view, const ProcessWindowRequest *request);
static bool build_time_window_snapshot(ProcessSnapshotView *view, const ProcessWindowRequest *request);
static int find_active_process_slot(int pid);
static int allocate_process_slot(int pid);
static void release_inactive_process_slots(const bool *slot_seen, size_t seen_count);
static int collect_process_records(ProcessRecord **records_out,
                                   int *total_count,
                                   int *thread_count_out,
                                   double *sample_time_out,
                                   uint64_t *sample_ticks_out);
static SystemMetrics collect_system_metrics(int default_process_count,
                                            int default_visible_process_count,
                                            int default_thread_count);
static void reset_order_cache(ProcessOrderCache *cache);
static ProcessOrderContext make_order_context(const ProcessWindowRequest *request);
static bool order_context_equal(const ProcessOrderContext *a, const ProcessOrderContext *b);
static int lookup_previous_index(const ProcessOrderCache *cache, int slot, uint32_t generation);
static void update_order_cache(ProcessOrderCache *cache,
                               const ProcessViewRow *entries,
                               size_t count,
                               const ProcessOrderContext *context);

static void log_debug(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[TopBackend] %s\n", buf);
#ifdef __APPLE__
    if (g_os_log) {
        os_log(g_os_log, "%{public}s", buf);
    }
#endif
}

static double current_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t time_window_snapshot_index(double selection_start,
                                           double selection_end,
                                           uint64_t fallback) {
    double center = (selection_start + selection_end) * 0.5;
    uint64_t selection_seconds = (uint64_t)llround(center);
    uint64_t upper = selection_seconds & 0xffffffffULL;
    uint64_t lower = fallback & 0xffffffffULL;
    uint64_t combined = (upper << 32) | lower;
    if (combined == 0) {
        return fallback != 0 ? fallback : 1;
    }
    return combined;
}

static int logical_cpu_count(void) {
    if (logical_cpu_count_cached > 0) {
        return logical_cpu_count_cached;
    }
#ifdef __APPLE__
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname("hw.logicalcpu", &value, &size, NULL, 0) == 0 && value > 0) {
        logical_cpu_count_cached = value;
        return value;
    }
#endif
    long sysconf_value = sysconf(_SC_NPROCESSORS_ONLN);
    if (sysconf_value > 0) {
        logical_cpu_count_cached = (int)sysconf_value;
        return logical_cpu_count_cached;
    }
    logical_cpu_count_cached = 1;
    return 1;
}

static ProcessIdentity *create_process_identity(int pid,
                                                bool is_kernel_thread,
                                                const char *user,
                                                const char *command) {
    ProcessIdentity *identity = malloc(sizeof(ProcessIdentity));
    if (!identity) {
        return NULL;
    }
    memset(identity, 0, sizeof(*identity));
    identity->pid = pid;
    identity->ref_count = 1;
    identity->is_kernel_thread = is_kernel_thread;
    if (user) {
        strncpy(identity->user, user, sizeof(identity->user) - 1);
        identity->user[sizeof(identity->user) - 1] = '\0';
    }
    if (command) {
        strncpy(identity->command, command, sizeof(identity->command) - 1);
        identity->command[sizeof(identity->command) - 1] = '\0';
    }
    return identity;
}

static ProcessIdentity *retain_process_identity(ProcessIdentity *identity) {
    if (identity) {
        identity->ref_count++;
    }
    return identity;
}

static void release_process_identity(ProcessIdentity *identity) {
    if (!identity) {
        return;
    }
    if (identity->ref_count > 1) {
        identity->ref_count--;
        return;
    }
    free(identity);
}

static bool ensure_process_slot_capacity(size_t required_capacity) {
    if (required_capacity <= process_slot_capacity) {
        return true;
    }
    size_t new_capacity = process_slot_capacity == 0 ? 256 : process_slot_capacity;
    while (new_capacity < required_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(ProcessSlot)) {
        return false;
    }
    ProcessSlot *new_slots = realloc(process_slots, sizeof(ProcessSlot) * new_capacity);
    if (!new_slots) {
        return false;
    }
    memset(new_slots + process_slot_capacity,
           0,
           sizeof(ProcessSlot) * (new_capacity - process_slot_capacity));
    process_slots = new_slots;
    process_slot_capacity = new_capacity;
    return true;
}

static bool ensure_bool_capacity(bool **values, size_t *capacity, size_t required_capacity) {
    if (required_capacity <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity == 0 ? 256 : *capacity;
    while (new_capacity < required_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(bool)) {
        return false;
    }
    bool *new_values = realloc(*values, sizeof(bool) * new_capacity);
    if (!new_values) {
        return false;
    }
    memset(new_values + *capacity, 0, sizeof(bool) * (new_capacity - *capacity));
    *values = new_values;
    *capacity = new_capacity;
    return true;
}

static bool ensure_process_record_capacity(ProcessRecord **records,
                                           size_t *capacity,
                                           size_t required_capacity) {
    if (required_capacity <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity == 0 ? 256 : *capacity;
    while (new_capacity < required_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(ProcessRecord)) {
        return false;
    }
    ProcessRecord *new_records = realloc(*records, sizeof(ProcessRecord) * new_capacity);
    if (!new_records) {
        return false;
    }
    *records = new_records;
    *capacity = new_capacity;
    return true;
}

static void release_process_record_identities(ProcessRecord *records, size_t count) {
    if (!records) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        release_process_identity(records[i].identity);
        records[i].identity = NULL;
    }
}

static int find_active_process_slot(int pid) {
    for (size_t i = 0; i < process_slot_count; ++i) {
        if (process_slots[i].active && process_slots[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

static int allocate_process_slot(int pid) {
    size_t slot_index = SIZE_MAX;
    for (size_t i = 0; i < process_slot_count; ++i) {
        if (!process_slots[i].active) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == SIZE_MAX) {
        if (!ensure_process_slot_capacity(process_slot_count + 1)) {
            return -1;
        }
        slot_index = process_slot_count++;
        process_slots[slot_index].generation = 0;
    }
    ProcessSlot *slot = &process_slots[slot_index];
    slot->generation += 1;
    if (slot->generation == 0) {
        slot->generation = 1;
    }
    release_process_identity(slot->identity);
    slot->identity = NULL;
    slot->pid = pid;
    slot->active = true;
    return (int)slot_index;
}

static void release_inactive_process_slots(const bool *slot_seen, size_t seen_count) {
    for (size_t i = 0; i < process_slot_count; ++i) {
        bool seen = (slot_seen && i < seen_count) ? slot_seen[i] : false;
        if (process_slots[i].active && !seen) {
            process_slots[i].active = false;
            process_slots[i].pid = 0;
            release_process_identity(process_slots[i].identity);
            process_slots[i].identity = NULL;
        }
    }
}

#ifdef __APPLE__
static void initialize_timebase(void) {
    if (mach_timebase_info(&timebase_info) != KERN_SUCCESS || timebase_info.denom == 0) {
        timebase_info.numer = 1;
        timebase_info.denom = 1;
    }
}

static void ensure_timebase_info(void) {
    if (!timebase_initialized) {
        initialize_timebase();
        timebase_initialized = true;
    }
}

static uint64_t current_time_ticks(void) {
    return mach_absolute_time();
}

static double ticks_to_seconds(uint64_t ticks) {
    ensure_timebase_info();
    long double nanos = (long double)ticks * (long double)timebase_info.numer;
    nanos /= (long double)timebase_info.denom;
    return (double)(nanos / 1000000000.0L);
}
#endif

#ifdef __linux__
static uint64_t current_time_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double ticks_to_seconds(uint64_t ticks) {
    return (double)ticks / 1000000000.0;
}
#endif

static void fatal(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static bool set_blocking(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

static void trim_whitespace(char *str) {
    if (!str) {
        return;
    }
    char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    size_t len = (size_t)(end - start);
    if (start != str && len > 0) {
        memmove(str, start, len);
    } else if (start != str && len == 0) {
        // all whitespace
        *str = '\0';
        return;
    }
    str[len] = '\0';
}

static void extract_short_command(const char *command_line, char *out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!command_line) {
        return;
    }

    char working[512];
    strncpy(working, command_line, sizeof(working) - 1);
    working[sizeof(working) - 1] = '\0';
    trim_whitespace(working);
    size_t length = strlen(working);
    if (length == 0) {
        return;
    }

    if ((working[0] == '"' && working[length - 1] == '"') ||
        (working[0] == '\'' && working[length - 1] == '\'')) {
        if (length >= 2) {
            memmove(working, working + 1, length - 2);
            working[length - 2] = '\0';
        }
    }

    trim_whitespace(working);
    if (working[0] == '\0') {
        return;
    }

    const char *base = working;
    char *last_slash = strrchr(working, '/');
    char *last_backslash = strrchr(working, '\\');
    char *candidate = working;

    if (last_slash && last_backslash) {
        candidate = (last_slash > last_backslash) ? last_slash + 1 : last_backslash + 1;
    } else if (last_slash) {
        candidate = last_slash + 1;
    } else if (last_backslash) {
        candidate = last_backslash + 1;
    }

    trim_whitespace(candidate);

    const char *final = candidate[0] != '\0' ? candidate : base;
    strncpy(out, final, out_cap - 1);
    out[out_cap - 1] = '\0';
}

static void url_decode(const char *input, size_t input_len, char *output, size_t out_size) {
    if (!output || out_size == 0) {
        return;
    }
    size_t write_index = 0;
    if (!input) {
        output[0] = '\0';
        return;
    }
    for (size_t i = 0; i < input_len && write_index + 1 < out_size; ) {
        char c = input[i];
        if (c == '+') {
            output[write_index++] = ' ';
            i++;
            continue;
        }
        if (c == '%' && i + 2 < input_len &&
            isxdigit((unsigned char)input[i + 1]) && isxdigit((unsigned char)input[i + 2])) {
            char hex[3] = { input[i + 1], input[i + 2], '\0' };
            int value = (int)strtol(hex, NULL, 16);
            output[write_index++] = (char)value;
            i += 3;
            continue;
        }
        output[write_index++] = c;
        i++;
    }
    output[write_index] = '\0';
}

static bool copy_query_value(const char *query, const char *key, char *out, size_t out_size) {
    if (!query || !key || !out || out_size == 0) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *cursor = query;
    while (cursor && *cursor) {
        const char *amp = strchr(cursor, '&');
        size_t segment_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
        if (segment_len >= key_len + 1 &&
            strncasecmp(cursor, key, key_len) == 0 &&
            cursor[key_len] == '=') {
            size_t value_len = segment_len - key_len - 1;
            url_decode(cursor + key_len + 1, value_len, out, out_size);
            return true;
        }
        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }
    return false;
}

static bool parse_size_value(const char *value, size_t *out) {
    if (!value || !*value || !out) {
        return false;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long long parsed = strtoull(value, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return false;
    }
    *out = (size_t)parsed;
    return true;
}

static bool parse_uint64_value(const char *value, uint64_t *out) {
    if (!value || !out) {
        return false;
    }
    errno = 0;
    char *endptr = NULL;
    unsigned long long parsed = strtoull(value, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

static bool parse_double_value(const char *value, double *out) {
    if (!value || !out) {
        return false;
    }
    errno = 0;
    char *endptr = NULL;
    double parsed = strtod(value, &endptr);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

static bool parse_bool_value(const char *value, bool *out) {
    if (!value || !out) {
        return false;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static ProcessSortColumn parse_sort_column_value(const char *value) {
    if (!value || !*value) {
        return PROCESS_SORT_NONE;
    }
    if (strcasecmp(value, "pid") == 0) {
        return PROCESS_SORT_PID;
    }
    if (strcasecmp(value, "command") == 0) {
        return PROCESS_SORT_COMMAND;
    }
    if (strcasecmp(value, "user") == 0) {
        return PROCESS_SORT_USER;
    }
    if (strcasecmp(value, "cpu") == 0) {
        return PROCESS_SORT_CPU;
    }
    if (strcasecmp(value, "cputime") == 0 || strcasecmp(value, "cpu_time") == 0) {
        return PROCESS_SORT_CPU_TIME;
    }
    if (strcasecmp(value, "memory") == 0) {
        return PROCESS_SORT_MEMORY;
    }
    return PROCESS_SORT_NONE;
}

static void apply_default_window_request(ProcessWindowRequest *request) {
    if (!request) {
        return;
    }
    request->has_sort = true;
    request->sort_column = PROCESS_SORT_CPU;
    request->sort_ascending = false;
    request->window_start = 0;
    request->window_end = DEFAULT_WINDOW_SIZE;
    request->has_window = true;
    request->search_term[0] = '\0';
    request->requested_snapshot = 0;
    request->has_snapshot = false;
    request->has_time_selection = true;
    request->time_selection.mode = TIME_SELECTION_TRAILING;
    request->time_selection.duration = DEFAULT_TIME_WINDOW_SEC;
    request->time_selection.start_time = 0.0;
    request->time_selection.end_time = 0.0;
}

static double clamp_time_window_duration(double seconds) {
    if (seconds < MIN_TIME_WINDOW_SEC) {
        return MIN_TIME_WINDOW_SEC;
    }
    if (seconds > MAX_TIME_WINDOW_SEC) {
        return MAX_TIME_WINDOW_SEC;
    }
    return seconds;
}

static void normalize_window_request(ProcessWindowRequest *request) {
    if (!request) {
        return;
    }
    if (!request->has_window) {
        request->window_start = 0;
        request->window_end = DEFAULT_WINDOW_SIZE;
        request->has_window = true;
    }
    if (request->window_end <= request->window_start) {
        if (request->window_start > SIZE_MAX - DEFAULT_WINDOW_SIZE) {
            request->window_end = SIZE_MAX;
        } else {
            request->window_end = request->window_start + DEFAULT_WINDOW_SIZE;
        }
    }

    if (request->has_time_selection) {
        if (request->time_selection.mode == TIME_SELECTION_TRAILING) {
            if (request->time_selection.duration <= 0.0) {
                request->time_selection.duration = DEFAULT_TIME_WINDOW_SEC;
            }
            request->time_selection.duration = clamp_time_window_duration(request->time_selection.duration);
        } else if (request->time_selection.mode == TIME_SELECTION_FIXED) {
            if (request->time_selection.end_time <= request->time_selection.start_time) {
                request->has_time_selection = false;
            } else {
                double span_seconds = request->time_selection.end_time - request->time_selection.start_time;
                if (span_seconds > MAX_TIME_WINDOW_SEC) {
                    request->time_selection.end_time = request->time_selection.start_time + MAX_TIME_WINDOW_SEC;
                }
            }
        }
    }
}

static void apply_query_to_window_request(ProcessWindowRequest *request, const char *query) {
    if (!request || !query || *query == '\0') {
        return;
    }

    char buffer[MAX_QUERY_VALUE_LEN];
    if (copy_query_value(query, "start", buffer, sizeof(buffer))) {
        size_t value = 0;
        if (parse_size_value(buffer, &value)) {
            request->window_start = value;
            request->has_window = true;
        }
    }
    if (copy_query_value(query, "end", buffer, sizeof(buffer))) {
        size_t value = 0;
        if (parse_size_value(buffer, &value)) {
            request->window_end = value;
            request->has_window = true;
        }
    }
    if (copy_query_value(query, "count", buffer, sizeof(buffer))) {
        size_t value = 0;
        if (parse_size_value(buffer, &value)) {
            request->window_end = value > SIZE_MAX - request->window_start
                ? SIZE_MAX
                : request->window_start + value;
            request->has_window = true;
        }
    }
    if (copy_query_value(query, "sort", buffer, sizeof(buffer))) {
        ProcessSortColumn column = parse_sort_column_value(buffer);
        if (column != PROCESS_SORT_NONE) {
            request->sort_column = column;
            request->has_sort = true;
        }
    }
    if (copy_query_value(query, "direction", buffer, sizeof(buffer))) {
        bool ascending = true;
        if (parse_bool_value(buffer, &ascending)) {
            request->sort_ascending = ascending;
        } else if (strcasecmp(buffer, "asc") == 0) {
            request->sort_ascending = true;
        } else if (strcasecmp(buffer, "desc") == 0) {
            request->sort_ascending = false;
        }
    }
    if (copy_query_value(query, "search", buffer, sizeof(buffer))) {
        trim_whitespace(buffer);
        strncpy(request->search_term, buffer, sizeof(request->search_term) - 1);
        request->search_term[sizeof(request->search_term) - 1] = '\0';
    }
    if (copy_query_value(query, "snapshot", buffer, sizeof(buffer))) {
        uint64_t snap = 0;
        if (parse_uint64_value(buffer, &snap)) {
            request->requested_snapshot = snap;
            request->has_snapshot = true;
        }
    }

    char time_mode_value[MAX_QUERY_VALUE_LEN];
    if (copy_query_value(query, "timeMode", time_mode_value, sizeof(time_mode_value))) {
        if (strcasecmp(time_mode_value, "none") == 0) {
            request->has_time_selection = false;
            request->time_selection.mode = TIME_SELECTION_NONE;
        } else if (strcasecmp(time_mode_value, "trailing") == 0) {
            request->has_time_selection = true;
            request->time_selection.mode = TIME_SELECTION_TRAILING;
            double duration = DEFAULT_TIME_WINDOW_SEC;
            char duration_value[MAX_QUERY_VALUE_LEN];
            if (copy_query_value(query, "timeDuration", duration_value, sizeof(duration_value))) {
                double parsed = 0.0;
                if (parse_double_value(duration_value, &parsed)) {
                    duration = parsed;
                }
            }
            request->time_selection.duration = clamp_time_window_duration(duration);
        } else if (strcasecmp(time_mode_value, "fixed") == 0) {
            double start_value = 0.0;
            double end_value = 0.0;
            char start_buffer[MAX_QUERY_VALUE_LEN];
            char end_buffer[MAX_QUERY_VALUE_LEN];
            bool has_start = copy_query_value(query, "timeStart", start_buffer, sizeof(start_buffer)) &&
                             parse_double_value(start_buffer, &start_value);
            bool has_end = copy_query_value(query, "timeEnd", end_buffer, sizeof(end_buffer)) &&
                           parse_double_value(end_buffer, &end_value);
            if (has_start && has_end && end_value > start_value) {
                request->has_time_selection = true;
                request->time_selection.mode = TIME_SELECTION_FIXED;
                request->time_selection.start_time = start_value;
                request->time_selection.end_time = end_value;
                double span = end_value - start_value;
                if (span > MAX_TIME_WINDOW_SEC) {
                    request->time_selection.end_time = request->time_selection.start_time + MAX_TIME_WINDOW_SEC;
                }
            } else {
                request->has_time_selection = false;
                request->time_selection.mode = TIME_SELECTION_NONE;
            }
        }
    }

    normalize_window_request(request);
}

#ifdef __APPLE__
static int collect_process_records(ProcessRecord **records_out,
                                   int *total_count,
                                   int *thread_count_out,
                                   double *sample_time_out,
                                   uint64_t *sample_ticks_out) {
    if (!records_out) {
        return -1;
    }
    *records_out = NULL;

    if (total_count) {
        *total_count = 0;
    }

    bool *slot_seen = NULL;
    size_t slot_seen_capacity = 0;
    ProcessRecord *records = NULL;
    size_t record_capacity = 0;

    double sample_time = current_time_seconds();
    uint64_t sample_ticks = current_time_ticks();
    size_t history_count = 0;
    uint64_t previous_ticks = 0;
    CpuHistorySample *history = copy_cpu_history(&history_count, &previous_ticks);
    uint64_t elapsed_ticks = 0;
    if (previous_ticks > 0 && sample_ticks > previous_ticks) {
        elapsed_ticks = sample_ticks - previous_ticks;
    }

    int pid_list_size = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (pid_list_size <= 0) {
        free(slot_seen);
        free(history);
        return -1;
    }
    pid_t *pid_list = malloc((size_t)pid_list_size);
    if (!pid_list) {
        free(slot_seen);
        free(history);
        return -1;
    }

    int bytes_used = proc_listpids(PROC_ALL_PIDS, 0, pid_list, pid_list_size);
    if (bytes_used <= 0) {
        free(records);
        free(slot_seen);
        free(pid_list);
        free(history);
        return -1;
    }

    int pid_count = bytes_used / (int)sizeof(pid_t);
    int used = 0;
    int discovered_count = 0;
    int total_threads = 0;
    for (int i = 0; i < pid_count; ++i) {
        pid_t pid = pid_list[i];
        if (pid <= 0) {
            continue;
        }
        discovered_count++;
        struct proc_taskallinfo info;
        int result = proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &info, sizeof(info));
        if (result != sizeof(info)) {
            continue;
        }

        int slot_index = find_active_process_slot(pid);
        if (slot_index < 0) {
            slot_index = allocate_process_slot(pid);
        }
        if (slot_index < 0) {
            continue;
        }
        if (!ensure_bool_capacity(&slot_seen, &slot_seen_capacity, (size_t)slot_index + 1) ||
            !ensure_process_record_capacity(&records, &record_capacity, (size_t)used + 1)) {
            release_process_record_identities(records, (size_t)used);
            free(records);
            free(slot_seen);
            free(pid_list);
            free(history);
            return -1;
        }

        ProcessSlot *slot = &process_slots[slot_index];
        slot_seen[slot_index] = true;

        ProcessRecord *record = &records[used];
        memset(record, 0, sizeof(*record));
        record->slot = slot_index;
        record->slot_generation = slot->generation;
        record->memory_kb = (long)(info.ptinfo.pti_resident_size / 1024);
        uint64_t total_time_ticks = (uint64_t)info.ptinfo.pti_total_user + (uint64_t)info.ptinfo.pti_total_system;
        record->total_cpu_time_ticks = total_time_ticks;
        record->sample_cpu_time_ms = (long)(ticks_to_seconds(total_time_ticks) * 1000.0);
        record->sample_cpu_percent = compute_cpu_percent(slot_index,
                                                         slot->generation,
                                                         pid,
                                                         total_time_ticks,
                                                         history,
                                                         history_count,
                                                         elapsed_ticks);

        if (!slot->identity) {
            char user[MAX_PROCESS_USER_LEN] = {0};
            char command[MAX_PROCESS_COMMAND_LEN] = {0};
            resolve_username(info.pbsd.pbi_uid, user, sizeof(user));
            if (!read_process_short_command(pid, command, sizeof(command))) {
                strncpy(command, info.pbsd.pbi_comm, sizeof(command) - 1);
                command[sizeof(command) - 1] = '\0';
            }
            slot->identity = create_process_identity(pid, false, user, command);
            if (!slot->identity) {
                release_process_record_identities(records, (size_t)used);
                free(records);
                free(slot_seen);
                free(pid_list);
                free(history);
                return -1;
            }
        }

        record->identity = retain_process_identity(slot->identity);
        slot->active = true;
        slot->pid = pid;

        used++;
        if (info.ptinfo.pti_threadnum > 0) {
            total_threads += (int)info.ptinfo.pti_threadnum;
        }
    }

    free(pid_list);
    free(history);

    if (total_count) {
        *total_count = discovered_count;
    }
    if (thread_count_out) {
        *thread_count_out = total_threads;
    }
    release_inactive_process_slots(slot_seen, slot_seen_capacity);
    update_cpu_history(records, (size_t)used, sample_ticks);
    free(slot_seen);
    if (sample_time_out) {
        *sample_time_out = sample_time;
    }
    if (sample_ticks_out) {
        *sample_ticks_out = sample_ticks;
    }
    *records_out = records;
    return used;
}
#endif

#ifdef __linux__
static bool linux_process_is_kernel_thread_from_stat_flags(unsigned flags) {
    return (flags & LINUX_PF_KTHREAD) != 0;
}

static void linux_read_process_status_metadata(pid_t pid, uid_t *uid_out, bool *is_kernel_thread_out) {
    if (uid_out) {
        *uid_out = 0;
    }
    bool is_kernel_thread = false;

    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
    FILE *status_fp = fopen(status_path, "r");
    if (!status_fp) {
        return;
    }

    char status_line[256];
    while (fgets(status_line, sizeof(status_line), status_fp)) {
        if (uid_out && strncmp(status_line, "Uid:", 4) == 0) {
            unsigned int real_uid;
            if (sscanf(status_line + 4, "%u", &real_uid) == 1) {
                *uid_out = (uid_t)real_uid;
            }
            continue;
        }
        if (is_kernel_thread_out && strncmp(status_line, "Kthread:", 8) == 0) {
            unsigned int raw_is_kernel_thread = 0;
            if (sscanf(status_line + 8, "%u", &raw_is_kernel_thread) == 1) {
                is_kernel_thread = raw_is_kernel_thread != 0;
            }
        }
    }

    fclose(status_fp);
    if (is_kernel_thread_out) {
        *is_kernel_thread_out = is_kernel_thread;
    }
}

static bool linux_read_process_stat(pid_t pid, uid_t *uid_out, unsigned long *utime_out,
                                    unsigned long *stime_out, long *rss_out,
                                    int *num_threads_out, bool *is_kernel_thread_out,
                                    char *comm_out, size_t comm_size) {
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE *fp = fopen(stat_path, "r");
    if (!fp) {
        return false;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Extract comm from between parentheses
    char *start = strchr(line, '(');
    char *end = strrchr(line, ')');
    if (!start || !end || end <= start) {
        return false;
    }

    if (comm_out && comm_size > 0) {
        size_t len = (size_t)(end - start - 1);
        if (len >= comm_size) {
            len = comm_size - 1;
        }
        memcpy(comm_out, start + 1, len);
        comm_out[len] = '\0';
    }

    // Parse fields after (comm)
    char *p = end + 1;
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    long cutime, cstime, priority, nice, num_threads;
    long rss;

    // Fields: state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt
    //         utime stime cutime cstime priority nice num_threads itrealvalue starttime
    //         vsize rss ...
    // Note: %*d used for suppressed fields (itrealvalue, starttime, vsize) to avoid gcc warnings
    int fields = sscanf(p, " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %*d %*d %*d %ld",
                        &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
                        &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime,
                        &cutime, &cstime, &priority, &nice, &num_threads, &rss);
    if (fields < 19) {
        return false;
    }

    if (utime_out) *utime_out = utime;
    if (stime_out) *stime_out = stime;
    if (rss_out) *rss_out = rss;
    if (num_threads_out) *num_threads_out = (int)num_threads;
    bool status_is_kernel_thread = false;
    linux_read_process_status_metadata(pid, uid_out, &status_is_kernel_thread);
    if (is_kernel_thread_out) {
        *is_kernel_thread_out = linux_process_is_kernel_thread_from_stat_flags(flags) || status_is_kernel_thread;
    }

    return true;
}

static bool linux_process_is_kernel_thread(int pid) {
    bool is_kernel_thread = false;
    linux_read_process_status_metadata(pid, NULL, &is_kernel_thread);
    if (is_kernel_thread) {
        return true;
    }

    unsigned long utime = 0;
    unsigned long stime = 0;
    long rss = 0;
    int num_threads = 0;
    char comm[2] = {0};
    return linux_read_process_stat(pid, NULL, &utime, &stime, &rss, &num_threads,
                                   &is_kernel_thread, comm, sizeof(comm)) && is_kernel_thread;
}

static int collect_process_records(ProcessRecord **records_out,
                                   int *total_count,
                                   int *thread_count_out,
                                   double *sample_time_out,
                                   uint64_t *sample_ticks_out) {
    if (!records_out) {
        return -1;
    }
    *records_out = NULL;

    if (total_count) {
        *total_count = 0;
    }

    bool *slot_seen = NULL;
    size_t slot_seen_capacity = 0;
    ProcessRecord *records = NULL;
    size_t record_capacity = 0;

    double sample_time = current_time_seconds();
    uint64_t sample_ticks = current_time_ticks();
    size_t history_count = 0;
    uint64_t previous_ticks = 0;
    CpuHistorySample *history = copy_cpu_history(&history_count, &previous_ticks);
    uint64_t elapsed_ticks = 0;
    if (previous_ticks > 0 && sample_ticks > previous_ticks) {
        elapsed_ticks = sample_ticks - previous_ticks;
    }

    // Get clock tick frequency for converting utime/stime to nanoseconds
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        hz = 100;
    }

    // Get page size for converting rss to bytes
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }

    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        free(slot_seen);
        free(history);
        return -1;
    }

    int used = 0;
    int discovered_count = 0;
    int total_threads = 0;
    struct dirent *dir_entry;

    while ((dir_entry = readdir(proc_dir)) != NULL) {
        if (dir_entry->d_type != DT_DIR) {
            continue;
        }

        char *endptr;
        long pid_val = strtol(dir_entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid_val <= 0) {
            continue;
        }
        pid_t pid = (pid_t)pid_val;
        discovered_count++;

        uid_t uid = 0;
        unsigned long utime = 0, stime = 0;
        long rss = 0;
        int num_threads = 0;
        char comm[256] = {0};
        bool is_kernel_thread = false;

        if (!linux_read_process_stat(pid, &uid, &utime, &stime, &rss, &num_threads,
                                     &is_kernel_thread, comm, sizeof(comm))) {
            continue;
        }

        int slot_index = find_active_process_slot(pid);
        if (slot_index < 0) {
            slot_index = allocate_process_slot(pid);
        }
        if (slot_index < 0) {
            continue;
        }
        if (!ensure_bool_capacity(&slot_seen, &slot_seen_capacity, (size_t)slot_index + 1) ||
            !ensure_process_record_capacity(&records, &record_capacity, (size_t)used + 1)) {
            release_process_record_identities(records, (size_t)used);
            free(records);
            free(slot_seen);
            closedir(proc_dir);
            free(history);
            return -1;
        }

        ProcessSlot *slot = &process_slots[slot_index];
        slot_seen[slot_index] = true;

        ProcessRecord *record = &records[used];
        memset(record, 0, sizeof(*record));
        record->slot = slot_index;
        record->slot_generation = slot->generation;

        record->memory_kb = (long)(rss * page_size / 1024);

        uint64_t total_time_ticks = ((uint64_t)utime + (uint64_t)stime) * (1000000000ULL / (uint64_t)hz);
        record->total_cpu_time_ticks = total_time_ticks;
        record->sample_cpu_time_ms = (long)(ticks_to_seconds(total_time_ticks) * 1000.0);
        record->sample_cpu_percent = compute_cpu_percent(slot_index,
                                                         slot->generation,
                                                         pid,
                                                         total_time_ticks,
                                                         history,
                                                         history_count,
                                                         elapsed_ticks);

        if (!slot->identity) {
            char user[MAX_PROCESS_USER_LEN] = {0};
            char command[MAX_PROCESS_COMMAND_LEN] = {0};
            resolve_username(uid, user, sizeof(user));
            if (!read_process_short_command(pid, command, sizeof(command))) {
                strncpy(command, comm, sizeof(command) - 1);
                command[sizeof(command) - 1] = '\0';
            }
            slot->identity = create_process_identity(pid, is_kernel_thread, user, command);
            if (!slot->identity) {
                release_process_record_identities(records, (size_t)used);
                free(records);
                free(slot_seen);
                closedir(proc_dir);
                free(history);
                return -1;
            }
        }

        record->identity = retain_process_identity(slot->identity);
        slot->active = true;
        slot->pid = pid;

        used++;
        total_threads += num_threads;
    }

    closedir(proc_dir);
    free(history);

    if (total_count) {
        *total_count = discovered_count;
    }
    if (thread_count_out) {
        *thread_count_out = total_threads;
    }
    release_inactive_process_slots(slot_seen, slot_seen_capacity);
    update_cpu_history(records, (size_t)used, sample_ticks);
    free(slot_seen);
    if (sample_time_out) {
        *sample_time_out = sample_time;
    }
    if (sample_ticks_out) {
        *sample_ticks_out = sample_ticks;
    }
    *records_out = records;
    return used;
}
#endif

static bool append_process_frame(unsigned char **ptr,
                                 unsigned char *end,
                                 const ProcessViewRow *entry,
                                 int previous_index) {
    if (!ptr || !*ptr || !entry || !entry->record) {
        return false;
    }

    unsigned char *p = *ptr;
    if ((size_t)(end - p) < 4 + sizeof(float) + 8 + 8 + 4 + 4 + 4 + 1) {
        return false;
    }

    const ProcessRecord *record = entry->record;
    const ProcessIdentity *identity = record->identity;
    uint32_t pid = (identity && identity->pid > 0) ? (uint32_t)identity->pid : 0;
    p[0] = (unsigned char)(pid & 0xffu);
    p[1] = (unsigned char)((pid >> 8) & 0xffu);
    p[2] = (unsigned char)((pid >> 16) & 0xffu);
    p[3] = (unsigned char)((pid >> 24) & 0xffu);
    p += 4;

    float cpu_percent = (float)entry->display_cpu_percent;
    memcpy(p, &cpu_percent, sizeof(cpu_percent));
    p += sizeof(cpu_percent);

    uint64_t memory_kb = (record->memory_kb > 0) ? (uint64_t)record->memory_kb : 0;
    for (int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)((memory_kb >> (8 * i)) & 0xffu);
    }
    p += 8;

    uint64_t cpu_time_ms = (entry->display_cpu_time_ms > 0) ? (uint64_t)entry->display_cpu_time_ms : 0;
    for (int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)((cpu_time_ms >> (8 * i)) & 0xffu);
    }
    p += 8;

    const char *user = identity ? identity->user : "";
    const char *command = identity ? identity->command : "";
    size_t user_len = strnlen(user, MAX_PROCESS_USER_LEN);
    size_t command_len = strnlen(command, MAX_PROCESS_COMMAND_LEN);

    uint32_t ulen = (uint32_t)user_len;
    p[0] = (unsigned char)(ulen & 0xffu);
    p[1] = (unsigned char)((ulen >> 8) & 0xffu);
    p[2] = (unsigned char)((ulen >> 16) & 0xffu);
    p[3] = (unsigned char)((ulen >> 24) & 0xffu);
    p += 4;

    uint32_t clen = (uint32_t)command_len;
    p[0] = (unsigned char)(clen & 0xffu);
    p[1] = (unsigned char)((clen >> 8) & 0xffu);
    p[2] = (unsigned char)((clen >> 16) & 0xffu);
    p[3] = (unsigned char)((clen >> 24) & 0xffu);
    p += 4;

    int32_t prev_index = previous_index;
    if (prev_index < -1) {
        prev_index = -1;
    }
    for (int i = 0; i < 4; ++i) {
        p[i] = (unsigned char)(((uint32_t)prev_index >> (8 * i)) & 0xffu);
    }
    p += 4;

    *p++ = (identity && identity->is_kernel_thread) ? 1u : 0u;

    if ((size_t)(end - p) < user_len + command_len) {
        return false;
    }
    memcpy(p, user, user_len);
    p += user_len;
    memcpy(p, command, command_len);
    p += command_len;

    *ptr = p;
    return true;
}

#ifdef __APPLE__
static bool compute_cpu_metrics(double *user, double *system, double *idle) {
    if (!user || !system || !idle) {
        return false;
    }

    host_cpu_load_info_data_t cpu_info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t kr = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu_info, &count);
    if (kr != KERN_SUCCESS) {
        return false;
    }

    uint64_t current[CPU_STATE_MAX];
    current[CPU_STATE_USER] = cpu_info.cpu_ticks[CPU_STATE_USER];
    current[CPU_STATE_NICE] = cpu_info.cpu_ticks[CPU_STATE_NICE];
    current[CPU_STATE_SYSTEM] = cpu_info.cpu_ticks[CPU_STATE_SYSTEM];
    current[CPU_STATE_IDLE] = cpu_info.cpu_ticks[CPU_STATE_IDLE];

    static uint64_t previous[CPU_STATE_MAX] = {0};
    if (!has_previous_cpu_sample) {
        memcpy(previous, current, sizeof(previous));
        has_previous_cpu_sample = true;
        return false;
    }

    uint64_t user_diff = (current[CPU_STATE_USER] - previous[CPU_STATE_USER]) +
                         (current[CPU_STATE_NICE] - previous[CPU_STATE_NICE]);
    uint64_t system_diff = current[CPU_STATE_SYSTEM] - previous[CPU_STATE_SYSTEM];
    uint64_t idle_diff = current[CPU_STATE_IDLE] - previous[CPU_STATE_IDLE];
    uint64_t total = user_diff + system_diff + idle_diff;

    memcpy(previous, current, sizeof(previous));

    if (total == 0) {
        return false;
    }

    double scale = (double)logical_cpu_count();
    *user = (double)user_diff * 100.0 * scale / (double)total;
    *system = (double)system_diff * 100.0 * scale / (double)total;
    *idle = (double)idle_diff * 100.0 * scale / (double)total;
    return true;
}

static int collect_thread_count(void) {
    return -1;
}
#endif

#ifdef __linux__
// Linux CPU state indices for /proc/stat parsing
#define LINUX_CPU_USER 0
#define LINUX_CPU_NICE 1
#define LINUX_CPU_SYSTEM 2
#define LINUX_CPU_IDLE 3
#define LINUX_CPU_IOWAIT 4
#define LINUX_CPU_IRQ 5
#define LINUX_CPU_SOFTIRQ 6
#define LINUX_CPU_STATE_COUNT 7

static bool compute_cpu_metrics(double *user, double *system, double *idle) {
    if (!user || !system || !idle) {
        return false;
    }

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return false;
    }

    char line[256];
    uint64_t current[LINUX_CPU_STATE_COUNT] = {0};
    bool found = false;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            if (sscanf(line + 4, "%lu %lu %lu %lu %lu %lu %lu",
                       &current[LINUX_CPU_USER],
                       &current[LINUX_CPU_NICE],
                       &current[LINUX_CPU_SYSTEM],
                       &current[LINUX_CPU_IDLE],
                       &current[LINUX_CPU_IOWAIT],
                       &current[LINUX_CPU_IRQ],
                       &current[LINUX_CPU_SOFTIRQ]) >= 4) {
                found = true;
            }
            break;
        }
    }
    fclose(fp);

    if (!found) {
        return false;
    }

    static uint64_t previous[LINUX_CPU_STATE_COUNT] = {0};
    if (!has_previous_cpu_sample) {
        memcpy(previous, current, sizeof(previous));
        has_previous_cpu_sample = true;
        return false;
    }

    uint64_t user_diff = (current[LINUX_CPU_USER] - previous[LINUX_CPU_USER]) +
                         (current[LINUX_CPU_NICE] - previous[LINUX_CPU_NICE]);
    uint64_t system_diff = (current[LINUX_CPU_SYSTEM] - previous[LINUX_CPU_SYSTEM]) +
                           (current[LINUX_CPU_IRQ] - previous[LINUX_CPU_IRQ]) +
                           (current[LINUX_CPU_SOFTIRQ] - previous[LINUX_CPU_SOFTIRQ]);
    uint64_t idle_diff = (current[LINUX_CPU_IDLE] - previous[LINUX_CPU_IDLE]) +
                         (current[LINUX_CPU_IOWAIT] - previous[LINUX_CPU_IOWAIT]);
    uint64_t total = user_diff + system_diff + idle_diff;

    memcpy(previous, current, sizeof(previous));

    if (total == 0) {
        return false;
    }

    double scale = (double)logical_cpu_count();
    *user = (double)user_diff * 100.0 * scale / (double)total;
    *system = (double)system_diff * 100.0 * scale / (double)total;
    *idle = (double)idle_diff * 100.0 * scale / (double)total;
    return true;
}

static int collect_thread_count(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    int count = -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "procs_running", 13) == 0) {
            // This isn't total threads, but we'll count them from /proc
            break;
        }
    }
    fclose(fp);

    // Count threads by iterating /proc/[pid]/task directories
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        return -1;
    }

    count = 0;
    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        if (entry->d_type != DT_DIR) {
            continue;
        }
        char *endptr;
        long pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid <= 0) {
            continue;
        }

        char task_path[64];
        snprintf(task_path, sizeof(task_path), "/proc/%ld/task", pid);
        DIR *task_dir = opendir(task_path);
        if (task_dir) {
            struct dirent *task_entry;
            while ((task_entry = readdir(task_dir)) != NULL) {
                if (task_entry->d_name[0] != '.') {
                    count++;
                }
            }
            closedir(task_dir);
        }
    }
    closedir(proc_dir);

    return count > 0 ? count : -1;
}
#endif

static SystemMetrics collect_system_metrics(int default_process_count,
                                            int default_visible_process_count,
                                            int default_thread_count) {
    SystemMetrics metrics = {
        .has_cpu = false,
        .cpu_user = 0.0,
        .cpu_system = 0.0,
        .cpu_idle = 100.0,
        .process_count = default_process_count,
        .visible_process_count = default_visible_process_count,
        .thread_count = default_thread_count,
        .logical_cpu_count = logical_cpu_count(),
    };

    double user = 0.0, system = 0.0, idle = 0.0;
    if (compute_cpu_metrics(&user, &system, &idle)) {
        metrics.has_cpu = true;
        metrics.cpu_user = user;
        metrics.cpu_system = system;
        metrics.cpu_idle = idle;
    }

    if (metrics.thread_count <= 0) {
        int threads = collect_thread_count();
        if (threads >= 0) {
            metrics.thread_count = threads;
        }
    }

    return metrics;
}

#ifdef __APPLE__
static int read_process_thread_count(int pid) {
    if (pid <= 0) {
        return -1;
    }
    struct proc_taskinfo info;
    int result = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (result == sizeof(info)) {
        return (int)info.pti_threadnum;
    }
    return -1;
}

static bool read_process_launch_time(int pid, char *out, size_t out_size) {
    if (pid <= 0 || !out || out_size == 0) {
        return false;
    }
    struct proc_bsdinfo bsd = {0};
    int result = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd));
    if (result != sizeof(bsd)) {
        return false;
    }
    time_t start_time = (time_t)bsd.pbi_start_tvsec;
    struct tm tm;
    if (!localtime_r(&start_time, &tm)) {
        return false;
    }
    if (strftime(out, out_size, "%b %d, %Y %I:%M %p", &tm) == 0) {
        return false;
    }
    return true;
}

static bool read_process_short_command(int pid, char *out, size_t out_size) {
    if (pid <= 0 || !out || out_size == 0) {
        return false;
    }
    char buffer[PROC_PIDPATHINFO_MAXSIZE];
    int len = proc_name(pid, buffer, sizeof(buffer));
    if (len <= 0) {
        return false;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    strncpy(out, buffer, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}
#endif

#ifdef __linux__
static int read_process_thread_count(int pid) {
    if (pid <= 0) {
        return -1;
    }
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    DIR *task_dir = opendir(task_path);
    if (!task_dir) {
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(task_dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(task_dir);
    return count > 0 ? count : -1;
}

static bool read_process_launch_time(int pid, char *out, size_t out_size) {
    if (pid <= 0 || !out || out_size == 0) {
        return false;
    }

    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE *fp = fopen(stat_path, "r");
    if (!fp) {
        return false;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Find the closing paren of comm field, then parse remaining fields
    char *p = strrchr(line, ')');
    if (!p) {
        return false;
    }
    p++;

    // Fields after (comm): state(1), ppid(2), pgrp(3), session(4), tty_nr(5),
    // tpgid(6), flags(7), minflt(8), cminflt(9), majflt(10), cmajflt(11),
    // utime(12), stime(13), cutime(14), cstime(15), priority(16), nice(17),
    // num_threads(18), itrealvalue(19), starttime(20)
    unsigned long long starttime = 0;
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    long cutime, cstime, priority, nice, num_threads, itrealvalue;

    if (sscanf(p, " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu",
               &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
               &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime,
               &cutime, &cstime, &priority, &nice, &num_threads, &itrealvalue,
               &starttime) < 20) {
        return false;
    }

    // starttime is in clock ticks since boot, convert to wall time
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        hz = 100;
    }

    // Get system boot time from /proc/stat
    FILE *stat_fp = fopen("/proc/stat", "r");
    if (!stat_fp) {
        return false;
    }
    unsigned long long btime = 0;
    char stat_line[256];
    while (fgets(stat_line, sizeof(stat_line), stat_fp)) {
        if (strncmp(stat_line, "btime ", 6) == 0) {
            if (sscanf(stat_line + 6, "%llu", &btime) == 1) {
                break;
            }
        }
    }
    fclose(stat_fp);

    if (btime == 0) {
        return false;
    }

    time_t start_time = (time_t)(btime + starttime / (unsigned long long)hz);
    struct tm tm;
    if (!localtime_r(&start_time, &tm)) {
        return false;
    }
    if (strftime(out, out_size, "%b %d, %Y %I:%M %p", &tm) == 0) {
        return false;
    }
    return true;
}

static bool read_process_short_command(int pid, char *out, size_t out_size) {
    if (pid <= 0 || !out || out_size == 0) {
        return false;
    }
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
    FILE *fp = fopen(comm_path, "r");
    if (!fp) {
        return false;
    }
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Remove trailing newline
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '\n') {
        out[len - 1] = '\0';
    }
    return out[0] != '\0';
}
#endif

static int compare_cpu_history_samples(const void *lhs, const void *rhs) {
    const CpuHistorySample *a = lhs;
    const CpuHistorySample *b = rhs;
    if (a->slot < b->slot) {
        return -1;
    }
    if (a->slot > b->slot) {
        return 1;
    }
    if (a->generation < b->generation) {
        return -1;
    }
    if (a->generation > b->generation) {
        return 1;
    }
    if (a->pid < b->pid) {
        return -1;
    }
    if (a->pid > b->pid) {
        return 1;
    }
    return 0;
}

static CpuHistorySample *copy_cpu_history(size_t *count_out, uint64_t *timestamp_out) {
    CpuHistorySample *copy = NULL;
    size_t count = cpu_history_count;
    uint64_t captured_ticks = cpu_history_ticks;

    if (count > 0 && cpu_history) {
        copy = malloc(sizeof(CpuHistorySample) * count);
        if (copy) {
            memcpy(copy, cpu_history, sizeof(CpuHistorySample) * count);
        } else {
            count = 0;
            captured_ticks = 0;
        }
    }

    if (count_out) {
        *count_out = count;
    }
    if (timestamp_out) {
        *timestamp_out = captured_ticks;
    }
    return copy;
}

static void update_cpu_history(const ProcessRecord *records, size_t count, uint64_t timestamp_ticks) {
    CpuHistorySample *history_samples = NULL;
    if (count > 0 && records) {
        history_samples = malloc(sizeof(CpuHistorySample) * count);
        if (!history_samples) {
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            const ProcessRecord *record = &records[i];
            history_samples[i].pid = record->identity ? record->identity->pid : 0;
            history_samples[i].slot = record->slot;
            history_samples[i].generation = record->slot_generation;
            history_samples[i].total_time_ticks = record->total_cpu_time_ticks;
        }
        qsort(history_samples, count, sizeof(CpuHistorySample), compare_cpu_history_samples);
    }

    free(cpu_history);
    cpu_history = history_samples;
    cpu_history_count = count;
    cpu_history_ticks = timestamp_ticks;
}

static double compute_cpu_percent(int slot,
                                  uint32_t generation,
                                  pid_t pid,
                                  uint64_t total_units,
                                  const CpuHistorySample *history,
                                  size_t history_count,
                                  uint64_t elapsed_ticks) {
    if (!history || history_count == 0 || elapsed_ticks == 0) {
        return 0.0;
    }
    CpuHistorySample key = {
        .slot = slot,
        .generation = generation,
        .pid = pid,
        .total_time_ticks = 0
    };
    const CpuHistorySample *found = bsearch(&key,
                                            history,
                                            history_count,
                                            sizeof(CpuHistorySample),
                                            compare_cpu_history_samples);
    if (!found) {
        static int no_history_logs = 0;
        if (no_history_logs < 10) {
            log_debug("no history for pid=%d slot=%d gen=%u history_count=%zu",
                      pid,
                      slot,
                      generation,
                      history_count);
            no_history_logs++;
        }
        return 0.0;
    }
    if (total_units <= found->total_time_ticks) {
        return 0.0;
    }

    uint64_t delta_ticks = total_units - found->total_time_ticks;
    double percent = ((double)delta_ticks / (double)elapsed_ticks) * 100.0;
    return percent;
}

static void clear_process_snapshot(ProcessSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    for (size_t i = 0; i < snapshot->record_count; ++i) {
        release_process_identity(snapshot->records[i].identity);
        snapshot->records[i].identity = NULL;
    }
    free(snapshot->records);
    snapshot->records = NULL;
    snapshot->record_count = 0;
    snapshot->total_process_count = 0;
    snapshot->metrics = (SystemMetrics){0};
    snapshot->timestamp = 0.0;
    snapshot->tick_timestamp = 0;
    snapshot->snapshot_index = 0;
    snapshot->valid = false;
}

static ProcessSnapshot *append_snapshot_history(const ProcessSnapshot *snapshot) {
    if (!snapshot) {
        return NULL;
    }
    if (snapshot_history_count == MAX_SNAPSHOT_HISTORY) {
        size_t evict_index = snapshot_history_start;
        clear_process_snapshot(&snapshot_history[evict_index]);
        snapshot_history_start = (snapshot_history_start + 1) % MAX_SNAPSHOT_HISTORY;
        snapshot_history_count--;
    }
    size_t insert_index = (snapshot_history_start + snapshot_history_count) % MAX_SNAPSHOT_HISTORY;
    snapshot_history[insert_index] = *snapshot;
    snapshot_history_count++;
    return &snapshot_history[insert_index];
}

static ProcessSnapshot *history_entry_at(size_t order_index) {
    if (order_index >= snapshot_history_count) {
        return NULL;
    }
    size_t index = (snapshot_history_start + order_index) % MAX_SNAPSHOT_HISTORY;
    return &snapshot_history[index];
}

static size_t copy_cpu_metric_history(CpuMetricHistoryEntry **out_entries) {
    if (!out_entries) {
        return 0;
    }
    *out_entries = NULL;

    if (!current_snapshot.valid) {
        return 0;
    }

    size_t count = snapshot_history_count;
    bool only_current = false;
    if (count == 0) {
        count = 1;
        only_current = true;
    }

    CpuMetricHistoryEntry *entries = malloc(sizeof(CpuMetricHistoryEntry) * count);
    if (!entries) {
        return 0;
    }

    if (only_current) {
        entries[0].timestamp = current_snapshot.timestamp;
        entries[0].has_cpu = current_snapshot.metrics.has_cpu;
        entries[0].cpu_user = current_snapshot.metrics.has_cpu ? current_snapshot.metrics.cpu_user : 0.0;
        entries[0].cpu_system = current_snapshot.metrics.has_cpu ? current_snapshot.metrics.cpu_system : 0.0;
        entries[0].cpu_idle = current_snapshot.metrics.has_cpu ? current_snapshot.metrics.cpu_idle : 100.0;
    } else {
        for (size_t i = 0; i < count; ++i) {
            ProcessSnapshot *entry = history_entry_at(i);
            if (!entry) {
                entries[i].timestamp = 0.0;
                entries[i].has_cpu = false;
                entries[i].cpu_user = 0.0;
                entries[i].cpu_system = 0.0;
                entries[i].cpu_idle = 100.0;
                continue;
            }
            entries[i].timestamp = entry->timestamp;
            entries[i].has_cpu = entry->metrics.has_cpu;
            if (entry->metrics.has_cpu) {
                entries[i].cpu_user = entry->metrics.cpu_user;
                entries[i].cpu_system = entry->metrics.cpu_system;
                entries[i].cpu_idle = entry->metrics.cpu_idle;
            } else {
                entries[i].cpu_user = 0.0;
                entries[i].cpu_system = 0.0;
                entries[i].cpu_idle = 100.0;
            }
        }
    }

    *out_entries = entries;
    return count;
}

static size_t build_cpu_history_json(char *buffer, size_t cap) {
    if (!buffer || cap == 0) {
        return 0;
    }
    CpuMetricHistoryEntry *entries = NULL;
    size_t count = copy_cpu_metric_history(&entries);
    if (count == 0 || !entries) {
        int n = snprintf(buffer, cap, "{\"samples\":[]}");
        if (n < 0 || (size_t)n >= cap) {
            free(entries);
            return 0;
        }
        free(entries);
        return (size_t)n;
    }

    size_t len = 0;
    int n = snprintf(buffer, cap, "{\"samples\":[");
    if (n < 0 || (size_t)n >= cap) {
        free(entries);
        return 0;
    }
    len += (size_t)n;

    for (size_t i = 0; i < count; ++i) {
        const CpuMetricHistoryEntry *entry = &entries[i];
        const char *sep = (i == 0) ? "" : ",";
        n = snprintf(buffer + len,
                     cap - len,
                     "%s{\"timestamp\":%.3f,\"hasCPU\":%s,\"userPercent\":%.2f,\"systemPercent\":%.2f,\"idlePercent\":%.2f}",
                     sep,
                     entry->timestamp,
                     entry->has_cpu ? "true" : "false",
                     entry->cpu_user,
                     entry->cpu_system,
                     entry->cpu_idle);
        if (n < 0 || (size_t)n >= cap - len) {
            free(entries);
            return 0;
        }
        len += (size_t)n;
    }

    n = snprintf(buffer + len, cap - len, "]}");
    free(entries);
    if (n < 0 || (size_t)n >= cap - len) {
        return 0;
    }
    len += (size_t)n;
    return len;
}

static bool copy_single_snapshot(const ProcessSnapshot *source, ProcessSnapshotView *view) {
    ProcessViewRow *entries = NULL;
    if (source->record_count > 0) {
        entries = malloc(sizeof(ProcessViewRow) * source->record_count);
        if (!entries) {
            return false;
        }
        for (size_t i = 0; i < source->record_count; ++i) {
            entries[i].record = &source->records[i];
            entries[i].display_cpu_percent = source->records[i].sample_cpu_percent;
            entries[i].display_cpu_time_ms = source->records[i].sample_cpu_time_ms;
        }
    }
    view->entries = entries;
    view->entry_count = source->record_count;
    view->total_process_count = source->total_process_count;
    view->metrics = source->metrics;
    view->timestamp = source->timestamp;
    view->tick_timestamp = source->tick_timestamp;
    view->snapshot_index = source->snapshot_index;
    view->selection_start = 0.0;
    view->selection_end = 0.0;
    view->has_time_window = false;
    view->selection_is_trailing = false;
    return true;
}

typedef struct {
    int slot;
    uint32_t generation;
    uint64_t first_ticks;
    const ProcessRecord *last_record;
} AggregateProcessState;

typedef struct {
    AggregateProcessState *primary_states;
    bool *primary_used;
    size_t primary_capacity;
    AggregateProcessState *overflow_states;
    size_t overflow_count;
    size_t overflow_capacity;
    size_t count;
} AggregateProcessStateTable;

static void free_aggregate_state_table(AggregateProcessStateTable *table) {
    if (!table) {
        return;
    }
    free(table->primary_states);
    free(table->primary_used);
    free(table->overflow_states);
    memset(table, 0, sizeof(*table));
}

static bool init_aggregate_state_table(AggregateProcessStateTable *table, size_t primary_capacity) {
    if (!table) {
        return false;
    }
    memset(table, 0, sizeof(*table));
    table->primary_capacity = primary_capacity;
    if (primary_capacity == 0) {
        return true;
    }
    table->primary_states = calloc(primary_capacity, sizeof(AggregateProcessState));
    table->primary_used = calloc(primary_capacity, sizeof(bool));
    if (!table->primary_states || !table->primary_used) {
        free_aggregate_state_table(table);
        return false;
    }
    return true;
}

static AggregateProcessState *find_or_insert_overflow_aggregate_state(AggregateProcessStateTable *table,
                                                                      int slot_index,
                                                                      uint32_t generation,
                                                                      bool *isNewOut) {
    size_t left = 0;
    size_t right = table->overflow_count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        AggregateProcessState *mid_state = &table->overflow_states[mid];
        if (mid_state->slot == slot_index && mid_state->generation == generation) {
            *isNewOut = false;
            return mid_state;
        }
        bool mid_less = (mid_state->slot < slot_index) ||
                        (mid_state->slot == slot_index && mid_state->generation < generation);
        if (mid_less) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    size_t insert_index = left;
    if (table->overflow_count == table->overflow_capacity) {
        size_t new_capacity = table->overflow_capacity == 0 ? 16 : table->overflow_capacity * 2;
        AggregateProcessState *resized = realloc(table->overflow_states,
                                                 sizeof(AggregateProcessState) * new_capacity);
        if (!resized) {
            return NULL;
        }
        table->overflow_states = resized;
        table->overflow_capacity = new_capacity;
    }
    if (insert_index < table->overflow_count) {
        memmove(&table->overflow_states[insert_index + 1],
                &table->overflow_states[insert_index],
                (table->overflow_count - insert_index) * sizeof(AggregateProcessState));
    }

    AggregateProcessState *state = &table->overflow_states[insert_index];
    table->overflow_count++;
    table->count++;
    *isNewOut = true;
    return state;
}

static void append_aggregated_process_view_row(const AggregateProcessState *state,
                                               uint64_t elapsed_ticks,
                                               bool selection_is_trailing,
                                               ProcessViewRow *entries,
                                               size_t *count) {

    ProcessViewRow entry = {0};
    entry.record = state->last_record;

    uint64_t delta_ticks = state->last_record->total_cpu_time_ticks - state->first_ticks;
    if (elapsed_ticks > 0 && delta_ticks > 0) {
        entry.display_cpu_percent = ((double)delta_ticks / (double)elapsed_ticks) * 100.0;
    } else {
        entry.display_cpu_percent = 0.0;
    }

    if (selection_is_trailing) {
        entry.display_cpu_time_ms = state->last_record->sample_cpu_time_ms;
    } else {
        entry.display_cpu_time_ms = (long)(ticks_to_seconds(delta_ticks) * 1000.0);
    }
    entries[(*count)++] = entry;
}

static void resolve_username(uid_t uid, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    struct passwd pwd;
    struct passwd *result = NULL;
    char buffer[256];
    if (getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &result) == 0 && result && result->pw_name) {
        strncpy(out, result->pw_name, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    snprintf(out, out_size, "%u", (unsigned int)uid);
}

#ifdef __APPLE__
static const char *fd_type_label(uint32_t fdtype) {
    switch (fdtype) {
        case PROX_FDTYPE_VNODE:
            return "VNODE";
        case PROX_FDTYPE_SOCKET:
            return "SOCKET";
        case PROX_FDTYPE_PSHM:
            return "PSHM";
        case PROX_FDTYPE_PSEM:
            return "PSEM";
        case PROX_FDTYPE_KQUEUE:
            return "KQUEUE";
        case PROX_FDTYPE_PIPE:
            return "PIPE";
        case PROX_FDTYPE_FSEVENTS:
            return "FSEVENTS";
        case PROX_FDTYPE_ATALK:
            return "ATALK";
        default:
            return "OTHER";
    }
}

static void format_ipv4_endpoint(const struct in_addr *addr,
                                 uint16_t port,
                                 char *out,
                                 size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    char address[INET_ADDRSTRLEN] = "*";
    if (addr && addr->s_addr != 0) {
        inet_ntop(AF_INET, addr, address, sizeof(address));
    }
    if (port > 0) {
        snprintf(out, out_size, "%s:%u", address, port);
    } else {
        snprintf(out, out_size, "%s", address);
    }
}

static void format_ipv6_endpoint(const struct in6_addr *addr,
                                 uint16_t port,
                                 char *out,
                                 size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    char address[INET6_ADDRSTRLEN] = "*";
    if (addr && !IN6_IS_ADDR_UNSPECIFIED(addr)) {
        inet_ntop(AF_INET6, addr, address, sizeof(address));
    }
    if (port > 0) {
        snprintf(out, out_size, "[%s]:%u", address, port);
    } else {
        snprintf(out, out_size, "%s", address);
    }
}

static bool build_vnode_entry(pid_t pid, int fd, OpenFileEntry *entry, bool *permission_denied) {
    struct vnode_fdinfowithpath vnode_info;
    int bytes = proc_pidfdinfo(pid,
                               PROC_PIDFDVNODEPATHINFO,
                               fd,
                               &vnode_info,
                               sizeof(vnode_info));
    if (bytes != sizeof(vnode_info)) {
        if (permission_denied && (errno == EPERM || errno == EACCES)) {
            *permission_denied = true;
        }
        return false;
    }
    snprintf(entry->type, sizeof(entry->type), "VNODE");
    strncpy(entry->name, vnode_info.pvip.vip_path, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    return true;
}

static const struct in_sockinfo *extract_inet_info(const struct socket_info *info) {
    if (!info) {
        return NULL;
    }
    if (info->soi_kind == SOCKINFO_TCP) {
        return &info->soi_proto.pri_tcp.tcpsi_ini;
    }
    if (info->soi_kind == SOCKINFO_IN) {
        return &info->soi_proto.pri_in;
    }
    return NULL;
}

static bool build_socket_entry(pid_t pid, int fd, OpenFileEntry *entry, bool *permission_denied) {
    struct socket_fdinfo fdinfo;
    int bytes = proc_pidfdinfo(pid,
                               PROC_PIDFDSOCKETINFO,
                               fd,
                               &fdinfo,
                               sizeof(fdinfo));
    if (bytes != sizeof(fdinfo)) {
        if (permission_denied && (errno == EPERM || errno == EACCES)) {
            *permission_denied = true;
        }
        return false;
    }

    const struct socket_info *info = &fdinfo.psi;
    snprintf(entry->type, sizeof(entry->type), "SOCKET");
    entry->name[0] = '\0';

    const struct in_sockinfo *inet_info = extract_inet_info(info);

    switch (info->soi_family) {
        case AF_INET:
        case AF_INET6: {
            if (!inet_info) {
                snprintf(entry->name,
                         sizeof(entry->name),
                         "family=%d type=%d",
                         info->soi_family,
                         info->soi_kind);
                break;
            }

            if ((inet_info->insi_vflag & INI_IPV6) != 0 || info->soi_family == AF_INET6) {
                char local[80];
                char remote[80];
                struct in6_addr local_addr = inet_info->insi_laddr.ina_6;
                struct in6_addr remote_addr = inet_info->insi_faddr.ina_6;
                format_ipv6_endpoint(&local_addr,
                                     (uint16_t)inet_info->insi_lport,
                                     local,
                                     sizeof(local));
                format_ipv6_endpoint(&remote_addr,
                                     (uint16_t)inet_info->insi_fport,
                                     remote,
                                     sizeof(remote));
                if (remote[0] != '\0' && strcmp(remote, "*") != 0) {
                    snprintf(entry->name, sizeof(entry->name), "%s -> %s", local, remote);
                } else {
                    snprintf(entry->name, sizeof(entry->name), "%s", local);
                }
            } else {
                char local[64];
                char remote[64];
                struct in_addr local_addr = inet_info->insi_laddr.ina_46.i46a_addr4;
                struct in_addr remote_addr = inet_info->insi_faddr.ina_46.i46a_addr4;
                format_ipv4_endpoint(&local_addr,
                                     (uint16_t)inet_info->insi_lport,
                                     local,
                                     sizeof(local));
                format_ipv4_endpoint(&remote_addr,
                                     (uint16_t)inet_info->insi_fport,
                                     remote,
                                     sizeof(remote));
                if (remote[0] != '\0' && strcmp(remote, "*") != 0) {
                    snprintf(entry->name, sizeof(entry->name), "%s -> %s", local, remote);
                } else {
                    snprintf(entry->name, sizeof(entry->name), "%s", local);
                }
            }
            break;
        }
        case AF_UNIX: {
            const struct sockaddr_un *sun = &info->soi_proto.pri_un.unsi_addr.ua_sun;
            if (sun->sun_path[0] != '\0') {
                snprintf(entry->name, sizeof(entry->name), "%.*s", (int)sizeof(sun->sun_path), sun->sun_path);
            } else {
                snprintf(entry->name, sizeof(entry->name), "UNIX socket");
            }
            break;
        }
        default:
            snprintf(entry->name,
                     sizeof(entry->name),
                     "family=%d type=%d",
                     info->soi_family,
                     info->soi_kind);
            break;
    }

    return true;
}
#endif

static int compare_process_view_rows_internal(const ProcessViewRow *lhs,
                                              const ProcessViewRow *rhs,
                                              const ProcessSortContext *context) {
    if (!lhs || !rhs || !lhs->record || !rhs->record || !context) {
        return 0;
    }
    ProcessSortColumn column = context->column;
    if (column == PROCESS_SORT_NONE) {
        column = PROCESS_SORT_CPU;
    }
    const ProcessIdentity *lidentity = lhs->record ? lhs->record->identity : NULL;
    const ProcessIdentity *ridentity = rhs->record ? rhs->record->identity : NULL;
    int lpid = lidentity ? lidentity->pid : 0;
    int rpid = ridentity ? ridentity->pid : 0;
    const char *lcommand = lidentity ? lidentity->command : "";
    const char *rcommand = ridentity ? ridentity->command : "";
    const char *luser = lidentity ? lidentity->user : "";
    const char *ruser = ridentity ? ridentity->user : "";
    int result = 0;
    switch (column) {
        case PROCESS_SORT_PID:
            if (lpid < rpid) {
                result = -1;
            } else if (lpid > rpid) {
                result = 1;
            } else {
                result = strcasecmp(lcommand, rcommand);
            }
            break;
        case PROCESS_SORT_COMMAND:
            result = strcasecmp(lcommand, rcommand);
            if (result == 0) {
                if (lpid < rpid) {
                    result = -1;
                } else if (lpid > rpid) {
                    result = 1;
                }
            }
            break;
        case PROCESS_SORT_USER:
            result = strcasecmp(luser, ruser);
            if (result == 0) {
                result = strcasecmp(lcommand, rcommand);
                if (result == 0) {
                    if (lpid < rpid) {
                        result = -1;
                    } else if (lpid > rpid) {
                        result = 1;
                    }
                }
            }
            break;
        case PROCESS_SORT_CPU: {
            double l = lhs->display_cpu_percent;
            double r = rhs->display_cpu_percent;
            if (l < r) {
                result = -1;
            } else if (l > r) {
                result = 1;
            } else {
                if (lpid < rpid) {
                    result = -1;
                } else if (lpid > rpid) {
                    result = 1;
                }
            }
            break;
        }
        case PROCESS_SORT_CPU_TIME:
            if (lhs->display_cpu_time_ms < rhs->display_cpu_time_ms) {
                result = -1;
            } else if (lhs->display_cpu_time_ms > rhs->display_cpu_time_ms) {
                result = 1;
            } else {
                if (lpid < rpid) {
                    result = -1;
                } else if (lpid > rpid) {
                    result = 1;
                }
            }
            break;
        case PROCESS_SORT_MEMORY:
            if (lhs->record->memory_kb < rhs->record->memory_kb) {
                result = -1;
            } else if (lhs->record->memory_kb > rhs->record->memory_kb) {
                result = 1;
            } else if (lpid != rpid) {
                result = (lpid < rpid) ? -1 : 1;
            }
            break;
        case PROCESS_SORT_NONE:
        default:
            result = 0;
            break;
    }

    if (!context->ascending) {
        result = -result;
    }
    return result;
}

static int process_view_row_comparator(const void *lhs, const void *rhs) {
    const ProcessViewRow *left = lhs;
    const ProcessViewRow *right = rhs;
    return compare_process_view_rows_internal(left, right, &current_sort_context);
}

static void sort_process_view_rows(ProcessViewRow *entries, size_t count, const ProcessWindowRequest *request) {
    if (!entries || count <= 1) {
        return;
    }
    ProcessSortContext context = current_sort_context;
    if (request) {
        context.column = request->has_sort ? request->sort_column : PROCESS_SORT_CPU;
        context.ascending = request->has_sort ? request->sort_ascending : false;
    }
    current_sort_context = context;
    qsort(entries, count, sizeof(ProcessViewRow), process_view_row_comparator);
}

static void reset_order_cache(ProcessOrderCache *cache) {
    if (!cache) {
        return;
    }
    free(cache->slot_indices);
    free(cache->slot_generations);
    free(cache->overflow_entries);
    cache->slot_indices = NULL;
    cache->slot_generations = NULL;
    cache->slot_capacity = 0;
    cache->overflow_entries = NULL;
    cache->overflow_count = 0;
    cache->overflow_capacity = 0;
    cache->valid = false;
    memset(&cache->context, 0, sizeof(cache->context));
}

static ProcessOrderContext make_order_context(const ProcessWindowRequest *request) {
    ProcessOrderContext context = {0};
    if (request) {
        context.sort_column = request->has_sort ? request->sort_column : PROCESS_SORT_CPU;
        context.sort_ascending = request->has_sort ? request->sort_ascending : false;
        context.has_time_selection = request->has_time_selection;
        if (request->has_time_selection) {
            context.time_selection = request->time_selection;
        }
        strncpy(context.search_term, request->search_term, sizeof(context.search_term) - 1);
        context.search_term[sizeof(context.search_term) - 1] = '\0';
    } else {
        context.sort_column = PROCESS_SORT_CPU;
        context.sort_ascending = false;
        context.has_time_selection = false;
        context.search_term[0] = '\0';
    }
    return context;
}

static bool order_context_equal(const ProcessOrderContext *a, const ProcessOrderContext *b) {
    if (!a || !b) {
        return false;
    }
    if (a->sort_column != b->sort_column || a->sort_ascending != b->sort_ascending) {
        return false;
    }
    if (strncmp(a->search_term, b->search_term, sizeof(a->search_term)) != 0) {
        return false;
    }
    return true;
}

static int lookup_previous_index(const ProcessOrderCache *cache, int slot, uint32_t generation) {
    if (!cache || !cache->valid || slot < 0 || (size_t)slot >= cache->slot_capacity) {
        return -1;
    }
    if (cache->slot_generations[slot] != generation) {
        for (size_t i = 0; i < cache->overflow_count; ++i) {
            if (cache->overflow_entries[i].slot == slot &&
                cache->overflow_entries[i].generation == generation) {
                return cache->overflow_entries[i].index;
            }
        }
        return -1;
    }
    return cache->slot_indices[slot];
}

static bool ensure_order_cache_overflow_capacity(ProcessOrderCache *cache, size_t required_capacity) {
    if (required_capacity <= cache->overflow_capacity) {
        return true;
    }
    size_t new_capacity = cache->overflow_capacity == 0 ? 8 : cache->overflow_capacity;
    while (new_capacity < required_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(ProcessOrderCacheEntry)) {
        return false;
    }
    ProcessOrderCacheEntry *new_entries = realloc(cache->overflow_entries,
                                                  sizeof(ProcessOrderCacheEntry) * new_capacity);
    if (!new_entries) {
        return false;
    }
    cache->overflow_entries = new_entries;
    cache->overflow_capacity = new_capacity;
    return true;
}

static bool ensure_order_cache_slot_capacity(ProcessOrderCache *cache, size_t required_capacity) {
    if (required_capacity <= cache->slot_capacity) {
        return true;
    }
    size_t new_capacity = cache->slot_capacity == 0 ? 256 : cache->slot_capacity;
    while (new_capacity < required_capacity) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(int) ||
        new_capacity > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }
    int *new_indices = realloc(cache->slot_indices, sizeof(int) * new_capacity);
    if (!new_indices) {
        return false;
    }
    uint32_t *new_generations = realloc(cache->slot_generations, sizeof(uint32_t) * new_capacity);
    if (!new_generations) {
        cache->slot_indices = new_indices;
        return false;
    }
    for (size_t i = cache->slot_capacity; i < new_capacity; ++i) {
        new_indices[i] = -1;
        new_generations[i] = 0;
    }
    cache->slot_indices = new_indices;
    cache->slot_generations = new_generations;
    cache->slot_capacity = new_capacity;
    return true;
}

static bool update_order_cache_overflow(ProcessOrderCache *cache, int slot, uint32_t generation, int index) {
    for (size_t i = 0; i < cache->overflow_count; ++i) {
        if (cache->overflow_entries[i].slot == slot &&
            cache->overflow_entries[i].generation == generation) {
            cache->overflow_entries[i].index = index;
            return true;
        }
    }
    if (!ensure_order_cache_overflow_capacity(cache, cache->overflow_count + 1)) {
        return false;
    }
    cache->overflow_entries[cache->overflow_count++] = (ProcessOrderCacheEntry) {
        .slot = slot,
        .generation = generation,
        .index = index
    };
    return true;
}

static void update_order_cache(ProcessOrderCache *cache,
                               const ProcessViewRow *entries,
                               size_t count,
                               const ProcessOrderContext *context) {
    if (!cache || !context) {
        return;
    }
    if (!ensure_order_cache_slot_capacity(cache, process_slot_count)) {
        reset_order_cache(cache);
        return;
    }
    for (size_t i = 0; i < cache->slot_capacity; ++i) {
        cache->slot_indices[i] = -1;
        cache->slot_generations[i] = 0;
    }
    cache->overflow_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const ProcessRecord *record = entries[i].record;
        int slot = record->slot;
        if (slot >= 0 && (size_t)slot < cache->slot_capacity) {
            if (cache->slot_indices[slot] < 0 ||
                cache->slot_generations[slot] == record->slot_generation) {
                cache->slot_indices[slot] = (int)i;
                cache->slot_generations[slot] = record->slot_generation;
            } else if (!update_order_cache_overflow(cache,
                                                    slot,
                                                    record->slot_generation,
                                                    (int)i)) {
                reset_order_cache(cache);
                return;
            }
        }
    }
    cache->context = *context;
    cache->valid = true;
}

static bool entry_matches_search(const ProcessViewRow *entry, const char *term) {
    if (!term || term[0] == '\0') {
        return true;
    }
    if (!entry) {
        return false;
    }
    const ProcessIdentity *identity = entry->record ? entry->record->identity : NULL;
    return identity && strcasestr(identity->command, term) != NULL;
}

static bool refresh_process_snapshot(void) {
    ProcessRecord *records = NULL;

    int total_count = 0;
    double sample_seconds = 0.0;
    uint64_t sample_ticks = 0;
    int total_threads = 0;
    int used = collect_process_records(&records,
                                       &total_count,
                                       &total_threads,
                                       &sample_seconds,
                                       &sample_ticks);
    if (used < 0) {
        free(records);
        return false;
    }

    SystemMetrics metrics = collect_system_metrics(total_count, used, total_threads);
    double timestamp = sample_seconds > 0.0 ? sample_seconds : current_time_seconds();

    uint64_t next_index = current_snapshot.snapshot_index + 1;
    if (next_index == 0) {
        next_index = 1;
    }
    ProcessSnapshot snapshot_record = {
        .records = records,
        .record_count = (size_t)used,
        .total_process_count = used,
        .metrics = metrics,
        .timestamp = timestamp,
        .tick_timestamp = sample_ticks,
        .snapshot_index = next_index,
        .valid = true
    };
    ProcessSnapshot *stored = append_snapshot_history(&snapshot_record);
    if (stored) {
        current_snapshot = *stored;
    } else {
        current_snapshot.records = records;
        current_snapshot.record_count = (size_t)used;
        current_snapshot.total_process_count = used;
        current_snapshot.metrics = metrics;
        current_snapshot.timestamp = timestamp;
        current_snapshot.tick_timestamp = sample_ticks;
        current_snapshot.snapshot_index = next_index;
        current_snapshot.valid = true;
    }
    return true;
}

static void free_process_snapshot(ProcessSnapshotView *view) {
    if (!view) {
        return;
    }
    free(view->entries);
    view->entries = NULL;
    view->entry_count = 0;
}

static bool build_process_snapshot_view(ProcessSnapshotView *view, const ProcessWindowRequest *request) {
    if (request->has_time_selection) {
        return build_time_window_snapshot(view, request);
    } else {
        return copy_single_snapshot(&current_snapshot, view);
    }
}

static bool build_time_window_snapshot(ProcessSnapshotView *view, const ProcessWindowRequest *request) {
    if (!current_snapshot.valid || snapshot_history_count == 0) {
        return false;
    }

    ProcessSnapshot *latest_snapshot = history_entry_at(snapshot_history_count - 1);
    ProcessSnapshot *oldest_snapshot = history_entry_at(0);
    if (!latest_snapshot || !oldest_snapshot) {
        return false;
    }

    double desired_end = latest_snapshot->timestamp;
    double desired_start = desired_end - DEFAULT_TIME_WINDOW_SEC;
    bool selection_is_trailing;

    if (request->time_selection.mode == TIME_SELECTION_TRAILING) {
        double duration = request->time_selection.duration > 0.0
            ? request->time_selection.duration
            : DEFAULT_TIME_WINDOW_SEC;
        duration = clamp_time_window_duration(duration);
        desired_start = desired_end - duration;
        selection_is_trailing = true;
    } else if (request->time_selection.mode == TIME_SELECTION_FIXED) {
        desired_start = request->time_selection.start_time;
        desired_end = request->time_selection.end_time;
        selection_is_trailing = false;
    } else {
        selection_is_trailing = true;
    }

    if (desired_end > latest_snapshot->timestamp) {
        desired_end = latest_snapshot->timestamp;
    }
    if (desired_end < oldest_snapshot->timestamp) {
        desired_end = oldest_snapshot->timestamp;
    }
    if (desired_start < oldest_snapshot->timestamp) {
        desired_start = oldest_snapshot->timestamp;
    }
    if (desired_start > desired_end) {
        desired_start = desired_end;
    }

    size_t first_index = snapshot_history_count - 1;
    for (size_t i = 0; i < snapshot_history_count; ++i) {
        ProcessSnapshot *entry = history_entry_at(i);
        if (!entry) {
            continue;
        }
        if (entry->timestamp >= desired_start) {
            first_index = i;
            break;
        }
    }

    size_t last_index = snapshot_history_count - 1;
    for (size_t i = snapshot_history_count; i > 0; --i) {
        ProcessSnapshot *entry = history_entry_at(i - 1);
        if (!entry) {
            continue;
        }
        if (entry->timestamp <= desired_end) {
            last_index = i - 1;
            break;
        }
    }
    if (last_index < first_index) {
        last_index = first_index;
    }

    ProcessSnapshot *first_snapshot = history_entry_at(first_index);
    ProcessSnapshot *last_snapshot = history_entry_at(last_index);
    if (!first_snapshot || !last_snapshot) {
        return false;
    }

    double actual_start = first_snapshot->timestamp;
    double actual_end = last_snapshot->timestamp;
    uint64_t selection_snapshot_index = time_window_snapshot_index(desired_start,
                                                                   desired_end,
                                                                   last_snapshot->snapshot_index);
    uint64_t elapsed_ticks = 0;
    if (last_snapshot->tick_timestamp > first_snapshot->tick_timestamp) {
        elapsed_ticks = last_snapshot->tick_timestamp - first_snapshot->tick_timestamp;
    }

    if (elapsed_ticks == 0 || first_index == last_index) {
        bool copied = copy_single_snapshot(last_snapshot, view);
        if (copied) {
            view->selection_start = actual_start;
            view->selection_end = actual_end;
            view->has_time_window = true;
            double end_delta = fabs(actual_end - latest_snapshot->timestamp);
            view->selection_is_trailing = selection_is_trailing && (end_delta <= 0.2);
            view->snapshot_index = selection_snapshot_index;
        }
        return copied;
    }

    AggregateProcessStateTable state_table = {0};
    if (!init_aggregate_state_table(&state_table, process_slot_count)) {
        return false;
    }

    for (size_t record_index = 0; record_index < first_snapshot->record_count; ++record_index) {
        const ProcessRecord *record = &first_snapshot->records[record_index];
        if (record->slot < 0 || (size_t)record->slot >= state_table.primary_capacity) {
            continue;
        }
        AggregateProcessState *state = &state_table.primary_states[record->slot];

        state->slot = record->slot;
        state->generation = record->slot_generation;
        state->first_ticks = record->total_cpu_time_ticks;
        state->last_record = record;

        state_table.primary_used[record->slot] = true;
        state_table.count++;
    }

    for (size_t history_offset = first_index + 1; history_offset <= last_index; ++history_offset) {
        ProcessSnapshot *snapshot_entry = history_entry_at(history_offset);
        if (!snapshot_entry) {
            continue;
        }
        for (size_t record_index = 0; record_index < snapshot_entry->record_count; ++record_index) {
            const ProcessRecord *record = &snapshot_entry->records[record_index];

            size_t slot = (size_t)record->slot;
            if (record->slot < 0 || slot >= state_table.primary_capacity) {
                continue;
            }
            AggregateProcessState *pstate = &state_table.primary_states[slot];
            if (!state_table.primary_used[slot]) {
                pstate->slot = record->slot;
                pstate->generation = record->slot_generation;
                pstate->first_ticks = record->total_cpu_time_ticks;
                pstate->last_record = record;

                state_table.primary_used[slot] = true;
                state_table.count++;
            } else if (pstate->generation == record->slot_generation) {
                pstate->last_record = record;
            } else {
                bool isNew;
                AggregateProcessState *ostate = find_or_insert_overflow_aggregate_state(&state_table,
                                                                                       record->slot,
                                                                                       record->slot_generation,
                                                                                       &isNew);

                if (!ostate) {
                    free_aggregate_state_table(&state_table);
                    return false;
                } else if (isNew) {
                    ostate->slot = record->slot;
                    ostate->generation = record->slot_generation;
                    ostate->first_ticks = record->total_cpu_time_ticks;
                    ostate->last_record = record;
                } else {
                    ostate->last_record = record;
                }
            }
        }
    }

    ProcessViewRow *aggregated_entries = NULL;
    if (state_table.count > 0) {
        aggregated_entries = malloc(sizeof(ProcessViewRow) * state_table.count);
        if (!aggregated_entries) {
            free_aggregate_state_table(&state_table);
            return false;
        }
    }

    size_t aggregated_count = 0;
    for (size_t i = 0; i < state_table.primary_capacity; ++i) {
        if (state_table.primary_used[i]) {
            append_aggregated_process_view_row(&state_table.primary_states[i],
                                               elapsed_ticks,
                                               selection_is_trailing,
                                               aggregated_entries,
                                               &aggregated_count);
        }
    }
    for (size_t i = 0; i < state_table.overflow_count; ++i) {
        append_aggregated_process_view_row(&state_table.overflow_states[i],
                                           elapsed_ticks,
                                           selection_is_trailing,
                                           aggregated_entries,
                                           &aggregated_count);
    }

    free_aggregate_state_table(&state_table);

    view->entries = aggregated_entries;
    view->entry_count = aggregated_count;
    view->total_process_count = (int)aggregated_count;
    view->metrics = current_snapshot.metrics;
    view->timestamp = actual_end;
    view->tick_timestamp = last_snapshot->tick_timestamp;
    view->snapshot_index = selection_snapshot_index;
    view->selection_start = actual_start;
    view->selection_end = actual_end;
    view->has_time_window = true;
    double end_delta = fabs(actual_end - latest_snapshot->timestamp);
    view->selection_is_trailing = selection_is_trailing && (end_delta <= 0.2);

    return true;
}

static size_t encode_process_stream_frame(unsigned char *buffer,
                                          size_t cap,
                                          const ProcessSnapshotView *view,
                                          const ProcessWindowRequest *request,
                                          ProcessOrderCache *order_cache) {
    if (!buffer || cap < 32 || !view) {
        return 0;
    }

    ProcessWindowRequest effective_request;
    if (request) {
        effective_request = *request;
        normalize_window_request(&effective_request);
    } else {
        apply_default_window_request(&effective_request);
    }

    size_t base_count = view->entry_count;
    ProcessViewRow *filtered = NULL;
    size_t filtered_count = 0;
    if (base_count > 0) {
        filtered = malloc(sizeof(ProcessViewRow) * base_count);
        if (!filtered) {
            return 0;
        }
        const char *term = effective_request.search_term;
        if (term && term[0] != '\0') {
            for (size_t i = 0; i < base_count; ++i) {
                if (entry_matches_search(&view->entries[i], term)) {
                    filtered[filtered_count++] = view->entries[i];
                }
            }
        } else {
            memcpy(filtered, view->entries, sizeof(ProcessViewRow) * base_count);
            filtered_count = base_count;
        }
    }

    if (filtered_count > 1) {
        sort_process_view_rows(filtered, filtered_count, &effective_request);
    }

    ProcessOrderContext current_context = make_order_context(&effective_request);
    bool use_order_cache = (order_cache != NULL);
    bool cache_valid = false;
    if (use_order_cache && order_cache->valid && order_context_equal(&order_cache->context, &current_context)) {
        cache_valid = true;
    }
    if (use_order_cache && !cache_valid) {
        order_cache->valid = false;
        for (size_t i = 0; i < order_cache->slot_capacity; ++i) {
            order_cache->slot_indices[i] = -1;
            order_cache->slot_generations[i] = 0;
        }
        order_cache->overflow_count = 0;
    }

    size_t window_start = effective_request.window_start < filtered_count ? effective_request.window_start : filtered_count;
    size_t window_end = effective_request.window_end < filtered_count ? effective_request.window_end : filtered_count;
    if (window_end < window_start) {
        window_end = window_start;
    }
    size_t window_count = window_end > window_start ? (window_end - window_start) : 0;

    int *previous_indices = NULL;
    if (use_order_cache) {
        if (cache_valid) {
            previous_indices = malloc(sizeof(int) * filtered_count);
            if (filtered_count > 0 && !previous_indices) {
                free(filtered);
                return 0;
            }
            for (size_t i = 0; i < filtered_count; ++i) {
                const ProcessRecord *record = filtered[i].record;
                previous_indices[i] = lookup_previous_index(order_cache,
                                                            record->slot,
                                                            record->slot_generation);
            }
        }
        update_order_cache(order_cache, filtered, filtered_count, &current_context);
    }

    unsigned char *ptr = buffer;
    unsigned char *end = buffer + cap;

    size_t header_size = sizeof(view->timestamp) +
                         sizeof(uint32_t) * 3 +
                         sizeof(uint64_t) +
                         sizeof(double) * 2 +
                         8;
    if ((size_t)(end - ptr) < header_size) {
        free(filtered);
        free(previous_indices);
        return 0;
    }

    memcpy(ptr, &view->timestamp, sizeof(view->timestamp));
    ptr += sizeof(view->timestamp);

    uint32_t count32 = (uint32_t)window_count;
    ptr[0] = (unsigned char)(count32 & 0xffu);
    ptr[1] = (unsigned char)((count32 >> 8) & 0xffu);
    ptr[2] = (unsigned char)((count32 >> 16) & 0xffu);
    ptr[3] = (unsigned char)((count32 >> 24) & 0xffu);
    ptr += 4;

    uint32_t window_start32 = (uint32_t)window_start;
    ptr[0] = (unsigned char)(window_start32 & 0xffu);
    ptr[1] = (unsigned char)((window_start32 >> 8) & 0xffu);
    ptr[2] = (unsigned char)((window_start32 >> 16) & 0xffu);
    ptr[3] = (unsigned char)((window_start32 >> 24) & 0xffu);
    ptr += 4;

    uint32_t filtered_total32 = (uint32_t)filtered_count;
    ptr[0] = (unsigned char)(filtered_total32 & 0xffu);
    ptr[1] = (unsigned char)((filtered_total32 >> 8) & 0xffu);
    ptr[2] = (unsigned char)((filtered_total32 >> 16) & 0xffu);
    ptr[3] = (unsigned char)((filtered_total32 >> 24) & 0xffu);
    ptr += 4;

    uint64_t snapshot_index = view->snapshot_index;
    for (int i = 0; i < 8; ++i) {
        ptr[i] = (unsigned char)((snapshot_index >> (8 * i)) & 0xffu);
    }
    ptr += sizeof(uint64_t);

    double selection_start = view->selection_start;
    double selection_end = view->selection_end;
    memcpy(ptr, &selection_start, sizeof(selection_start));
    ptr += sizeof(selection_start);
    memcpy(ptr, &selection_end, sizeof(selection_end));
    ptr += sizeof(selection_end);

    uint8_t selection_flags = 0;
    if (view->has_time_window) {
        selection_flags |= SELECTION_FLAG_HAS_WINDOW;
    }
    if (view->selection_is_trailing) {
        selection_flags |= SELECTION_FLAG_TRAILING;
    }
    ptr[0] = selection_flags;
    for (int i = 1; i < 8; ++i) {
        ptr[i] = 0;
    }
    ptr += 8;

    for (size_t i = 0; i < window_count; ++i) {
        size_t filtered_index = window_start + i;
        int previous_index = previous_indices ? previous_indices[filtered_index] : -1;
        if (!append_process_frame(&ptr, end, &filtered[filtered_index], previous_index)) {
            break;
        }
    }

    SystemMetrics metrics = view->metrics;
    if (!metrics.has_cpu) {
        metrics.cpu_user = 0.0;
        metrics.cpu_system = 0.0;
        metrics.cpu_idle = 100.0;
    }
    uint8_t metrics_flag = metrics.has_cpu ? 1 : 0;
    float cpu_user = (float)metrics.cpu_user;
    float cpu_system = (float)metrics.cpu_system;
    float cpu_idle = (float)metrics.cpu_idle;
    uint32_t process_count = metrics.process_count > 0 ? (uint32_t)metrics.process_count : (uint32_t)view->total_process_count;
    if (process_count == 0) {
        process_count = (uint32_t)base_count;
    }
    uint32_t visible_process_count = metrics.visible_process_count > 0
        ? (uint32_t)metrics.visible_process_count
        : (uint32_t)view->total_process_count;
    if (visible_process_count == 0) {
        visible_process_count = (uint32_t)base_count;
    }
    uint32_t thread_count = metrics.thread_count > 0 ? (uint32_t)metrics.thread_count : 0;
    uint32_t logical_cpu_count_value = metrics.logical_cpu_count > 0 ? (uint32_t)metrics.logical_cpu_count : (uint32_t)logical_cpu_count();

    if ((size_t)(end - ptr) < 1 + 3 * sizeof(float) + 16) {
        free(filtered);
        free(previous_indices);
        return 0;
    }

    *ptr++ = metrics_flag;
    memcpy(ptr, &cpu_user, sizeof(cpu_user));
    ptr += sizeof(cpu_user);
    memcpy(ptr, &cpu_system, sizeof(cpu_system));
    ptr += sizeof(cpu_system);
    memcpy(ptr, &cpu_idle, sizeof(cpu_idle));
    ptr += sizeof(cpu_idle);

    ptr[0] = (unsigned char)(process_count & 0xffu);
    ptr[1] = (unsigned char)((process_count >> 8) & 0xffu);
    ptr[2] = (unsigned char)((process_count >> 16) & 0xffu);
    ptr[3] = (unsigned char)((process_count >> 24) & 0xffu);
    ptr += 4;

    ptr[0] = (unsigned char)(visible_process_count & 0xffu);
    ptr[1] = (unsigned char)((visible_process_count >> 8) & 0xffu);
    ptr[2] = (unsigned char)((visible_process_count >> 16) & 0xffu);
    ptr[3] = (unsigned char)((visible_process_count >> 24) & 0xffu);
    ptr += 4;

    ptr[0] = (unsigned char)(thread_count & 0xffu);
    ptr[1] = (unsigned char)((thread_count >> 8) & 0xffu);
    ptr[2] = (unsigned char)((thread_count >> 16) & 0xffu);
    ptr[3] = (unsigned char)((thread_count >> 24) & 0xffu);
    ptr += 4;

    ptr[0] = (unsigned char)(logical_cpu_count_value & 0xffu);
    ptr[1] = (unsigned char)((logical_cpu_count_value >> 8) & 0xffu);
    ptr[2] = (unsigned char)((logical_cpu_count_value >> 16) & 0xffu);
    ptr[3] = (unsigned char)((logical_cpu_count_value >> 24) & 0xffu);
    ptr += 4;

    size_t *additional_indices = NULL;
    size_t additional_count = 0;
    if (previous_indices && filtered_count > 0) {
        additional_indices = malloc(sizeof(size_t) * filtered_count);
        if (!additional_indices) {
            free(filtered);
            free(previous_indices);
            free(additional_indices);
            return 0;
        }
        for (size_t i = 0; i < filtered_count; ++i) {
            if (i < window_start || i >= window_end) {
                int32_t prev = previous_indices[i];
                if (prev >= 0 && prev >= (int32_t)window_start && prev < (int32_t)window_end) {
                    additional_indices[additional_count++] = i;
                }
            }
        }
    }

    if ((size_t)(end - ptr) < sizeof(uint32_t)) {
        free(filtered);
        free(previous_indices);
        free(additional_indices);
        return 0;
    }
    uint32_t additional_count32 = (uint32_t)additional_count;
    ptr[0] = (unsigned char)(additional_count32 & 0xffu);
    ptr[1] = (unsigned char)((additional_count32 >> 8) & 0xffu);
    ptr[2] = (unsigned char)((additional_count32 >> 16) & 0xffu);
    ptr[3] = (unsigned char)((additional_count32 >> 24) & 0xffu);
    ptr += 4;

    for (size_t i = 0; i < additional_count; ++i) {
        uint32_t index_value = (uint32_t)additional_indices[i];
        if ((size_t)(end - ptr) < sizeof(uint32_t)) {
            free(filtered);
            free(previous_indices);
            free(additional_indices);
            return 0;
        }
        ptr[0] = (unsigned char)(index_value & 0xffu);
        ptr[1] = (unsigned char)((index_value >> 8) & 0xffu);
        ptr[2] = (unsigned char)((index_value >> 16) & 0xffu);
        ptr[3] = (unsigned char)((index_value >> 24) & 0xffu);
        ptr += 4;
        if (!append_process_frame(&ptr,
                                  end,
                                  &filtered[additional_indices[i]],
                                  previous_indices[additional_indices[i]])) {
            free(filtered);
            free(previous_indices);
            free(additional_indices);
            return 0;
        }
    }

    free(filtered);
    free(previous_indices);
    free(additional_indices);
    size_t frame_len = (size_t)(ptr - buffer);
    return frame_len;
}

#ifdef __APPLE__
static bool collect_open_files(int pid, OpenFileEntry **entries_out, size_t *count_out, char **error_out) {
    if (entries_out) {
        *entries_out = NULL;
    }
    if (count_out) {
        *count_out = 0;
    }
    if (error_out) {
        *error_out = NULL;
    }

    size_t fdinfo_capacity = 256;
    struct proc_fdinfo *fdinfos = NULL;
    int bytes = 0;
    while (true) {
        struct proc_fdinfo *new_fdinfos = realloc(fdinfos, sizeof(struct proc_fdinfo) * fdinfo_capacity);
        if (!new_fdinfos) {
            free(fdinfos);
            if (error_out) {
                *error_out = strdup("Unable to inspect process file descriptors.");
            }
            return false;
        }
        fdinfos = new_fdinfos;

        bytes = proc_pidinfo(pid,
                             PROC_PIDLISTFDS,
                             0,
                             fdinfos,
                             (int)(sizeof(struct proc_fdinfo) * fdinfo_capacity));
        if (bytes < (int)(sizeof(struct proc_fdinfo) * fdinfo_capacity)) {
            break;
        }
        if (fdinfo_capacity > SIZE_MAX / (sizeof(struct proc_fdinfo) * 2)) {
            break;
        }
        fdinfo_capacity *= 2;
    }
    if (bytes <= 0) {
        free(fdinfos);
        if (error_out) {
            if (errno == EPERM || errno == EACCES) {
                *error_out = strdup("Permission denied while collecting open files.");
            } else {
                *error_out = strdup("Unable to collect file descriptors.");
            }
        }
        return false;
    }

    int fd_count = bytes / (int)sizeof(struct proc_fdinfo);
    OpenFileEntry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    bool permission_denied = false;

    for (int i = 0; i < fd_count; ++i) {
        struct proc_fdinfo info = fdinfos[i];
        OpenFileEntry entry = {"", "", ""};
        snprintf(entry.descriptor, sizeof(entry.descriptor), "%d", info.proc_fd);

        bool added = false;
        switch (info.proc_fdtype) {
            case PROX_FDTYPE_VNODE:
                added = build_vnode_entry(pid, info.proc_fd, &entry, &permission_denied);
                break;
            case PROX_FDTYPE_SOCKET:
                added = build_socket_entry(pid, info.proc_fd, &entry, &permission_denied);
                break;
            default:
                snprintf(entry.type, sizeof(entry.type), "%s", fd_type_label(info.proc_fdtype));
                entry.name[0] = '\0';
                added = true;
                break;
        }

        if (added) {
            if (!append_open_file_entry(&entries, &entry_count, &entry_capacity, &entry)) {
                free(entries);
                free(fdinfos);
                if (error_out) {
                    *error_out = strdup("Failed to allocate open file entries.");
                }
                return false;
            }
        }
    }

    free(fdinfos);

    if (entry_count == 0) {
        free(entries);
        entries = NULL;
    } else if (entry_count < entry_capacity) {
        OpenFileEntry *resized = realloc(entries, entry_count * sizeof(OpenFileEntry));
        if (resized) {
            entries = resized;
        }
    }

    if (entries_out) {
        *entries_out = entries;
    } else if (entries) {
        free(entries);
    }

    if (count_out) {
        *count_out = entry_count;
    }

    if (error_out) {
        if (permission_denied) {
            *error_out = strdup("Permission denied while collecting open files.");
        } else {
            *error_out = NULL;
        }
    }

    return true;
}
#endif

#ifdef __linux__
static const char *linux_classify_fd_link(const char *link) {
    if (strncmp(link, "socket:", 7) == 0) {
        return "SOCKET";
    } else if (strncmp(link, "pipe:", 5) == 0) {
        return "PIPE";
    } else if (strncmp(link, "anon_inode:", 11) == 0) {
        const char *type = link + 11;
        if (strncmp(type, "[eventfd]", 9) == 0) {
            return "EVENTFD";
        } else if (strncmp(type, "[eventpoll]", 11) == 0) {
            return "EPOLL";
        } else if (strncmp(type, "[signalfd]", 10) == 0) {
            return "SIGNALFD";
        } else if (strncmp(type, "[timerfd]", 9) == 0) {
            return "TIMERFD";
        }
        return "ANON";
    } else if (link[0] == '/') {
        return "FILE";
    }
    return "OTHER";
}

static bool collect_open_files(int pid, OpenFileEntry **entries_out, size_t *count_out, char **error_out) {
    if (entries_out) {
        *entries_out = NULL;
    }
    if (count_out) {
        *count_out = 0;
    }
    if (error_out) {
        *error_out = NULL;
    }

    char fd_dir_path[64];
    snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);
    DIR *fd_dir = opendir(fd_dir_path);
    if (!fd_dir) {
        if (error_out) {
            if (errno == EACCES || errno == EPERM) {
                *error_out = strdup("Permission denied while collecting open files.");
            } else {
                *error_out = strdup("Unable to collect file descriptors.");
            }
        }
        return false;
    }

    OpenFileEntry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    bool permission_denied = false;
    struct dirent *dir_entry;

    while ((dir_entry = readdir(fd_dir)) != NULL) {
        if (dir_entry->d_name[0] == '.') {
            continue;
        }

        char *endptr;
        long fd_num = strtol(dir_entry->d_name, &endptr, 10);
        if (*endptr != '\0' || fd_num < 0) {
            continue;
        }

        char link_path[320];
        snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir_path, dir_entry->d_name);

        char link_target[MAX_OPEN_NAME_LEN];
        ssize_t link_len = readlink(link_path, link_target, sizeof(link_target) - 1);
        if (link_len < 0) {
            if (errno == EACCES || errno == EPERM) {
                permission_denied = true;
            }
            continue;
        }
        link_target[link_len] = '\0';

        OpenFileEntry entry = {"", "", ""};
        snprintf(entry.descriptor, sizeof(entry.descriptor), "%ld", fd_num);
        snprintf(entry.type, sizeof(entry.type), "%s", linux_classify_fd_link(link_target));
        strncpy(entry.name, link_target, sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';

        if (!append_open_file_entry(&entries, &entry_count, &entry_capacity, &entry)) {
            free(entries);
            closedir(fd_dir);
            if (error_out) {
                *error_out = strdup("Failed to allocate open file entries.");
            }
            return false;
        }
    }

    closedir(fd_dir);

    if (entry_count == 0) {
        free(entries);
        entries = NULL;
    } else if (entry_count < entry_capacity) {
        OpenFileEntry *resized = realloc(entries, entry_count * sizeof(OpenFileEntry));
        if (resized) {
            entries = resized;
        }
    }

    if (entries_out) {
        *entries_out = entries;
    } else if (entries) {
        free(entries);
    }

    if (count_out) {
        *count_out = entry_count;
    }

    if (error_out) {
        if (permission_denied) {
            *error_out = strdup("Permission denied while collecting open files.");
        } else {
            *error_out = NULL;
        }
    }

    return true;
}
#endif

static long parse_cpu_time_ms(const char *value) {
    if (!value) {
        return 0;
    }
    char temp[64];
    strncpy(temp, value, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    int segments[4] = {0};
    int count = 0;
    char *tok = strtok(temp, ":");
    while (tok && count < 4) {
        segments[count++] = atoi(tok);
        tok = strtok(NULL, ":");
    }
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (count == 3) {
        hours = segments[0];
        minutes = segments[1];
        seconds = segments[2];
    } else if (count == 2) {
        minutes = segments[0];
        seconds = segments[1];
    } else if (count == 1) {
        seconds = segments[0];
    }
    return (long)(hours * 3600 + minutes * 60 + seconds) * 1000L;
}

static bool fetch_process_detail(int pid,
                                 ProcessDetail *out,
                                 OpenFileEntry **files_out,
                                 size_t *file_count_out,
                                 char **files_error_out) {
    if (!out || pid <= 0) {
        return false;
    }

    char command[256];
    snprintf(command, sizeof(command),
             "ps -p %d -o pid=,ppid=,pcpu=,rss=,vsz=,time=,user=,comm=", pid);

    FILE *pipe = popen(command, "r");
    if (!pipe) {
        return false;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), pipe)) {
        pclose(pipe);
        return false;
    }
    pclose(pipe);

    ProcessDetail detail = {0};
    char time_buf[64] = {0};
    char user_buf[64] = {0};
    char command_buf[128] = {0};

    if (sscanf(line, "%d %d %lf %ld %ld %63s %63s %127s",
               &detail.pid,
               &detail.parent_pid,
               &detail.cpu_percent,
               &detail.rss_kb,
               &detail.vsz_kb,
               time_buf,
               user_buf,
               command_buf) < 8) {
        return false;
    }

    detail.cpu_time_ms = parse_cpu_time_ms(time_buf);
    strncpy(detail.user, user_buf, sizeof(detail.user) - 1);
#ifdef __linux__
    detail.is_kernel_thread = linux_process_is_kernel_thread(pid);
#endif

    // Short command (basename)
    if (!read_process_short_command(pid, detail.short_command, sizeof(detail.short_command))) {
        extract_short_command(command_buf, detail.short_command, sizeof(detail.short_command));
        if (detail.short_command[0] == '\0') {
            strncpy(detail.short_command, command_buf, sizeof(detail.short_command) - 1);
            detail.short_command[sizeof(detail.short_command) - 1] = '\0';
        }
    }

    // Full command line
    detail.command[0] = '\0';
    char cmd_query[128];
    snprintf(cmd_query, sizeof(cmd_query), "ps -p %d -o command=", pid);
    pipe = popen(cmd_query, "r");
    if (pipe) {
        if (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = '\0';
            strncpy(detail.command, line, sizeof(detail.command) - 1);
            detail.command[sizeof(detail.command) - 1] = '\0';
        }
        pclose(pipe);
    }
    if (detail.command[0] == '\0') {
        strncpy(detail.command, detail.short_command, sizeof(detail.command) - 1);
        detail.command[sizeof(detail.command) - 1] = '\0';
    }

    if (detail.short_command[0] == '\0') {
        extract_short_command(detail.command, detail.short_command, sizeof(detail.short_command));
    }

    // Thread count
    detail.thread_count = 0;
    int proc_threads = read_process_thread_count(pid);
    if (proc_threads > 0) {
        detail.thread_count = proc_threads;
    }

    // Launch time
    detail.launch_time[0] = '\0';
    if (!read_process_launch_time(pid, detail.launch_time, sizeof(detail.launch_time))) {
        snprintf(cmd_query, sizeof(cmd_query), "ps -p %d -o lstart=", pid);
        pipe = popen(cmd_query, "r");
        if (pipe) {
            if (fgets(line, sizeof(line), pipe)) {
                line[strcspn(line, "\r\n")] = '\0';
                struct tm tm = {0};
                if (strptime(line, "%a %b %d %H:%M:%S %Y", &tm)) {
                    strftime(detail.launch_time, sizeof(detail.launch_time), "%b %d, %Y %I:%M %p", &tm);
                } else {
                    strncpy(detail.launch_time, line, sizeof(detail.launch_time) - 1);
                    detail.launch_time[sizeof(detail.launch_time) - 1] = '\0';
                }
            }
            pclose(pipe);
        }
    }

    // Parent command
    detail.parent_short_command[0] = '\0';
    if (detail.parent_pid > 0 && detail.parent_pid != detail.pid) {
        if (!read_process_short_command(detail.parent_pid, detail.parent_short_command, sizeof(detail.parent_short_command))) {
            snprintf(cmd_query, sizeof(cmd_query), "ps -p %d -o command=", detail.parent_pid);
            pipe = popen(cmd_query, "r");
            if (pipe) {
                if (fgets(line, sizeof(line), pipe)) {
                    line[strcspn(line, "\r\n")] = '\0';
                    char parent_temp[sizeof(detail.parent_short_command)];
                    strncpy(parent_temp, line, sizeof(parent_temp) - 1);
                    parent_temp[sizeof(parent_temp) - 1] = '\0';
                    char *space = strchr(parent_temp, ' ');
                    if (space) {
                        *space = '\0';
                    }
                    char *slash_parent = strrchr(parent_temp, '/');
                    const char *parent_base = (slash_parent && slash_parent[1] != '\0') ? slash_parent + 1 : parent_temp;
                    strncpy(detail.parent_short_command, parent_base, sizeof(detail.parent_short_command) - 1);
                    detail.parent_short_command[sizeof(detail.parent_short_command) - 1] = '\0';
                }
                pclose(pipe);
            }
        }
    }

    OpenFileEntry *open_files = NULL;
    size_t open_file_count = 0;
    char *open_files_error = NULL;
    collect_open_files(pid, &open_files, &open_file_count, &open_files_error);

    *out = detail;
    if (files_out) {
        *files_out = open_files;
    } else {
        free(open_files);
    }
    if (file_count_out) {
        *file_count_out = open_file_count;
    }
    if (files_error_out) {
        *files_error_out = open_files_error;
    } else {
        free(open_files_error);
    }
    return true;
}

static void escape_json(const char *src, char *dst, size_t cap) {
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && out + 2 < cap; ++p) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            if (out + 2 >= cap) {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = (char)ch;
        } else if (ch >= 0x20 && ch < 0x7f) {
            dst[out++] = (char)ch;
        } else {
            if (out + 6 >= cap) {
                break;
            }
            snprintf(dst + out, cap - out, "\\u%04x", ch);
            out += 6;
        }
    }
    dst[out] = '\0';
}

static size_t build_outer_plugin_payload(const char *query, char *buffer, size_t cap) {
    if (!query || !*query || cap == 0) {
        return 0;
    }

    bool detail_mode = false;
    int pid = 0;

    const char *cursor = query;
    while (*cursor) {
        const char *amp = strchr(cursor, '&');
        size_t segment_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
        if (segment_len > 0) {
            const char *equals = memchr(cursor, '=', segment_len);
            if (equals && equals + 1 < cursor + segment_len) {
                size_t key_len = (size_t)(equals - cursor);
                size_t value_len = segment_len - key_len - 1;
                if (key_len > 0 && value_len > 0) {
                    char key[32];
                    char value[64];
                    size_t copy_key = key_len < sizeof(key) - 1 ? key_len : sizeof(key) - 1;
                    size_t copy_value = value_len < sizeof(value) - 1 ? value_len : sizeof(value) - 1;
                    memcpy(key, cursor, copy_key);
                    key[copy_key] = '\0';
                    memcpy(value, equals + 1, copy_value);
                    value[copy_value] = '\0';
                    for (char *p = key; *p; ++p) {
                        *p = (char)tolower((unsigned char)*p);
                    }
                    for (char *p = value; *p; ++p) {
                        if (*p == '+') {
                            *p = ' ';
                        }
                    }
                    if (strcmp(key, "mode") == 0) {
                        for (char *p = value; *p; ++p) {
                            *p = (char)tolower((unsigned char)*p);
                        }
                        if (strcmp(value, "detail") == 0) {
                            detail_mode = true;
                        }
                    } else if (strcmp(key, "pid") == 0) {
                        pid = atoi(value);
                    }
                }
            }
        }
        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }

    if (!detail_mode || pid <= 0) {
        return 0;
    }

    ProcessDetail detail;
    OpenFileEntry *open_files = NULL;
    size_t open_file_count = 0;
    char *open_files_error = NULL;
    if (!fetch_process_detail(pid, &detail, &open_files, &open_file_count, &open_files_error)) {
        return 0;
    }

    char full_command_escaped[256];
    char short_cmd_escaped[192];
    char user_escaped[192];
    escape_json(detail.command, full_command_escaped, sizeof(full_command_escaped));
    escape_json(detail.short_command, short_cmd_escaped, sizeof(short_cmd_escaped));
    escape_json(detail.user, user_escaped, sizeof(user_escaped));

    size_t len = 0;
    bool has_parent_short = detail.parent_short_command[0] != '\0';
    char parent_short_escaped[192];
    if (has_parent_short) {
        escape_json(detail.parent_short_command, parent_short_escaped, sizeof(parent_short_escaped));
    }

    bool has_launch_time = detail.launch_time[0] != '\0';
    char launch_escaped[128];
    if (has_launch_time) {
        escape_json(detail.launch_time, launch_escaped, sizeof(launch_escaped));
    }

    char parent_json[256];
    if (has_parent_short) {
        snprintf(parent_json, sizeof(parent_json), "\"%s\"", parent_short_escaped);
    } else {
        strcpy(parent_json, "null");
    }

    char launch_json[160];
    if (has_launch_time) {
        snprintf(launch_json, sizeof(launch_json), "\"%s\"", launch_escaped);
    } else {
        strcpy(launch_json, "null");
    }

    char thread_json[32];
    if (detail.thread_count > 0) {
        snprintf(thread_json, sizeof(thread_json), "%d", detail.thread_count);
    } else {
        strcpy(thread_json, "null");
    }

    int n = snprintf(buffer + len, cap - len,
                     "{\"mode\":\"detail\",\"detail\":{\"pid\":%d,\"detailAPIPath\":\"/api/processes/%d\","
                     "\"listAPIPath\":\"/api/processes\",\"parentPID\":%d,\"parentCommand\":%s,"
                     "\"command\":\"%s\",\"shortCommand\":\"%s\",\"commandLine\":\"%s\",\"isKernelThread\":%s",
                     detail.pid,
                     detail.pid,
                     detail.parent_pid,
                     parent_json,
                     short_cmd_escaped,
                     short_cmd_escaped,
                     full_command_escaped,
                     detail.is_kernel_thread ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - len) {
        len = 0;
        goto cleanup;
    }
    len += (size_t)n;

    n = snprintf(buffer + len, cap - len,
                 ",\"snapshot\":{\"pid\":%d,\"parentPID\":%d,\"parentCommand\":%s,\"command\":\"%s\","
                 "\"shortCommand\":\"%s\",\"commandLine\":\"%s\",\"cpuPercent\":%.2f,\"memoryKilobytes\":%ld,"
                 "\"isKernelThread\":%s,"
                 "\"cpuTimeMilliseconds\":%ld,\"user\":\"%s\",\"threadCount\":%s,\"launchTime\":%s,\"openFiles\":[",
                 detail.pid,
                 detail.parent_pid,
                 parent_json,
                 short_cmd_escaped,
                 short_cmd_escaped,
                 full_command_escaped,
                 detail.cpu_percent,
                 detail.rss_kb,
                 detail.is_kernel_thread ? "true" : "false",
                 detail.cpu_time_ms,
                 user_escaped,
                 thread_json,
                 launch_json);
    if (n < 0 || (size_t)n >= cap - len) {
        len = 0;
        goto cleanup;
    }
    len += (size_t)n;

    for (size_t i = 0; i < open_file_count; ++i) {
        if (len >= cap) {
            len = 0;
            goto cleanup;
        }
        if (i > 0) {
            if (len + 1 >= cap) {
                len = 0;
                goto cleanup;
            }
            buffer[len++] = ',';
        }
        char descriptor_escaped[128];
        char type_escaped[128];
        char name_escaped[256];
        escape_json(open_files[i].descriptor, descriptor_escaped, sizeof(descriptor_escaped));
        escape_json(open_files[i].type, type_escaped, sizeof(type_escaped));
        escape_json(open_files[i].name, name_escaped, sizeof(name_escaped));
        n = snprintf(buffer + len, cap - len,
                     "{\"descriptor\":\"%s\",\"type\":\"%s\",\"name\":\"%s\"}",
                     descriptor_escaped,
                     type_escaped,
                     name_escaped);
        if (n < 0 || (size_t)n >= cap - len) {
            len = 0;
            goto cleanup;
        }
        len += (size_t)n;
    }

    if (len + strlen("],\"openFilesError\":") >= cap) {
        len = 0;
        goto cleanup;
    }
    memcpy(buffer + len, "],\"openFilesError\":", strlen("],\"openFilesError\":"));
    len += strlen("],\"openFilesError\":");

    if (open_files_error) {
        char error_escaped[256];
        escape_json(open_files_error, error_escaped, sizeof(error_escaped));
        n = snprintf(buffer + len, cap - len, "\"%s\"", error_escaped);
        if (n < 0 || (size_t)n >= cap - len) {
            len = 0;
            goto cleanup;
        }
        len += (size_t)n;
    } else {
        if (len + 4 >= cap) {
            len = 0;
            goto cleanup;
        }
        memcpy(buffer + len, "null", 4);
        len += 4;
    }

    if (len + 3 >= cap) {
        len = 0;
        goto cleanup;
    }
    buffer[len++] = '}';
    buffer[len++] = '}';
    buffer[len++] = '}';

cleanup:
    free(open_files);
    free(open_files_error);
    return len;
}

static bool client_init(StreamClient *client, int fd) {
    if (!client) {
        return false;
    }
    memset(client, 0, sizeof(*client));
    client->fd = fd;
    client->state = CONN_STATE_READING_REQUEST;
    client->frame_buffer = malloc(STREAM_BUFFER_SIZE);
    if (!client->frame_buffer) {
        memset(client, 0, sizeof(*client));
        return false;
    }
    return true;
}

static void client_destroy(StreamClient *client) {
    if (!client) {
        return;
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client->sent_rows);
    free(client->write_buffer);
    free(client->frame_buffer);
    reset_order_cache(&client->order_cache);
    memset(client, 0, sizeof(*client));
}

static bool client_registry_ensure_capacity(ClientRegistry *registry, size_t needed) {
    if (!registry) {
        return false;
    }
    if (registry->capacity >= needed) {
        return true;
    }

    size_t new_capacity = registry->capacity == 0 ? 16 : registry->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    ClientSlot *new_slots = realloc(registry->slots, sizeof(ClientSlot) * new_capacity);
    if (!new_slots) {
        return false;
    }
    memset(new_slots + registry->capacity, 0, sizeof(ClientSlot) * (new_capacity - registry->capacity));
    registry->slots = new_slots;
    registry->capacity = new_capacity;
    return true;
}

static StreamClient *client_registry_add(int fd) {
    if (!client_registry_ensure_capacity(&g_clients, g_clients.count + 1)) {
        return NULL;
    }

    for (size_t i = 0; i < g_clients.capacity; ++i) {
        ClientSlot *slot = &g_clients.slots[i];
        if (slot->exists) {
            continue;
        }
        memset(slot, 0, sizeof(*slot));
        slot->exists = true;
        if (!client_init(&slot->client, fd)) {
            memset(slot, 0, sizeof(*slot));
            return NULL;
        }
        g_clients.count++;
        return &slot->client;
    }

    return NULL;
}

static bool client_registry_remove(StreamClient *client) {
    if (!client) {
        return false;
    }

    for (size_t i = 0; i < g_clients.capacity; ++i) {
        ClientSlot *slot = &g_clients.slots[i];
        if (!slot->exists || &slot->client != client) {
            continue;
        }
        client_destroy(&slot->client);
        memset(slot, 0, sizeof(*slot));
        if (g_clients.count > 0) {
            g_clients.count--;
        }
        return true;
    }

    return false;
}

static StreamClient *find_client_by_id(const char *client_id) {
    if (!client_id || client_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < g_clients.capacity; ++i) {
        ClientSlot *slot = &g_clients.slots[i];
        if (slot->exists && strcmp(slot->client.client_id, client_id) == 0) {
            return &slot->client;
        }
    }
    return NULL;
}

static bool poll_state_init(PollState *state, int listen_fd) {
    if (!state) {
        return false;
    }
    state->capacity = 64;
    state->fds = calloc(state->capacity, sizeof(struct pollfd));
    state->client_indices = calloc(state->capacity, sizeof(size_t));
    if (!state->fds || !state->client_indices) {
        free(state->fds);
        free(state->client_indices);
        return false;
    }
    state->fds[0].fd = listen_fd;
    state->fds[0].events = POLLIN;
    state->client_indices[0] = (size_t)-1;
    state->count = 1;
    state->listen_fd_index = 0;
    return true;
}

static bool poll_state_ensure_capacity(size_t needed) {
    if (g_poll.capacity >= needed) {
        return true;
    }

    size_t new_capacity = g_poll.capacity == 0 ? 64 : g_poll.capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    struct pollfd *new_fds = realloc(g_poll.fds, sizeof(struct pollfd) * new_capacity);
    if (!new_fds) {
        return false;
    }
    g_poll.fds = new_fds;

    size_t *new_indices = realloc(g_poll.client_indices, sizeof(size_t) * new_capacity);
    if (!new_indices) {
        return false;
    }
    g_poll.client_indices = new_indices;
    g_poll.capacity = new_capacity;
    return true;
}

static void poll_rebuild(void) {
    g_poll.count = 1;
    for (size_t i = 0; i < g_clients.capacity; ++i) {
        ClientSlot *slot = &g_clients.slots[i];
        if (!slot->exists) {
            continue;
        }
        StreamClient *c = &slot->client;
        if (c->state == CONN_STATE_CLOSING) {
            continue;
        }
        if (!poll_state_ensure_capacity(g_poll.count + 1)) {
            break;
        }
        size_t idx = g_poll.count++;
        g_poll.fds[idx].fd = c->fd;
        g_poll.fds[idx].revents = 0;
        short events = POLLIN;
        if (c->write_buffer_used > c->write_buffer_sent) {
            events |= POLLOUT;
        }
        g_poll.fds[idx].events = events;
        g_poll.client_indices[idx] = i;
    }
}

static bool queue_write_data(StreamClient *client, const void *data, size_t len) {
    if (!client || !data || len == 0) {
        return false;
    }
    size_t needed = client->write_buffer_used + len;
    if (needed > client->write_buffer_capacity) {
        size_t new_cap = client->write_buffer_capacity == 0 ? WRITE_BUFFER_INITIAL_SIZE : client->write_buffer_capacity;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        unsigned char *new_buf = realloc(client->write_buffer, new_cap);
        if (!new_buf) {
            return false;
        }
        client->write_buffer = new_buf;
        client->write_buffer_capacity = new_cap;
    }
    memcpy(client->write_buffer + client->write_buffer_used, data, len);
    client->write_buffer_used += len;
    return true;
}

static void flush_write_buffer(StreamClient *client) {
    if (!client || client->write_buffer_sent >= client->write_buffer_used) {
        return;
    }
    size_t remaining = client->write_buffer_used - client->write_buffer_sent;
    ssize_t written = send(client->fd,
                           client->write_buffer + client->write_buffer_sent,
                           remaining,
                           MSG_NOSIGNAL);
    if (written > 0) {
        client->write_buffer_sent += (size_t)written;
        if (client->write_buffer_sent >= client->write_buffer_used) {
            client->write_buffer_sent = 0;
            client->write_buffer_used = 0;
        }
    } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        client->state = CONN_STATE_CLOSING;
    }
}

static void queue_response(StreamClient *client, int status, const char *status_text,
                           const char *ctype, const void *body, size_t body_len,
                           bool close_after) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "\r\n",
                     status, status_text, ctype, body_len,
                     close_after ? "close" : "keep-alive");
    if (n > 0 && (size_t)n < sizeof(header)) {
        queue_write_data(client, header, (size_t)n);
    }
    if (body && body_len > 0) {
        queue_write_data(client, body, body_len);
    }
    client->close_after_write = close_after;
    client->state = CONN_STATE_WRITING_RESPONSE;
}

static void queue_plain_text_response(StreamClient *client, int status, const char *msg, bool close_after) {
    const char *status_text = (status == 200) ? "OK" :
                              (status == 400) ? "Bad Request" :
                              (status == 404) ? "Not Found" :
                              (status == 500) ? "Internal Server Error" :
                              (status == 503) ? "Service Unavailable" : "Error";
    queue_response(client, status, status_text, "text/plain; charset=utf-8",
                   msg, strlen(msg), close_after);
}

static void queue_json_response(StreamClient *client, const char *json, size_t len, bool close_after) {
    queue_response(client, 200, "OK", "application/json; charset=utf-8", json, len, close_after);
}

static void write_outer_uint32_le(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
}

static void write_outer_uint64_le(unsigned char *bytes, uint64_t value) {
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
    bytes[4] = (unsigned char)((value >> 32) & 0xff);
    bytes[5] = (unsigned char)((value >> 40) & 0xff);
    bytes[6] = (unsigned char)((value >> 48) & 0xff);
    bytes[7] = (unsigned char)((value >> 56) & 0xff);
}

static void queue_outer_descriptor(StreamClient *client, const char *query) {
    char plugin_data[JSON_BUFFER_SIZE];
    size_t plugin_len = build_outer_plugin_payload(query, plugin_data, sizeof(plugin_data));

    size_t path_len = strlen(kBundleUrlPath);
    size_t header_len = 40;
    size_t data_offset = header_len + path_len;
    size_t total_len = data_offset + plugin_len;
    unsigned char *payload = malloc(total_len);
    if (!payload) {
        queue_plain_text_response(client, 500, "out of memory\n", true);
        return;
    }

    payload[0] = 'O';
    payload[1] = 'U';
    payload[2] = 'T';
    payload[3] = 'R';
    write_outer_uint32_le(payload + 4, 1);
    write_outer_uint64_le(payload + 8, (uint64_t)header_len);
    write_outer_uint64_le(payload + 16, (uint64_t)path_len);
    write_outer_uint64_le(payload + 24, (uint64_t)data_offset);
    write_outer_uint64_le(payload + 32, (uint64_t)plugin_len);
    memcpy(payload + header_len, kBundleUrlPath, path_len);
    if (plugin_len > 0) {
        memcpy(payload + data_offset, plugin_data, plugin_len);
    }

    queue_response(client, 200, "OK", "application/vnd.outerframe",
                   payload, total_len, true);
    free(payload);
}

static void queue_bundle_file(StreamClient *client, const char *path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        char message[4096 + 64];
        snprintf(message, sizeof(message), "bundle not found at %s\n", path);
        queue_plain_text_response(client, 404, message, true);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        queue_plain_text_response(client, 500, "failed to stat bundle\n", true);
        return;
    }

    size_t file_size = (size_t)st.st_size;
    unsigned char *file_data = malloc(file_size);
    if (!file_data) {
        close(file_fd);
        queue_plain_text_response(client, 500, "out of memory\n", true);
        return;
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t n = read(file_fd, file_data + total_read, file_size - total_read);
        if (n <= 0) {
            break;
        }
        total_read += (size_t)n;
    }
    close(file_fd);

    if (total_read != file_size) {
        free(file_data);
        queue_plain_text_response(client, 500, "failed to read bundle\n", true);
        return;
    }

    queue_response(client, 200, "OK", "application/octet-stream",
                   file_data, file_size, true);
    free(file_data);
}

static void queue_history_response(StreamClient *client, const char *query) {
    ProcessWindowRequest request;
    apply_default_window_request(&request);
    if (query && *query) {
        apply_query_to_window_request(&request, query);
    }

    ProcessSnapshotView snapshot_view = (ProcessSnapshotView){0};
    if (!build_process_snapshot_view(&snapshot_view, &request)) {
        queue_plain_text_response(client, 500, "failed to build history view\n", true);
        return;
    }

    unsigned char *frame_buffer = malloc(STREAM_BUFFER_SIZE);
    if (!frame_buffer) {
        free_process_snapshot(&snapshot_view);
        queue_plain_text_response(client, 500, "out of memory\n", true);
        return;
    }

    size_t frame_len = encode_process_stream_frame(frame_buffer,
                                                   STREAM_BUFFER_SIZE,
                                                   &snapshot_view,
                                                   &request,
                                                   NULL);
    free_process_snapshot(&snapshot_view);
    if (frame_len == 0) {
        free(frame_buffer);
        queue_plain_text_response(client, 500, "failed to encode history\n", true);
        return;
    }

    queue_response(client, 200, "OK", "application/octet-stream",
                   frame_buffer, frame_len, true);
    free(frame_buffer);
}

static void queue_cpu_history_response(StreamClient *client) {
    char *json = malloc(JSON_BUFFER_SIZE);
    if (!json) {
        queue_plain_text_response(client, 500, "out of memory\n", true);
        return;
    }
    size_t len = build_cpu_history_json(json, JSON_BUFFER_SIZE);
    if (len == 0) {
        free(json);
        queue_plain_text_response(client, 500, "failed to build cpu history\n", true);
        return;
    }
    queue_json_response(client, json, len, true);
    free(json);
}

static void generate_and_queue_frame(StreamClient *client) {

    ProcessSnapshotView snapshot_view = {0};
    if (!build_process_snapshot_view(&snapshot_view, &client->request)) {
        return;
    }

    ProcessOrderContext current_context = make_order_context(&client->request);
    if (!client->has_last_sent_context ||
        !order_context_equal(&current_context, &client->last_sent_context)) {
        reset_order_cache(&client->order_cache);
        client->has_last_sent_context = true;
        client->last_sent_context = current_context;
        client->sent_rows_valid = false;
    }

    if (!client->sent_rows_valid ||
        client->sent_snapshot_index != snapshot_view.snapshot_index ||
        snapshot_view.entry_count > client->sent_rows_capacity) {
        if (snapshot_view.entry_count > client->sent_rows_capacity) {
            free(client->sent_rows);
            client->sent_rows = calloc(snapshot_view.entry_count, sizeof(bool));
            if (!client->sent_rows) {
                client->sent_rows_capacity = 0;
                client->sent_rows_count = 0;
                client->sent_rows_valid = false;
            } else {
                client->sent_rows_capacity = snapshot_view.entry_count;
            }
        }
        if (client->sent_rows && snapshot_view.entry_count <= client->sent_rows_capacity) {
            memset(client->sent_rows, 0, sizeof(bool) * snapshot_view.entry_count);
            client->sent_rows_count = snapshot_view.entry_count;
            client->sent_rows_valid = true;
            client->sent_snapshot_index = snapshot_view.snapshot_index;
        }
    }

    size_t desired_start = client->request.window_start < snapshot_view.entry_count
        ? client->request.window_start : snapshot_view.entry_count;
    size_t desired_end = client->request.window_end < snapshot_view.entry_count
        ? client->request.window_end : snapshot_view.entry_count;
    if (desired_end < desired_start) {
        desired_end = desired_start;
    }

    if (!client->sent_rows_valid && snapshot_view.entry_count == 0) {
        free_process_snapshot(&snapshot_view);
        return;
    }

    size_t send_start = desired_start;
    size_t send_end = desired_end;
    if (client->sent_rows_valid && !client->needs_immediate_frame) {
        send_start = desired_end;
        for (size_t i = desired_start; i < desired_end; ++i) {
            if (!client->sent_rows[i]) {
                send_start = i;
                break;
            }
        }
        if (send_start == desired_end) {
            free_process_snapshot(&snapshot_view);
            return;
        }
        size_t i = send_start + 1;
        while (i < desired_end && !client->sent_rows[i]) {
            ++i;
        }
        send_end = i;
    }
    client->needs_immediate_frame = false;

    ProcessWindowRequest send_request = client->request;
    send_request.window_start = send_start;
    send_request.window_end = send_end;

    size_t frame_len = encode_process_stream_frame(client->frame_buffer,
                                                   STREAM_BUFFER_SIZE,
                                                   &snapshot_view,
                                                   &send_request,
                                                   &client->order_cache);
    free_process_snapshot(&snapshot_view);

    if (frame_len == 0) {
        return;
    }

    if (client->sent_rows_valid) {
        size_t mark_end = send_request.window_end < client->sent_rows_count
            ? send_request.window_end : client->sent_rows_count;
        for (size_t i = send_request.window_start; i < mark_end; ++i) {
            client->sent_rows[i] = true;
        }
    }

    queue_write_data(client, client->frame_buffer, frame_len);
}

static void broadcast_frames_to_streaming_clients(void) {
    for (size_t i = 0; i < g_clients.capacity; ++i) {
        if (g_clients.slots[i].exists
            && g_clients.slots[i].client.state == CONN_STATE_STREAMING) {
            generate_and_queue_frame(&g_clients.slots[i].client);
        }
    }
}

static void handle_viewport_for_stream(StreamClient *request_client, const char *query) {
    char client_id[MAX_CLIENT_ID_LEN];
    client_id[0] = '\0';
    if (!query || !copy_query_value(query, "clientId", client_id, sizeof(client_id)) ||
        client_id[0] == '\0') {
        queue_plain_text_response(request_client, 400, "missing clientId\n", true);
        return;
    }

    StreamClient *stream_client = find_client_by_id(client_id);
    if (!stream_client || stream_client->state != CONN_STATE_STREAMING) {
        queue_plain_text_response(request_client, 404, "stream client not found\n", true);
        return;
    }

    apply_query_to_window_request(&stream_client->request, query);
    stream_client->needs_immediate_frame = true;
    generate_and_queue_frame(stream_client);

    queue_plain_text_response(request_client, 200, "ok\n", true);
}

static void handle_process_signal_request(StreamClient *client, const char *query, int signal_number) {
    if (!client || !query) {
        queue_plain_text_response(client, 400, "missing pid\n", true);
        return;
    }

    char pid_value[32];
    pid_value[0] = '\0';
    if (!copy_query_value(query, "pid", pid_value, sizeof(pid_value)) || pid_value[0] == '\0') {
        queue_plain_text_response(client, 400, "missing pid\n", true);
        return;
    }

    char *end = NULL;
    long pid_long = strtol(pid_value, &end, 10);
    if (!end || *end != '\0' || pid_long <= 0 || pid_long > INT_MAX) {
        queue_plain_text_response(client, 400, "invalid pid\n", true);
        return;
    }

    pid_t pid = (pid_t)pid_long;
    if (kill(pid, signal_number) == 0) {
        queue_plain_text_response(client, 200, "ok\n", true);
        return;
    }

    switch (errno) {
        case ESRCH:
            queue_plain_text_response(client, 404, "process not found\n", true);
            return;
        case EPERM:
            queue_plain_text_response(client, 403, "permission denied\n", true);
            return;
        case EINVAL:
            queue_plain_text_response(client, 400, "invalid signal\n", true);
            return;
        default:
            queue_plain_text_response(client, 500, strerror(errno), true);
            return;
    }
}

static void queue_process_detail_response(StreamClient *client, int pid) {
    ProcessDetail detail;
    OpenFileEntry *open_files = NULL;
    size_t open_count = 0;
    char *open_error = NULL;
    if (!fetch_process_detail(pid, &detail, &open_files, &open_count, &open_error)) {
        queue_plain_text_response(client, 404, "process not found\n", true);
        return;
    }

    char json[JSON_BUFFER_SIZE];
    size_t len = 0;
    double timestamp = current_time_seconds();
    char user_escaped[192];
    char command_escaped[256];
    char short_cmd_escaped[192];
    char parent_short_escaped[192];
    char launch_escaped[128];
    escape_json(detail.user, user_escaped, sizeof(user_escaped));
    escape_json(detail.command, command_escaped, sizeof(command_escaped));
    escape_json(detail.short_command, short_cmd_escaped, sizeof(short_cmd_escaped));
    bool has_parent_short = detail.parent_short_command[0] != '\0';
    if (has_parent_short) {
        escape_json(detail.parent_short_command, parent_short_escaped, sizeof(parent_short_escaped));
    }
    bool has_launch_time = detail.launch_time[0] != '\0';
    if (has_launch_time) {
        escape_json(detail.launch_time, launch_escaped, sizeof(launch_escaped));
    }

    char launch_json[160];
    if (has_launch_time) {
        snprintf(launch_json, sizeof(launch_json), "\"%s\"", launch_escaped);
    } else {
        strcpy(launch_json, "null");
    }

    char parent_json[256];
    if (has_parent_short) {
        snprintf(parent_json, sizeof(parent_json), "\"%s\"", parent_short_escaped);
    } else {
        strcpy(parent_json, "null");
    }

    char thread_json[32];
    if (detail.thread_count > 0) {
        snprintf(thread_json, sizeof(thread_json), "%d", detail.thread_count);
    } else {
        strcpy(thread_json, "null");
    }

    int n = snprintf(json + len, sizeof(json) - len,
                     "{\"timestamp\":%.3f,\"process\":{\"pid\":%d,\"parentPID\":%d,\"cpuPercent\":%.2f,"
                     "\"memoryKilobytes\":%ld,\"virtualMemoryKilobytes\":%ld,\"cpuTimeMilliseconds\":%ld,"
                     "\"isKernelThread\":%s,"
                     "\"user\":\"%s\",\"command\":\"%s\",\"shortCommand\":\"%s\",\"commandLine\":\"%s\",\"threadCount\":%s,"
                     "\"launchTime\":%s,\"parentShortCommand\":%s,\"openFiles\":[",
                     timestamp,
                     detail.pid,
                     detail.parent_pid,
                     detail.cpu_percent,
                     detail.rss_kb,
                     detail.vsz_kb,
                     detail.cpu_time_ms,
                     detail.is_kernel_thread ? "true" : "false",
                     user_escaped,
                     command_escaped,
                     short_cmd_escaped,
                     command_escaped,
                     thread_json,
                     launch_json,
                     parent_json);
    if (n < 0 || (size_t)n >= sizeof(json) - len) {
        free(open_files);
        free(open_error);
        queue_plain_text_response(client, 500, "failed to build detail json\n", true);
        return;
    }
    len += (size_t)n;

    for (size_t i = 0; i < open_count; ++i) {
        if (i > 0) {
            json[len++] = ',';
        }
        char descriptor_escaped[128];
        char type_escaped[128];
        char name_escaped[256];
        escape_json(open_files[i].descriptor, descriptor_escaped, sizeof(descriptor_escaped));
        escape_json(open_files[i].type, type_escaped, sizeof(type_escaped));
        escape_json(open_files[i].name, name_escaped, sizeof(name_escaped));
        n = snprintf(json + len, sizeof(json) - len,
                     "{\"descriptor\":\"%s\",\"type\":\"%s\",\"name\":\"%s\"}",
                     descriptor_escaped,
                     type_escaped,
                     name_escaped);
        if (n < 0 || (size_t)n >= sizeof(json) - len) {
            free(open_files);
            free(open_error);
            queue_plain_text_response(client, 500, "failed to build detail json\n", true);
            return;
        }
        len += (size_t)n;
    }

    if (len + strlen("],\"openFilesError\":") >= sizeof(json)) {
        free(open_files);
        free(open_error);
        queue_plain_text_response(client, 500, "failed to build detail json\n", true);
        return;
    }
    memcpy(json + len, "],\"openFilesError\":", strlen("],\"openFilesError\":"));
    len += strlen("],\"openFilesError\":");

    if (open_error) {
        char error_escaped[256];
        escape_json(open_error, error_escaped, sizeof(error_escaped));
        n = snprintf(json + len, sizeof(json) - len, "\"%s\"", error_escaped);
        if (n < 0 || (size_t)n >= sizeof(json) - len) {
            free(open_files);
            free(open_error);
            queue_plain_text_response(client, 500, "failed to build detail json\n", true);
            return;
        }
        len += (size_t)n;
    } else {
        memcpy(json + len, "null", 4);
        len += 4;
    }

    if (len + 2 >= sizeof(json)) {
        free(open_files);
        free(open_error);
        queue_plain_text_response(client, 500, "failed to build detail json\n", true);
        return;
    }
    json[len++] = '}';
    json[len++] = '}';

    free(open_files);
    free(open_error);
    queue_json_response(client, json, len, true);
}

static void process_client_request(StreamClient *client) {
    char *request = client->read_buffer;
    char *line_end = memmem(request, client->read_buffer_used, "\r\n", 2);
    if (!line_end) {
        queue_plain_text_response(client, 400, "bad request\n", true);
        return;
    }
    *line_end = '\0';

    char method[16];
    char target[1024];
    char version[16];
    if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3) {
        queue_plain_text_response(client, 400, "bad request\n", true);
        return;
    }

    bool head_only = (strcasecmp(method, "HEAD") == 0);
    if (!head_only && !(strcasecmp(method, "GET") == 0)) {
        queue_plain_text_response(client, 400, "unsupported method\n", true);
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(target, "/") == 0 || strcmp(target, "/index") == 0 ||
        strcmp(target, "/outer/process-monitor.outer") == 0) {
        queue_outer_descriptor(client, query);
        return;
    }
    if (strcmp(target, kBundleUrlPath) == 0) {
        queue_plain_text_response(client, 200, "macos-arm\nmacos-x86\n", true);
        return;
    }
    if (strcmp(target, kBundleUrlPathMacosArm) == 0) {
        const char *path = g_bundle_file_path_macos_arm[0] ? g_bundle_file_path_macos_arm : kBundleFilePathMacosArm;
        queue_bundle_file(client, path);
        return;
    }
    if (strcmp(target, kBundleUrlPathMacosX86) == 0) {
        const char *path = g_bundle_file_path_macos_x86[0] ? g_bundle_file_path_macos_x86 : kBundleFilePathMacosX86;
        queue_bundle_file(client, path);
        return;
    }
    static const char process_api_prefix[] = "/api/processes/";
    const size_t process_api_prefix_length = sizeof(process_api_prefix) - 1;
    if (strncmp(target, process_api_prefix, process_api_prefix_length) == 0) {
        const char *process_api_target = target + process_api_prefix_length;

        if (strcmp(process_api_target, "viewport") == 0) {
            if (head_only) {
                queue_plain_text_response(client, 400, "unsupported method\n", true);
                return;
            }
            handle_viewport_for_stream(client, query);
            return;
        }

        if (strcmp(process_api_target, "history") == 0) {
            if (head_only) {
                queue_plain_text_response(client, 400, "unsupported method\n", true);
                return;
            }
            queue_history_response(client, query);
            return;
        }

        if (strcmp(process_api_target, "cpu_history") == 0) {
            if (head_only) {
                queue_plain_text_response(client, 400, "unsupported method\n", true);
                return;
            }
            queue_cpu_history_response(client);
            return;
        }

        if (strcmp(process_api_target, "quit") == 0) {
            if (head_only) {
                queue_plain_text_response(client, 400, "unsupported method\n", true);
                return;
            }
            handle_process_signal_request(client, query, SIGTERM);
            return;
        }

        if (strcmp(process_api_target, "force-quit") == 0) {
            if (head_only) {
                queue_plain_text_response(client, 400, "unsupported method\n", true);
                return;
            }
            handle_process_signal_request(client, query, SIGKILL);
            return;
        }

        if (strcmp(process_api_target, "stream") == 0) {
            if (!query
                || !copy_query_value(query, "clientId", client->client_id, sizeof(client->client_id))
                || client->client_id[0] == '\0') {
                queue_plain_text_response(client, 400, "missing clientId\n", true);
                return;
            }

            apply_default_window_request(&client->request);
            apply_query_to_window_request(&client->request, query);

            const char *header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Cache-Control: no-store\r\n"
                "Connection: keep-alive\r\n"
                "\r\n";
            queue_write_data(client, header, strlen(header));

            client->state = CONN_STATE_STREAMING;
            client->needs_immediate_frame = true;
            log_debug("stream client started fd=%d clientId=%s", client->fd, client->client_id);
            return;
        }

        if (strspn(process_api_target, "0123456789") == strlen(process_api_target)) {
            int pid = atoi(process_api_target);
            queue_process_detail_response(client, pid);
            return;
        }
    }

    queue_plain_text_response(client, 404, "not found\n", true);
}

static void handle_accept(int listen_fd) {
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return;
    }

    if (!set_blocking(client_fd, false)) {
        close(client_fd);
        return;
    }

    StreamClient *client = client_registry_add(client_fd);
    if (!client) {
        close(client_fd);
        return;
    }

    log_debug("accepted client fd=%d", client_fd);
}

static void handle_client_read(StreamClient *client) {
    if (client->state == CONN_STATE_STREAMING) {
        char buf[1];
        ssize_t n = recv(client->fd, buf, sizeof(buf), MSG_PEEK);
        if (n == 0) {
            log_debug("streaming client disconnected fd=%d", client->fd);
            client->state = CONN_STATE_CLOSING;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            client->state = CONN_STATE_CLOSING;
        }
        return;
    }

    if (client->state != CONN_STATE_READING_REQUEST) {
        return;
    }

    size_t remaining = sizeof(client->read_buffer) - client->read_buffer_used - 1;
    if (remaining == 0) {
        client->state = CONN_STATE_CLOSING;
        return;
    }

    ssize_t n = recv(client->fd,
                     client->read_buffer + client->read_buffer_used,
                     remaining, 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            client->state = CONN_STATE_CLOSING;
        }
        return;
    }
    if (n == 0) {
        client->state = CONN_STATE_CLOSING;
        return;
    }

    client->read_buffer_used += (size_t)n;
    client->read_buffer[client->read_buffer_used] = '\0';

    if (memmem(client->read_buffer, client->read_buffer_used, "\r\n\r\n", 4)) {
        process_client_request(client);
        client->read_buffer_used = 0;
    }
}

static void handle_client_write(StreamClient *client) {
    flush_write_buffer(client);

    if (client->write_buffer_sent >= client->write_buffer_used) {
        if (client->close_after_write) {
            client->state = CONN_STATE_CLOSING;
        } else if (client->state == CONN_STATE_WRITING_RESPONSE) {
            client->state = CONN_STATE_READING_REQUEST;
        }
    }
}

static void handle_client_event(StreamClient *client, short revents) {
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        client->state = CONN_STATE_CLOSING;
        return;
    }

    if (revents & POLLIN) {
        handle_client_read(client);
    }

    if ((revents & POLLOUT) && client->state != CONN_STATE_CLOSING) {
        handle_client_write(client);
    }
}

static void remove_closing_clients(void) {
    for (size_t i = 0; i < g_clients.capacity; ++i) {
        ClientSlot *slot = &g_clients.slots[i];
        if (!slot->exists) {
            continue;
        }
        StreamClient *c = &slot->client;
        if (c->state == CONN_STATE_CLOSING) {
            log_debug("removing client fd=%d", c->fd);
            client_registry_remove(c);
        }
    }
}

static char g_outerctl_path[PATH_MAX] = {0};
static char g_app_icon_path[PATH_MAX] = {0};
static char g_backend_label[256] = {0};
static char g_backend_identifier[256] = {0};
static char g_listen_socket_path[PATH_MAX] = {0};
static int g_listen_port = -1;
static volatile sig_atomic_t g_shutdown_requested = 0;

static const char *kAppName = "Top";

static const char *backend_announcement_identifier(void) {
    if (g_backend_label[0] != '\0') {
        return g_backend_label;
    }
    if (g_backend_identifier[0] != '\0') {
        return g_backend_identifier;
    }
    return NULL;
}

static int ensure_directory_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    char temp[PATH_MAX];
    snprintf(temp, sizeof(temp), "%s", path);
    size_t length = strlen(temp);
    for (size_t i = 1; i <= length; ++i) {
        if (temp[i] != '/' && temp[i] != '\0') {
            continue;
        }
        char saved = temp[i];
        temp[i] = '\0';
        if (temp[0] != '\0' && mkdir(temp, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
        temp[i] = saved;
    }
    return 0;
}

static void run_outerctl_announcement(const char *action, int port, const char *socket_path) {
    const char *backend_id = backend_announcement_identifier();
    if (g_outerctl_path[0] == '\0' || !backend_id) {
        return;
    }

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%d", port);
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        log_debug("Failed to create outerctl pipe: %s", strerror(errno));
        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
    }

    pid_t child = fork();
    if (child == 0) {
        if (pipe_fds[0] >= 0) {
            close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);
        }

        const char *arguments[20];
        size_t argument_count = 0;
        arguments[argument_count++] = g_outerctl_path;
        if (strcmp(action, "ADD") == 0) {
            arguments[argument_count++] = "app";
            arguments[argument_count++] = "add";
            arguments[argument_count++] = "--backend";
            arguments[argument_count++] = backend_id;
            if (socket_path && socket_path[0] != '\0') {
                arguments[argument_count++] = "--socket-path";
                arguments[argument_count++] = socket_path;
            } else if (port >= 0 && port <= 65535) {
                arguments[argument_count++] = "--port";
                arguments[argument_count++] = port_buffer;
            } else {
                _exit(127);
            }
            arguments[argument_count++] = "--name";
            arguments[argument_count++] = kAppName;
            arguments[argument_count++] = "--url";
            arguments[argument_count++] = "/";
            if (g_app_icon_path[0] != '\0') {
                arguments[argument_count++] = "--icon-file";
                arguments[argument_count++] = g_app_icon_path;
            }
        } else if (strcmp(action, "REMOVE") == 0) {
            arguments[argument_count++] = "app";
            arguments[argument_count++] = "remove";
            arguments[argument_count++] = "--backend";
            arguments[argument_count++] = backend_id;
            if (socket_path && socket_path[0] != '\0') {
                arguments[argument_count++] = "--socket-path";
                arguments[argument_count++] = socket_path;
            } else if (port >= 0 && port <= 65535) {
                arguments[argument_count++] = "--port";
                arguments[argument_count++] = port_buffer;
            } else {
                _exit(127);
            }
        } else {
            _exit(127);
        }

        arguments[argument_count] = NULL;
        execv(g_outerctl_path, (char *const *)arguments);
        _exit(127);
    }

    if (child > 0) {
        if (pipe_fds[1] >= 0) {
            close(pipe_fds[1]);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        char output[4096];
        size_t output_length = 0;
        if (pipe_fds[0] >= 0) {
            while (output_length + 1 < sizeof(output)) {
                ssize_t bytes_read = read(pipe_fds[0],
                                          output + output_length,
                                          sizeof(output) - output_length - 1);
                if (bytes_read > 0) {
                    output_length += (size_t)bytes_read;
                    continue;
                }
                if (bytes_read == 0) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            close(pipe_fds[0]);
        }
        output[output_length] = '\0';
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            if (output_length > 0) {
                log_debug("outerctl %s for %s completed with output: %s",
                          action,
                          backend_id,
                          output);
            }
        } else if (WIFEXITED(status)) {
            log_debug("outerctl %s for %s failed with exit status %d%s%s",
                      action,
                      backend_id,
                      WEXITSTATUS(status),
                      output_length > 0 ? ": " : "",
                      output_length > 0 ? output : "");
        } else if (WIFSIGNALED(status)) {
            log_debug("outerctl %s for %s terminated by signal %d%s%s",
                      action,
                      backend_id,
                      WTERMSIG(status),
                      output_length > 0 ? ": " : "",
                      output_length > 0 ? output : "");
        }
    } else {
        if (pipe_fds[0] >= 0) {
            close(pipe_fds[0]);
        }
        if (pipe_fds[1] >= 0) {
            close(pipe_fds[1]);
        }
        log_debug("Failed to fork outerctl process: %s", strerror(errno));
    }
}

static void send_announcement(const char *action, int port, const char *socket_path) {
    run_outerctl_announcement(action, port, socket_path);
}

static void handle_shutdown_signal(int signal_number) {
    (void)signal_number;
    g_shutdown_requested = 1;
}

static void cleanup_handler(void) {
    if (g_listen_socket_path[0] != '\0') {
        send_announcement("REMOVE", 0, g_listen_socket_path);
        unlink(g_listen_socket_path);
    } else if (g_listen_port >= 0) {
        send_announcement("REMOVE", g_listen_port, NULL);
    }
}

static bool parse_port_argument(const char *value, int *port_out) {
    if (!value || value[0] == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long port = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || port < 0 || port > 65535) {
        return false;
    }

    *port_out = (int)port;
    return true;
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s [--port PORT | --socket-path PATH] [--label LABEL] [--pid-mode ID] [--bundles-dir DIR] [--icon-file PATH]\n",
            program_name && program_name[0] != '\0' ? program_name : "TopBackend");
}

static int create_tcp_listener(int requested_port, int *actual_port_out) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fatal("socket");
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(listen_fd);
        fatal("setsockopt");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)requested_port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        fatal("bind");
    }

    if (requested_port == 0) {
        socklen_t addr_len = sizeof(addr);
        if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
            close(listen_fd);
            fatal("getsockname");
        }
        *actual_port_out = ntohs(addr.sin_port);
    } else {
        *actual_port_out = requested_port;
    }

    if (listen(listen_fd, 64) < 0) {
        close(listen_fd);
        fatal("listen");
    }

    return listen_fd;
}

static void default_socket_path(char *out, size_t out_size) {
    const char *label = g_backend_label[0] ? g_backend_label : "dev.outergroup.Top";
#ifdef __APPLE__
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
        return;
    }

    const char *socket_root = getenv("HOME");
    if (!socket_root || socket_root[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            socket_root = pw->pw_dir;
        }
    }
    snprintf(out, out_size, "%s/Library/%s", socket_root ? socket_root : ".", label);
#else
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        snprintf(out, out_size, "%s/%s", runtime_dir, label);
        return;
    }

    snprintf(out, out_size, "/run/user/%d/%s", (int)getuid(), label);
#endif
}

static int create_unix_listener(const char *requested_socket_path) {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fatal("socket");
    }

    if (requested_socket_path && requested_socket_path[0] != '\0') {
        snprintf(g_listen_socket_path, sizeof(g_listen_socket_path), "%s", requested_socket_path);
    } else {
        default_socket_path(g_listen_socket_path, sizeof(g_listen_socket_path));
    }
    char socket_dir[PATH_MAX];
    snprintf(socket_dir, sizeof(socket_dir), "%s", g_listen_socket_path);
    char *last_slash = strrchr(socket_dir, '/');
    if (!last_slash) {
        close(listen_fd);
        fatal("socket path");
    }
    *last_slash = '\0';
    if (ensure_directory_exists(socket_dir) != 0) {
        close(listen_fd);
        fatal("mkdir");
    }
    unlink(g_listen_socket_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(g_listen_socket_path) >= sizeof(addr.sun_path)) {
        close(listen_fd);
        fatal("socket path too long");
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_listen_socket_path);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        fatal("bind");
    }
    if (chmod(g_listen_socket_path, 0600) != 0) {
        close(listen_fd);
        fatal("chmod");
    }

    if (listen(listen_fd, 64) < 0) {
        close(listen_fd);
        fatal("listen");
    }

    return listen_fd;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction shutdown_action;
    memset(&shutdown_action, 0, sizeof(shutdown_action));
    shutdown_action.sa_handler = handle_shutdown_signal;
    sigemptyset(&shutdown_action.sa_mask);
    sigaction(SIGINT, &shutdown_action, NULL);
    sigaction(SIGTERM, &shutdown_action, NULL);
    sigaction(SIGHUP, &shutdown_action, NULL);

    const char *outerctl_path = getenv("OUTERCTL_PATH");
    if (outerctl_path && strlen(outerctl_path) >= sizeof(g_outerctl_path)) {
        fprintf(stderr, "OUTERCTL_PATH is too long.\n");
        return 2;
    }
    if (outerctl_path && outerctl_path[0] != '\0') {
        snprintf(g_outerctl_path, sizeof(g_outerctl_path), "%s", outerctl_path);
    }

    int requested_port = -1;
    char requested_socket_path[PATH_MAX] = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            strncpy(g_backend_label, argv[i + 1], sizeof(g_backend_label) - 1);
            i++;
        } else if (strcmp(argv[i], "--pid-mode") == 0 && i + 1 < argc) {
            strncpy(g_backend_identifier, argv[i + 1], sizeof(g_backend_identifier) - 1);
            i++;
        } else if (strcmp(argv[i], "--bundles-dir") == 0 && i + 1 < argc) {
            snprintf(g_bundle_file_path_macos_arm, sizeof(g_bundle_file_path_macos_arm),
                     "%s/TopContent.bundle.macos-arm.aar", argv[i + 1]);
            snprintf(g_bundle_file_path_macos_x86, sizeof(g_bundle_file_path_macos_x86),
                     "%s/TopContent.bundle.macos-x86.aar", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--icon-file") == 0 && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof(g_app_icon_path)) {
                fprintf(stderr, "--icon-file path is too long.\n");
                return 2;
            }
            snprintf(g_app_icon_path, sizeof(g_app_icon_path), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (!parse_port_argument(argv[i + 1], &requested_port)) {
                fprintf(stderr, "Invalid --port value: %s\n", argv[i + 1]);
                print_usage(argv[0]);
                return 2;
            }
            requested_socket_path[0] = '\0';
            i++;
        } else if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof(requested_socket_path)) {
                fprintf(stderr, "--socket-path is too long.\n");
                return 2;
            }
            snprintf(requested_socket_path, sizeof(requested_socket_path), "%s", argv[i + 1]);
            requested_port = -1;
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    // Auto-detect mode if neither --label nor --pid-mode specified
    if (g_backend_label[0] == '\0' && g_backend_identifier[0] == '\0') {
#ifdef __APPLE__
        const char *xpc_name = getenv("XPC_SERVICE_NAME");
        if (xpc_name && xpc_name[0] != '\0' && strcmp(xpc_name, "0") != 0) {
            strncpy(g_backend_label, xpc_name, sizeof(g_backend_label) - 1);
        } else
#endif
        {
            snprintf(g_backend_identifier, sizeof(g_backend_identifier),
                     "com.probablymarcus.Top.debug");
        }
    }

#ifdef __APPLE__
    {
        const char *log_subsystem = g_backend_label[0] ? g_backend_label : g_backend_identifier;
        if (log_subsystem[0]) {
            g_os_log = os_log_create(log_subsystem, "general");
        }
    }
#endif

    int listen_fd = -1;
    if (requested_port >= 0) {
        listen_fd = create_tcp_listener(requested_port, &g_listen_port);
    } else {
        listen_fd = create_unix_listener(requested_socket_path);
    }

    set_blocking(listen_fd, false);
    if (!poll_state_init(&g_poll, listen_fd)) {
        fatal("poll_state_init");
    }

    atexit(cleanup_handler);
    if (g_listen_port >= 0) {
        send_announcement("ADD", g_listen_port, NULL);
        log_debug("Listening on http://127.0.0.1:%d/", g_listen_port);
        printf("TopBackend listening on http://127.0.0.1:%d/\n", g_listen_port);
        fflush(stdout);
    } else {
        send_announcement("ADD", 0, g_listen_socket_path);
        log_debug("Listening on %s/", g_listen_socket_path);
    }

    double next_refresh = current_time_seconds() + STREAM_INTERVAL_SEC;

    for (;;) {
        if (g_shutdown_requested) {
            break;
        }

        double now = current_time_seconds();
        int timeout_ms = (int)((next_refresh - now) * 1000.0);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 1000) timeout_ms = 1000;

        poll_rebuild();

        int ready = poll(g_poll.fds, (nfds_t)g_poll.count, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        for (size_t i = 0; i < g_poll.count && ready > 0; ++i) {
            if (g_poll.fds[i].revents == 0) {
                continue;
            }
            ready--;

            if ((int)i == g_poll.listen_fd_index) {
                handle_accept(listen_fd);
            } else {
                size_t client_index = g_poll.client_indices[i];
                if (client_index < g_clients.capacity && g_clients.slots[client_index].exists) {
                    handle_client_event(&g_clients.slots[client_index].client, g_poll.fds[i].revents);
                }
            }
        }

        now = current_time_seconds();
        if (now >= next_refresh) {
            refresh_process_snapshot();
            broadcast_frames_to_streaming_clients();
            next_refresh = now + STREAM_INTERVAL_SEC;
        }

        remove_closing_clients();
    }

    close(listen_fd);
    return 0;
}
