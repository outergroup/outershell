#include "../Resources/OuterShellIdle.h"
#include <assert.h>
int main(void) {
    OuterShellIdleState state = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    assert(outer_shell_idle_seconds(&state) == 0);
    assert(!outer_shell_idle_poll(&state, true));
    state.last_activity -= 61;
    assert(outer_shell_idle_seconds(&state) >= 60);
    outer_shell_idle_activity(&state, true);
    assert(outer_shell_idle_seconds(&state) == 0);
    assert(!outer_shell_idle_poll(&state, true));
    assert(outer_shell_idle_seconds(&state) < 1);
    outer_shell_idle_stop(&state);
    assert(!outer_shell_idle_poll(&state, false));
    assert(outer_shell_idle_poll(&state, true));
    pthread_mutex_destroy(&state.mutex);
}
