#ifndef _SETJMP_H
#define _SETJMP_H

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* jmp_buf：保存 8 个 callee-saved 寄存器（rbx rbp r12 r13 r14 r15 rsp rip）。
 * 定义为 long long[8] 保证 8 字节对齐。rip 存的是 setjmp 调用点之后的
 * 返回地址，longjmp 恢复后从那里继续。不保存信号掩码（C 标准 setjmp
 * 不要求；sigsetjmp/siglongjmp 见 todo.md）。 */
typedef long long jmp_buf[8];

LIBC_EXPORT int setjmp(jmp_buf env);
LIBC_EXPORT void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX 别名（无下划线） */
LIBC_EXPORT int _setjmp(jmp_buf env);
LIBC_EXPORT void _longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* _SETJMP_H */
