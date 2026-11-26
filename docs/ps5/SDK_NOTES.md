# ps5-payload-sdk notes

Facts recorded 2026-11-25 while installing the toolchain (Task 1). Every prototype below is copied from the installed headers.

## Install

- SDK: ps5-payload-sdk, release tag **v0.50** (`releases/latest` redirected to `.../releases/tag/v0.50`; zip `ps5-payload-sdk.zip`, 8,839,112 bytes). The zip contains no version file; the tag is from the redirect.
- Install path: `/opt/ps5-payload-sdk` (the zip unpacked directly to that name, no move needed).
- Layout: `bin/ ldscripts/ samples/ target/ toolchain/ win/`. Headers are in `target/include/` (so `target/include/ps5/kernel.h`, not `include/ps5/kernel.h`); libs in `target/lib/`.
- `target/include/ps5/` contains: `iommu.h kernel.h klog.h mdbg.h nid.h payload.h`.
- Host toolchain (apt, WSL Ubuntu-24.04): clang-18 / lld-18 version 18.1.3 (Ubuntu 1ubuntu1). The SDK also ships its own `bin/prospero-clang` (Ubuntu clang 18.1.3, target `x86_64-sie-ps5`).
- Env var in default user's `~/.bashrc`: `export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk`.

## hello_world build (worked)

```
mkdir -p ~/src/ps5-sdk/samples && cp -r /opt/ps5-payload-sdk/samples/hello_world ~/src/ps5-sdk/samples/
cd ~/src/ps5-sdk/samples/hello_world
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

Runs `/opt/ps5-payload-sdk/bin/prospero-clang -Wall -Werror -g -o hello_world.elf main.c`.
`file`: `ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, with debug_info, not stripped`.
The sample Makefile `test` target deploys with `$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT)` (defaults `ps5`, `9021`). Not run in this task.

## ps5/kernel.h

Includes `<sys/cdefs.h>`, `<sys/types.h>`. All functions are in `__BEGIN_DECLS`.

Absolute kernel pointers (`extern const intptr_t`): `KERNEL_ADDRESS_TEXT_BASE`, `_DATA_BASE`, `_ALLPROC`, `_PRISON0`, `_ROOTVNODE`, `_SECURITY_FLAGS`, `_UTOKEN_FLAGS`, `_QA_FLAGS`, `_TARGETID`, `_BUS_DATA_DEVICES` (each prefixed `KERNEL_ADDRESS_`).

Offsets (`extern const off_t`): `KERNEL_OFFSET_PROC_P_UCRED`, `_PROC_P_FD`, `_PROC_P_PID`, `_PROC_P_VMSPACE`, `_UCRED_CR_UID`, `_UCRED_CR_RUID`, `_UCRED_CR_SVUID`, `_UCRED_CR_RGID`, `_UCRED_CR_SCEAUTHID`, `_UCRED_CR_SCECAPS`, `_UCRED_CR_SCEATTRS`, `_FILEDESC_FD_RDIR`, `_FILEDESC_FD_JDIR`, `_VMSPACE_VM_PMAP`.

Firmware version and raw kernel memory:
```
uint32_t kernel_get_fw_version(void);
int32_t kernel_copyin(const void *udaddr, intptr_t kaddr, size_t len);
int32_t kernel_copyout(intptr_t kaddr, void *udaddr, size_t  len);
int32_t kernel_setlong(intptr_t addr, uint64_t val);
int32_t kernel_setint(intptr_t addr, uint32_t val);
int32_t kernel_setshort(intptr_t addr, uint16_t val);
int32_t kernel_setchar(intptr_t addr, uint8_t val);
uint64_t kernel_getlong(intptr_t addr);
uint32_t kernel_getint(intptr_t addr);
uint16_t kernel_getshort(intptr_t addr);
uint8_t  kernel_getchar(intptr_t addr);
```

Process / thread lookup (return kernel pointers):
```
intptr_t kernel_get_proc(pid_t pid);
intptr_t kernel_get_proc_thread(pid_t pid, pid_t tid);
intptr_t kernel_get_proc_ucred(pid_t pid);
intptr_t kernel_get_proc_filedesc(pid_t pid);
intptr_t kernel_get_proc_file(pid_t pid, int fd);
```

Read/write another process's memory (kernel.h):
```
int kernel_proc_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len);
int kernel_proc_copyout(pid_t pid, intptr_t addr, void *buf, size_t len);
uint64_t kernel_proc_getlong(pid_t pid, intptr_t addr);
uint32_t kernel_proc_getint(pid_t pid, intptr_t addr);
uint16_t kernel_proc_getshort(pid_t pid, intptr_t addr);
uint8_t  kernel_proc_getchar(pid_t pid, intptr_t addr);
int kernel_proc_setchar(pid_t pid, intptr_t addr, char val);
int kernel_proc_setshort(pid_t pid, intptr_t addr, short val);
int kernel_proc_setint(pid_t pid, intptr_t addr, int val);
int kernel_proc_setlong(pid_t pid, intptr_t addr, long val);
```

Remote page protection:
```
int kernel_get_vmem_protection(pid_t pid, intptr_t addr, size_t len);
int kernel_set_vmem_protection(pid_t pid, intptr_t addr, size_t len, int prot);
int kernel_mprotect(pid_t pid, intptr_t addr, size_t size, int prot);
```

Dynlib lookup in another process (useful to find a module base / resolve symbols without remote code):
```
int      kernel_dynlib_handle(pid_t pid, const char* basename, uint32_t *handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);
intptr_t kernel_dynlib_entry_addr(pid_t pid, uint32_t handle);
intptr_t kernel_dynlib_init_addr(pid_t pid, uint32_t handle);
intptr_t kernel_dynlib_fini_addr(pid_t pid, uint32_t handle);
```

ucred / privilege helpers:
```
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);
int32_t kernel_get_ucred_attrs(pid_t pid, uint8_t attrs[32]);
int32_t kernel_set_ucred_attrs(pid_t pid, const uint8_t attrs[32]);
uid_t   kernel_get_ucred_uid(pid_t pid);
int32_t kernel_set_ucred_uid(pid_t pid, uid_t uid);
uid_t   kernel_get_ucred_ruid(pid_t pid);
int32_t kernel_set_ucred_ruid(pid_t pid, uid_t ruid);
uid_t   kernel_get_ucred_svuid(pid_t pid);
int32_t kernel_set_ucred_svuid(pid_t pid, uid_t svuid);
gid_t   kernel_get_ucred_rgid(pid_t pid);
int32_t kernel_set_ucred_rgid(pid_t pid, gid_t rgid);
gid_t   kernel_get_ucred_svgid(pid_t pid);
int32_t kernel_set_ucred_svgid(pid_t pid, gid_t svgid);
intptr_t kernel_get_ucred_prison(pid_t pid);
int      kernel_set_ucred_prison(pid_t pid, intptr_t prison);
```

Other (declared, not process r/w): `kernel_overlap_sockets(pid_t, int, int)`, `kernel_get/set_qaflags(uint8_t[16])`, `kernel_get_targetid(uint8_t*)`/`kernel_set_targetid(uint8_t)`, `kernel_get_utokflags`/`kernel_set_utokflags`, `kernel_get_secflags(uint32_t*)`/`kernel_set_secflags(uint32_t)`, `kernel_get_root_vnode(void)`, `kernel_get/set_proc_rootdir`, `kernel_get/set_proc_jaildir`.

## ps5/mdbg.h

Includes `<sys/cdefs.h> <stdint.h> <unistd.h>`. Alternative process memory r/w (debugger path):
```
int mdbg_copyout(pid_t pid, intptr_t addr, void *buf, size_t len);
int mdbg_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len);
int mdbg_setchar(pid_t pid, intptr_t addr, char val);
int mdbg_setshort(pid_t pid, intptr_t addr, short val);
int mdbg_setint(pid_t pid, intptr_t addr, int val);
int mdbg_setlong(pid_t pid, intptr_t addr, long val);
long  mdbg_getlong(pid_t pid, intptr_t addr);
int   mdbg_getint(pid_t pid, intptr_t addr);
short mdbg_getshort(pid_t pid, intptr_t addr);
char  mdbg_getchar(pid_t pid, intptr_t addr);
```
Not confirmed: how `mdbg_*` differs in behavior or required privileges from `kernel_proc_*` (headers have no comments; no sample uses `mdbg_*`). Samples using `kernel_proc_copyout`/`mdbg_copyout`: only `samples/test_privileges/main.c` matched the grep.

## ps5/payload.h

```
typedef struct payload_args {
  int (*sys_dynlib_dlsym)(int, const char*, void*);
  int* rwpipe;
  int* rwpair;
  intptr_t kpipe_addr;
  intptr_t kdata_base_addr;
  int* payloadout;
} payload_args_t;

payload_args_t* payload_get_args(void);
void payload_exit(int exit_code);
```
No process, ptrace, or syscall helper here.

## ps5/klog.h (kernel log)

```
int klog_printf(const char *fmt, ...);
int klog_puts(const char *s);
int klog_perror(const char *s);
```

## Remote syscalls

None of `kernel.h`, `mdbg.h`, `payload.h` provides a remote-syscall / remote-call helper. The SDK has no header for one (`target/include/ps5/` holds only the six headers listed above). Remote execution would have to be built on ptrace (attach, set regs, continue) or by writing code into the target with `kernel_proc_copyin`/`mdbg_copyin`.

## ptrace and sysctl in the SDK libc

- `ptrace(2)`: **yes**. `sys/ptrace.h` line 218: `int ptrace(int _request, pid_t _pid, caddr_t _addr, int _data);`. `libc.a` exports `T ptrace`; `libkernel.so`/`libkernel_sys.so` also export `ptrace`. Requests seen in the header: `PT_READ_D` (2), `PT_CONTINUE` (7), `PT_ATTACH` (10), `PT_DETACH` (11), `PT_IO` (12, uses `struct ptrace_io_desc`), `PT_GETREGS` (33).
- Privilege caveat: `samples/test_privileges/main.c` (`sys_ptrace`, lines 59-90) does not call libc `ptrace`. It saves the process authid and caps via `kernel_get_ucred_authid`/`kernel_get_ucred_caps`, sets authid `0x4800000000010003l` and all-0xff caps with `kernel_set_ucred_authid`/`kernel_set_ucred_caps`, calls `__syscall(SYS_ptrace, request, pid, addr, data)`, then restores them. This suggests plain ptrace needs elevated ucred; not tested on hardware.
- `sysctl`: **yes**. `sys/sysctl.h` line 1075: `int sysctl(const int *, u_int, void *, size_t *, const void *, size_t);` (also `sysctlbyname`, `sysctlnametomib` in libkernel). `CTL_KERN`/`KERN_PROC` (14) with `KERN_PROC_PROC` (8) or `KERN_PROC_PID` (1) are defined.
- `syscall`/`__syscall` are in `libc.a` (`T syscall`); `<sys/syscall.h>` provides `SYS_ptrace`.
- Sony helpers (undeclared in SDK headers; samples declare them locally): `int sceKernelGetAppInfo(pid_t pid, app_info_t *info);` (exported by libkernel.so), `int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);`, `int sceNotificationSend(int userId, bool isLogged, const char* payload);` (libSceNotification.so is in `target/lib`).

## Samples to copy from

- Process listing: `/opt/ps5-payload-sdk/samples/ps/main.c`. Uses `int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}`, two `sysctl()` calls (size, then data), walks `struct kinfo_proc` by `ki_structsize`, prints `ki_pid`, `ki_comm`, `kernel_get_ucred_authid(ki_pid)`, and `sceKernelGetAppInfo` (title id). Includes `<sys/proc.h> <sys/user.h> <sys/sysctl.h> <ps5/kernel.h>`.
- Notification (toast): `/opt/ps5-payload-sdk/samples/notify/main.c`. `sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM /*0xFE*/, true, json)` with a JSON `rawData` template.
- Notification (simple text): `/opt/ps5-payload-sdk/samples/notify_debug/main.c`. Fills `notify_request_t { char useless1[45]; char message[3075]; }` and calls `sceKernelSendNotificationRequest(0, &req, sizeof req, 0)`.
- ptrace with privilege swap: `/opt/ps5-payload-sdk/samples/test_privileges/main.c`.
- Other: `samples/arbitrary_syscall/main.c` (inline `syscall` asm), `samples/hello_dlfcn`, `samples/hello_kenv`.

## Not confirmed

- No hardware run: nothing here was executed on a console.
- Exact semantics, return codes and required privileges of `kernel_proc_*` vs `mdbg_*` (headers carry no docs; implementations are in prebuilt libs).
- Whether libc `ptrace` works unprivileged for a payload (see caveat above).
