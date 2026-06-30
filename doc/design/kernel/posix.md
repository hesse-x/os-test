# POSIX 接口覆盖

## 当前架构设计

### Syscall 编号表

NR_SYSCALL=63（编号 0-62，slot 8 为 NULL/已删除 sys_spawn）。

| 编号 | 名称 | 实现位置 | 说明 |
|------|------|---------|------|
| 0 | SYS_GETPID | trap.c | |
| 1 | SYS_YIELD | trap.c | |
| 2 | SYS_RECV | trap.c | 统一 recv 队列 |
| 3 | SYS_REQ | trap.c | ≤56B 同步 IPC |
| 4 | SYS_RESP | trap.c | |
| 5 | SYS_IRQ_BIND | trap.c | |
| 6 | SYS_EXIT | trap.c | 进程退出 |
| 7 | SYS_WAITPID | trap.c | pid>0 / pid==-1 |
| 8 | — | NULL slot | sys_spawn 已删除 |
| 9 | SYS_MMAP | trap.c | |
| 10 | SYS_MUNMAP | trap.c | |
| 11 | SYS_SHM_CREATE | trap.c | |
| 12 | SYS_SHM_ATTACH | trap.c | |
| 13 | SYS_PIPE | trap.c | |
| 14 | SYS_WRITE | trap.c | |
| 15 | SYS_READ | trap.c | |
| 16 | SYS_CLOSE | trap.c | |
| 17 | SYS_NOTIFY | trap.c | |
| 18 | SYS_GETTIME | trap.c | |
| 19 | SYS_CLOCK | trap.c | |
| 20 | SYS_MSG | trap.c | ≤64KB 变长 IPC |
| 21 | SYS_MSG_RESP | trap.c | |
| 22 | SYS_IOPERM | trap.c | |
| 23 | SYS_DUP2 | trap.c | |
| 24 | SYS_FCNTL | trap.c | F_GETFL/F_SETFL |
| 25 | SYS_DMA_ALLOC | trap.c | |
| 26 | SYS_DMA_FREE | trap.c | |
| 27 | SYS_PCI_DEV_INFO | trap.c | |
| 28 | SYS_BLOCK_ASYNC | trap.c | |
| 29 | SYS_INSTALL_FD | trap.c | |
| 30 | SYS_SOCKET | socket.c | AF_UNIX only |
| 31 | SYS_BIND | socket.c | |
| 32 | SYS_LISTEN | socket.c | |
| 33 | SYS_ACCEPT | socket.c | |
| 34 | SYS_CONNECT | socket.c | |
| 35 | SYS_SOCKETPAIR | socket.c | |
| 36 | SYS_SENDMSG | socket.c | SCM_RIGHTS fd 传递 |
| 37 | SYS_RECVMSG | socket.c | |
| 38 | SYS_SHUTDOWN | socket.c | |
| 39 | SYS_POLL | socket.c | pipe/socket/dev/tty |
| 40 | SYS_LSEEK | trap.c | |
| 41 | SYS_MEMFD_CREATE | trap.c | |
| 42 | SYS_FTRUNCATE | trap.c | |
| 43 | SYS_KILL | trap.c | pid>0/pid==0/pid<0(pgsignal) |
| 44 | SYS_SIGACTION | trap.c | |
| 45 | SYS_SIGRETURN | trap.c | |
| 46 | SYS_DEBUG_PRINT | trap.c | |
| 47 | SYS_OPEN | vfs.c | VFS/FAT32 |
| 48 | SYS_STAT | vfs.c | |
| 49 | SYS_MKDIR | vfs.c | |
| 50 | SYS_UNLINK | vfs.c | |
| 51 | SYS_RMDIR | vfs.c | |
| 52 | SYS_DEV_CREATE | vfs.c | |
| 53 | SYS_GETDENTS | vfs.c | |
| 54 | SYS_IOCTL | trap.c | FD_TTY: pty_ioctl |
| 55 | SYS_FSTAT | vfs.c | |
| 56 | SYS_FDEV_PID | trap.c | |
| 57 | SYS_FORK | proc.c | |
| 58 | SYS_EXECVE | proc.c | |
| 59 | SYS_SETSID | trap.c | |
| 60 | SYS_SETPGID | trap.c | |
| 61 | SYS_GETPGID | trap.c | |
| 62 | SYS_GETSID | trap.c | |

### libc 已实现 POSIX 函数

| 分类 | 函数 | 头文件 | 底层 syscall |
|------|------|--------|-------------|
| 进程 | getpid, fork, execve, spawn, _exit, exit, waitpid, setsid, setpgid, getpgid, getsid | unistd.h, sys/wait.h, sys/process.h, stdlib.h | SYS_GETPID/FORK/EXECVE/EXIT/WAITPID/SETSID/SETPGID |
| I/O | read, write, close, pipe, dup2, fcntl, open, lseek, ioctl, poll | unistd.h, fcntl.h, sys/poll.h, sys/ioctl.h | SYS_READ/WRITE/CLOSE/PIPE/DUP2/FCNTL/OPEN/LSEEK/IOCTL/POLL |
| FILE | printf, fprintf, vfprintf, fflush, putchar, fputc, fputs, puts, fgetc, getchar, fopen, fclose, fread, fwrite, fseek, ftell, rewind, sprintf, snprintf, perror | stdio.h | SYS_WRITE/READ |
| 内存 | malloc, free, calloc, realloc, mmap, munmap | stdlib.h, sys/mman.h | SYS_MMAP/MUNMAP |
| 字符串 | strlen, strcmp, strncmp, strcpy, strncpy, strcat, strchr, memcpy, memset, memmove, memcmp, strstr, strtok, strtok_r, strerror, bzero | string.h | — (纯用户态) |
| 字符类 | isdigit, isalpha, isalnum, isprint, isspace, ispunct, islower, isupper, tolower, toupper | ctype.h | — (纯用户态) |
| 数值 | atoi, atol, strtol, strtoul, abs, labs, rand, srand, qsort | stdlib.h | rand 种子用 SYS_GETTIME |
| 时间 | timespec_get, clock, sleep, usleep | time.h, unistd.h | SYS_GETTIME/CLOCK/RECV(timeout) |
| 信号 | kill, sigaction, sigprocmask, raise, signal | signal.h | SYS_KILL/SIGACTION/SIGRETURN |
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
| POSIX signal 扩展 | sigpending/sigsuspend/sigaltstack，当前只实现 sigaction+sigprocmask+kill | 中 |
| perror errno 映射 | 当前 perror 只输出字符串，需 errno→字符串完整映射 | 低 |
| strftime | 时间格式化，需时区支持（当前无时区概念） | 低 |
| opendir/readdir 内核化 | 当前 opendir 通过 sys_open + sys_getdents 实现，readdir 需 dirent 结构转换 | 低 |
| fs_driver 源码删除 | driver/fs_driver.cc 文件仍存在于磁盘（CMake 构建目标已移除） | 中 |
| IPC 接口降级到驱动专用 | recv/resp/req/msg 从公共 libc 头移到 driver/ipc.h，详见 [ipc.md](ipc.md) | 低 |
