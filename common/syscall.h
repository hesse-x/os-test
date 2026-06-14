#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include <stdint.h>
#include "arch/x64/utils.h"

// ===================== Syscall numbers =====================
#define SYS_GETPID       0
#define SYS_YIELD        1
#define SYS_WAIT         2
#define SYS_NOTIFY       3
#define SYS_IRQ_BIND     4
#define SYS_EXIT         5
#define SYS_WAITPID      6
#define SYS_SPAWN        7
#define SYS_MMAP         8
#define SYS_MUNMAP       9
#define SYS_SERIAL_WRITE 10
#define SYS_FB_INFO      11
#define SYS_SHM_CREATE   12
#define SYS_SHM_ATTACH   13
#define SYS_PIPE         14
#define SYS_WRITE        15
#define SYS_READ         16
#define SYS_CLOSE        17
#define SYS_LOAD_DEV     18
#define SYS_LOOKUP_DEV   19

// ===================== Syscall helpers (arch-specific) =====================
// Defined in arch/x64/utils.h as __syscall0, __syscall1, etc.

// ===================== Syscall return convention =====================
// 0 = success (for status-only syscalls: wait/notify/exit/irq_bind/munmap)
// positive errno = error (for status-only syscalls)
// For value-returning syscalls: 0 = failure, nonzero = success
//   sys_mmap: returns mapped address on success, NULL on failure
//   sys_spawn/sys_waitpid: returns pid on success, 0 on failure
//   sys_getpid: always succeeds (returns pid >= 1)

// ===================== Semantic wrappers =====================
static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

static inline int sys_wait(uint32_t timeout_ms) {
    return (int)__syscall1(SYS_WAIT, (int64_t)timeout_ms);
}

static inline int sys_notify(int32_t pid) {
    return (int)__syscall1(SYS_NOTIFY, (int64_t)pid);
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

static inline int64_t sys_spawn(const void *elf_data, uint64_t elf_size, uint32_t iopl) {
    return __syscall3(SYS_SPAWN, (int64_t)(uintptr_t)elf_data, (int64_t)elf_size, (int64_t)iopl);
}

static inline void *sys_mmap(size_t size) {
    return (void *)__syscall1(SYS_MMAP, (int64_t)size);
}

static inline int sys_munmap(void *addr, size_t size) {
    return (int)__syscall2(SYS_MUNMAP, (int64_t)(uintptr_t)addr, (int64_t)size);
}

static inline int sys_serial_write(const char *buf, size_t len) {
    return (int)__syscall2(SYS_SERIAL_WRITE, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int sys_fb_info(void *buf) {
    return (int)__syscall1(SYS_FB_INFO, (int64_t)(uintptr_t)buf);
}

static inline void *sys_shm_create(size_t size) {
    return (void *)__syscall1(SYS_SHM_CREATE, (int64_t)size);
}

static inline void *sys_shm_attach(int32_t target_pid) {
    return (void *)__syscall1(SYS_SHM_ATTACH, (int64_t)target_pid);
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

static inline int sys_load_dev(int32_t pid, int dev_type) {
    return (int)__syscall2(SYS_LOAD_DEV, (int64_t)pid, (int64_t)dev_type);
}

static inline int32_t sys_lookup_dev(int dev_type) {
    return (int32_t)__syscall1(SYS_LOOKUP_DEV, (int64_t)dev_type);
}

#endif // COMMON_SYSCALL_H
