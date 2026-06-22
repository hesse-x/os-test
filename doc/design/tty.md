# TTY 子系统设计

## 概述

参考 macOS BSD 层（功能在内核态）+ Linux API（接口对齐），实现 TTY 子系统。内核维护 line discipline、信号分发、session/pgid 等不可替代的机制；terminal 进程作为 pty master holder（类似 macOS Terminal.app），只负责 VT100 渲染和键盘输入转发，不再承担信号生成和线路处理策略。

## 动机

当前架构缺陷：

| 问题 | 现状 |
|------|------|
| Ctrl+C 无信号 | 0x03 作为原始字节穿过 pipe，shell 收到 `\x03` 而非 SIGINT |
| 无 canonical/raw 模式 | vi 等程序需要 raw 模式（tcsetattr），当前只能传原始字节 |
| 无 session/pgid | proc_t 缺 sid/pgid/ctty，无法做 job control |
| isatty() 有 bug | 比较 `target_pid == DEV_TERMINAL`（常量5），实际 target_pid 是动态 PID |
| 无 pty | 无法支持多终端、ssh、Wayland compositor 等需要 pty 的场景 |
| terminal 职责过重 | terminal 进程承担信号检测策略，但信号分发（按 pgid 遍历 procs[]）只有内核能做 |
| 两条单向 pipe | 两个独立 pipe 替代了 tty 的双向语义，stdout pipe 关闭不代表 stdin EOF |

## 架构

### 数据流

```
键盘 → kbd_driver SHM → terminal 读 kbd_ring → sys_write(master_fd)
  → 内核 line discipline 处理：
    · canonical 模式: echo 回 master read buffer, 行缓冲积累到 '\n' 或 VEOF
    · VINTR(Ctrl+C): pgsignal(fg_pgid, SIGINT), 不写入 slave
    · VSUSP(Ctrl+Z): pgsignal(fg_pgid, SIGTSTP), 不写入 slave
    · raw 模式: 字节直接进 slave input buffer
  → slave input buffer → shell sys_read(slave_fd)

shell sys_write(slave_fd) → slave output buffer → master read buffer
  → terminal sys_read(master_fd) → VT100 → cell buffer → KMS
```

与当前架构的区别：两条 pipe → 一个 pty pair（master + slave，各自双向）；line discipline 在内核处理输入侧（master→slave）；输出侧（slave→master）不做 line discipline 处理（raw 转发）。

### 组件划分

| 组件 | 位置 | 说明 |
|------|------|------|
| `struct tty` + ring buffers | 内核 `kernel/tty.h/tty.cc` | tty 核心数据结构 + 读写逻辑 |
| Line discipline | 内核 `kernel/tty.cc` | canonical/raw 模式、特殊字符、echo |
| Signal 分发 | 内核 `kernel/trap.cc` | pgsignal() 按 pgid 遍历 procs[] 投递信号 |
| Session/pgid | 内核 `kernel/proc.h` | proc_t 新增 sid/pgid/ctty 字段 |
| Pty 创建 | 内核 `kernel/tty.cc` | sys_pty_open 创建 master/slave fd pair |
| termios ioctl | 内核 `kernel/tty.cc` | TCGETS/TCSETS/TIOCSCTTY/TIOCGPGRP/TIOCSPGRP |
| Terminal 进程 | 用户态 `driver/terminal.cc` | pty master holder，VT100 渲染，键盘转发 |
| libc 封装 | 用户态 `user/include/termios.h` 等 | tcgetattr/tcsetattr/isatty/ttyname 等 |

## 内核数据结构

### struct tty

```c
// kernel/tty.h
#define TTY_BUF_SIZE 4096   // 与 pipe 相同
#define NCCS 19              // Linux 兼容

struct termios {
    tcflag_t c_iflag;    // input flags: ICRNL IGNCR INLCR BRKINT ISTRIP IXON IXOFF
    tcflag_t c_oflag;    // output flags: OPOST ONLCR
    tcflag_t c_cflag;    // control flags: CS8 CLOCAL (对 pty 大部分不适用)
    tcflag_t c_lflag;    // local flags: ISIG ICANON ECHO ECHOE ECHOK ECHONL NOFLSH TOSTOP
    cc_t     c_cc[NCCS]; // special characters
};

// c_cc indices (Linux 兼容)
#define VINTR    0   // Ctrl+C → SIGINT
#define VQUIT    1   // Ctrl+\ → SIGQUIT
#define VERASE   2   // Ctrl+H/Backspace → erase char
#define VKILL    3   // Ctrl+U → erase line
#define VEOF     4   // Ctrl+D → end of input
#define VTIME    5   // raw mode read timeout (0.1s units)
#define VMIN     6   // raw mode min read count
#define VSWTC    7   // unused
#define VSTART   8   // Ctrl+Q → resume output (IXON)
#define VSTOP    9   // Ctrl+S → stop output (IXON)
#define VSUSP    10  // Ctrl+Z → SIGTSTP
#define VEOL     11  // end-of-line (unused, set to _POSIX_VDISABLE)
#define VREPRINT 12  // Ctrl+R → reprint line
#define VDISCARD 13  // Ctrl+O → discard output
#define VWERASE  14  // Ctrl+W → erase word
#define VLNEXT   15  // Ctrl+V → literal next
#define VEOL2    16  // unused

// c_iflag bits
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0001000
#define IXOFF   0002000

// c_oflag bits
#define OPOST   0000001
#define ONLCR   0000004

// c_cflag bits
#define CS8     0000060
#define CLOCAL  0004000

// c_lflag bits
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

struct tty {
    // Ring buffers
    uint8_t  t_ibuf[TTY_BUF_SIZE];  // slave input (master write → ldisc → slave reads from here)
    uint32_t t_ibuf_head;
    uint32_t t_ibuf_tail;
    uint8_t  t_obuf[TTY_BUF_SIZE];  // slave output (slave write → master reads from here)
    uint32_t t_obuf_head;
    uint32_t t_obuf_tail;

    // Canonical line editing buffer (ICANON mode accumulates here until '\n'/VEOF)
    uint8_t  t_linebuf[TTY_BUF_SIZE]; // current line being edited
    uint32_t t_linebuf_len;

    // termios settings
    struct termios t_termios;

    // Session / foreground process group
    pid_t t_sid;       // session ID (0 = no session leader)
    pid_t t_pgid;      // foreground process group ID

    // Pty pair linkage
    struct tty *t_pair; // slave→master, master→slave

    // Side identification
    int t_is_master;    // 1 = master side, 0 = slave side

    // Blocked processes
    pid_t t_ibuf_waiter;  // pid blocked waiting for slave input data (WAIT_PIPE)
    pid_t t_obuf_waiter;  // pid blocked waiting for slave output space (WAIT_PIPE)
    pid_t t_master_read_waiter;  // pid blocked waiting for master read data
    pid_t t_master_write_waiter; // pid blocked waiting for master write space

    // Reference counting
    int t_refcount;      // total open fd count (master fds + slave fds)
};
```

### proc_t 扩展

```c
// kernel/proc.h — 新增字段
typedef struct proc_t {
    // ... existing fields ...

    pid_t sid;           // session ID (0 = not in any session, pid for session leader)
    pid_t pgid;          // process group ID (0 = not in any group, = pid for group leader)
    struct tty *ctty;    // controlling terminal (NULL = none)
} proc_t;
```

### fd_table 扩展

```c
// kernel/proc.h — 新增 fd 类型
#define FD_TTY   7   // pty master or slave

typedef struct file {
    int type;            // ... + FD_TTY
    int flags;
    union {
        // ... existing members ...
        struct tty *tty;     // if type == FD_TTY
    };
} file_t;
```

## Line Discipline

### Canonical 模式（ICANON + ISIG + ECHO）

输入路径（master write → line discipline → slave input buffer）：

1. **普通可打印字符**：追加到 `t_linebuf`，如果 ECHO 启用则 echo 到 master read buffer（terminal 看到按键回显）
2. **'\n' / VEOL**：追加到 `t_linebuf`，echo '\n'（含 ONLCR 则 echo '\r\n'），将整行从 `t_linebuf` 拷贝到 `t_ibuf`（slave 可读），唤醒 `t_ibuf_waiter`
3. **VINTR (Ctrl+C)**：如果 ISIG 启用 → `pgsignal(t_pgid, SIGINT)`，不写入 `t_linebuf` 不写入 `t_ibuf`，如果 ECHO 启用则 echo "^C\n" 到 master
4. **VQUIT (Ctrl+\)**：如果 ISIG 启用 → `pgsignal(t_pgid, SIGQUIT)`，同上，echo "^\\n"
5. **VSUSP (Ctrl+Z)**：如果 ISIG 启用 → `pgsignal(t_pgid, SIGTSTP)`，同上，echo "^Z\n"
6. **VERASE (Backspace/Ctrl+H)**：从 `t_linebuf` 删除末尾一字节，如果 ECHOE 启用则 echo "\b \b" 到 master
7. **VWERASE (Ctrl+W)**：从 `t_linebuf` 删除末尾一个 word，echo 类似 VERASE
8. **VKILL (Ctrl+U)**：清空 `t_linebuf`，如果 ECHOK 启用则 echo "^U\n"（或 ECHOE 则逐字 erase）
9. **VEOF (Ctrl+D)**：如果 `t_linebuf_len > 0`，将行缓冲内容拷贝到 `t_ibuf`（不带 '\n'），唤醒 `t_ibuf_waiter`；如果 `t_linebuf_len == 0`，slave read 返回 0（EOF）
10. **VLNEXT (Ctrl+V)**：下一个字符不做特殊处理，直接追加到 `t_linebuf`
11. **VSTART/VSTOP (Ctrl+Q/Ctrl+S)**：如果 IXON 启用，控制输出流（start/stop），不写入 `t_linebuf`

### Raw 模式（~ICANON ~ISIG ~ECHO）

- 所有字节直接从 master write buffer 拷贝到 `t_ibuf`
- 无 echo、无行编辑、无信号生成
- VMIN/VTIME 控制 read 阻塞行为：
  - VMIN > 0, VTIME = 0：等 VMIN 字节后返回
  - VMIN = 0, VTIME > 0：定时器模式，有数据立即返回，无数据等 VTIME 后返回 0
  - VMIN > 0, VTIME > 0：读第一个字节启动定时器
  - VMIN = 0, VTIME = 0：非阻塞，立即返回

### 输出路径（slave write → master read）

slave write 不做 line discipline 处理（raw 转发），但 OPOST 启用时做简单输出处理：
- ONLCR：'\n' → '\r\n'（换行前加回车）

### Default termios

```c
// 初始 termios 设置（Linux 兼容默认值）
static const struct termios default_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {
        [VINTR]  = 0x03,   // ^C
        [VQUIT]  = 0x1C,   // ^\
        [VERASE] = 0x7F,   // DEL (some systems use 0x08 = ^H)
        [VKILL]  = 0x15,   // ^U
        [VEOF]   = 0x04,   // ^D
        [VTIME]  = 0,
        [VMIN]   = 1,
        [VSWTC]  = 0,
        [VSTART] = 0x11,   // ^Q
        [VSTOP]  = 0x13,   // ^S
        [VSUSP]  = 0x1A,   // ^Z
        [VEOL]   = 0,      // _POSIX_VDISABLE
        [VREPRINT] = 0x12, // ^R
        [VDISCARD] = 0x0F, // ^O
        [VWERASE] = 0x17,  // ^W
        [VLNEXT] = 0x16,   // ^V
        [VEOL2]  = 0,      // _POSIX_VDISABLE
    },
};
```

## Pty (Pseudoterminal)

### sys_pty_open

```c
// syscall: sys_pty_open(int *master_fd_ptr, int *slave_fd_ptr)
// 创建 pty master/slave fd pair
//
// 返回:
//   0 = success, master_fd_ptr 和 slave_fd_ptr 写入 fd 号
//   -ENOMEM = kmalloc tty struct 失败
//   -EMFILE = 无空闲 fd slot
//
// 语义:
//   1. kmalloc(struct tty) 两次（master tty + slave tty），初始化 ring buffers + termios
//   2. master->t_pair = slave, slave->t_pair = master
//   3. master->t_is_master = 1, slave->t_is_master = 0
//   4. master->t_refcount = 1, slave->t_refcount = 1
//   5. 在当前进程 fd_table 分配两个空闲 slot
//   6. master_fd: type=FD_TTY, flags=O_RDWR, tty=master_tty
//   7. slave_fd: type=FD_TTY, flags=O_RDWR, tty=slave_tty
//   8. 写入 master_fd_ptr 和 slave_fd_ptr 到用户空间
```

**为什么用 sys_pty_open 而不是 /dev/ptmx**：初始实现用 syscall 更简单，不需要 VFS /dev/ptmx 设备节点。未来实现 /dev/ptmx 后，`open("/dev/ptmx")` 内部调用 `sys_pty_open` 即可，API 层面完全对齐 Linux。

### fd 操作

**master 侧 read/write**：

- `sys_read(master_fd)`：从 `slave->t_obuf` 读（slave 输出 + echo 回显），obuf 空 + slave 侧无引用则返回 0 (EOF)
- `sys_write(master_fd)`：写入 `master->t_ibuf`，经 line discipline 处理后进入 `slave->t_ibuf`（canonical 模式）或直接进入 `slave->t_ibuf`（raw 模式）；echo 字节回 `slave->t_obuf`

**slave 侧 read/write**：

- `sys_read(slave_fd)`：从 `slave->t_ibuf` 读（来自 master 经 line discipline 处理的输入）
- `sys_write(slave_fd)`：写入 `slave->t_obuf`（terminal 从 master read 取走）

**sys_close(fd)**：

- refcount--，归零则 kfree tty struct
- slave 侧最后一个 fd 关闭时：master read 返回 0 (EOF)
- master 侧最后一个 fd 关闭时：向 slave 侧所有进程发 SIGHUP（if session exists），slave write 返回 -EPIPE

### fd 继承

`sys_spawn` 对 FD_TTY 的继承与 FD_PIPE 类似：
- 复制 fd_table entry，tty refcount++
- slave fd 可被 shell 的子进程继承（fd 0/1/2 都指向同一个 slave tty）

### 与 pipe 的对比

| 特性 | Pipe | Pty slave |
|------|------|-----------|
| 方向 | 单向（read end / write end） | 双向（同一 fd 可读可写） |
| 缓冲 | 4KB ring buffer | slave 有独立 ibuf + obuf |
| Line discipline | 无 | 有（canonical/raw） |
| Signal | 无 | 有（ISIG → SIGINT/SIGTSTP） |
| Echo | 无 | canonical 模式 echo 回 master |
| EOF | write end 关闭 → read 返回 0 | slave 侧无引用 → master read 返回 0 |
| Controlling tty | 无 | slave 可成为 ctty |
| isatty() | 返回 0 | 返回 1 |

## Session / Process Group

### 进程关系

```
Session (sid=4)
  ├─ Process Group (pgid=4, foreground) ← t_pgid in tty
  │   ├─ shell (pid=4, session leader, group leader)
  │   └─ hello.elf (pid=5, same pgid=4)
  └─ Process Group (pgid=6, background)
      ├─ sleep.elf (pid=6, group leader)
```

### 规则

1. **Session leader**：调用 `setsid()` 的进程成为 session leader，`sid = pid`，`pgid = pid`，脱离原 session
2. **Process group**：`setpgid(pid, pgid)` 将进程加入指定 group，pgid 必须是该 session 内已有进程的 pid（或等于调用者 pid 创建新 group）
3. **Controlling terminal**：session leader 打开 pty slave（TIOCSCTTY ioctl）时，该 slave 成为此 session 的 controlling terminal。一个 session 只有一个 ctty。一个 tty 只能被一个 session 作为 ctty。
4. **Foreground process group**：每个 tty 有 `t_pgid`，由 `tcsetpgrp()` 设置。只有 session 内的 pgid 才能设为 foreground。
5. **Background read**：非 foreground pgid 的进程从 controlling terminal slave read → 收到 SIGTTIN（被 stop），默认 stop 进程
6. **Background write**：如果 TOSTOP 启用，非 foreground pgid 的进程从 controlling terminal slave write → 收到 SIGTTOUP

### syscalls

```c
// setsid() — 创建新 session
// 前置条件: 调用者不能已经是 session leader (即没有其他进程 sid == calling_pid)
// 效果: sid = pid, pgid = pid, ctty = NULL (脱离原 controlling terminal)
// 返回: sid (>= 1) on success, -EPERM if already session leader

// setpgid(pid, pgid) — 设置进程组
// pid=0 表示调用者自身, pgid=0 表示 pgid=calling_pid (创建新 group)
// 前置条件: 目标进程必须是调用者自身或其子进程(且子进程未 exec)；pgid 必须在该 session 内
// 返回: 0 on success, 各种 errno

// getpgid(pid) — 获取进程组 ID
// pid=0 表示调用者自身
// 返回: pgid on success, -ESRCH if pid 不存在

// getsid(pid) — 获取 session ID
// pid=0 表示调用者自身
// 返回: sid on success, -ESRCH/-EPERM
```

### pgsignal

```c
// pgsignal(pgid, sig) — 向进程组发送信号
// 遍历 procs[0..MAX_PROC-1]，对每个 procs[i].pgid == pgid 的进程设 pending bits
// 语义与 kill(-pgid, sig) 等价（Linux kill(pid<0) = 按 pgid 发信号）
//
// 调用场景:
//   line discipline 检测 VINTR → pgsignal(tty->t_pgid, SIGINT)
//   line discipline 检测 VSUSP → pgsignal(tty->t_pgid, SIGTSTP)
//   kill(-pgid, sig) → pgsignal(abs(pgid), sig)
```

## ioctl

### 新 syscall

```c
// SYS_IOCTL = 51
// sys_ioctl(fd, request, arg)
//   fd: 文件描述符
//   request: ioctl request code (Linux UAPI 兼容)
//   arg: request-specific argument (值或指针)
//
// 当前仅支持 FD_TTY fd 的 TTY ioctl。其它 fd 类型返回 -ENOTTY。
// arg 作为指针时，内核 copy_from_user/copy_to_user 适当大小。
```

### TTY ioctl requests (Linux 兼容 request codes)

| Request | Code | arg 类型 | 说明 |
|---------|------|----------|------|
| TCGETS | 0x5401 | `struct termios *` | tcgetattr：获取 termios |
| TCSETS | 0x5402 | `struct termios *` | tcsetattr(TCSANOW)：立即设置 |
| TCSETSW | 0x5403 | `struct termios *` | tcsetattr(TCSADRAIN)：等输出排空后设置 |
| TCSETSF | 0x5404 | `struct termios *` | tcsetattr(TCSAFLUSH)：排空输出+清空输入后设置 |
| TIOCGPGRP | 0x540F | `pid_t *` | tcgetpgrp：获取 foreground pgid |
| TIOCSPGRP | 0x540E | `pid_t *` | tcsetpgrp：设置 foreground pgid |
| TIOCSCTTY | 0x540E | `int` (force) | 设置 controlling terminal (force=1 强制夺取) |
| TIOCNOTTY | 0x540F | 无 | 脱离 controlling terminal |
| TIOCSIG | 0x5410 | `int` (sig) | 从 pty master 向 slave session 发信号 |
| TIOCGWINSZ | 0x5413 | `struct winsize *` | 获取窗口大小 (rows/cols) |
| TIOCSWINSZ | 0x5414 | `struct winsize *` | 设置窗口大小 (触发 SIGWINCH) |

**注**：Linux 中 TIOCSCTTY 和 TIOCSPGRP 的 request code 实际值需要确认。上面列的是常见值，实现时查 Linux `asm/ioctls.h` 确保完全对齐。

### winsize

```c
struct winsize {
    unsigned short ws_row;     // 行数
    unsigned short ws_col;     // 列数
    unsigned short ws_xpixel;  // 像素宽 (unused, 设为 0)
    unsigned short ws_ypixel;  // 像素高 (unused, 设为 0)
};
```

Terminal 进程在 attach display SHM 后通过 `ioctl(master_fd, TIOCSWINSZ, &ws)` 设置窗口大小。Shell 通过 `ioctl(slave_fd, TIOCGWINSZ, &ws)` 获取终端行列数（替代硬编码 80x25）。窗口大小变化时内核向 slave session 发 SIGWINCH。

## sys_kill 扩展

当前 `sys_kill(pid, sig)` 只支持 `pid > 0`（指定进程）。需扩展支持 Linux 语义：

| pid 值 | 语义 |
|--------|------|
| pid > 0 | 发信号到指定进程 |
| pid == 0 | 发信号到调用者所在进程组的所有进程 |
| pid == -1 | 发信号到所有进程（除 init 和自身）— **暂不实现，返回 -EPERM** |
| pid < -1 | 发信号到进程组 abs(pid) 的所有进程 — 调用 `pgsignal(abs(pid), sig)` |

## 新 syscall 编号

当前 NR_SYSCALL 范围 0-50（SYS_DEBUG_PRINT=#50）。新增：

| # | Name | Signature | 说明 |
|---|------|-----------|------|
| 51 | SYS_IOCTL | `sys_ioctl(fd, request, arg)` | 通用设备控制 |
| 52 | SYS_SETSID | `sys_setsid()` | 创建新 session |
| 53 | SYS_SETPGID | `sys_setpgid(pid, pgid)` | 设置进程组 |
| 54 | SYS_GETPGID | `sys_getpgid(pid)` | 获取进程组 |
| 55 | SYS_GETSID | `sys_getsid(pid)` | 获取 session ID |
| 56 | SYS_PTY_OPEN | `sys_pty_open(master_fd_ptr, slave_fd_ptr)` | 创建 pty pair |

NR_SYSCALL 更新为 57。

## libc 封装

### 新头文件

```
user/include/termios.h     — struct termios, c_cc indices, flag bits, tcgetattr/tcsetattr/tcgetpgrp/tcsetpgrp
user/include/sys/ioctl.h   — ioctl() 声明
```

### libc 函数

```c
// termios.h
int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
// optional_actions: TCSANOW(0), TCSADRAIN(1), TCSAFLUSH(2)

pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgid);

// sys/ioctl.h
int ioctl(int fd, unsigned long request, ...);

// unistd.h 扩展
pid_t setsid(void);
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getsid(pid_t pid);

// isatty() 修复（不再比较 target_pid == DEV_TERMINAL）
int isatty(int fd) {
    // 方案: sys_ioctl(fd, TCGETS, &tmp) — 成功返回 1, -ENOTTY 返回 0
    struct termios tmp;
    return ioctl(fd, TCGETS, &tmp) == 0 ? 1 : 0;
}

// ttyname() — 通过 /dev/pts/N 路径返回（暂返回空字符串）
char *ttyname(int fd);
```

### libc fd_table 扩展

```c
// user/lib/file.cc
enum fd_type_t { FD_NONE = 0, FD_PIPE, FD_FILE, FD_DEV, FD_TTY };

struct file_fd_entry {
    enum fd_type_t type;
    int flags;
    union {
        uint64_t file_size;  // FD_FILE
        pid_t target_pid;    // FD_DEV
        // FD_TTY: no extra data needed, kernel handles everything
    };
};
```

## Terminal 进程改造

### 当前架构（两条 pipe）

```c
// driver/terminal.cc (当前)
pipe(p_stdin);   // p_stdin[0] = shell stdin read, p_stdin[1] = terminal stdin write
pipe(p_stdout);  // p_stdout[0] = terminal stdout read, p_stdout[1] = shell stdout write
dup2(p_stdin[0], 0);   // for shell inheritance
dup2(p_stdout[1], 1);  // for shell inheritance
spawn(shell);
dup2(p_stdout[0], 0);  // terminal reads shell output
dup2(p_stdin[1], 1);   // terminal writes keystrokes
```

### 新架构（pty pair）

```c
// driver/terminal.cc (改造后)
int master_fd, slave_fd;
sys_pty_open(&master_fd, &slave_fd);

// Shell 需要一个 slave fd 作为 fd 0/1/2 + controlling terminal
// 在当前进程 fd_table 中布局:
//   slave_fd → dup2(slave_fd, 0), dup2(slave_fd, 1), dup2(slave_fd, 2)
//   fd 0/1/2 都指向同一个 pty slave (双向, O_RDWR)
dup2(slave_fd, 0);
dup2(slave_fd, 1);
dup2(slave_fd, 2);
close(slave_fd);   // 原始 slave_fd slot 释放 (refcount 仍 > 0, fd 0/1/2 各持一份)

spawn(shell);      // shell inherits fd 0/1/2 (all slave side)
// Shell 启动后调用 setsid() + ioctl(0, TIOCSCTTY, 0) 建立会话

close(0); close(1); close(2);  // terminal 关闭继承的 slave fds

// terminal 自己保留 master_fd (唯一与 shell/子进程通信的 fd)
// terminal 主循环:
//   - 读 kbd_ring → write(master_fd, &ch, 1)  // 键盘输入
//   - read(master_fd, buf, 256)                // shell 输出 + echo
//   - VT100 处理 → cell buffer → display flush

// 设置窗口大小
struct winsize ws = { rows, cols, 0, 0 };
ioctl(master_fd, TIOCSWINSZ, &ws);
```

**简化**：terminal 只持一个 master_fd，所有交互通过这一个 fd 完成（双向）。不再需要 dup2 重排两条 pipe 的复杂步骤。

### Shell 改造

Shell 启动后需：

```c
// shell/shell.cc (改造后)
pid_t sid = setsid();            // 成为 session leader
ioctl(0, TIOCSCTTY, 0);          // fd 0 (slave) 成为 controlling terminal
// 后续: spawn 子进程时, 子进程继承 fd 0/1/2 (slave side)
//       shell 设置 tcsetpgrp(0, child_pgid) 将子进程放到前台
```

## 初始化流程

### 当前流程

```
kernel_main → 加载 init.elf → init spawn kbd_driver/kms_driver/terminal
→ terminal 创建 pipe → spawn shell → shell 运行
```

### 新流程

```
kernel_main → 加载 init.elf → init spawn kbd_driver/kms_driver/terminal
→ terminal sys_pty_open → dup2 slave to 0/1/2 → spawn shell
→ shell setsid + TIOCSCTTY → shell 运行
```

内核不参与 pty 创建决策——terminal 进程自行决定何时创建 pty、spawn shell。内核只提供机制（pty_open, setsid, ioctl）。

## 实现步骤

### Step 1: 内核 tty 数据结构 + sys_pty_open

- 新建 `kernel/tty.h` + `kernel/tty.cc`
- 实现 `struct tty` 初始化、ring buffer 读写辅助函数
- 实现 `sys_pty_open(master_fd_ptr, slave_fd_ptr)`
- fd_table 新增 FD_TTY 类型
- `sys_read/sys_write/sys_close` 新增 FD_TTY 分支

**验证**: `sys_pty_open` 成功返回 master+slave fd pair，master write → slave read 正常传递数据（raw 模式）

### Step 2: Line discipline (canonical 模式)

- 实现 `tty_ldisc_input(tty, byte)` — canonical 模式处理
- 支持 VINTR/VSUSP/VQUIT → pgsignal
- 支持 VERASE/VKILL/VWERASE 行编辑
- 支持 ECHO/ECHOE/ECHOK echo 到 master read buffer
- 支持 VEOF/VLNEXT/ICRNL

**验证**: Terminal write Ctrl+C 到 master → 内核发 SIGINT 到 foreground pgid → shell 子进程退出

### Step 3: pgsignal + session/pgid

- proc_t 新增 sid/pgid/ctty 字段
- 实现 `sys_setsid/sys_setpgid/sys_getpgid/sys_getsid`
- 实现 `pgsignal(pgid, sig)` — 遍历 procs[] 按 pgid 投递
- `sys_kill` 扩展支持 pid < 0（按 pgid 发信号）
- `sys_spawn` 初始化子进程 sid/pgid（继承父进程 pgid）

**验证**: shell `setsid()` → `setpgid()` → `kill(-pgid, SIGINT)` 正确投递到进程组

### Step 4: ioctl + termios

- 实现 `sys_ioctl(fd, request, arg)`
- TCGETS/TCSETS/TCSETSW/TCSETSF
- TIOCSCTTY/TIOCNOTTY
- TIOCGPGRP/TIOCSPGRP
- TIOCGWINSZ/TIOCSWINSZ + SIGWINCH
- TIOCSIG（master 侧向 slave session 发信号）

**验证**: shell `tcgetattr(0, &t)` + `tcsetattr(0, TCSANOW, &raw)` 切换到 raw 模式，vi 类程序可运行

### Step 5: Terminal 进程改造

- terminal.cc 改为 pty master holder
- 移除两条 pipe 创建 + dup2 重排逻辑
- 改为 sys_pty_open + dup2 slave to 0/1/2 + spawn shell
- 设置 TIOCSWINSZ
- Shell 启动时 setsid + TIOCSCTTY

**验证**: Ctrl+C → shell 子进程收到 SIGINT 退出 → shell 回到提示符；tcsetattr 切换 raw 模式 → 键盘输入不经 line discipline 处理

### Step 6: libc 封装

- 新建 `user/include/termios.h`
- 新建 `user/include/sys/ioctl.h`
- libc: tcgetattr/tcsetattr/tcgetpgrp/tcsetpgrp/isatty/ttyname
- libc: setsid/setpgid/getpgid/getsid
- libc: ioctl()
- libc fd_table 新增 FD_TTY 类型

**验证**: 用户程序 `isatty(0)` 返回 1；`tcgetattr(0, &t)` 正确获取 termios

### Step 7: Poll 扩展

- `sys_poll` 扩展 FD_TTY 支持（master/slave fd 的 POLLIN/POLLOUT/POLLHUP）
- Terminal 进程改为 poll 驱动（替代当前 1ms recv timeout 轮询）

**验证**: terminal `poll(master_fd, POLLIN, timeout)` 阻塞等待 shell 输出，不 busy-poll

## 与 Wayland 的关系

Wayland compositor 需要 pty 来运行图形终端客户端：

```
Wayland compositor → sys_pty_open → 创建 pty pair
  → compositor 持 master fd → 渲染终端窗口
  → 终端客户端进程持 slave fd (ctty) → 运行 shell
```

compositor 通过 master fd 读写与终端客户端交互，内核 line discipline 处理信号和行编辑。这完全复用了本次设计的 TTY 子系统，无需额外修改。

## 与 gcc 构建的关系

gcc 构建依赖 termios（configure 脚本检测终端属性）、isatty（many tools 检查 stdout 是否是终端）、进程组（make 的 job server 用 pipe + pgid 协调并行编译）。本次设计覆盖这些依赖。

## 技术债务与后续工作

| 项目 | 说明 | 优先级 |
|------|------|--------|
| /dev/ptmx | `open("/dev/ptmx")` 替代 `sys_pty_open`，需 VFS /dev 设备节点 | 中 |
| /dev/pts/N | slave 侧设备文件，`open("/dev/pts/0")` 获取 slave fd | 中 |
| Serial console tty | COM1 串口作为内核 console tty（sys_write FD_SERIAL 合并进 tty） | 低 |
| SIGWINCH | 窗口大小变化时向 session 投递 SIGWINCH | 中 |
| sigprocmask | 信号阻塞掩码（EINTR 中断阻塞 syscall） | 中 |
| EINTR | 阻塞 syscall 被信号中断返回 -EINTR | 中 |
| 多终端 (virtual console) | Ctrl+Fn 切换多个 terminal/shell session | 低 |
| TIOCSCTTY force | force=1 时从其他 session 夺取 controlling terminal | 低 |
| output line discipline | OPOST 的完整实现（OCRNL/ONOCR/ONLRET 等） | 低 |
