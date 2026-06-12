#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include <stdint.h>
#include "arch/x64/utils.h"

// ===================== Syscall numbers =====================
#define SYS_PUTC     0
#define SYS_GETPID   1
#define SYS_YIELD    2
#define SYS_GETC     3
#define SYS_WAIT     4
#define SYS_NOTIFY   5
#define SYS_IRQ_BIND 6
#define SYS_SBRK     7
#define SYS_EXIT     8
#define SYS_WAITPID  9
#define SYS_SPAWN    10
#define SYS_MMAP     11
#define SYS_MUNMAP   12
#define SYS_SERIAL_WRITE 13
#define SYS_FB_INFO      14
#define SYS_SHM_CREATE   15
#define SYS_SHM_ATTACH   16

// ===================== Syscall helpers (arch-specific) =====================
// Defined in arch/x64/utils.h as __syscall0, __syscall1, etc.

// ===================== Syscall return convention =====================
// 0 = success (for status-only syscalls: wait/notify/exit/irq_bind/munmap)
// positive errno = error (for status-only syscalls)
// For value-returning syscalls: 0 = failure, nonzero = success
//   sys_sbrk: returns old brk (>=0x600000) on success, 0 on failure
//   sys_mmap: returns mapped address on success, NULL on failure
//   sys_spawn/sys_waitpid: returns pid on success, 0 on failure
//   sys_getpid: always succeeds (returns pid >= 1)

// ===================== Semantic wrappers =====================
static inline int sys_putc(char c) {
    return (int)__syscall1(SYS_PUTC, (int64_t)c);
}

static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

static inline int sys_getc() {
    return (int)__syscall0(SYS_GETC);
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

static inline uint64_t sys_sbrk(int64_t increment) {
    return (uint64_t)__syscall1(SYS_SBRK, increment);
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

#endif // COMMON_SYSCALL_H
