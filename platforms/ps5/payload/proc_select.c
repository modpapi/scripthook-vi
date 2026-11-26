/* Pure process-selection policy for the injector. No PS5 dependencies, so it
 * is compiled both into the PS5 payload (via proc.c) and into the host unit
 * tests (tests/test_proc.c). Keep it that way: no sysctl, no kinfo_proc, no
 * libc beyond <string.h>. */
#include "proc.h"

#include <string.h>

pid_t shvi_select_game_pid(const shvi_proc_ent *ents, size_t n,
                           const char *name, pid_t self_pid,
                           int *ambiguous) {
    *ambiguous = 0;

    pid_t found = -1;
    for (size_t i = 0; i < n; i++) {
        const shvi_proc_ent *e = &ents[i];
        if (e->pid == self_pid) {
            continue;
        }
        if (strcmp(e->comm, name) != 0 && strcmp(e->tdname, name) != 0) {
            continue;
        }
        if (found == -1) {
            found = e->pid;       /* first match */
        } else if (e->pid != found) {
            *ambiguous = 1;       /* a second, distinct match: unsafe to guess */
            return -1;
        }
    }
    return found;
}
