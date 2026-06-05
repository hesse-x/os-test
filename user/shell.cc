#include "syscall.h"

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
