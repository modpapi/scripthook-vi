/* ScriptHook VI PS5 loader (shvi_loader.elf): the payload sent to the console's ELF loader.
 *
 * Today (milestone 1): find the running GTA VI process, make it write a line to
 * /data/shvi/hello.log from inside its own address space, and report the pid
 * and fd by on-screen notification + kernel log. It also lists the mod images
 * installed in /data/shvi/mods/ (<mod>.elf, built by shvi_add_mod for ps5):
 * mapping them into the game is the next milestone (W^X code mapping), so they
 * are reported, not started. Send a mod image to the ELF loader on its own for a
 * dry run outside the game. Story mode / offline only.
 *
 * Usage (argv from the ELF loader): optional argv[1] overrides the process
 * name to match (default "eboot.bin").
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <ps5/klog.h>

#include "proc.h"
#include "inject.h"

#define LOG_DIR  "/data/shvi"
#define LOG_PATH LOG_DIR "/hello.log"
#define MODS_DIR LOG_DIR "/mods"

/* Declared by the SDK samples (notify_debug/main.c), not in a public header. */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void
notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    klog_puts(req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* Counts <name>.elf files in MODS_DIR and writes their names (comma separated) to out. */
static int
list_mods(char *out, size_t n) {
    int count = 0;
    size_t used = 0;
    DIR *d = opendir(MODS_DIR);
    if (n) out[0] = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".elf") != 0) continue;
        ++count;
        if (used + len + 2 < n) {
            if (used) out[used++] = ',';
            memcpy(out + used, e->d_name, len);
            used += len;
            out[used] = 0;
        }
    }
    closedir(d);
    return count;
}

int
main(int argc, char *argv[]) {
    const char *name = (argc >= 2 && argv[1] && argv[1][0]) ? argv[1]
                                                            : "eboot.bin";

    pid_t pid = find_game(name);
    if (pid < 0) {
        notify("shvi: game process not found");
        return 1;
    }

    /* The directory is created by us (the payload has fs access); the target
     * only opens the file. */
    mkdir("/data", 0777);
    if (mkdir(LOG_DIR, 0777) && errno != EEXIST) {
        notify("shvi: cannot create %s (errno %d)", LOG_DIR, errno);
        return 2;
    }

    char line[160];
    snprintf(line, sizeof(line), "hello from inside GTA VI pid=%d\n", pid);

    int64_t fd = -1;
    int rc = inject_write_line(pid, LOG_PATH, line, &fd);
    if (rc) {
        notify("shvi: injection failed (pid %d, rc %d)", pid, rc);
        return 3;
    }

    notify("shvi: injected into pid %d, wrote %s (fd %lld)", pid, LOG_PATH,
           (long long)fd);

    char mods[512];
    int nmods = list_mods(mods, sizeof(mods));
    if (nmods)
        notify("shvi: %d mod image(s) in %s: %s (mapping into the game: next milestone)",
               nmods, MODS_DIR, mods);
    else
        notify("shvi: no mod images in %s", MODS_DIR);
    return 0;
}
