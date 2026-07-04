#include <stddef.h>
#include <stdint.h>
#include "xos/syscall_nums.h"

// bootstrap 阶段可用：纯 syscall，不依赖 GOT/printf
// fd=2 是 stderr，execve 后 fd 表保留 stdin/stdout/stderr
// ld.md §3.2.4
// hidden：跨文件可见但不导出动态符号表，避免走 PLT（bootstrap 前 GOT 未填）

__attribute__((visibility("hidden")))
void dl_puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    // 直接 syscall，不走 PLT
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_WRITE), "D"(2), "S"(s), "d"(len)
                 : "rcx", "r11", "memory");
}

// dl_put_hex：打印寄存器值/地址，bootstrap 早期可用
// hexdigits 用局部数组（栈上），不依赖 .rodata 重定位
__attribute__((visibility("hidden")))
void dl_put_hex(uint64_t val) {
    char buf[17];
    const char hexdigits[] = "0123456789abcdef";  // 局部数组，栈上
    for (int i = 15; i >= 0; i--) {
        buf[i] = hexdigits[val & 0xf];
        val >>= 4;
    }
    buf[16] = '\n';
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_WRITE), "D"(2), "S"(buf), "d"(17)
                 : "rcx", "r11", "memory");
}
