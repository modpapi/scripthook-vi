/* Borrowed-thread remote execution for the injector. See inject.h.
 *
 * Technique (the well-trodden PS5/FreeBSD ptrace "pt_call" pattern):
 *   - Elevate our own ucred (authid + all caps) only around each ptrace call,
 *     exactly as the SDK's samples/test_privileges does, then restore it.
 *   - PT_ATTACH + waitpid to stop the target.
 *   - Place a one-byte breakpoint (0xCC) at the target's ELF entry; that
 *     address is already executable, so it serves as a return trampoline.
 *   - To call f(a0..a5): save GPRs, set rip=f, args in rdi..r9, push the
 *     trampoline as the return address on a 16-byte-aligned stack, continue,
 *     and wait for the SIGTRAP when the call returns into our breakpoint.
 *     rax then holds the return value. Restore the saved GPRs afterwards.
 *   - Strings (the path and the line) are staged in the target's own stack,
 *     well below rsp, which is already writable (no new mapping needed).
 *
 * NOTE: not yet run on hardware. The structure mirrors known-good homebrew
 * tools; exact libkernel module basename and symbol names are confirmed on the
 * first console run (see docs/CONSOLE.md).
 */
#include "inject.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <machine/reg.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

/* ---- privilege-swapped ptrace (from samples/test_privileges) ------------- */

static int
sys_ptrace(int request, pid_t pid, caddr_t addr, int data) {
    uint8_t privcaps[16];
    memset(privcaps, 0xff, sizeof(privcaps));
    pid_t mypid = getpid();
    uint8_t caps[16];
    uint64_t authid;
    int ret;

    if (!(authid = kernel_get_ucred_authid(mypid))) {
        return -1;
    }
    if (kernel_get_ucred_caps(mypid, caps)) {
        return -1;
    }
    if (kernel_set_ucred_authid(mypid, 0x4800000000010003l)) {
        return -1;
    }
    if (kernel_set_ucred_caps(mypid, privcaps)) {
        return -1;
    }

    ret = (int)__syscall(SYS_ptrace, request, pid, addr, data);

    kernel_set_ucred_authid(mypid, authid);
    kernel_set_ucred_caps(mypid, caps);
    return ret;
}

/* ---- target memory r/w --------------------------------------------------- */

static int
target_read(pid_t pid, intptr_t addr, void *buf, size_t len) {
    return kernel_proc_copyout(pid, addr, buf, len);
}

static int
target_write(pid_t pid, const void *buf, intptr_t addr, size_t len) {
    int prot = kernel_get_vmem_protection(pid, addr, len);
    int added = 0;
    if (prot >= 0 && !(prot & PROT_WRITE)) {
        kernel_set_vmem_protection(pid, addr, len, prot | PROT_WRITE);
        added = 1;
    }
    int rc = kernel_proc_copyin(pid, buf, addr, len);
    if (added) {
        kernel_set_vmem_protection(pid, addr, len, prot);
    }
    return rc;
}

/* ---- the remote-call engine ---------------------------------------------- */

#define SHVI_SCRATCH_OFF 0x2000   /* stage strings this far below the saved rsp */

typedef struct {
    pid_t          pid;
    struct reg     saved;      /* GPRs to restore when done */
    intptr_t       bp_addr;    /* breakpoint / return trampoline */
    uint8_t        bp_orig;    /* original byte at bp_addr */
    intptr_t       scratch;    /* writable staging area in the target stack */
    int            attached;
    int            bp_set;
} remote_ctx;

static int
remote_begin(remote_ctx *c, pid_t pid) {
    memset(c, 0, sizeof(*c));
    c->pid = pid;

    if (sys_ptrace(PT_ATTACH, pid, 0, 0)) {
        klog_printf("shvi: PT_ATTACH failed (errno %d)\n", errno);
        return -1;
    }
    if (waitpid(pid, 0, 0) < 0) {
        klog_printf("shvi: waitpid after attach failed\n");
        sys_ptrace(PT_DETACH, pid, 0, 0);
        return -1;
    }
    c->attached = 1;

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&c->saved, 0)) {
        klog_printf("shvi: PT_GETREGS failed\n");
        return -1;
    }

    /* Trampoline: a byte we overwrite with int3 at the ELF entry. */
    c->bp_addr = kernel_dynlib_entry_addr(pid, 0);
    if (!c->bp_addr) {
        klog_printf("shvi: no entry addr for pid %d\n", pid);
        return -1;
    }
    if (target_read(pid, c->bp_addr, &c->bp_orig, 1)) {
        klog_printf("shvi: read trampoline byte failed\n");
        return -1;
    }
    uint8_t int3 = 0xCC;
    if (target_write(pid, &int3, c->bp_addr, 1)) {
        klog_printf("shvi: set trampoline int3 failed\n");
        return -1;
    }
    c->bp_set = 1;

    c->scratch = (intptr_t)(c->saved.r_rsp - SHVI_SCRATCH_OFF);
    return 0;
}

/* Restore everything and detach. Safe to call after a partial begin. */
static void
remote_end(remote_ctx *c) {
    if (c->bp_set) {
        target_write(c->pid, &c->bp_orig, c->bp_addr, 1);
    }
    if (c->attached) {
        sys_ptrace(PT_SETREGS, c->pid, (caddr_t)&c->saved, 0);
        sys_ptrace(PT_DETACH, c->pid, 0, 0);
    }
}

/* Call target function `fn` with up to 6 integer args; returns rax, or sets
 * *ok=0 on failure. */
static int64_t
remote_call(remote_ctx *c, intptr_t fn,
            uint64_t a0, uint64_t a1, uint64_t a2,
            uint64_t a3, uint64_t a4, uint64_t a5, int *ok) {
    struct reg r = c->saved;
    r.r_rip = fn;
    r.r_rdi = a0;
    r.r_rsi = a1;
    r.r_rdx = a2;
    r.r_rcx = a3;
    r.r_r8  = a4;
    r.r_r9  = a5;

    /* 16-byte align, then push the trampoline as the return address so that
     * after `fn` returns we land on the int3 (SysV: rsp%16==8 on entry). */
    uint64_t sp = (uint64_t)(c->scratch - 0x200);
    sp &= ~(uint64_t)0xF;
    sp -= 8;
    if (target_write(c->pid, &c->bp_addr, (intptr_t)sp, 8)) {
        *ok = 0;
        return -1;
    }
    r.r_rsp = (int64_t)sp;

    if (sys_ptrace(PT_SETREGS, c->pid, (caddr_t)&r, 0)) {
        *ok = 0;
        return -1;
    }
    if (sys_ptrace(PT_CONTINUE, c->pid, (caddr_t)1, 0)) {
        *ok = 0;
        return -1;
    }
    int status = 0;
    if (waitpid(c->pid, &status, 0) < 0) {
        *ok = 0;
        return -1;
    }

    struct reg after;
    if (sys_ptrace(PT_GETREGS, c->pid, (caddr_t)&after, 0)) {
        *ok = 0;
        return -1;
    }
    *ok = 1;
    return (int64_t)after.r_rax;
}

/* ---- libkernel symbol resolution in the target --------------------------- */

static intptr_t
resolve(pid_t pid, const char *sym) {
    static const char *mods[] = {
        "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx",
        "libkernel.so",   "libkernel_sys.so",
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        uint32_t h;
        if (kernel_dynlib_handle(pid, mods[i], &h)) {
            continue;
        }
        intptr_t a = kernel_dynlib_dlsym(pid, h, sym);
        if (a) {
            return a;
        }
    }
    return 0;
}

/* ---- the milestone: write a line from inside the target ------------------ */

int
inject_write_line(pid_t pid, const char *path, const char *line,
                  int64_t *out_fd) {
    remote_ctx c;
    if (remote_begin(&c, pid)) {
        remote_end(&c);
        return -1;
    }

    intptr_t fn_open  = resolve(pid, "open");
    intptr_t fn_write = resolve(pid, "write");
    intptr_t fn_close = resolve(pid, "close");
    if (!fn_open || !fn_write || !fn_close) {
        klog_printf("shvi: resolve open/write/close failed (%p/%p/%p)\n",
                    (void *)fn_open, (void *)fn_write, (void *)fn_close);
        remote_end(&c);
        return -2;
    }

    /* Stage path and line in the target stack scratch area. */
    size_t path_len = strlen(path) + 1;
    size_t line_len = strlen(line);
    intptr_t path_addr = c.scratch;
    intptr_t line_addr = c.scratch + (intptr_t)path_len + 8;
    if (target_write(pid, path, path_addr, path_len) ||
        target_write(pid, line, line_addr, line_len)) {
        klog_printf("shvi: staging strings failed\n");
        remote_end(&c);
        return -3;
    }

    int ok = 0;
    int64_t fd = remote_call(&c, fn_open, (uint64_t)path_addr,
                             O_WRONLY | O_CREAT | O_APPEND, 0666, 0, 0, 0, &ok);
    if (!ok || fd < 0) {
        klog_printf("shvi: remote open failed (ok=%d fd=%lld)\n", ok,
                    (long long)fd);
        remote_end(&c);
        return -4;
    }

    int64_t wr = remote_call(&c, fn_write, (uint64_t)fd, (uint64_t)line_addr,
                             line_len, 0, 0, 0, &ok);
    (void)wr;
    remote_call(&c, fn_close, (uint64_t)fd, 0, 0, 0, 0, 0, &ok);

    if (out_fd) {
        *out_fd = fd;
    }
    remote_end(&c);
    return 0;
}
