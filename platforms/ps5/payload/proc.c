/* PS5-only process discovery: query sysctl KERN_PROC, build the entry list,
 * and apply the pure policy in proc_select.c. See proc.h. */
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/sysctl.h>
#include <sys/user.h>

pid_t
find_game(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0)) {
        return -1;
    }
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        return -1;
    }
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) {
        free(buf);
        return -1;
    }

    /* Upper bound on process count for the entry array. */
    size_t cap = buf_size / sizeof(struct kinfo_proc) + 1;
    shvi_proc_ent *ents = calloc(cap, sizeof(*ents));
    if (!ents) {
        free(buf);
        return -1;
    }

    size_t n = 0;
    for (uint8_t *ptr = buf; ptr < buf + buf_size && n < cap;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        if (ki->ki_structsize == 0) {
            break;  /* malformed: avoid an infinite loop */
        }
        ptr += ki->ki_structsize;

        ents[n].pid = ki->ki_pid;
        snprintf(ents[n].comm, sizeof(ents[n].comm), "%s", ki->ki_comm);
        snprintf(ents[n].tdname, sizeof(ents[n].tdname), "%s", ki->ki_tdname);
        n++;
    }

    int ambiguous = 0;
    pid_t pid = shvi_select_game_pid(ents, n, name, getpid(), &ambiguous);

    free(ents);
    free(buf);
    return pid;
}
