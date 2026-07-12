# libc — 用户态 C 标准库

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 输出机制 | FILE 缓冲 + write_fn 抽象 | 改 flush 函数即可适配新输出路径，printf 不动 |
| 2 | printf 格式 | 实用集（无 %f） | %s/%d/%u/%c/%x/%X/%p/%ld/%lu/%lX/%% + 宽度前缀（%02x），覆盖 OS 调试场景 |
| 3 | FILE 结构 | fd/buffer/mode/flags/write_fn | 为 read/write 预留，加 sys_read/sys_write 后 FILE 可直接使用 |
| 4 | libc _start | 提供 | 用户程序写 main() 即可 |
| 5 | stdout buffer | 静态数组（1024 字节） | 不依赖 malloc，malloc 坏了 printf 还可用于调试 |
| 6 | stderr | 无缓冲（_IONBF） | 错误输出立即可见 |
| 7 | 用户态 malloc | size-class slab + sys_mmap | 替代旧 sbrk + 显式空闲链表。详见 [mem.md](mem.md) |
| 8 | 时间接口 | timespec_get(TIME_UTC) + clock() | 内核 sys_gettime/sys_clock 封装，C11/C99 标准接口 |
| 9 | 时间语义 | 单调时间（非 wall time） | 内核无 RTC，timespec_get 返回系统启动后单调时间；clock 返回进程 CPU 时间 |
| 10 | 数学库（libm） | 独立 `libm.a`/`libm.so`，`__builtin_*` 包装器 | GCC x86-64 builtin 直接编译为 `fsin`/`fcos`/`sqrtsd` 等指令，无需手写数值算法。`math.h` 的 `static inline` 在 `-O0` 和 `hypot` 等场景仍需外部符号 |
| 11 | libm 构建 | `add_user_lib(m ...)` + `add_user_lib(m_so ...)`，独立 `libm.map` 版本脚本 | libm 是独立标准库，不与 libc 合并维护。`LIBM_1.0` 版本节点导出全部 math.h 符号 + sincos 等 GNU 扩展 |

### FILE 结构体

user/include/stdio.h : FILE（typedef struct _FILE）

字段：
- fd : int — 文件描述符（stdout=1, stderr=2, stdin=0）
- buf : char* — I/O 缓冲区
- buf_size : int — 缓冲区容量
- buf_pos : int — 缓冲区当前写入位置
- buf_mode : int — _IONBF / _IOLBF / _IOFBF
- flags : int — _F_WRITE / _F_READ / _F_EOF / _F_ERR
- write_fn : 函数指针 — 输出函数（FILE*, const char*, int len）
- read_fn : 函数指针 — 输入函数（FILE*, char*, int len）
- offset : off_t — 当前文件偏移（fseek/ftell）
- ungot : int — ungetc 推回（-1 表示无）
- lock : pthread_mutex_t — per-FILE 锁（flockfile/funlockfile）
- user_data : void* — 自定义流用户指针（open_memstream 存 memstream_ctx）

常量：EOF(-1), _IONBF(0), _IOLBF(1), _IOFBF(2), _F_WRITE(1), _F_READ(2), _F_EOF(4), _F_ERR(8)

### stdout / stderr

user/lib/stdio.cc 内部：
- stdout：fd=1，line-buffered（_IOLBF），静态 buf[1024]，write_fn=sys_putc_flush
- stderr：fd=2，unbuffered（_IONBF），write_fn=sys_putc_flush

sys_putc_flush：逐字节 sys_putc 输出。sys_write_flush（一次 syscall 输出整段）为待完成项。

### printf / vfprintf

user/lib/stdio.cc : vfprintf

支持的 specifier：%d, %u, %ld, %lu, %x, %X, %lX, %p, %s, %c, %%。宽度前缀：%02x。不支持精度、左对齐、+标志。

内部流程：vfprintf 逐字符扫描 fmt → 遇 % 解析参数 → 格式化到 FILE buffer → fputc_internal 判断缓冲条件（unbuffered 直接 write_fn，line-buffered 遇 \n flush，full-buffered 满 flush）。

### _start 入口点

user/lib/start.cc : _start — stdio_init → main → sys_exit(ret)

ld 按需拉入 libc.a 成员。shell 链接 libc.a 时 shell.o 已定义 _start，libc.a 的 _start.o 不会被拉入，无冲突。

### 时间函数

user/include/time.h : struct timespec / timespec_get / clock / CLOCKS_PER_SEC / TIME_UTC

user/lib/time.cc：

timespec_get(ts, TIME_UTC)：sys_gettime() 获取纳秒 → tv_sec = ns / 10^9, tv_nsec = ns % 10^9 → 返回 base。非 TIME_UTC 返回 0。

clock()：sys_clock() 获取 cpu_time_ns → 返回 (clock_t)(ns / 1000)，匹配 CLOCKS_PER_SEC=1000000。

内核侧时间 syscall：

| 编号 | 名称 | 签名 | 说明 |
|------|------|------|------|
| 18 | sys_gettime | uint64_t sys_gettime() | sched_clock() 纳秒，全局单调时钟 |
| 19 | sys_clock | uint64_t sys_clock() | 当前进程 cpu_time_ns |

per-process CPU 时间记账：proc_t 字段 cpu_time_ns + last_sched。schedule() 切出前累加 cpu_time_ns，sys_exit 设 ZOMBIE 前最终记账。详见 [schedule.md](schedule.md)。

timespec 结构字段：tv_sec : time_t, tv_nsec : long

### open_memstream

user/include/stdio.h : open_memstream（POSIX.1-2008 动态内存流）

user/lib/stdio.cc : open_memstream / memstream_write_fn

动态内存流：调用方提供 `char **bufptr` 和 `size_t *sizeptr`，写入的数据通过 `write_fn` 回调实时管理动态缓冲（realloc 扩容）。fclose 释放 FILE 和内部 `memstream_ctx`，但保留 `*bufptr` 缓冲（所有权转用户）。

实现要点（W1/F2 方案）：
- FILE 配置 `_IONBF` + `buf=NULL`，走 `file_putc_internal` 直写路径，不碰 `f->buf`/`f->buf_pos`
- `bufptr`/`sizeptr` 存入堆分配的 `struct memstream_ctx`，挂到 `f->user_data`
- `memstream_write_fn` 从 `user_data` 取 ctx，用 `*sizeptr` 作写入游标、`f->buf_size` 作容量槽
- `fclose` 统一 `if (f->user_data) free(f->user_data)`（F2 逻辑），依赖所有静态 FILE 的 `user_data=NULL` 初始化

### 标准头补全

阶段 0 新增的标准头（libdrm/libinput 接入前置）：

| 头文件 | 内容 | 备注 |
|--------|------|------|
| `alloca.h` | `alloca(sz)` → `__builtin_alloca(sz)` | 纯宏 |
| `libgen.h` | `basename(char *path)` 声明 | glibc 语义（返回内部指针，不修改入参） |
| `sys/time.h` | `#include <xos/time.h>` | 最小集：提供 `struct timeval`/`time_t`；`gettimeofday` 等留待按需补 |
| `sys/param.h` | `PAGE_SIZE`（来自 `arch/x64/memlayout.h`）/ `PAGESIZE` / `MIN` / `MAX` | `PAGE_SIZE` 为架构相关常量，不硬编码 |

`basename` 实现在 user/lib/string.cc：`strrchr(path, '/') + 1`，无 `/` 返回原指针。等价 glibc `_GNU_SOURCE` 版，libdrm 调用安全。

`getpagesize` 实现在 user/lib/unistd.cc：返回 4096（x86-64）。

### 链接方式

add_user_elf(name [C] SOURCES ... LINK_LIBS c)。ld -Ttext 0x400000 链接 libc.a。libc.a 按需拉入成员。

include 路径：-I. -Iuser/include → 自定义头文件优先。宿主机 freestanding 头文件（stdint.h, stddef.h, stdarg.h）通过 gcc 默认路径可用。

### libc 模块

| 模块 | 源文件 | 内容 |
|------|--------|------|
| _start | user/lib/start_main.cc | `__libc_start_main` 入口点（crt0.o 提供 `_start`） |
| stdio | user/lib/stdio.cc | FILE, printf/vfprintf, fputc/fputs/puts/fflush, **fgets** |
| string | user/lib/string.cc | strlen/strcmp/strcpy/strcat/strchr, memcpy/memset/memmove, **ffs** |
| malloc | user/lib/malloc.cc | size-class slab + sys_mmap |
| time | user/lib/time.cc | timespec_get, clock |
| unistd | user/lib/unistd.cc | POSIX syscall 封装 |
| file | user/lib/file.cc | fopen/fclose/fread/fwrite/fseek/rewind/feof/ferror/freopen/ftell/fdopen/flockfile/funlockfile, **scandir** |
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
| strtol | user/lib/strtol.c | strtol/atoi |
| stdlib_misc | user/lib/stdlib_misc.c | exit/abort/mkstemp/mktemp/realpath/qsort/rand/srand 等 |
| uname | user/lib/uname.c | uname |
| sleep | user/lib/sleep.c | sleep/usleep |
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

- FILE 和 printf：user/lib/stdio.cc / user/include/stdio.h
- _start：user/lib/start.cc
- string：user/lib/string.cc / user/include/string.h
- malloc：user/lib/malloc.cc / user/include/stdlib.h
- time：user/lib/time.cc / user/include/time.h
- syscall 封装：arch/x64/utils.h : __syscall0-6 / common/syscall.h

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| sys_write 输出优化 | 当前 stdout 逐字节 sys_putc，改为 sys_write 一次 syscall 输出整段 | 高 |
| %f 浮点格式化 | 依赖用户态 SSE/FPU 上下文保存 + 浮点格式化算法 | 低 |
| time() / gettimeofday() | wall time 接口，需 RTC 硬件支撑 | 低 |
| strftime | 时间格式化，依赖 time() | 低 |
