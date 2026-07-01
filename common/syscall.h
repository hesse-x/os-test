#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include "common/syscall_nums.h"
#include "common/errno.h"
#include "common/mman.h"
#include "arch/x64/utils.h"

// ===================== Unified syscall return convention =====================
//   Success: return >= 0 (value meaning depends on syscall)
//   Failure: return -errno (negative value)
// All __syscallN return int64_t from kernel; wrappers cast as needed.

// ===================== Kernel memory stats (shared with user space) =====================
struct kernel_mem_stats {
    int total_pages;       // atomic_t: physical total pages
    int used_pages;        // atomic_t: pages allocated
    int slab_used_bytes;   // atomic_t: slab bytes in use
    size_t slab_peak_bytes; // slab peak usage
    int kmalloc_calls;     // atomic_t: kmalloc call count
    int kfree_calls;       // atomic_t: kfree call count
};

// ===================== Semantic wrappers (user-space only) =====================
#ifndef __KERNEL__

#ifdef __cplusplus
extern "C" {
#endif
extern int errno;  // defined in libc (user/lib/unistd.cc), declared in user/include/errno.h
#ifdef __cplusplus
}
#endif

// --- pid/yield (always succeed) ---
static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

#ifndef ARCH_SET_FS
#define ARCH_SET_FS  0x1002
#endif
#ifndef ARCH_GET_FS
#define ARCH_GET_FS  0x1003
#endif

static inline int64_t sys_arch_prctl(int64_t code, int64_t addr) {
    return __syscall2(SYS_ARCH_PRCTL, code, addr);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

// --- IPC status-only: recv/req/resp/msg/msg_resp/irq_bind ---
static inline int sys_recv(void *buf, void *data_buf, size_t data_buf_len,
                            uint32_t timeout_ms) {
    int64_t r = __syscall4(SYS_RECV, (int64_t)(uintptr_t)buf,
        (int64_t)(uintptr_t)data_buf, (int64_t)data_buf_len, (int64_t)timeout_ms);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_req(int32_t pid, void *request, void *reply) {
    int64_t r = __syscall3(SYS_REQ, (int64_t)pid, (int64_t)(uintptr_t)request, (int64_t)(uintptr_t)reply);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_resp(void *reply) {
    int64_t r = __syscall1(SYS_RESP, (int64_t)(uintptr_t)reply);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
                           void *reply_buf, size_t reply_len) {
    int64_t r = __syscall5(SYS_MSG, (int64_t)target_pid,
        (int64_t)(uintptr_t)msg_buf, (int64_t)msg_len,
        (int64_t)(uintptr_t)reply_buf, (int64_t)reply_len);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_msg_resp(void *resp_buf, size_t resp_len) {
    int64_t r = __syscall2(SYS_MSG_RESP, (int64_t)(uintptr_t)resp_buf,
        (int64_t)resp_len);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_irq_bind(int irq) {
    int64_t r = __syscall1(SYS_IRQ_BIND, (int64_t)irq);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- exit (does not return) ---
static inline void sys_exit(int32_t exit_code) {
    __syscall1(SYS_EXIT, (int64_t)exit_code);
    // does not return
}

// --- fork/waitpid/execve ---
static inline int64_t sys_waitpid(int32_t pid, int32_t *exit_code) {
    int64_t r = __syscall2(SYS_WAITPID, (int64_t)pid, (int64_t)(uintptr_t)exit_code);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int64_t sys_fork(void) {
    return __syscall0(SYS_FORK);
}

static inline int sys_execve(const char *pathname, char *const argv[], char *const envp[]) {
    int64_t r = __syscall3(SYS_EXECVE, (int64_t)(uintptr_t)pathname,
                            (int64_t)(uintptr_t)argv, (int64_t)(uintptr_t)envp);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- mmap/munmap ---
static inline void *sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset) {
    int64_t r = __syscall6(SYS_MMAP, (int64_t)(uintptr_t)addr, (int64_t)size,
        (int64_t)prot, (int64_t)flags, (int64_t)fd, (int64_t)offset);
    if (r < 0) { errno = -(int)r; return MAP_FAILED; }
    return (void *)(uintptr_t)r;
}

static inline int sys_munmap(void *addr, size_t size) {
    int64_t r = __syscall2(SYS_MUNMAP, (int64_t)(uintptr_t)addr, (int64_t)size);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- memfd_create/ftruncate ---
static inline int sys_pipe(int *fd_ptr) {
    int64_t r = __syscall1(SYS_PIPE, (int64_t)(uintptr_t)fd_ptr);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- write/read/lseek ---
static inline int64_t sys_write(int fd, const void *buf, size_t len) {
    int64_t r = __syscall3(SYS_WRITE, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int64_t sys_read(int fd, void *buf, size_t len) {
    int64_t r = __syscall3(SYS_READ, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

// --- close/notify ---
static inline int sys_close(int fd) {
    int64_t r = __syscall1(SYS_CLOSE, (int64_t)fd);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_notify(int32_t pid) {
    int64_t r = __syscall1(SYS_NOTIFY, (int64_t)pid);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- gettime/clock (always succeed, return uint64_t time value) ---
static inline uint64_t sys_gettime() {
    return (uint64_t)__syscall0(SYS_GETTIME);
}

static inline uint64_t sys_clock() {
    return (uint64_t)__syscall0(SYS_CLOCK);
}

// --- ioperm/dup2/fcntl ---
static inline int sys_ioperm(unsigned long from, unsigned long num, int turn_on) {
    int64_t r = __syscall3(SYS_IOPERM, (int64_t)from, (int64_t)num, (int64_t)turn_on);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_dup2(int old_fd, int new_fd) {
    int64_t r = __syscall2(SYS_DUP2, (int64_t)old_fd, (int64_t)new_fd);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

static inline int sys_fcntl(int fd, int cmd, int arg) {
    int64_t r = __syscall3(SYS_FCNTL, (int64_t)fd, (int64_t)cmd, (int64_t)arg);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

// --- dma_alloc/dma_free ---
static inline int sys_dma_alloc(size_t size, void **vaddr, uint64_t *paddr) {
    int64_t r = __syscall3(SYS_DMA_ALLOC, (int64_t)size,
        (int64_t)(uintptr_t)vaddr, (int64_t)(uintptr_t)paddr);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_dma_free(void *vaddr) {
    int64_t r = __syscall1(SYS_DMA_FREE, (int64_t)(uintptr_t)vaddr);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- pci_dev_info ---
static inline int sys_pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func,
                                    struct pci_dev_info *out) {
    int64_t r = __syscall4(SYS_PCI_DEV_INFO, (int64_t)bus, (int64_t)dev,
        (int64_t)func, (int64_t)(uintptr_t)out);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- block_async (returns cookie on success) ---
// Async block I/O: returns cookie (>0) on success, -1 on error (errno set).
// Completion delivered via RECV_NOTIFY with cookie+result+lba+count in data.
static inline int sys_block_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
    int64_t r = __syscall4(SYS_BLOCK_ASYNC, (int64_t)lba,
        (int64_t)(uintptr_t)buf, (int64_t)count, (int64_t)dir);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

// --- install_fd (returns fd on success) ---
// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — SYS_INSTALL_FD
// Register an FD_FILE fd in the kernel fd_table.
// Returns: fd (>=3) on success, -1 on failure (errno set)
static inline int sys_install_fd(int32_t fs_pid, int32_t fs_fd,
                                  uint64_t offset, int flags,
                                  uint64_t file_size) {
    int64_t r = __syscall5(SYS_INSTALL_FD, (int64_t)fs_pid, (int64_t)fs_fd,
        (int64_t)offset, (int64_t)flags, (int64_t)file_size);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

// --- lseek (returns offset on success) ---
static inline int64_t sys_lseek(int fd, int64_t offset, int whence) {
    int64_t r = __syscall3(SYS_LSEEK, (int64_t)fd, (int64_t)offset, (int64_t)whence);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

// --- memfd_create/ftruncate ---
static inline int sys_memfd_create(const char *name, unsigned int flags) {
    int64_t r = __syscall2(SYS_MEMFD_CREATE, (int64_t)(uintptr_t)name, (int64_t)flags);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

static inline int sys_ftruncate(int fd, int64_t size) {
    int64_t r = __syscall2(SYS_FTRUNCATE, (int64_t)fd, (int64_t)size);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

// --- Signal syscalls ---
struct sigaction;  // forward declaration
static inline int sys_kill(int32_t pid, int sig) {
    int64_t r = __syscall2(SYS_KILL, (int64_t)pid, (int64_t)sig);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oldact) {
    int64_t r = __syscall3(SYS_SIGACTION, (int64_t)sig,
        (int64_t)(uintptr_t)act, (int64_t)(uintptr_t)oldact);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_sigreturn(void) {
    int64_t r = __syscall0(SYS_SIGRETURN);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_debug_memstat(struct kernel_mem_stats *buf, int len) {
    int64_t r = __syscall2(SYS_DEBUG_MEMSTAT, (int64_t)(uintptr_t)buf, (int64_t)len);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

// --- VFS syscalls ---
static inline int sys_open(const char *path, int flags, ...) {
    int64_t r = __syscall2(SYS_OPEN, (int64_t)(uintptr_t)path, (int64_t)flags);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

static inline int sys_stat(const char *path, void *stat_buf) {
    int64_t r = __syscall2(SYS_STAT, (int64_t)(uintptr_t)path, (int64_t)(uintptr_t)stat_buf);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_mkdir(const char *path, uint32_t mode) {
    int64_t r = __syscall2(SYS_MKDIR, (int64_t)(uintptr_t)path, (int64_t)mode);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_unlink(const char *path) {
    int64_t r = __syscall1(SYS_UNLINK, (int64_t)(uintptr_t)path);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_rmdir(const char *path) {
    int64_t r = __syscall1(SYS_RMDIR, (int64_t)(uintptr_t)path);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_dev_create(const char *name, int shm_fd) {
    int64_t r = __syscall3(SYS_DEV_CREATE, (int64_t)(uintptr_t)name, (int64_t)shm_fd, 0);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int sys_getdents(int fd, void *buf, size_t len) {
    int64_t r = __syscall3(SYS_GETDENTS, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
    if (r < 0) { errno = -(int)r; return -1; }
    return (int)r;
}

// --- ioctl/fstat/fdev_pid ---
static inline int64_t sys_ioctl(int fd, uint32_t cmd, uint64_t arg) {
    int64_t r = __syscall3(SYS_IOCTL, (int64_t)fd, (int64_t)cmd, arg);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int64_t sys_fstat(int fd, uint64_t buf) {
    int64_t r = __syscall2(SYS_FSTAT, (int64_t)fd, buf);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int64_t sys_fdev_pid(int fd) {
    int64_t r = __syscall1(SYS_FDEV_PID, (int64_t)fd);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

// --- Session/pgid syscalls ---
static inline int64_t sys_setsid() {
    int64_t r = __syscall0(SYS_SETSID);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int sys_setpgid(uint64_t pid, uint64_t pgid) {
    int64_t r = __syscall2(SYS_SETPGID, (int64_t)pid, (int64_t)pgid);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}

static inline int64_t sys_getpgid(uint64_t pid) {
    int64_t r = __syscall1(SYS_GETPGID, (int64_t)pid);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

static inline int64_t sys_getsid(uint64_t pid) {
    int64_t r = __syscall1(SYS_GETSID, (int64_t)pid);
    if (r < 0) { errno = -(int)r; return -1; }
    return r;
}

#endif // __KERNEL__

#endif // COMMON_SYSCALL_H
