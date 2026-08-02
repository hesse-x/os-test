# libc — 用户态 C 标准库

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 输出机制 | musl 上游 FILE（`struct _IO_FILE`）+ `__stdio_write` | stdio 整体迁 musl（`musl_stdio_objs`，详见下“stdio 模块”）；printf/fread/fopen 全家由 musl `src/stdio/*` 提供 |
| 2 | printf 格式 | musl vfprintf 全集（含 %f/%g/%e 浮点） | %s/%d/%u/%c/%x/%X/%p/%ld/%lu/%lX/%% + 宽度/精度/左对齐/标志 + %f/%g/%e（vfprintf 内置 `fmt_fp`，链接 libm）。旧手写版无 %f 的缺口随迁移补齐 |
| 3 | FILE 结构 | musl `struct _IO_FILE`（`src/internal/stdio_impl.h`） | flags/rpos/rend/close/wend/wpos/wbase/read/write/seek/buf/buf_size/prev/next/fd/.../lock/cookie。旧手写 `struct _FILE` 已退役 |
| 4 | libc _start | 提供 | 用户程序写 main() 即可 |
| 5 | stdout buffer | musl 行缓冲，串口下退化无缓冲 | musl `__stdout_write` 探 `TIOCGWINSZ`；`/dev/serial` 只答 `TCGETS` → `lbf=-1` 无缓冲。PTY 答案 TIOCGWINSZ 保持行缓冲。exit 时 `__stdio_exit` 仍 flush 全部流（详见 todo.md“stdio 全量迁移”剩余缺口） |
| 6 | stderr | 无缓冲（`__stderr_write`） | 错误输出立即可见 |
| 7 | 用户态 malloc | size-class slab + sys_mmap | 替代旧 sbrk + 显式空闲链表。详见 [mem.md](mem.md) |
| 8 | 时间接口 | timespec_get(TIME_UTC) + clock() | 内核 sys_gettime/sys_clock 封装，C11/C99 标准接口 |
| 9 | 时间语义 | 单调时间（非 wall time） | 内核无 RTC，timespec_get 返回系统启动后单调时间；clock 返回进程 CPU 时间 |
| 10 | 数学库（libm） | 独立 `libm.a`/`libm.so`，`__builtin_*` 包装器 | GCC x86-64 builtin 直接编译为 `fsin`/`fcos`/`sqrtsd` 等指令，无需手写数值算法。`math.h` 的 `static inline` 在 `-O0` 和 `hypot` 等场景仍需外部符号 |
| 11 | libm 构建 | `add_user_lib(m ...)` + `add_user_lib(m_so ...)`，独立 `libm.map` 版本脚本 | libm 是独立标准库，不与 libc 合并维护。`LIBM_1.0` 版本节点导出全部 math.h 符号 + sincos 等 GNU 扩展 |

### stdio 模块（musl 上游）

stdio 整体迁 musl 上游 `src/stdio/*`（`musl_stdio_objs`，最后一个核心 libc 模块；详见 todo.md“stdio 全量迁移到 musl”）。`user/include/stdio.h` 是薄 shim，纯转发 `#include "musl/include/stdio.h"`（编译期 `-I third_party` 解析；`install-headers.sh` §3k 发布 musl 真身到 sysroot）。旧手写 `user/lib/stdio.cc`（799 行）+ `user/include/stdio.h`（自定义 `struct _FILE`）已退役。

FILE 是 musl `struct _IO_FILE`（`third_party/musl/src/internal/stdio_impl.h`）：`flags/rpos/rend/close/wend/wpos/wbase/read/write/seek/buf/buf_size/prev/next/fd/.../lock/cookie`。`bits/alltypes.h` 已 `typedef struct _IO_FILE FILE`。全树无非-stdio 代码解引用 FILE 字段（全走 API），布局翻转无害。

提供全家：printf/scanf 引擎（vfprintf/vfscanf）+ 包装器、字节 getc/putc 家族 + `*_unlocked`、fopen/fclose/fread/fwrite/fseek/ftell + fseeko/ftello、getdelim/getline、open_memstream/fmemopen/fopencookie、flockfile/...、asprintf/vasprintf。vfprintf 内置 `fmt_fp` 浮点打印（链接 libm，补齐旧手写版 `%f`/`%g`/`%e` 缺口）；vfscanf 经 `__intscan`/`__floatscan`（在 `musl_stdlib_objs`）。

ofl 链（open-FILE-list）转真：`__ofl_lock`/`__ofl_unlock`/`__ofl_add`（`ofl.c`/`ofl_add.c` 在 `musl_pthread`）把 FILE 挂全局链；`__lockfile.c`（FLOCK/FUNLOCK）真被 musl stdio 路径调用；`__stdin_used`/`__stdout_used`/`__stderr_used` 由 musl stdin.c/stdout.c/stderr.c 强定义；exit 时 `__stdio_exit`（`__stdio_exit.c`，`musl_stdio_objs` glob）走 ofl + `*_used` flush 全部流。

exclude 名单（8 防多定义/拖依赖 + 23 宽字符）：`ofl.c`/`__lockfile.c`（在 `musl_pthread`）、`rename.c`（在 `musl_unistd_objs`）、`popen.c`/`pclose.c`（需 posix_spawn）、`tmpfile.c`/`tmpnam.c`/`tempnam.c`（需 `__randname`→`__clock_gettime`，time 未迁）；`dprintf.c`/`vdprintf.c` **不** exclude——loader 直接解析到 musl 原生版（走 vfprintf），所有 loader 调用点都在 `reloc_all(&ldso)`（`dynlink.c:1432`）之后，PLT 已就绪，无需 boot-safe shim。23 个宽字符 w* 文件（`vfwprintf`/`vfwscanf`/`fgetwc`/...）引用 `isw*`/`wcsnlen`/`btowc`/`__c_locale` 等未编译符号，带上会导致 ld-musl 运行期重定位失败，且本 OS 无宽字符消费方、不发布 `<wchar.h>`，故排除。`__stdio_exit.c` 由 glob 带入（旧手写 `stdio_exit.c` 已删）。`libc.map` `<stdio.h>` 块导出 81 个窄字符符号；内部 `__isoc99_*`/`_IO_*` 别名隐藏。multibyte 缩到窄路径最小集（`wctomb`/`wcrtomb`/`mbrtowc`/`mbsinit`/`internal`，供 `%ls` link-time 引用）。详见 todo.md“stdio 全量迁移”剩余缺口。

stdout 行为变化（非 bug）：musl `__stdout_write` 探 `TIOCGWINSZ`，`/dev/serial` 只答 `TCGETS` → stdout `lbf=-1` 无缓冲（每字符一次 `sys_write`）；PTY 答 TIOCGWINSZ 保持行缓冲。功能不损（exit flush 生效）。

### _start 入口点

`_start` → `__libc_start_main`（musl `src/env`，`musl_stdlib_objs`）→ main → exit。详见 todo.md“stdlib 全量迁移到 musl”节。

ld 按需拉入 libc.a 成员。shell 链接 libc.a 时 shell.o 已定义 _start，libc.a 的 _start.o 不会被拉入，无冲突。

### 时间函数

user/include/time.h : struct timespec / timespec_get / clock / CLOCKS_PER_SEC / TIME_UTC

`timespec_get`/`clock` 及其余 `<time.h>` 全量来自 musl（`musl_time_objs`，`time.cmake`）；`sleep`/`usleep` 来自 musl `src/unistd/{sleep,usleep}.c`（`musl_unistd_objs`，POSIX 语义：EINTR 返回剩余）。仓库已无 `time.cc`。

内核侧时间 syscall：

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 18 | sys_gettime | uint64_t sys_gettime() | sched_clock() 纳秒，全局单调时钟 |
| 19 | sys_clock | uint64_t sys_clock() | 当前进程 cpu_time_ns |

per-process CPU 时间记账：proc_t 字段 cpu_time_ns + last_sched。schedule() 切出前累加 cpu_time_ns，sys_exit 设 ZOMBIE 前最终记账。详见 [schedule.md](schedule.md)。

timespec 结构字段：tv_sec : time_t, tv_nsec : long

### 标准头补全

阶段 0 新增的标准头（libdrm/libinput 接入前置）：

| 头文件 | 内容 | 备注 |
|--------|------|------|
| `alloca.h` | `alloca(sz)` → `__builtin_alloca(sz)` | 纯宏 |
| `libgen.h` | `basename(char *path)` 声明 | POSIX 语义（musl `src/misc/basename.c`，可能修改入参：截断尾部 `/`） |
| `sys/time.h` | `#include <xos/time.h>` | 最小集：提供 `struct timeval`/`time_t`；`gettimeofday` 等留待按需补 |
| `sys/param.h` | `PAGE_SIZE`（来自 `arch/x64/memlayout.h`）/ `PAGESIZE` / `MIN` / `MAX` | `PAGE_SIZE` 为架构相关常量，不硬编码 |

`basename` 由 musl `src/misc/basename.c` 提供，随 `musl_string_objs` 一并编入 libc（详见 `string.md`）；POSIX 语义，会截断尾部 `/`（写入 `\0`），故不可对只读字面量调用。`__xpg_basename` 为其 weak alias。

`getpagesize` 实现在 user/lib/unistd.cc：返回 4096（x86-64）。

### 链接方式

add_user_elf(name [C] SOURCES ... LINK_LIBS c)。ld -Ttext 0x400000 链接 libc.a。libc.a 按需拉入成员。

include 路径：-I. -Iuser/include → 自定义头文件优先。宿主机 freestanding 头文件（stdint.h, stddef.h, stdarg.h）通过 gcc 默认路径可用。

### libc 模块

| 模块 | 源文件 | 内容 |
|------|--------|------|
| _start | musl `src/env/__libc_start_main.c`（`musl_stdlib_objs`） | `__libc_start_main` + `__init_libc` + `libc_start_init`（weak → 动态路径被 dynlink `do_init_fini` 覆盖）。退役手写 `start_main.cc`/`musl_startup.c`（stdlib.md） |
| stdio | musl `src/stdio/*`（`musl_stdio_objs`） | FILE(`struct _IO_FILE`), printf/vfprintf/vfscanf, getc/putc 家族 + `*_unlocked`, fopen/fclose/fread/fwrite/fseek/ftell + fseeko/ftello, getdelim/getline, open_memstream/fmemopen/fopencookie, flockfile/..., asprintf/vasprintf。`user/include/stdio.h` 薄 shim 转 musl 头；详见上“stdio 模块” |
| string | musl `src/string/*`（`musl_string_objs`） | strlen/strcmp/strcpy/strcat/strchr, memcpy/memmove/memset(x86_64 asm), **ffs**/basename/dirname |
| malloc | user/lib/malloc.cc | size-class slab + sys_mmap |
| time | musl `src/time/*`（`musl_time_objs`） | timespec_get, clock, nanosleep/clock_nanosleep, gmtime/localtime/mktime, strftime/strptime, __tz |
| unistd | musl `src/unistd/*`（`musl_unistd_objs`）+ user/lib/unistd.cc 残留 | POSIX syscall 封装（含 sleep/usleep，POSIX EINTR 返剩余） |
| file | user/lib/file.cc | fopen/fclose/fread/fwrite/fseek/rewind/feof/ferror/freopen/ftell/fdopen/flockfile/funlockfile |
| stdlib | musl `src/stdlib/*`+`src/prng/{rand,rand_r}`+`src/internal/{intscan,shgetc,floatscan}`+`src/env/*`+`src/exit/*`（`musl_stdlib_objs`） | abs/labs/llabs/imaxabs/imaxdiv/div/ldiv/lldiv, atoi/atol/atoll, strtol 全家(strtoimax/strtoumax), strtod/strtof/strtold/atof, qsort/bsearch, rand/srand/rand_r(RAND_MAX=0x7fffffff), environ/getenv/setenv/putenv/unsetenv/clearenv, exit/atexit/abort/quick_exit/at_quick_exit/_Exit, __libc_start_main 启动链 |
| stdlib_misc | user/lib/stdlib_misc.c | 暂留子集：mkstemp/mktemp/realpath/mknod/chmod/getpagesize/sysconf（musl 无法替换，详见 todo.md）。remove/getline/getdelim/fscanf/scanf/sscanf/vfscanf 已随 stdio 迁 musl 移出 |
| sys_ipc | user/lib/sys_ipc.cc | IPC 封装 |
| sys_shm | user/lib/sys_shm.cc | SHM 封装 |
| sys_wait | user/lib/sys_wait.cc | waitpid 封装 |
| sys_mman | user/lib/sys_mman.cc | mmap/munmap 封装 |
| sys_irq | user/lib/sys_irq.cc | IRQ 封装 |
| sys_device | user/lib/sys_device.cc | 设备接口封装（device_register_shm, dev_wait_ready） |
| sys_process | user/lib/sys_process.cc | fork/execve/waitpid/setsid/setpgid/getpgid/getsid/getuid/geteuid/getgid/getegid/setuid/setgid/getppid/getpgrp/umask/gethostname/sethostname |
| sys_pci | user/lib/sys_pci.cc | PCI 封装 |
| signal | user/lib/signal.cc | 信号封装（kill/sigaction/sigprocmask/sigpending/raise/signal/alarm/pause） |
| ctype | user/lib/ctype.c | isdigit/isalpha 等 |
| uname | user/lib/uname.c | uname |
| assert | user/lib/assert.c | assert |
| errno | user/lib/errno.cc | errno TLS + strerror |
| input_client | user/lib/input_client.cc | input SHM ring 客户端（input_client_poll） |
| tls | user/lib/tls.cc | TLS（errno, pthread 所需） |
| pthread | user/lib/pthread.cc | pthread_mutex_t / pthread_cond_t |
| setjmp | user/lib/setjmp.S | setjmp/longjmp（x86-64 寄存器保存） |
| io_multiplex | user/lib/io_multiplex.cc | ppoll/pselect6 封装 |
| fnmatch | user/lib/fnmatch.c | POSIX fnmatch（libinput 依赖） |

### libm 模块

`libm.a`（静态）+ `libm.so`（动态）是两个独立的 CMake target（`m` 和 `m_so`），编译同组源文件。

| 源文件 | 内容 |
|--------|------|
| `lib/math/math_basic.c` | double 精度函数，全部 `__builtin_*` 包装（sin/cos/tan/sqrt/hypot/fmod/pow/exp/log/log2/floor/ceil/round/fabs/atan2 等 44 个函数） |
| `lib/math/math_float.c` | float 精度函数，同上模式（sinf/cosf/sqrtf/hypotf/fmodf/powf/expf/logf/floorf/ceilf/roundf/fabsf/atan2f 等 32 个函数） |
| `lib/math/sincos.c` | `sincos`/`sincosf`（GNU 扩展，GCC builtin 分别调 sin+cos） |

编译标志 `-D__LIBM_BUILD__` 防止 `math.h` 的 `static inline` 内联函数与 out-of-line 定义冲突。`libm.so` 使用 `-fvisibility=default`（区别于 libinput.so 的 hidden）确保所有 math.h 函数对外可见。

符号版本脚本 `user/libm.map` 定义 `LIBM_1.0` 版本节点，导出全部 `math.h` 函数 + `sincos`/`sincosf`，内部符号 `local: *;`。

libm 编译不依赖 libc（freestanding，`-nostdlib`），但运行时 libm.so 的 `DT_NEEDED` 依赖 libc.so（链接时通过 `SO_LINK_LIBS c` 记录）。

### 与其他模块的关系

| 模块 | 说明 |
|------|------|
| 内存管理 | malloc 底层 sys_mmap。详见 [mem.md](mem.md) |
| 系统调用 | libc 所有 syscall 封装。详见 [syscall.md](syscall.md) |
| 构建系统 | libc.a 为 CMake target c。详见 [cmake.md](cmake.md) |

### 关键源码位置

- FILE 和 printf：musl `src/stdio/*`（`musl_stdio_objs`）/ `user/include/stdio.h`（薄 shim 转 musl 头）
- _start：musl `src/env/__libc_start_main.c`（`musl_stdlib_objs`）
- string：user/lib/string.cc / user/include/string.h
- malloc：user/lib/malloc.cc / user/include/stdlib.h
- time：musl `src/time/*`（`musl_time_objs`）/ `user/include/time.h`；`sleep`/`usleep` 走 musl `src/unistd/{sleep,usleep}.c`
- syscall 封装：arch/x64/utils.h : __syscall0-6 / common/syscall.h

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| time() / gettimeofday() | wall time 接口，需 RTC 硬件支撑 | 低 |
| strftime | 时间格式化，依赖 time() | 低 |
