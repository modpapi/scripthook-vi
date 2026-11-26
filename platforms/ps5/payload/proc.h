/* scripthook-vi-ps5 injector: process discovery.
 *
 * The selection POLICY (shvi_select_game_pid) is pure and host-testable;
 * it lives in proc_select.c and has no PS5 dependencies. find_game() is the
 * PS5-only wrapper that queries the kernel (sysctl KERN_PROC) and applies the
 * policy; it lives in proc.c.
 */
#ifndef SHVI_PS5_PROC_H
#define SHVI_PS5_PROC_H

#include <sys/types.h>
#include <stddef.h>

/* One process as seen in the sysctl KERN_PROC listing. comm is kinfo_proc's
 * ki_comm (the executable name); tdname is ki_tdname (the main thread name).
 * Either may carry the game's name, so the policy checks both. */
typedef struct {
    pid_t pid;
    char  comm[32];
    char  tdname[32];
} shvi_proc_ent;

/* Pick the process whose comm OR tdname equals `name`, excluding `self_pid`.
 *
 * Returns the matching pid. If no process matches, returns -1 and clears
 * *ambiguous. If more than one DISTINCT pid matches, the choice is unsafe:
 * returns -1 and sets *ambiguous to 1 (the caller refuses and lists them).
 * `ambiguous` must be non-NULL.
 */
pid_t shvi_select_game_pid(const shvi_proc_ent *ents, size_t n,
                           const char *name, pid_t self_pid,
                           int *ambiguous);

/* Query the kernel and apply the policy. PS5 only (uses sysctl + kinfo_proc).
 * Returns the game pid, or -1 if not found / ambiguous. */
pid_t find_game(const char *name);

#endif /* SHVI_PS5_PROC_H */
