# POSIX 接口覆盖

## 当前架构设计

### Syscall 编号表

NR_SYSCALL=86（编号 0-85 连续，无空槽）。

| 编号 | 名称 | 实现位置 | 说明 |
|------|------|---------|------|
| 0 | SYS_GETPID | trap.c | 返回 tgid（进程 ID） |
| 1 | SYS_YIELD | trap.c | |
| 2 | SYS_RECV | trap.c | 统一 recv 队列 |
| 3 | SYS_REQ | trap.c | ≤56B 同步 IPC |
| 4 | SYS_RESP | trap.c | |
| 5 | SYS_IRQ_BIND | trap.c | |
| 6 | SYS_EXIT | trap.c | 进程退出 |
| 7 | SYS_WAITPID | trap.c | pid>0 / pid==-1 |
| 8 | SYS_MMAP | trap.c | |
| 9 | SYS_MUNMAP | trap.c | |
| 10 | SYS_PIPE | trap.c | |
| 11 | SYS_WRITE | trap.c | |
| 12 | SYS_READ | trap.c | |
| 13 | SYS_CLOSE | trap.c | |
| 14 | SYS_NOTIFY | trap.c | |
| 15 | SYS_GETTIME | trap.c | |
| 16 | SYS_CLOCK | trap.c | |
| 17 | SYS_MSG | trap.c | ≤64KB 变长 IPC |
| 18 | SYS_MSG_RESP | trap.c | |
| 19 | SYS_IOPERM | trap.c | |
| 20 | SYS_DUP2 | trap.c | |
| 21 | SYS_FCNTL | syscall.c | F_GETFL/F_SETFL/F_DUPFD/F_DUPFD_CLOEXEC/F_ADD_SEALS/F_GET_SEALS/F_GETFD/F_SETFD |
| 22 | SYS_DMA_ALLOC | trap.c | |
| 23 | SYS_DMA_FREE | trap.c | |
| 24 | SYS_PCI_DEV_INFO | trap.c | |
| 25 | SYS_BLOCK_ASYNC | trap.c | |
| 26 | SYS_INSTALL_FD | trap.c | |
| 27 | SYS_SOCKET | socket.c | AF_UNIX only |
| 28 | SYS_BIND | socket.c | |
| 29 | SYS_LISTEN | socket.c | |
| 30 | SYS_ACCEPT | socket.c | |
| 31 | SYS_CONNECT | socket.c | |
| 32 | SYS_SOCKETPAIR | socket.c | |
| 33 | SYS_SENDMSG | socket.c | SCM_RIGHTS fd 传递 |
| 34 | SYS_RECVMSG | socket.c | |
| 35 | SYS_SHUTDOWN | socket.c | |
| 36 | SYS_POLL | socket.c | pipe/socket/dev/tty |
| 37 | SYS_LSEEK | trap.c | |
| 38 | SYS_MEMFD_CREATE | trap.c | |
| 39 | SYS_FTRUNCATE | trap.c | |
| 40 | SYS_KILL | trap.c | pid>0/pid==0/pid<0(pgsignal) |
| 41 | SYS_SIGACTION | trap.c | |
| 42 | SYS_SIGRETURN | trap.c | |
| 43 | SYS_DEBUG_MEMSTAT | syscall.c | |
| 44 | SYS_OPEN | vfs.c | VFS/FAT32 |
| 45 | SYS_STAT | vfs.c | |
| 46 | SYS_MKDIR | vfs.c | |
| 47 | SYS_UNLINK | vfs.c | |
| 48 | SYS_RMDIR | vfs.c | |
| 49 | SYS_DEV_CREATE | vfs.c | |
| 50 | SYS_GETDENTS | vfs.c | |
| 51 | SYS_IOCTL | trap.c | FD_TTY: pty_ioctl |
| 52 | SYS_FSTAT | vfs.c | |
| 53 | SYS_FDEV_PID | trap.c | |
| 54 | SYS_FORK | proc.c | |
| 55 | SYS_EXECVE | proc.c | |
| 56 | SYS_SETSID | trap.c | |
| 57 | SYS_SETPGID | trap.c | |
| 58 | SYS_GETPGID | trap.c | |
| 59 | SYS_GETSID | trap.c | |
| 60 | SYS_CLONE | proc.c | 线程创建，详见 [thread.md](thread.md) |
| 61 | SYS_FUTEX | proc.c | 用户态互斥，详见 [thread.md](thread.md) |
| 62 | SYS_ARCH_PRCTL | trap.c | FS_BASE 管理，详见 [thread.md](thread.md) |
| 63 | SYS_TGKILL | signal.c | 线程定向信号，详见 [thread.md](thread.md) |
| 64 | SYS_EXIT_GROUP | proc.c | 杀整个线程组，详见 [thread.md](thread.md) |
| 65 | SYS_SET_TID_ADDRESS | proc.c | clear_tid 设置，详见 [thread.md](thread.md) |
| 66 | SYS_GETTID | trap.c | 返回线程 ID，详见 [thread.md](thread.md) |
| 67 | SYS_SIGPROCMASK | signal.c | 信号屏蔽字，详见 [thread.md](thread.md) |
| 68 | SYS_PTHREAD_SET_CANCEL_HANDLER | signal.c | pthread cancel handler 注册 |
| 69 | SYS_GETUID | proc.c | real UID |
| 70 | SYS_GETEUID | proc.c | effective UID |
| 71 | SYS_GETGID | proc.c | real GID |
| 72 | SYS_GETEGID | proc.c | effective GID |
| 73 | SYS_SETUID | proc.c | 单用户系统先放宽 |
| 74 | SYS_SETGID | proc.c | 同上 |
| 75 | SYS_GETPPID | signal.c | 线程组父 PID |
| 76 | SYS_GETPGRP | proc.c | 进程组 ID |
| 77 | SYS_UMASK | proc.c | old=current->umask; current->umask=arg&0777 |
| 78 | SYS_GETHOSTNAME | proc.c | copy_to_user 全局 hostname |
| 79 | SYS_SETHOSTNAME | proc.c | copy_from_user 写全局 hostname |
| 80 | SYS_ALARM | signal.c | 定时投递 SIGALRM |
| 81 | SYS_PAUSE | signal.c | 可中断睡眠，等信号唤醒 |
| 82 | SYS_TRUNCATE | vfs.c | 路径截断文件 |
| 83 | SYS_FSYNC | vfs.c | 写回单个 inode 脏页 |
| 84 | SYS_SYNC | vfs.c | 写回全部脏页 |
| 85 | SYS_SIGPENDING | signal.c | 返回未决信号集（per-task ∪ shared，不滤 blocked） |

### libc 已实现 POSIX 函数

| 分类 | 函数 | 头文件 | 底层 syscall |
|------|------|--------|-------------|
| 进程 | getpid, fork, execve, spawn, _exit, exit, waitpid, setsid, setpgid, getpgid, getsid, getuid, geteuid, getgid, getegid, setuid, setgid, getppid, getpgrp, umask, gethostname, sethostname | unistd.h, sys/wait.h, sys/process.h, stdlib.h | SYS_GETPID/FORK/EXECVE/EXIT/WAITPID/SETSID/SETPGID/GETUID/GETEUID/GETGID/GETEGID/SETUID/SETGID/GETPPID/GETPGRP/UMASK/GETHOSTNAME/SETHOSTNAME |
| I/O | read, write, close, pipe, dup2, fcntl(F_DUPFD/F_DUPFD_CLOEXEC), open, lseek, ioctl, poll, truncate, fsync, sync | unistd.h, fcntl.h, sys/poll.h, sys/ioctl.h | SYS_READ/WRITE/CLOSE/PIPE/DUP2/FCNTL/OPEN/LSEEK/IOCTL/POLL/TRUNCATE/FSYNC/SYNC |
| FILE | printf, fprintf, vfprintf, fflush, putchar, fputc, fputs, puts, fgetc, getchar, fopen, fclose, fread, fwrite, fseek, ftell, rewind, sprintf, snprintf, perror | stdio.h | SYS_WRITE/READ |
| 内存 | malloc, free, calloc, realloc, mmap, munmap | stdlib.h, sys/mman.h | SYS_MMAP/MUNMAP |
| 字符串 | strlen, strcmp, strncmp, strcpy, strncpy, strcat, strchr, memcpy, memset, memmove, memcmp, strstr, strtok, strtok_r, strerror, bzero | string.h | — (纯用户态) |
| 字符类 | isdigit, isalpha, isalnum, isprint, isspace, ispunct, islower, isupper, tolower, toupper | ctype.h | — (纯用户态) |
| 数值 | atoi, atol, strtol, strtoul, abs, labs, rand, srand, qsort, mkstemp, mktemp, realpath | stdlib.h | rand 种子用 SYS_GETTIME；mkstemp/mktemp/realpath 为纯用户态（O_CREAT\|O_EXCL\|O_RDWR + 路径规范化） |
| 时间 | timespec_get, clock, sleep, usleep | time.h, unistd.h | SYS_GETTIME/CLOCK/RECV(timeout) |
| 信号 | kill, sigaction, sigprocmask, sigpending, raise, signal, alarm, pause | signal.h | SYS_KILL/SIGACTION/SIGRETURN/SIGPENDING/ALARM/PAUSE |
| TTY | isatty, tcgetattr, tcsetattr, ttyname, ioctl | termios.h, sys/ioctl.h | SYS_IOCTL |
| 文件系统 | stat, fstat, mkdir, unlink, rmdir, opendir, readdir, closedir, getcwd, access | sys/stat.h, dirent.h, unistd.h | SYS_STAT/FSTAT/MKDIR/UNLINK/RMDIR/GETDENTS |
| 进程信息 | uname | sys/utsname.h | SYS_DEBUG_PRINT (内核填充) |
| 断言 | assert | assert.h | — (abort 用 _exit) |

### OS 特有接口（非 POSIX）

| 函数 | 头文件 | 用途 |
|------|--------|------|
| irq_bind | sys/irq.h | 绑定 IRQ 到进程 |
| device_register | sys/device.h | 驱动自注册 |
| dev_create | sys/device.h | 创建设备节点（含 shm_fd 关联） |
| ioperm | unistd.h | I/O 端口权限 |
| recv / req / resp / notify / msg / msg_resp | sys/ipc.h | 微内核 IPC 四层 |
| req_fd / notify_fd / msg_fd | sys/ipc.h | 基于 fd 的 IPC 便利封装 |
| dma_alloc / dma_free | sys/mman.h | DMA 物理内存分配 |
| pci_dev_info | sys/pci.h | PCIe 设备信息查询 |

### libc 构建体系

libc.a 为 CMake target `c`，用户 ELF 通过 `LINK_LIBS c` 链接。编译加 `-I. -Iuser/include` 让自定义头文件优先。源文件分布在 user/lib/（*.cc / *.c），头文件在 user/include/ + user/include/sys/。

### errno 约定

syscall 返回负 errno（-ENOMEM 等），POSIX 函数约定返回 -1 并设置 errno。封装层统一模式：`r = sys_xxx(args); if (r < 0) { errno = -r; return -1; } return r;`

**errno 编号偏离 Linux**：现有编号与 Linux 不一致（如 `EINTR=38`，Linux=4）。`include/uapi/xos/errno.h` 已补全常用常量，采用**局部补值**策略：未占用号位直接用 Linux 值（`ENOMSG=42`…`EOPNOTSUPP=95`）；两个与现有占用冲突的 Linux 号位推到空闲高位——`EACCES=100`（Linux=13，被 `EMFILE=13` 占用）、`ENFILE=101`（Linux=23，被 `ECONNREFUSED=23` 占用）。交叉编译程序 `#include <errno.h>` 后硬编码值会错，但本 OS 用户程序用自己的 libc 头，影响有限。

### 进程身份与权限（syscalls 69-79）

`proc_t` 新增 `uid/euid/gid/egid/umask`（uint32_t，默认 0/0/0/0/0022），fork/clone 时继承。FAT32 无权限位落盘，uid/gid/mode 仅存内存 inode cache（sys_fstat 读 inode 内存值，不再硬编码 0）。umask getter/setter 已实现，fat32_open 创建文件时未读取 umask（见 [vfs.md](vfs.md) 待完成项）。

hostname 为全局变量（`static char hostname[256] = "myos"`，配 spinlock），复用 uname 的 utsname 结构。

### alarm / pause（syscalls 80-81）

alarm 使用 `xtask_t.alarm_deadline`（sched_clock() 纳秒绝对值，0 = 无 alarm），**不进 timer_queue**（避免与 nanosleep 抢 wait_node）。timer_handler 每 tick 内联扫描：deadline 到期则 `force_sig(SIGALRM)`，SIGALRM 默认动作为 terminate。

pause 状态置 TASK_INTERRUPTIBLE 并 `schedule()`，被信号唤醒后返回 -EINTR。复用现有信号唤醒路径（force_sig/sys_kill wake 逻辑）。

### fcntl F_DUPFD / F_DUPFD_CLOEXEC

sys_fcntl switch 新增两个 case，复用 `alloc_fd(files, min_fd)` + `fd_install` + `file_get`。F_DUPFD_CLOEXEC 复用现有 FD_CLOEXEC 标志位机制（F_GETFD/F_SETFD 已实现）。out 标签的 `file_put` 已被 `file_get` 抵消，引用计数平衡。

### 文件系统扩展（syscalls 82-84）

truncate(path, len)：resolve_path 得 inode → `fat32_ftruncate(inode, len)` → inode_put。fat32_ftruncate 为按 inode + 任意 len 的封装（缩小时释放簇，扩展时分配簇补零 + 更新目录项 size）。

fsync(fd)：遍历该 inode 在 page_cache 中可能 page_index 范围逐个 lookup + writeback dirty 页 + 回写 FAT 目录项元数据。**无 per-inode 脏页链表**，短期 O(页数) 遍历可接受。

sync()：`page_cache_flush_all()` 遍历 `page_cache_hash[]` 全表所有 dirty cache_page 写回 + 回写所有 inode 元数据。

### mkstemp / realpath（纯 libc）

mkstemp：循环填充 XXXXXX 处随机字母 + `open(O_CREAT|O_EXCL|O_RDWR, 0600)`，依赖 fat32 O_EXCL 语义（O_CREAT 分支已补：文件已存在时 O_EXCL 返回 EEXIST）。

realpath：路径规范化（处理 . / .. / 多斜杠）+ getcwd 拼接绝对路径。当前无 symlink，realpath 退化为路径规范化。

### 与其他模块的关系

- IPC：recv/req/msg 等详见 [ipc.md](ipc.md)
- 进程管理：fork/execve/waitpid 详见 [proc.md](proc.md)
- VFS：open/stat/mkdir 等详见 [vfs.md](vfs.md)
- PTY：isatty/tcsetattr/ioctl 详见 [terminal.md](terminal.md)
- 构建系统：详见 [cmake_user_build.md](cmake_user_build.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| FILE 行缓冲/全缓冲 | isatty 检测终端时切换行缓冲，当前终端 fd 行缓冲已实现，非终端需全缓冲验证 | 中 |
| execve argv/envp | 当前 argv=NULL, envp=NULL，需支持参数和环境变量传入新程序 | 高 |
| envp 环境变量 | getenv/setenv/clearenv，全局 environ 数组 | 中 |
| POSIX signal 扩展 | sigsuspend/sigwait/sigaltstack，当前实现 sigaction+sigprocmask+kill+sigpending+alarm+pause。机制与依赖顺序见 [thread.md](kernel/thread.md) POSIX 信号扩展待完成项 | 中 |
| perror errno 映射 | 当前 perror 只输出字符串，需 errno→字符串完整映射 | 低 |
| strftime | 时间格式化，需时区支持（当前无时区概念） | 低 |
| opendir/readdir 内核化 | 当前 opendir 通过 sys_open + sys_getdents 实现，readdir 需 dirent 结构转换 | 低 |
| fs_driver 源码删除 | driver/fs_driver.cc 文件仍存在于磁盘（CMake 构建目标已移除） | 中 |
| errno 全表对齐 Linux | 策略 B：重排所有编号 + 全量审计内核 `return -E…`，当前局部补值（EACCES=100, ENFILE=101 高位回避冲突） | 低 |
| IPC 接口降级到驱动专用 | recv/resp/req/msg 从公共 libc 头移到 driver/ipc.h，详见 [ipc.md](ipc.md) | 低 |
