#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>

// syscall convention: eax=syscall#, ebx=arg1, int 0x80

static inline int32_t syscall0(int32_t num) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num));
    return ret;
}

static inline int32_t syscall1(int32_t num, int32_t arg1) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(arg1));
    return ret;
}

#define SYS_PUTC   0
#define SYS_GETPID 1
#define SYS_YIELD  2
#define SYS_GETC   3

static inline void putc(char c)       { syscall1(SYS_PUTC, c); }
static inline int32_t getpid()        { return syscall0(SYS_GETPID); }
static inline void yield()            { syscall0(SYS_YIELD); }
static inline char getc()             { return (char)syscall0(SYS_GETC); }

#endif // USER_SYSCALL_H
