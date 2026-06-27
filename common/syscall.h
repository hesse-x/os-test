#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include "common/syscall_nums.h"
#include "arch/x64/utils.h"

// ===================== Syscall return convention =====================
// 0 = success (for status-only syscalls: recv/req/resp/notify/exit/irq_bind/munmap)
// positive errno = error (for status-only syscalls)
// For value-returning syscalls: 0 = failure, nonzero = success
//   sys_mmap: returns mapped address on success, NULL on failure
//   sys_spawn/sys_waitpid: returns pid on success, 0 on failure
//   sys_getpid: always succeeds (returns pid >= 1)

// ===================== Semantic wrappers (user-space only) =====================
#ifndef __KERNEL__
static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

static inline int sys_recv(void *buf, void *data_buf, size_t data_buf_len,
                            uint32_t timeout_ms) {
    return (int)__syscall4(SYS_RECV, (int64_t)(uintptr_t)buf,
        (int64_t)(uintptr_t)data_buf, (int64_t)data_buf_len, (int64_t)timeout_ms);
}

static inline int sys_req(int32_t pid, void *request, void *reply) {
    return (int)__syscall3(SYS_REQ, (int64_t)pid, (int64_t)(uintptr_t)request, (int64_t)(uintptr_t)reply);
}

static inline int sys_resp(void *reply) {
    return (int)__syscall1(SYS_RESP, (int64_t)(uintptr_t)reply);
}

static inline int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
                           void *reply_buf, size_t reply_len) {
    return (int)__syscall5(SYS_MSG, (int64_t)target_pid,
        (int64_t)(uintptr_t)msg_buf, (int64_t)msg_len,
        (int64_t)(uintptr_t)reply_buf, (int64_t)reply_len);
}

static inline int sys_msg_resp(void *resp_buf, size_t resp_len) {
    return (int)__syscall2(SYS_MSG_RESP, (int64_t)(uintptr_t)resp_buf,
        (int64_t)resp_len);
}

static inline int sys_irq_bind(int irq) {
    return (int)__syscall1(SYS_IRQ_BIND, (int64_t)irq);
}

static inline void sys_exit(int32_t exit_code) {
    __syscall1(SYS_EXIT, (int64_t)exit_code);
    // does not return
}

static inline int64_t sys_waitpid(int32_t pid, int32_t *exit_code) {
    return __syscall2(SYS_WAITPID, (int64_t)pid, (int64_t)(uintptr_t)exit_code);
}

static inline int64_t sys_spawn(const void *elf_data, uint64_t elf_size) {
    return __syscall2(SYS_SPAWN, (int64_t)(uintptr_t)elf_data, (int64_t)elf_size);
}

static inline void *sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset) {
    return (void *)__syscall6(SYS_MMAP, (int64_t)(uintptr_t)addr, (int64_t)size,
        (int64_t)prot, (int64_t)flags, (int64_t)fd, (int64_t)offset);
}

static inline int sys_munmap(void *addr, size_t size) {
    return (int)__syscall2(SYS_MUNMAP, (int64_t)(uintptr_t)addr, (int64_t)size);
}

// DEPRECATED: sys_dev_req removed — use sys_ioctl instead

static inline int sys_shm_create(size_t size) {
    return (int)__syscall1(SYS_SHM_CREATE, (int64_t)size);
}

static inline int sys_shm_attach(int32_t id, int mode) {
    return (int)__syscall2(SYS_SHM_ATTACH, (int64_t)id, (int64_t)mode);
}

static inline int sys_pipe(int *fd_ptr) {
    return (int)__syscall1(SYS_PIPE, (int64_t)(uintptr_t)fd_ptr);
}

static inline int64_t sys_write(int fd, const void *buf, size_t len) {
    return __syscall3(SYS_WRITE, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int64_t sys_read(int fd, void *buf, size_t len) {
    return __syscall3(SYS_READ, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int sys_close(int fd) {
    return (int)__syscall1(SYS_CLOSE, (int64_t)fd);
}

static inline int sys_notify(int32_t pid) {
    return (int)__syscall1(SYS_NOTIFY, (int64_t)pid);
}

static inline uint64_t sys_gettime() {
    return (uint64_t)__syscall0(SYS_GETTIME);
}

static inline uint64_t sys_clock() {
    return (uint64_t)__syscall0(SYS_CLOCK);
}

static inline int sys_ioperm(unsigned long from, unsigned long num, int turn_on) {
    return (int)__syscall3(SYS_IOPERM, (int64_t)from, (int64_t)num, (int64_t)turn_on);
}

static inline int sys_dup2(int old_fd, int new_fd) {
    return (int)__syscall2(SYS_DUP2, (int64_t)old_fd, (int64_t)new_fd);
}

static inline int sys_fcntl(int fd, int cmd, int arg) {
    return (int)__syscall3(SYS_FCNTL, (int64_t)fd, (int64_t)cmd, (int64_t)arg);
}

static inline int sys_dma_alloc(size_t size, void **vaddr, uint64_t *paddr) {
    return (int)__syscall3(SYS_DMA_ALLOC, (int64_t)size,
        (int64_t)(uintptr_t)vaddr, (int64_t)(uintptr_t)paddr);
}

static inline int sys_dma_free(void *vaddr) {
    return (int)__syscall1(SYS_DMA_FREE, (int64_t)(uintptr_t)vaddr);
}

static inline int sys_pci_dev_info(uint8_t bus, uint8_t dev, uint8_t func,
                                    struct pci_dev_info *out) {
    return (int)__syscall4(SYS_PCI_DEV_INFO, (int64_t)bus, (int64_t)dev,
        (int64_t)func, (int64_t)(uintptr_t)out);
}

// Async block I/O: returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY with cookie+result+lba+count in data.
static inline int sys_block_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
    return (int)__syscall4(SYS_BLOCK_ASYNC, (int64_t)lba,
        (int64_t)(uintptr_t)buf, (int64_t)count, (int64_t)dir);
}

// sys_install_fd(fs_pid, fs_fd, offset, flags, file_size) — syscall 29
// Register an FD_FILE fd in the kernel fd_table.
// Called by libc open() after getting fs_fd from fs_driver.
// Returns: fd (>=3) on success, negative errno on failure
static inline int sys_install_fd(int32_t fs_pid, int32_t fs_fd,
                                  uint64_t offset, int flags,
                                  uint64_t file_size) {
    return (int)__syscall5(SYS_INSTALL_FD, (int64_t)fs_pid, (int64_t)fs_fd,
        (int64_t)offset, (int64_t)flags, (int64_t)file_size);
}

static inline int64_t sys_lseek(int fd, int64_t offset, int whence) {
    return __syscall3(SYS_LSEEK, (int64_t)fd, (int64_t)offset, (int64_t)whence);
}

// ===================== memfd_create / ftruncate =====================
static inline int sys_memfd_create(const char *name, unsigned int flags) {
    return (int)__syscall2(SYS_MEMFD_CREATE, (int64_t)(uintptr_t)name, (int64_t)flags);
}

static inline int sys_ftruncate(int fd, int64_t size) {
    return (int)__syscall2(SYS_FTRUNCATE, (int64_t)fd, (int64_t)size);
}

// ===================== Signal syscalls =====================
struct sigaction;  // forward declaration (defined in common/signal.h or user/include/signal.h)
static inline int sys_kill(int32_t pid, int sig) {
    return (int)__syscall2(SYS_KILL, (int64_t)pid, (int64_t)sig);
}

static inline int sys_sigaction(int sig, const struct sigaction *act,
                                struct sigaction *oldact) {
    return (int)__syscall3(SYS_SIGACTION, (int64_t)sig,
        (int64_t)(uintptr_t)act, (int64_t)(uintptr_t)oldact);
}

static inline int sys_sigreturn(void) {
    return (int)__syscall0(SYS_SIGRETURN);
}

static inline int sys_debug_print(const char *buf, int len) {
    return (int)__syscall2(SYS_DEBUG_PRINT, (int64_t)(uintptr_t)buf, (int64_t)len);
}

// ===================== VFS syscalls =====================
static inline uint64_t sys_open(const char *path, int flags, ...) {
    return (uint64_t)__syscall2(SYS_OPEN, (int64_t)(uintptr_t)path, (int64_t)flags);
}

static inline int sys_stat(const char *path, void *stat_buf) {
    return (int)__syscall2(SYS_STAT, (int64_t)(uintptr_t)path, (int64_t)(uintptr_t)stat_buf);
}

static inline int sys_mkdir(const char *path, uint32_t mode) {
    return (int)__syscall2(SYS_MKDIR, (int64_t)(uintptr_t)path, (int64_t)mode);
}

static inline int sys_unlink(const char *path) {
    return (int)__syscall1(SYS_UNLINK, (int64_t)(uintptr_t)path);
}

static inline int sys_rmdir(const char *path) {
    return (int)__syscall1(SYS_RMDIR, (int64_t)(uintptr_t)path);
}

static inline int sys_dev_create(const char *name, uint32_t dev_type) {
    return (int)__syscall2(SYS_DEV_CREATE, (int64_t)(uintptr_t)name, (int64_t)dev_type);
}

static inline int sys_getdents(int fd, void *buf, size_t len) {
    return (int)__syscall3(SYS_GETDENTS, (int64_t)fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline long sys_ioctl(int fd, uint32_t cmd, uint64_t arg) {
    return __syscall3(SYS_IOCTL, (int64_t)fd, (int64_t)cmd, arg);
}

static inline long sys_fstat(int fd, uint64_t buf) {
    return __syscall2(SYS_FSTAT, (int64_t)fd, buf);
}

static inline long sys_fdev_pid(int fd) {
    return __syscall1(SYS_FDEV_PID, (int64_t)fd);
}
#endif // __KERNEL__

#endif // COMMON_SYSCALL_H
