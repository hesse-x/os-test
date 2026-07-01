#include "stdio.h"
#include <unistd.h>

extern "C" int main(void);

// _start is entered via iret (not call), so RSP is 16-byte aligned on entry.
// The SysV ABI requires a called function to see RSP%16==8 (as if a return
// address had just been pushed). gcc's prologue for _start assumes exactly
// that, so we must inject an 8-byte adjustment before letting gcc generate
// its prologue. We do it with a naked helper that aligns the stack and
// tail-callss into the C body.
//
// Without this, every call from _start drifts by 8 bytes and movaps/movdqa
// in callees (now generated because user-space enables SSE) triggers #GP.
extern "C" __attribute__((naked)) void _start() {
    __asm__ volatile(
        "andq $-16, %%rsp\n\t"   // RSP%16 == 0
        "subq $8, %%rsp\n\t"     // RSP%16 == 8  (mimic "just after call")
        "jmp __libc_start\n\t"
        :::);
}

extern "C" void __libc_start() {
    fflush(stdout);

    int ret = main();

    fflush(stdout);
    _exit(ret);
}