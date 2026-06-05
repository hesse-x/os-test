#include "arch/x64/utils.h"

static inline void putc(char c)       { syscall1(SYS_PUTC, c); }
static inline char getc()             { return (char)syscall0(SYS_GETC); }

// Minimal shell: prompt + echo + newline
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
