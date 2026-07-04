#ifndef _ERRNO_H
#define _ERRNO_H

#include "xos/errno.h"

#ifdef __cplusplus
extern "C" {
#endif

// errno 通过 __errno_location() 返回 TLS 指针，每个线程独立。
// 动态链接下避免主 ELF 与 libc.so 各持一份 errno 副本导致写读分离
// （R_X86_64_COPY 把 libc.so 的 errno 拷贝到主 ELF .bss，libc.so 内部
//  syscall 失败写自己 .bss 的 errno，主 ELF 读自己的 COPY 副本永远拿 0）。
// TLS 模式：errno 在 TLS 模板里，主 ELF 和 libc.so 共享当前线程的同一份。
int *__errno_location(void);
#define errno (*__errno_location())

#ifdef __cplusplus
}
#endif

#endif /* _ERRNO_H */
