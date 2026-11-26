/* scripthook-vi-ps5 injector: borrowed-thread remote execution.
 *
 * The first milestone proves we can make the GAME execute code on our behalf.
 * PS5 enforces W^X, so mapping our OWN executable bytes into the target is a
 * JIT problem deferred to the code-mapping task. Here we instead CALL the
 * game's own already-executable functions (open/write/close) on a borrowed
 * game thread, which exercises the whole attach / save-regs / run / restore /
 * detach pipeline the later hooks will reuse.
 *
 * All functions return 0 on success, negative on failure, and never leave the
 * target stopped on the success path. They require the payload to run with
 * elevated ucred (handled internally via the authid/caps swap, per the SDK's
 * test_privileges sample).
 */
#ifndef SHVI_PS5_INJECT_H
#define SHVI_PS5_INJECT_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

/* Attach to `pid`, append `line` to `path` (creating it) from inside the
 * target by remote-calling its libkernel open/write/close, then detach.
 * On success, *out_fd receives the fd the target's open() returned (for the
 * log/notification, so repeated injections are visible). */
int inject_write_line(pid_t pid, const char *path, const char *line,
                      int64_t *out_fd);

#endif /* SHVI_PS5_INJECT_H */
