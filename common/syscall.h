#ifndef COMMON_SYSCALL_H
#define COMMON_SYSCALL_H

#include <stdint.h>

// ===================== Syscall numbers =====================
#define SYS_PUTC     0
#define SYS_GETPID   1
#define SYS_YIELD    2
#define SYS_GETC     3
#define SYS_WAIT     4
#define SYS_NOTIFY   5
#define SYS_IRQ_BIND 6

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

#endif // COMMON_SYSCALL_H
