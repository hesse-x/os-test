#ifndef FFICONFIG_H
#define FFICONFIG_H
/* ABI/类型特征 */
/* HAVE_LONG_DOUBLE_VARIANT 必须关(上游 configure.ac:88 默认 0,仅
 * configure.host 对 powerpc 等多 long-double 尺寸平台置 1)。定义后
 * prep_cif.c:137/276 的 `#if HAVE_LONG_DOUBLE_VARIANT` 块会发出对
 * ffi_prep_types() 的调用——该函数仅 powerpc 实现(ffi.c:40),x86 无定义,
 * 留下未解析 PLT 符号致 dl 重定位 FATAL。x86_64 long double 是单一 80-bit
 * x87,无变体,无需此机制。注意与 ffi.h.in 的 @HAVE_LONG_DOUBLE@ 解耦——
 * 后者由 _libffi_generate_ffi_header 设 1(ffi.h.in:64),不受影响。 */
#define HAVE_LONG_DOUBLE_VARIANT 0
/* 标准头齐全(对齐上游 autoconf AC_HEADER_STDC → STDC_HEADERS)。ffi_common.h:73
 * 用 `#if STDC_HEADERS #include <string.h>` 否则伪造 `#define memcpy bcopy` ——
 * 我们的 libc 有真 memcpy(string.h),必须走 include 路径,否则宏污染
 * string.h:48 的 memcpy 声明致语法错。 */
#define STDC_HEADERS 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
/* 汇编器能力(对齐上游 configure.ac:176 AC_DEFINE)。unix64.S:723 与
 * sysv.S:960 用 HAVE_AS_X86_PCREL 选 PCREL 宏形式:定义→`X - .`(label 自
 * 减当前地址,PC-relative),否则 fallback `X@rel`(binutils 的 R_X86_64_PC64
 * 风格 @rel 修饰符)。我们的 as 2.42 不接受 `@rel`(实测 .long x@rel 报
 * "junk at end of line"),但接受 `X - .`(实测通过)。定义此宏走 `X - .`
 * 分支,绕开 @rel。手写模板须收录——上游由 autoconf 探测生成,plan2 B1 漏列。 */
#define HAVE_AS_X86_PCREL 1
/* 符号可见性(对齐上游 configure.ac AH_BOTTOM 的双分支)。libffi 内部用
 * FFI_HIDDEN 标记不应导出的符号(ffi_common.h:144/148/152、prep_cif.c:110
 * 等用无参形式;unix64.S:61 等用带参 FFI_HIDDEN(name) 形式)。上游按
 * #ifdef LIBFFI_ASM 分两套:汇编(.S,顶部 #define LIBFFI_ASM)用带参
 * `.hidden name`;C 源用无参 `__attribute__((visibility("hidden")))`。
 * 手写模板须两套都给——只给无参会让 .S 里 FFI_HIDDEN(C(x)) 展开成
 * __attribute__((...))(x) 致 gas 报错;只给带参会让 C 源编译错。
 * 公共 API 仍由 ffi_so 的 -fvisibility=default 全导出。 */
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__((visibility("hidden")))
#endif
/* 闭包路径(closures.c FFI_MMAP_EXEC_WRIT 分支) */
#define FFI_MMAP_EXEC_WRIT 1    /* 走双映射 */
#define FFI_MMAP_EXEC_SELINUX 0 /* 关 selinux 探测(跳过 statfs/proc/mounts) */
#define FFI_MMAP_PAX 0          /* 关 PaX 探测 */
/* HAVE_MNTENT 不定义 → 跳过 mntent.h */
#define HAVE_MMAP 1
#define HAVE_MEMFD_CREATE 1 /* 走 memfd(closures.c:726 opts 第一项) */
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0 /* dlmalloc 内部 */
#define HAVE_ALLOCA_H 0
#define malloc_getpagesize 4096
/* 不定义 FFI_EXEC_STATIC_TRAMP — tramp.c 仍编,但只编 #else 桩分支
 * (ffi_tramp_is_supported()→0,alloc/free no-op),只为解析 closures.c
 * 的链接期符号引用;运行时门控全部走 dlmmap RWX 闭包路径 */
/* 不定义 FFI_DEBUG */
#endif
