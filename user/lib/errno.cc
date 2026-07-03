// user/lib/errno.cc — per-thread errno via TCB
//
// errno 经 __errno_location() 返回 &TCB.errno_val（FS_BASE 指向 TCB），
// 每个线程独立。
//
// 解决动态链接下 R_X86_64_COPY 把 libc.so 的 errno 拷贝到主 ELF .bss，
// 导致主 ELF 与 libc.so 各持一份 errno、写读分离的问题（libc.so 内部
// syscall 失败写自己 .bss 的 errno，主 ELF 读自己的 COPY 副本永远拿 0）。
//
// TCB 模式：errno_val 在 TCB 里（FS_BASE 指向），主 ELF 与 libc.so 都通过
// %fs:offset 访问当前线程的同一份 errno。无需 __thread（避免产生
// R_X86_64_DTPMOD64 等通用动态 TLS 重定位，ld.so 暂未实现 __tls_get_addr）。

#include "sys/tls.h"

extern "C" {

int *__errno_location(void) {
    // __pthread_current_tcb() 内部为 movq %fs:0, rax；返回 TCB 指针
    struct tcb *tcb = __pthread_current_tcb();
    return &tcb->errno_val;
}

}  // extern "C"
