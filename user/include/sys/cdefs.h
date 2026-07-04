#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

/* 公开 ABI 符号标记。libc.so 编译加 -fvisibility=hidden，仅 LIBC_EXPORT 标记的
 * 声明导出；内部符号自动 hidden。静态库忽略 visibility。 */
#define LIBC_EXPORT __attribute__((visibility("default")))

#endif /* _SYS_CDEFS_H */
