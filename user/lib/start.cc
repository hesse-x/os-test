#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <unistd.h>
#include "common/syscall.h"
#include "sys/tls.h"

extern "C" int main(void);

extern "C" __attribute__((naked)) void _start() {
    __asm__ volatile(
        "andq $-16, %%rsp\n\t"
        "subq $8, %%rsp\n\t"
        "jmp __libc_start\n\t"
        :::);
}

// TLS 初始化 + main + exit_group（杀全部线程）
extern "C" void __libc_start() {
    __libc_tls_init();

    fflush(stdout);
    int ret = main();
    fflush(stdout);

    // main 返回时用 exit_group 杀全部线程
    sys_exit_group(ret);
    __builtin_unreachable();
}
