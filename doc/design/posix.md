# POSIX 接口覆盖现状与实现方案

## 现状概述

- **系统调用：** 59 个（编号 0-58，slot 8 为 NULL），全部在 `kernel/trap.c` 中有真实 handler
- **kernel syscall 分布：** `trap.c` 核心 + `socket.c`（30-39）+ `vfs.c`/`fat32.c`/`devtmpfs.c`（47-56）
- **libc 头文件：** `user/include/` 下 + `sys/` 子目录
- **libc 实现：** `user/lib/` 下

---

## 现有 POSIX 封装覆盖表

### 已完整封装的标准 POSIX

| POSIX 函数 | 头文件 | 底层 syscall | 备注 |
|---|---|---|---|
| `getpid()` | `unistd.h` | `SYS_GETPID` | |
| `_exit()` | `unistd.h` | `SYS_EXIT` | 不 flush 缓冲区 |
| `exit()` | `stdlib.h` | `_exit()` | flush stdout 后调用 `_exit` |
| `read()` | `unistd.h` | `SYS_READ` | 用户态 fd_table 分发 |
| `write()` | `unistd.h` | `SYS_WRITE` | 用户态 fd_table 分发 |
| `close()` | `unistd.h` | `SYS_CLOSE` | 用户态 fd_table 同步 |
| `pipe()` | `unistd.h` | `SYS_PIPE` | |
| `open()` | `fcntl.h` | `SYS_OPEN` | VFS/FAT32 直通（不再走 IPC） |
| `dup2()` | `unistd.h` | `SYS_DUP2` | |
| `fcntl()` | `fcntl.h` | `SYS_FCNTL` | F_GETFL/F_SETFL 实现 |
| `waitpid()` | `sys/wait.h` | `SYS_WAITPID` | options 参数暂不使用 |
| `mmap()` | `sys/mman.h` | `SYS_MMAP` | |
| `munmap()` | `sys/mman.h` | `SYS_MUNMAP` | |
| `stat()` | `sys/stat.h` | `SYS_STAT` | VFS/FAT32 直通 |
| `chdir()` | — | `SYS_MSG` (→ fs_driver) | 实现在 `file.cc` 中（待迁移到内核） |
| `poll()` | `sys/poll.h` | `SYS_POLL` | 支持 pipe/socket/dev fd |
| `socket()` | `sys/socket.h` | `SYS_SOCKET` | AF_UNIX only |
| `bind()` | `sys/socket.h` | `SYS_BIND` | |
| `listen()` | `sys/socket.h` | `SYS_LISTEN` | |
| `accept()` | `sys/socket.h` | `SYS_ACCEPT` | 返回 fd |
| `connect()` | `sys/socket.h` | `SYS_CONNECT` | |
| `socketpair()` | `sys/socket.h` | `SYS_SOCKETPAIR` | |
| `sendmsg()` | `sys/socket.h` | `SYS_SENDMSG` | SCM_RIGHTS fd 传递 |
| `recvmsg()` | `sys/socket.h` | `SYS_RECVMSG` | SCM_RIGHTS fd 传递 |
| `shutdown()` | `sys/socket.h` | `SYS_SHUTDOWN` | |
| `printf()` | `stdio.h` | `SYS_WRITE` | 含 %d/%u/%x/%X/%p/%s/%c |
| `fprintf()` | `stdio.h` | `SYS_WRITE` | |
| `vfprintf()` | `stdio.h` | `SYS_WRITE` | |
| `fflush()` | `stdio.h` | `SYS_WRITE` | |
| `putchar()` | `stdio.h` | `SYS_WRITE` | |
| `fputc()` | `stdio.h` | `SYS_WRITE` | |
| `fputs()` | `stdio.h` | `SYS_WRITE` | |
| `puts()` | `stdio.h` | `SYS_WRITE` | |
| `fgetc()` | `stdio.h` | `SYS_READ` | |
| `getchar()` | `stdio.h` | `SYS_READ` | |
| `malloc()` | `stdlib.h` | `SYS_MMAP` | size-class slab |
| `free()` | `stdlib.h` | `SYS_MUNMAP` | |
| `calloc()` | `stdlib.h` | `SYS_MMAP` | |
| `realloc()` | `stdlib.h` | `SYS_MMAP` | |
| `strlen()` | `string.h` | — | 纯用户态 |
| `strcmp()` | `string.h` | — | |
| `strncmp()` | `string.h` | — | |
| `strcpy()` | `string.h` | — | |
| `strncpy()` | `string.h` | — | |
| `strcat()` | `string.h` | — | |
| `strchr()` | `string.h` | — | |
| `memcpy()` | `string.h` | — | |
| `memset()` | `string.h` | — | |
| `memmove()` | `string.h` | — | |
| `timespec_get()` | `time.h` | `SYS_GETTIME` | |
| `clock()` | `time.h` | `SYS_CLOCK` | |
| `sched_yield()` | `unistd.h` | `SYS_YIELD` | |

### 已封装的非标准扩展（OS 特有，无 POSIX 对应）

| 函数 | 头文件 | 用途 |
|---|---|---|
| `shm_create()` | `sys/shm.h` | 创建共享内存 fd |
| `shm_attach()` | `sys/shm.h` | 按 PID attach 共享内存 |
| `shm_attach_kernel()` | `sys/shm.h` | 按 SHM ID attach 内核共享内存 |
| `irq_bind()` | `sys/irq.h` | 绑定 IRQ 到当前进程 |
| `device_register()` | `sys/device.h` | 驱动自注册（dev_table） |
| `fb_info()` | `sys/fb.h` | 获取 framebuffer 信息 |
| `spawn()` | `sys/process.h` | 加载 ELF 创建子进程 |
| `ioperm()` | `unistd.h` | I/O 端口权限 |
| `recv()` / `req()` / `resp()` / `notify()` | `sys/ipc.h` | 微内核 IPC 四层 |
| `msg()` / `msg_resp()` | `sys/ipc.h` | 变长消息 IPC |
| `req_fd()` / `notify_fd()` / `msg_fd()` | `sys/ipc.h` | 基于 fd 的 IPC 便利封装 |
| `poll()` | `sys/ipc.h` | 多重声明（同 `sys/poll.h`） |

---

## Syscall 编号表（当前实际编号）

**NR_SYSCALL = 59**（0-58，slot 8 为 NULL）。

| 编号 | 名称 | 实现位置 |
|---|---|---|
| 0 | `SYS_GETPID` | `trap.c` |
| 1 | `SYS_YIELD` | `trap.c` |
| 2 | `SYS_RECV` | `trap.c` |
| 3 | `SYS_REQ` | `trap.c` |
| 4 | `SYS_RESP` | `trap.c` |
| 5 | `SYS_IRQ_BIND` | `trap.c` |
| 6 | `SYS_EXIT` | `trap.c` |
| 7 | `SYS_WAITPID` | `trap.c` |
| 8 | ~~SYS_SPAWN~~（已删除，slot NULL） | — |
| 9 | `SYS_MMAP` | `trap.c` |
| 10 | `SYS_MUNMAP` | `trap.c` |
| 11 | `SYS_SHM_CREATE` | `trap.c` |
| 12 | `SYS_SHM_ATTACH` | `trap.c` |
| 13 | `SYS_PIPE` | `trap.c` |
| 14 | `SYS_WRITE` | `trap.c` |
| 15 | `SYS_READ` | `trap.c` |
| 16 | `SYS_CLOSE` | `trap.c` |
| 17 | `SYS_NOTIFY` | `trap.c` |
| 18 | `SYS_GETTIME` | `trap.c` |
| 19 | `SYS_CLOCK` | `trap.c` |
| 20 | `SYS_MSG` | `trap.c` |
| 21 | `SYS_MSG_RESP` | `trap.c` |
| 22 | `SYS_IOPERM` | `trap.c` |
| 23 | `SYS_DUP2` | `trap.c` |
| 24 | `SYS_FCNTL` | `trap.c` |
| 25 | `SYS_DMA_ALLOC` | `trap.c` |
| 26 | `SYS_DMA_FREE` | `trap.c` |
| 27 | `SYS_PCI_DEV_INFO` | `trap.c` |
| 28 | `SYS_BLOCK_ASYNC` | `trap.c` |
| 29 | `SYS_INSTALL_FD` | `trap.c` |
| 30 | `SYS_SOCKET` | `socket.c` |
| 31 | `SYS_BIND` | `socket.c` |
| 32 | `SYS_LISTEN` | `socket.c` |
| 33 | `SYS_ACCEPT` | `socket.c` |
| 34 | `SYS_CONNECT` | `socket.c` |
| 35 | `SYS_SOCKETPAIR` | `socket.c` |
| 36 | `SYS_SENDMSG` | `socket.c` |
| 37 | `SYS_RECVMSG` | `socket.c` |
| 38 | `SYS_SHUTDOWN` | `socket.c` |
| 39 | `SYS_POLL` | `socket.c` |
| 40 | `SYS_LSEEK` | `trap.c` |
| 41 | `SYS_MEMFD_CREATE` | `trap.c` |
| 42 | `SYS_FTRUNCATE` | `trap.c` |
| 43 | `SYS_KILL` | `trap.c` |
| 44 | `SYS_SIGACTION` | `trap.c` |
| 45 | `SYS_SIGRETURN` | `trap.c` |
| 46 | `SYS_DEBUG_PRINT` | `trap.c` |
| 47 | `SYS_OPEN` | `vfs.c` |
| 48 | `SYS_STAT` | `vfs.c` |
| 49 | `SYS_MKDIR` | `vfs.c` |
| 50 | `SYS_UNLINK` | `vfs.c` |
| 51 | `SYS_RMDIR` | `vfs.c` |
| 52 | `SYS_DEV_CREATE` | `vfs.c` |
| 53 | `SYS_GETDENTS` | `vfs.c` |
| 54 | `SYS_IOCTL` | `trap.c` |
| 55 | `SYS_FSTAT` | `vfs.c` |
| 56 | `SYS_FDEV_PID` | `trap.c` |
| 57 | `SYS_FORK` | `trap.c` |
| 58 | `SYS_EXECVE` | `trap.c` |

> **注：** 早期 posix.md 曾提议重排编号（合并 DEV_MSG、合并 BLOCK_READ/WRITE 等），该重排方案未实施。当前 syscall 编号为增量追加，与 `common/syscall_nums.h` 一致。

---

## fs_driver 命令扩展

当前 fs_driver `file_req` 命令（与 `user/lib/file.cc` 同步）：

| 编号 | 命令 | 状态 |
|---|---|---|
| 1 | `FILE_CMD_OPEN` | 已有 |
| 2 | `FILE_CMD_READ` | 已有 |
| 3 | `FILE_CMD_WRITE` | 已有 |
| 4 | `FILE_CMD_CLOSE` | 已有 |
| 5 | `FILE_CMD_READDIR` | 已有 |
| 6 | `FILE_CMD_CREATE` | 已有 |
| 7 | `FILE_CMD_MKDIR` | 已有 |
| 8 | `FILE_CMD_RAW_READ` | 已有 |
| 9 | `FILE_CMD_STAT` | 已有 |
| — | ~~FILE_CMD_PING~~ | ❌ 移除（无实际用途） |
| **10** | **`FILE_CMD_UNLINK`** | **新增** |
| **11** | **`FILE_CMD_RMDIR`** | **新增** |
| **12** | **`FILE_CMD_FSTAT`** | **新增** |
| **13** | **`FILE_CMD_OPENDIR`** | **新增** |
| **14** | **`FILE_CMD_DIRENT`** | **新增** |
| **15** | **`FILE_CMD_CLOSEDIR`** | **新增** |
| **16** | **`FILE_CMD_SEEK`** | **新增** |
| **17** | **`FILE_CMD_ACCESS`** | **新增** |

### 新增命令定义

```c
#define FILE_CMD_UNLINK   10   // FAT 删除文件
#define FILE_CMD_RMDIR    11   // FAT 删除空目录
#define FILE_CMD_FSTAT    12   // 基于 session fd 返回 stat
#define FILE_CMD_OPENDIR  13   // 解析路径到 dir_cluster，返回 session fd
#define FILE_CMD_DIRENT   14   // 基于 session fd 返回下一条 fs_dirent
#define FILE_CMD_CLOSEDIR 15   // 关闭目录 session fd
#define FILE_CMD_SEEK     16   // 更新 session fd 的 cur_offset
#define FILE_CMD_ACCESS   17   // 检查文件是否存在
```

---

## libc 新增 POSIX 函数清单

### string.h

```c
int memcmp(const void *s1, const void *s2, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strerror(int errnum);
void bzero(void *s, size_t n);    // BSD 兼容
```

### stdio.h

```c
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void perror(const char *s);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
```

**FILE 打开模式：**

| mode | flags | FILE flags | 行为 |
|---|---|---|---|
| `"r"` | `O_RDONLY` | `_F_READ` | 文件必须存在 |
| `"r+"` | `O_RDWR` | `_F_READ\|_F_WRITE` | 文件必须存在 |
| `"w"` | `O_WRONLY\|O_CREAT\|O_TRUNC` | `_F_WRITE` | 创建/截断 |
| `"w+"` | `O_RDWR\|O_CREAT\|O_TRUNC` | `_F_READ\|_F_WRITE` | 创建/截断 |
| `"a"` | `O_WRONLY\|O_CREAT\|O_APPEND` | `_F_WRITE` | 创建/追加 |
| `"a+"` | `O_RDWR\|O_CREAT\|O_APPEND` | `_F_READ\|_F_WRITE` | 创建/追加，读从头 |

**FILE 分配：** `malloc(sizeof(FILE))` 动态分配，默认 4096B 全缓冲（`_IOFBF`）。通过 `isatty` 检测到终端时切换为行缓冲（`_IOLBF`）。

**sprintf/vsnprintf 实现：** 复用 vfprintf 格式化引擎，将输出目标从 `file_putc_internal` 改为写入 `char *` 缓冲区（指针累加），snprintf 增加长度限制。

### stdlib.h

```c
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
int abs(int x);
long labs(long x);
int rand(void);
void srand(unsigned seed);
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *));
```

**rand/srand：** LCG 伪随机数生成器。种子默认源自 `sys_gettime()` 纳秒时间低 32 位。标准常量 `RAND_MAX=32767`。

### unistd.h

```c
char *getcwd(char *buf, size_t size);
off_t lseek(int fd, off_t offset, int whence);
unsigned int sleep(unsigned seconds);
int usleep(useconds_t usec);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int isatty(int fd);
int mkdir(const char *path, mode_t mode);   // mode 暂忽略
```

**sleep/usleep/nanosleep 实现：** 基于 `sys_recv(NULL, NULL, 0, timeout_ms)` 阻塞。 
- `sleep(s)` = `sys_recv(NULL, NULL, 0, s*1000)` 
- `usleep(us)` = `sys_recv(NULL, NULL, 0, us/1000)` (usec 转毫秒，==0 时传 1)

**getcwd：** `cwd_path` 已在 `file.cc` 中维护，纯封装 `strcpy(buf, __get_cwd())`。

**access：** 基于 stat 检查文件是否存在，`F_OK`(0) 返回 0 / -1(ENOENT)。

**isatty：** 检查用户态 fd_table 中 type == FD_DEV && target_pid == DEV_TERMINAL。

### ctype.h（新文件）

```c
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int isspace(int c);
int ispunct(int c);
int islower(int c);
int isupper(int c);
int tolower(int c);
int toupper(int c);
```

### assert.h（新文件）

```c
#define assert(expr) \
    ((void)((expr) || (__assert_fail(#expr, __FILE__, __LINE__), 0)))

void __assert_fail(const char *expr, const char *file, int line);
```

### sys/utsname.h（新文件）

```c
#define UTSNAME_LEN 65

struct utsname {
    char sysname[UTSNAME_LEN];   // "Xos"
    char nodename[UTSNAME_LEN];  // "(none)"
    char release[UTSNAME_LEN];   // "0.1"
    char version[UTSNAME_LEN];   // __DATE__
    char machine[UTSNAME_LEN];   // "x86_64"
};

int uname(struct utsname *buf);
```

### dirent.h（新文件）

```c
typedef struct {
    int dd_fd;          // libc 管理的 opendir session fd
    // 内部缓存
} DIR;

struct dirent {
    ino_t d_ino;        // 0
    char  d_name[256];
};

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
```

**opendir：** 向 fs_driver 发 `FILE_CMD_OPENDIR(path)`，返回 session fd。`DIR` 结构体在 libc 中维护（`malloc` 分配），含 fd + 当前条目缓存。

**readdir：** 发 `FILE_CMD_DIRENT(session_fd)`，fs_driver 返回下一条 `fs_dirent`（类似 read），libc 转换为 `struct dirent`。缓存页减少调用次数。

**closedir：** 发 `FILE_CMD_CLOSEDIR(session_fd)` + `free(DIR)`。

---

## fs_driver 新增命令详细行为

### `FILE_CMD_UNLINK` (10)
- 输入：`path[256]`
- 行为：在 FAT32 目录中找到文件条目，标记 0xE5，释放 FAT 簇链
- 错误：`ENOENT`（不存在）、`EISDIR`（是目录）
- 线程安全：单线程事件循环

### `FILE_CMD_RMDIR` (11)
- 输入：`path[256]`
- 行为：找到目录条目，检查是否为空（仅包含 `.` 和 `..`），空则释放
- 错误：`ENOTEMPTY`、`ENOTDIR`
- 注意：当前 FAT32 驱动没有 ENOTEMPTY errno，可参用 `-EBUSY` 或新增

### `FILE_CMD_FSTAT` (12)
- 输入：`fs_fd`（session fd）
- 输出：`{ status, size, attr, date, time }`（与 stat 相同，但基于 fd 而非路径）
- 不需要路径解析开销

### `FILE_CMD_OPENDIR` (13)
- 输入：`path[256]`
- 输出：`{ status, fd }`（目录 session fd）
- 行为：解析路径到 dir_cluster，创建目录 session（与 open 的 session 分开管理），返回 fd
- 目录 session fd 与文件 session fd 共用同一 session 数组但标记 type=DIR_SESSION

### `FILE_CMD_DIRENT` (14)
- 输入：`fs_fd`（目录 session fd）+ `count`
- 输出：`{ status, count, total, data[] }`（`fs_dirent[]` 数组）
- 行为：从目录 session 的当前 readdir 状态读取下一批条目，推进游标
- EOF：`count=0`
- 注意：目录 session 复用现有 readdir 状态机（`pending_op->u.readdir`），但由 session fd 保持状态而非传入路径

### `FILE_CMD_CLOSEDIR` (15)
- 输入：`fs_fd`（目录 session fd）
- 行为：释放目录 session 状态

### `FILE_CMD_SEEK` (16)
- 输入：`{ fs_fd, offset, whence }`
- 行为：更新 session fd 的 `cur_offset`
  - `SEEK_SET`(0)：`cur_offset = offset`
  - `SEEK_CUR`(1)：`cur_offset += offset`
  - `SEEK_END`(2)：`cur_offset = file_size + offset`
- 文件 session fd 的 cur_offset 现在被内核 `file_data.offset` 和 fs_driver 两端维护，两者保持一致。
  **需注意：** 内核 `file_data.offset` 是权威值，fs_driver 的 cur_offset 跟随内核请求。
  `SYS_LSEEK` 更新内核 offset，后续 read/write 内核发 offset 到 fs_driver。
  fs_driver 的 `FILE_CMD_SEEK` 用于用户在 libc 层通过 `msg_fd` 直接操作 fs_driver（非 FD_FILE 路径），
  更精确地说：FD_FILE 路径的文件偏移由内核管理，fs_driver 仅作为存储后端。
  所以 `FILE_CMD_SEEK` 不用于内核 SYS_LSEEK 路径（内核直接更新 file_data.offset），
  而是保留供 fd-based msg 路径直接操作 fs_driver 使用（或未来扩展）。

### `FILE_CMD_ACCESS` (17)
- 输入：`path[256]`, `mode`
- 行为：检查文件是否存在（实际调用 stat 路径解析）
- 输出：`{ status }`
- 错误：`ENOENT`

---

## struct stat 扩展

### 新增 POSIX 类型（`sys/types.h`）

```c
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef uint32_t mode_t;
typedef uint32_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t  blksize_t;
typedef int32_t  blkcnt_t;
```

### 扩展后的 struct stat（`sys/stat.h`）

```c
struct stat {
    dev_t     st_dev;        // 0
    ino_t     st_ino;        // 0
    mode_t    st_mode;       // FAT attr → S_IFREG|0644 / S_IFDIR|0755
    nlink_t   st_nlink;      // 1
    uid_t     st_uid;        // 0
    gid_t     st_gid;        // 0
    off_t     st_size;
    blksize_t st_blksize;    // 512
    blkcnt_t  st_blocks;     // (st_size + 511) / 512
    struct timespec st_atim; // = st_mtim
    struct timespec st_mtim; // FAT date/time → unix time (粗略转换)
    struct timespec st_ctim; // = st_mtim
};
```

### S_IFMT 常量（`sys/stat.h`）

```c
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
```

FAT32 attr 到 `st_mode` 映射：
- `attr & 0x10` → `S_IFDIR | 0755`
- 否则 → `S_IFREG | (attr & 0x01 ? 0444 : 0644)`（只读文件 r--，否则 rw-）

---

## 内核改动清单

| 文件 | 改动 |
|---|---|
| `kernel/trap.cc` | 移除 `sys_dev_msg` handler（原编号 19）；移除 `sys_block_read`/`sys_block_write`，新增 `sys_block_io(lba,buf,count,dir)`；新增 `sys_lseek` handler（更新 `file_data.offset`，PIPE/SOCKET/DEV 返回 `-ESPIPE`）；重编 `syscall_table[]` 数组（45 项 0-44） |
| `kernel/trap.h` | 更新函数声明 |
| `common/syscall.h` | 全部 syscall `#define` 重编号（0-44）；移除 `SYS_DEV_MSG`；移除 `SYS_BLOCK_READ`/`SYS_BLOCK_WRITE`，新增 `SYS_BLOCK_IO`；新增 `SYS_LSEEK` |

---

## libc 改动清单

### 修改已有文件

| 文件 | 改动 |
|---|---|
| `common/syscall.h` | syscall 重编号；`sys_dev_msg` → 移除；`sys_block_read/write` → `sys_block_io`；新增 `sys_lseek` |
| `user/lib/file.cc` | `msg_fd()` 改用 `sys_msg(fd_table[fd].target_pid, ...)`；`disk_read/write` 改用 `sys_block_io`；新增 `lseek/getcwd/mkdir/unlink/rmdir/access/isatty/fstat/opendir/readdir/closedir` |
| `user/lib/stdio.cc` | 新增 `fopen/fclose/fread/fwrite/fseek/ftell/rewind`；新增 `sprintf/snprintf/vsprintf/vsnprintf`；新增 `perror` |
| `user/lib/string.cc` | 新增 `memcmp/strstr/strtok/strtok_r/strerror/bzero` |
| `driver/fs_driver.cc` | `disk_read_sync`/`disk_write_sync` 改用 `sys_block_io`；新增所有命令 handler（UNLINK/RMDIR/FSTAT/OPENDIR/DIRENT/CLOSEDIR/SEEK/ACCESS） |
| `user/lib/sys_ipc.cc` | 移除 `msg_fd` 依赖的 `sys_dev_msg`（实际已在 `file.cc` 中处理） |
| `user/include/sys/stat.h` | 扩展 `struct stat` 完整字段；新增 `S_IFMT`/`S_IRWXU` 等宏；新增 POSIX 文件类型宏 |
| `user/include/sys/types.h` | 新增 `dev_t/ino_t/mode_t/nlink_t/uid_t/gid_t/blksize_t/blkcnt_t` |
| `user/include/stdio.h` | 新增 `fopen/fclose/fread/fwrite/fseek/ftell/rewind/sprintf/snprintf/vsprintf/vsnprintf/perror` 声明；新增 BUFSIZ 常量 |
| `user/include/string.h` | 新增 `memcmp/strstr/strtok/strtok_r/strerror/bzero` 声明 |
| `user/include/stdlib.h` | 新增 `atoi/atol/strtol/strtoul/abs/labs/rand/srand/qsort/RAND_MAX` |
| `user/include/unistd.h` | 新增 `getcwd/lseek/sleep/usleep/access/unlink/rmdir/isatty/mkdir` 声明；新增 `SEEK_SET/SEEK_CUR/SEEK_END/F_OK/R_OK/W_OK/X_OK` |
| `user/include/time.h` | 新增 `nanosleep` 声明 |

### 新增头文件

| 文件 | 内容 |
|---|---|
| `user/include/ctype.h` | `isdigit/isalpha/isalnum/isprint/isspace/ispunct/islower/isupper/toupper/tolower` |
| `user/include/assert.h` | `assert(expr)` 宏 + `__assert_fail` |
| `user/include/sys/utsname.h` | `struct utsname` + `uname()` |
| `user/include/dirent.h` | `DIR` + `struct dirent` + `opendir/readdir/closedir` |

### 新增源文件

| 文件 | 内容 |
|---|---|
| `user/lib/ctype.c` | ctype 族函数实现 |
| `user/lib/strtol.c` | `atoi/atol/strtol/strtoul` |
| `user/lib/stdlib_misc.c` | `abs/labs/rand/srand/qsort` |
| `user/lib/uname.c` | `uname`（返回 "Xos"） |
| `user/lib/sleep.c` | `sleep/usleep/nanosleep` |
| `user/lib/sprintf.cc` | `sprintf/snprintf/vsprintf/vsnprintf`（或放在 stdio.cc） |

---

## 实现顺序建议

以上所有改动可以并行进行，因为不存在循环依赖。推荐按以下顺序提交：

| 阶段 | 内容 | 核验 |
|---|---|---|
| 1. syscall 清理 | 内核 trap.cc 重编号 + 移除 DEV_MSG + 合并 BLOCK_IO + 新增 LSEEK；common/syscall.h 更新 | 编译通过 |
| 2. fs_driver 扩展 | 新增 8 个命令 handler；disk_read/write 改用 sys_block_io | fs_driver 启动正常 |
| 3. libc string 扩展 | memcmp/strstr/strtok/strtok_r/strerror/bzero + ctype.h + assert.h | 单元测试 |
| 4. libc stdio 扩展 | sprintf/snprintf/vsprintf/vsnprintf + perror | printf 正常工作 |
| 5. libc stdlib 扩展 | atoi/strtol/abs/rand/qsort | 基本测试 |
| 6. unistd 扩展 | getcwd/lseek/sleep + mkdir/unlink/rmdir/access/isatty | shell 兼容 |
| 7. FILE I/O 完整 | fopen/fclose/fread/fwrite/fseek/ftell/rewind | 读写文件验证 |
| 8. struct stat + dirent | stat 扩展 + opendir/readdir/closedir + fstat | ls -l 正常 |
| 9. uname | sys/utsname.h + uname.c | 返回 Xos |

---

## errno 处理

当前 syscall 返回负 errno（`-ENOMEM` 等）。POSIX 函数约定：返回 -1 并设置 `errno`。

封装层统一模式：
```c
int64_t r = sys_xxx(args);
if (r < 0) {
    errno = (int)(-r);
    return -1;
}
return (int)r;
```

`errno` 定义在 `user/include/errno.h`，当前为单线程全局变量。

---

## 迁移策略

1. 用户态代码不再直接 `#include "common/syscall.h"`，改为使用对应 POSIX 头文件
2. libc 内部（`stdio.cc`、`malloc.cc`、`start.cc`）可保留 `sys_*` 调用，因为它们是 libc 自身实现，需要直接访问底层 syscall
3. 驱动专用接口（`recv/resp/req/msg/msg_resp`）降级到 `<driver/ipc.h>`，公共 libc 头只保留 POSIX 标准接口

---

## 替换示例

替换前（shell/shell.cc）：
```cpp
#include "common/syscall.h"
// ...
freq->client_pid = sys_getpid();
sys_notify(sys_lookup_dev(DEV_FS));
{ struct recv_msg m; sys_recv(&m, 0); }
int64_t child_pid = sys_spawn((const void *)elf_buf, (uint64_t)file_size, 0);
int64_t result = sys_waitpid((int32_t)child_pid, &exit_code);
```

替换后：
```cpp
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/device.h>
#include <sys/process.h>
#include <sys/wait.h>
#include <fcntl.h>
// ...
freq->client_pid = getpid();
int fs_fd = open("/dev/fs", O_RDWR);
msg_fd(fs_fd, &freq, sizeof(freq), &fresp, sizeof(fresp));
{ struct recv_msg m; recv(&m, 0); }
pid_t child_pid = spawn((const void *)elf_buf, (size_t)file_size, 0);
pid_t result = waitpid((pid_t)child_pid, &exit_code, 0);
```
