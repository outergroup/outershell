#ifndef OUTER_SHELL_IDLE_H
#define OUTER_SHELL_IDLE_H
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    bool idle;
    bool stop_requested;
    double last_activity;
} OuterShellIdleState;

static inline double outer_shell_idle_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static inline bool outer_shell_idle_poll(OuterShellIdleState *state, bool idle) {
    pthread_mutex_lock(&state->mutex);
    double now = outer_shell_idle_now();
    if (!idle || state->last_activity == 0) state->last_activity = now;
    state->idle = idle;
    bool stop = idle && state->stop_requested;
    pthread_mutex_unlock(&state->mutex);
    return stop;
}

static inline void outer_shell_idle_activity(OuterShellIdleState *state, bool active) {
    pthread_mutex_lock(&state->mutex);
    state->idle = false;
    if (active) state->last_activity = outer_shell_idle_now();
    pthread_mutex_unlock(&state->mutex);
}

static inline double outer_shell_idle_seconds(OuterShellIdleState *state) {
    pthread_mutex_lock(&state->mutex);
    double seconds = state->idle ? outer_shell_idle_now() - state->last_activity : 0;
    pthread_mutex_unlock(&state->mutex);
    return seconds;
}

static inline void outer_shell_idle_stop(OuterShellIdleState *state) {
    pthread_mutex_lock(&state->mutex);
    state->stop_requested = true;
    pthread_mutex_unlock(&state->mutex);
}
#endif
