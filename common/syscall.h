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

// ===================== Syscall helpers (arch-specific) =====================
// Defined in arch/x64/utils.h as __syscall0, __syscall1, etc.

// ===================== Semantic wrappers =====================
static inline void sys_putc(char c) {
    __syscall1(SYS_PUTC, (int64_t)c);
}

static inline int64_t sys_getpid() {
    return __syscall0(SYS_GETPID);
}

static inline void sys_yield() {
    __syscall0(SYS_YIELD);
}

static inline int64_t sys_getc() {
    return __syscall0(SYS_GETC);
}

static inline void sys_wait() {
    __syscall0(SYS_WAIT);
}

static inline void sys_notify(int32_t pid) {
    __syscall1(SYS_NOTIFY, (int64_t)pid);
}

static inline void sys_irq_bind(int irq) {
    __syscall1(SYS_IRQ_BIND, (int64_t)irq);
}

static inline int64_t sys_sbrk(int64_t increment) {
    return __syscall1(SYS_SBRK, increment);
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

#endif // COMMON_SYSCALL_H
