#include "arch/x64/utils.h"

static inline void putc(char c)       { syscall1(SYS_PUTC, c); }
static inline char getc()             { return (char)syscall0(SYS_GETC); }
static inline long getpid()           { return syscall0(SYS_GETPID); }

static void print_num(long n) {
    if (n >= 10) print_num(n / 10);
    putc('0' + (n % 10));
}

// Minimal shell: prompt with pid + echo + newline
extern "C" void _start() {
    while (1) {
        putc('>');
        putc(' ');

        while (1) {
            char c = getc();
            putc(c);
            if (c == '\n') break;
        }
    }
}
