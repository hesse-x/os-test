# libc.a 静态库 — printf + FILE + 用户态 C 标准库

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 输出机制 | 缓冲 + sys_putc（write_fn 抽象） | 将来加 sys_write 改 flush 函数即可，printf 不动 |
| printf 格式 specifier | 实用集（无 %f） | %s/%d/%u/%c/%x/%X/%p/%ld/%lu/%lX/%% + 宽度前缀（%02x），覆盖 OS 调试常用场景 |
| FILE 结构 | 完整（fd/buffer/mode/flags/write_fn） | 为 read/write 预留，加 sys_read/sys_write 后 FILE 可直接使用 |
| libc _start | 提供 | 用户程序写 `int main()` 即可，不需要知道 sys_exit |
| 改动范围 | 只改 hello | shell/drivers 保持原样，hello 用 libc.a 验证 |
| hello 语言 | C | 验收标准是"宿主机编译标准 C 程序"，hello.c 最自然 |
| stdout buffer | 静态数组（1024 字节） | 不依赖 malloc，malloc 坏了 printf 还能用于调试 |
| stderr | 无缓冲（_IONBF） | 错误输出立即可见，不等缓冲 |
| libc.a 模块划分 | _start.o / stdio.o / string.o / malloc.o | _start 独立避免 shell 链接冲突；ld 按需拉入成员 |
| 完整 printf（%f） | 后续 | 依赖用户态 SSE/FPU 上下文保存 + 浮点格式化算法（Ryu 等） |

## 文件结构

```
user/include/
  stdio.h    — FILE, printf/fprintf/vfprintf/fputc/fputs/puts/fflush, stdout/stderr, buffer mode 常量
  stdlib.h   — malloc/free/calloc/realloc/exit, EXIT_SUCCESS/EXIT_FAILURE
  string.h   — strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr, memcpy/memset/memmove

user/lib/
  stdio.cc   — FILE 实现, stdout/stderr 实例, vfprintf 格式化引擎, fputc/fputs/puts/fflush
  string.cc  — 字符串和内存操作函数
  start.cc   — _start 入口点（stdio_init → main → sys_exit）
  malloc.cc  — 已有，打包进 libc.a（不改动）

user/hello.c — printf("Hello, World!\n"); return 0;
```

## FILE 结构体

```c
typedef struct _FILE {
    int fd;              // 文件描述符（stdout=1, stderr=2, stdin=0）
    char *buf;           // I/O 缓冲区
    int buf_size;        // 缓冲区容量
    int buf_pos;         // 缓冲区当前写入位置
    int buf_mode;        // _IONBF / _IOLBF / _IOFBF
    int flags;           // _F_WRITE / _F_READ / _F_EOF / _F_ERR
    void (*write_fn)(struct _FILE *, const char *, int len);  // 输出函数
} FILE;
```

常量定义：
```c
#define EOF        (-1)
#define _IONBF     0   // 无缓冲
#define _IOLBF     1   // 行缓冲
#define _IOFBF     2   // 全缓冲
#define _F_WRITE   1
#define _F_READ    2
#define _F_EOF     4
#define _F_ERR     8
```

## stdout / stderr 初始化

```c
// stdio.cc 内部
static char stdout_buf[1024];

static void sys_putc_flush(FILE *f, const char *data, int len) {
    for (int i = 0; i < len; i++)
        sys_putc(data[i]);
}

// stdout: line-buffered, fd=1
FILE stdout_file = { 1, stdout_buf, 1024, 0, _IOLBF, _F_WRITE, sys_putc_flush };

// stderr: unbuffered, fd=2
FILE stderr_file = { 2, NULL, 0, 0, _IONBF, _F_WRITE, sys_putc_flush };
```

将来加 sys_write syscall 时，新增 `sys_write_flush`：
```c
static void sys_write_flush(FILE *f, const char *data, int len) {
    sys_write(f->fd, data, len);
}
```

只需在 _start 的 stdio_init 中改 write_fn 赋值，printf/vfprintf 一行不动。

## _start 入口点

```c
// user/lib/start.cc
extern "C" int main(void);
extern "C" void _start() {
    stdio_init();      // 初始化 stdout/stderr（当前已在静态初始化完成，预留扩展点）
    int ret = main();
    sys_exit(ret);
}
```

ld 处理 libc.a 时只拉入解决 undefined 的成员。shell 链接 libc.a 时 shell.o 已定义 _start，libc.a 的 _start.o 不会被拉入，无冲突。

## printf / vfprintf 格式化引擎

支持的 specifier：

| 格式 | 类型 | 说明 |
|------|------|------|
| %d | int | 十进制有符号 |
| %u | unsigned int | 十进制无符号 |
| %ld | long | 十进制有符号长整型 |
| %lu | unsigned long | 十进制无符号长整型 |
| %x | unsigned int | 十六进制小写 |
| %X | unsigned int | 十六进制大写 |
| %lX | unsigned long | 十六进制大写长整型 |
| %p | void* | 指针地址（0x 前缀 + 十六进制） |
| %s | char* | 字符串 |
| %c | char | 单字符 |
| %% | — | 输出 % |

宽度前缀：`%02x`（最小宽度 2，0 填充）。不支持精度（`.3d`）、左对齐（`%-`）、`+` 标志——后续可扩展。

**内部流程**：
1. vfprintf 逐字符扫描 fmt
2. 遇到 `%` → 解析宽度前缀和长度修饰符 → 从 va_list 取参数
3. 格式化到 FILE buffer（调用 fputc_internal）
4. fputc_internal 判断缓冲条件：
   - unbuffered（_IONBF）：直接调 write_fn
   - line-buffered（_IOLBF）：写入 buffer，遇到 `\n` flush
   - full-buffered（_IOFBF）：写入 buffer，buffer 满 flush
5. flush = 调用 FILE.write_fn(f, f->buf, f->buf_pos)，然后 buf_pos=0

## 链接方式

用户程序（hello.c）：
```bash
gcc -m64 -ffreestanding -nostdlib -fno-builtin -fno-pie -fno-stack-protector \
    -I. -Iuser/include -c user/hello.c -o build/hello.o
objcopy --remove-section .note.gnu.property build/hello.o
ld -m elf_x86_64 -Ttext 0x400000 -o build/hello.elf build/hello.o build/libc.a
```

链接顺序：hello.o 先，libc.a 后。ld 扫描 libc.a 按需拉入：
- hello.o 定义 main → undefined printf, exit
- libc.a: stdio.o（printf）→ 拉入 → 引用 strlen → string.o 拉入
- libc.a: string.o（strlen 等）→ 拉入
- libc.a: start.o（_start）→ 拉入 → 引用 main（已定义）、sys_exit（inline）
- libc.a: malloc.o → 仅当用户程序引用 malloc 时拉入

shell/drivers 保持 `-I.` + 手动链接 malloc.o，不改。

## include 路径

用户程序编译加 `-I. -Iuser/include`：
- `-Iuser/include` → 自定义 stdio.h/stdlib.h/string.h 优先于宿主机的版本
- `-I.` → common/syscall.h, common/shm.h, arch/x64/utils.h 可用
- 宿主机的 freestanding 头文件（stdint.h, stddef.h, stdarg.h）通过 gcc 默认路径可用（-ffreestanding 不排除它们）

libc 源文件（stdio.cc, string.cc, start.cc）编译也用 `-I. -Iuser/include`，用 g++ 编译（引用 common/syscall.h 的 inline wrapper）。

## 与 read/write 的衔接

当前 FILE.write_fn = sys_putc_flush。将来加 sys_read/sys_write syscall 后：

1. **sys_write(syscall #11)**：`sys_write(int fd, const char *buf, int len)` → 一次 syscall 输出整段字符串
2. _start 中 stdout/stderr 的 write_fn 改为 sys_write_flush
3. **sys_read(syscall #12)**：`sys_read(int fd, char *buf, int len)` → 从 stdin 读数据
4. stdin FILE 实例：fd=0, mode=read, line-buffered
5. fgetc/fgets/fread 实现：从 FILE buffer 读，buffer 空 → sys_read 填充
6. fopen/fclose：分配 FILE + 调 sys_open 获取 fd（需 sys_open syscall）

FILE 结构体已经为这些扩展预留了字段（fd, flags, buf_mode），不需要改结构体定义。