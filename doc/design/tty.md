# PTY 子系统设计

## 概述

实现 POSIX PTY（伪终端 master/slave 对），内核提供双向字节管道机制 + termios 存储 + fd 管理，用户态 Terminal 提供 line discipline 策略（VT100 解析、Ctrl-C→SIGINT、echo、行编辑）。对外接口与 Linux 一致（/dev/ptmx、TCGETS/TCSETS、isatty），内部遵循微内核原则（策略在用户态）。

## 动机

| 问题 | 现状 | PTY 解决 |
|------|------|---------|
| test_runner 屏幕不显示 | fd 1 = /dev/serial，不经 Terminal pipe | slave fd 继承后输出自动流向 Terminal master |
| Ctrl+C 无信号 | 0x03 作为字节穿过 pipe | Terminal 读到 Ctrl-C → sys_kill(SIGINT) |
| 无 canonical/raw 模式 | vi 等程序无法 tcsetattr | Terminal 读 termios 调整 ldisc 行为 |
| 两条单向 pipe | stdout/stdin 分离，不符合 TTY 语义 | PTY 是双向管道，fd 0/1 指向同一 slave |
| isatty() bug | 比较动态 PID | TCGETS ioctl 成功即 isatty=1 |
| 无 pty | 无法支持多终端、Wayland terminal | /dev/ptmx 创建 master/slave 对 |

## 架构

### 数据流

```
键盘 → kbd_driver SHM → Terminal 用户态 line discipline 处理：
  · Ctrl-C → sys_kill(-shell_pgid, SIGINT)，不写入 master
  · Ctrl-D → 不写入 master（或写入触发 EOF 语义）
  · canonical 模式: 积累字符直到 Enter → write(master_fd, line)
  · raw 模式: 每字符立即 write(master_fd, ch)
  → write(master_fd) → 内核 PTY m_to_s_buf → slave sys_read(slave_fd)

shell sys_write(slave_fd) → 内核 PTY s_to_m_buf → master sys_read(master_fd)
  → Terminal VT100 → cell buffer → KMS
```

**关键区别**：line discipline 在 Terminal（用户态），不在内核。内核 PTY 是纯字节管道。

### 组件划分

| 组件 | 位置 | 说明 |
|------|------|------|
| struct pty + ring buffers | 内核 `kernel/pty.h/pty.c` | 双向字节缓冲 + 阻塞等待 + refcount |
| termios 存储 | 内核 `kernel/pty.c` | TCGETS/TCSETS 直接读写内核 termios 字段 |
| /dev/ptmx 创建 | 内核 `kernel/pty.c` | open → 分配 index → 创建 pty + /dev/ptsN slave 节点 |
| Line discipline | 用户态 `driver/terminal.cc` | Ctrl-C→SIGINT, echo, canonical/raw, 行编辑 |
| Session/pgid | 内核 `kernel/proc.h` | proc_t 新增 sid/pgid/ctty（未来 job control） |
| Terminal 进程 | 用户态 `driver/terminal.cc` | pty master holder，VT100 渲染，键盘转发 |

## 内核数据结构

### struct pty

```c
#define PTY_BUF_SIZE 4096   // 与 pipe 相同

struct pty {
    // 双向缓冲
    uint8_t  m_to_s_buf[PTY_BUF_SIZE];  // master→slave 方向（Terminal 键盘输入）
    uint32_t m_to_s_head, m_to_s_tail;
    uint8_t  s_to_m_buf[PTY_BUF_SIZE];  // slave→master 方向（Shell 输出）
    uint32_t s_to_m_head, s_to_m_tail;

    // 阻塞等待
    pid_t    m_read_pid;    // master 读阻塞（-1=无）
    pid_t    m_write_pid;   // master 写阻塞
    pid_t    s_read_pid;    // slave 读阻塞
    pid_t    s_write_pid;   // slave 写阻塞

    // termios（内核存储，Terminal 通过 TCGETS 读取调整 ldisc 行为）
    struct termios t_termios;

    // 窗口大小
    struct winsize t_winsize;

    // PTY 编号和引用计数
    int      index;         // PTY 编号（对应 /dev/ptsN）
    int      master_refs;   // master fd 引用数
    int      slave_refs;    // slave fd 引用数
    int      slave_opened;  // slave 端是否已被打开

    // Session / foreground process group（未来 job control）
    pid_t    t_sid;         // session ID（0 = 无 session）
    pid_t    t_pgid;        // foreground process group ID
};
```

**与 Linux tty_struct 的区别**：Linux 有 linebuf（canonical 行缓冲）、ldisc 指针、driver 指针、link 指针等。这些功能在用户态 Terminal 实现，内核不需要。struct pty 只是两个 pipe 合成一个对象。

### struct termios（跟 Linux x86-64）

```c
#define NCCS 19

typedef unsigned long tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;    // ICRNL IGNCR INLCR BRKINT ISTRIP IXON IXOFF
    tcflag_t c_oflag;    // OPOST ONLCR
    tcflag_t c_cflag;    // CS8 CLOCAL
    tcflag_t c_lflag;    // ISIG ICANON ECHO ECHOE ECHOK NOFLSH TOSTOP IEXTEN
    cc_t     c_cc[NCCS]; // special characters
};
```

c_cc indices 和 flag bits 与 Linux 对齐（VINTR=0, VQUIT=1, VERASE=2, ... 等）。

### Default termios

```c
static const struct termios default_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = {
        [VINTR]  = 0x03,   // ^C
        [VQUIT]  = 0x1C,   // ^
        [VERASE] = 0x7F,   // DEL
        [VKILL]  = 0x15,   // ^U
        [VEOF]   = 0x04,   // ^D
        [VTIME]  = 0,
        [VMIN]   = 1,
        [VSTART] = 0x11,   // ^Q
        [VSTOP]  = 0x13,   // ^S
        [VSUSP]  = 0x1A,   // ^Z
    },
};
```

### struct winsize（跟 Linux）

```c
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;  // unused, 设为 0
    unsigned short ws_ypixel;  // unused, 设为 0
};
```

### proc_t 扩展（未来 job control，本阶段预留）

```c
typedef struct proc_t {
    // ... existing fields ...
    pid_t sid;           // session ID (0 = not in any session)
    pid_t pgid;          // process group ID (0 = not in any group)
    struct pty *ctty;    // controlling terminal (NULL = none)
} proc_t;
```

### fd_table 扩展

```c
#define FD_TTY   8   // pty master or slave（FD_FILE 已占 7）

typedef struct file {
    int type;            // ... + FD_TTY
    int flags;
    union {
        // ... existing members ...
        struct pty *pty;     // if type == FD_TTY
    };
} file_t;
```

master 和 slave 都用 FD_TTY 类型。master/slave 区别不在 fd 里 — Terminal 持有的 FD_TTY 是 master，Shell 持有的 FD_TTY 是 slave。区别由 pty 的引用计数（master_refs / slave_refs）隐式表达。

## PTY 创建：/dev/ptmx

### 设备注册

`/dev/ptmx` 注册在 devtmpfs，`dev_ops.driver_pid = 0`（内核设备）。open 回调创建 PTY 对：

```c
static struct dev_ops ptmx_ops = {
    .driver_pid = 0,
    .device_type = DEV_PTMX,
    .open = ptmx_open,    // 创建 PTY 对，返回 master fd
};
```

### ptmx_open 流程

```
open("/dev/ptmx")
  → ptmx_ops->open(proc, fd)
    1. 分配 PTY index（全局递增，0/1/2...）
    2. kmalloc(struct pty)，初始化 ring buffers + default_termios
    3. 在 devtmpfs 注册 "ptsN" slave 设备节点
    4. 修改 fd_table[fd]：type=FD_TTY, pty=new_pty, flags=O_RDWR
    5. 设置 pty->master_refs = 1
    6. 返回 0（fd 已分配）
```

devtmpfs_open 先分配 fd 为 FD_DEV 类型，ptmx_open 回调修改为 FD_TTY。

### slave 打开

Terminal 在 ptmx_open 后立即打开 slave：

```
slave_fd = open("/dev/ptsN")
  → devtmpfs_lookup("ptsN") → 找到 slave inode
  → devtmpfs_open → 分配 fd，type=FD_TTY, pty=同一 struct pty
  → pty->slave_refs++
  → pty->slave_opened = 1
```

### PTY 全局表

```c
#define MAX_PTY 16
static struct pty *pty_table[MAX_PTY];  // index → pty 指针
static int next_pty_index = 0;
static spinlock_t pty_alloc_lock;
```

限制 16 个 PTY 对（同时最多 16 个 terminal session）。devtmpfs MAX_DEV_ENTRIES=32，预留一半给其他设备。

## fd 操作

### master 侧 read/write

```
sys_read(master_fd):
  → pty = proc->fd_table[master_fd].pty
  → 从 pty->s_to_m_buf 读数据
  → 空：阻塞 m_read_pid = proc->pid, wait_event=WAIT_PIPE
  → slave_refs == 0 且缓冲空：返回 0 (EOF) — Shell 已退出

sys_write(master_fd):
  → pty = proc->fd_table[master_fd].pty
  → 写入 pty->m_to_s_buf（用户态 Terminal 已做 ldisc 处理的字节）
  → 唤醒 s_read_pid（Shell 从 slave 读）
```

**关键**：master write 的数据是 Terminal ldisc 处理后的结果（不是原始键盘字节）。Ctrl-C 不会出现在 master write 数据中（Terminal 直接 sys_kill），canonical 模式下 Enter 后才 write 整行。

### slave 侧 read/write

```
sys_read(slave_fd):
  → pty = proc->fd_table[slave_fd].pty
  → 从 pty->m_to_s_buf 读数据
  → 空：阻塞 s_read_pid = proc->pid, wait_event=WAIT_PIPE
  → master_refs == 0 且缓冲空：返回 0 (EOF)

sys_write(slave_fd):
  → pty = proc->fd_table[slave_fd].pty
  → 写入 pty->s_to_m_buf
  → 唤醒 m_read_pid（Terminal 从 master 读）
  → OPOST + ONLCR：'\n' → '\r\n'（内核做简单输出处理，这很小）
```

### sys_close(fd)

```
FD_TTY close:
  → pty = fd_table[fd].pty
  → 判断是 master 还是 slave：
    master_refs-- 还是 slave_refs--

  slave 侧最后一个 fd 关闭（slave_refs == 0）：
    → 清空 m_to_s_buf 剩余数据
    → 唤醒 m_read_pid（master read 返回 0 = EOF）
    → slave_opened = 0
    → 从 devtmpfs 删除 "ptsN" 设备节点

  master 侧最后一个 fd 关闭（master_refs == 0）：
    → 向 slave session 发 SIGHUP（如果有 session）
    → slave write 返回 -EPIPE
    → 唤醒 s_read_pid（slave read 返回 -EPIPE）

  master_refs == 0 && slave_refs == 0：
    → kfree(pty)
    → pty_table[index] = NULL
```

### sys_dup2 / sys_spawn

```
dup2(old_fd, new_fd):
  → FD_TTY: pty->master_refs++ 或 slave_refs++（取决于 old_fd 是 master 还是 slave）

spawn:
  → 继承 fd_table，每个 FD_TTY 的 refs++
```

Shell 的 fd 0/1/2 都是同一 slave fd（dup2 复制），slave_refs += 3。

## ioctl

### sys_ioctl(fd, request, arg)

当前 sys_ioctl 只支持 FD_DEV。扩展支持 FD_TTY：

```
FD_TTY ioctl:
  TCGETS (0x5401):  拷贝 pty->t_termios 到用户空间
  TCSETS (0x5402):  从用户空间拷贝 termios → pty->t_termios
  TCSETSW (0x5403): 同 TCSETS（输出排空后设置 — 简化实现，等同 TCSETS）
  TCSETSF (0x5404): 同 TCSETS + 清空 m_to_s_buf 和 s_to_m_buf
  TIOCGPGRP (0x540F): 返回 pty->t_pgid
  TIOCSPGRP (0x540E): 设置 pty->t_pgid（验证 pgid 属于 pty 的 session）
  TIOCSCTTY (0x540E): 设置进程的 ctty = pty（session leader 才能调用）
  TIOCGWINSZ (0x5413): 拷贝 pty->t_winsize 到用户空间
  TIOCSWINSZ (0x5414): 从用户空间拷贝 winsize → pty->t_winsize
                     → 如果大小变化，向 slave session 发 SIGWINCH
  TIOCGPTN:          返回 pty->index
  TIOCSPTLCK:        lock/unlock slave（暂不实现）

FD_DEV ioctl:  保持不变
其他 fd type:  返回 -ENOTTY
```

### Terminal 如何感知 termios 变化

Shell 调 TCSETS 设置 raw 模式后，Terminal 需要停止行编辑和 echo。Terminal 在每次从 master fd read 前调 TCGETS 获取当前 termios，根据 ICANON/ECHO/ISIG flag 调整 ldisc 行为。

```c
// Terminal 主循环
struct termios t;
ioctl(master_fd, TCGETS, &t);    // 每次循环开头检查

if (t.c_lflag & ICANON) {
    // canonical 模式：积累字符直到 Enter
} else {
    // raw 模式：每字符立即 write 到 master
}

if (t.c_lflag & ISIG) {
    // 检查 Ctrl-C → sys_kill(-pty_pgid, SIGINT)
} else {
    // Ctrl-C 作为普通字节写入 master
}

if (t.c_lflag & ECHO) {
    // 输入字符回显到屏幕
} else {
    // 不回显
}
```

## Line Discipline（用户态 Terminal）

### Canonical 模式（ICANON + ISIG + ECHO）

Terminal 的 ldisc 处理逻辑：

| 输入字符 | ISIG 开 | ECHO 开 | Terminal 行为 |
|----------|---------|---------|---------------|
| Ctrl-C (0x03) | 是 | — | sys_kill(-pgid, SIGINT)，不写入 master |
| Ctrl-Z (0x1A) | 是 | — | sys_kill(-pgid, SIGTSTP)，不写入 master |
| Ctrl-D (0x04) | — | — | 行缓冲有内容 → write 到 master 不带 \n；行缓冲空 → 不写（触发 slave read 返回 0? 需内核配合） |
| Backspace (0x7F) | — | ECHOE | 从行缓冲删末字节，屏幕 "^H" 回显 |
| Ctrl-U (0x15) | — | ECHOK | 清空行缓冲，屏幕回显 "^U\n" |
| Ctrl-W (0x17) | — | ECHOE | 从行缓冲删末 word |
| Enter (\n) | — | ECHO | write(master, line+"\n")，屏幕回显 |
| 普通字符 | — | ECHO | 追加到行缓冲，屏幕回显 |

### Raw 模式（~ICANON ~ECHO）

Terminal 每字符立即 write(master_fd, &ch, 1)。无行编辑、无 echo、Ctrl-C 作为普通字节（ISIG 关时不发信号）。

### 输出方向（slave→master）

slave write 的数据不做 ldisc 处理。Terminal 从 master read 后做 VT100 解析 + 渲染。OPOST+ONLCR（\n→\r\n）在内核 slave write 路径做。

## Session / Process Group（未来 job control）

本阶段预留 proc_t sid/pgid/ctty 字段，但 **setsid/setpgid 暂不实现**。

### 当前简化方案

Shell 启动后不调 setsid。Shell 和子进程的 pgid = Shell 的 pid（由 Terminal 在 spawn 时设置）。

Terminal 维护 foreground pgid（记录 Shell 的 pid），Ctrl-C 时 sys_kill(-shell_pid, SIGINT)。当 Shell spawn 子进程并把子进程放到前台时，Shell 通过 TIOCSPGRP 更新 pty->t_pgid，Terminal 下次 Ctrl-C 就 kill 新的 fg pgid。

### 未来完整 job control

需实现：sys_setsid、sys_setpgid、sys_getpgid、sys_getsid、pgsignal(pgid, sig)、sys_kill 扩展 pid<0。

## Terminal 进程改造

### 当前架构（两条 pipe）

```c
pipe(p_stdout);  // shell stdout → terminal 读
pipe(p_stdin);   // terminal 写 → shell stdin
dup2(p_stdin[0], 0);
dup2(p_stdout[1], 1);
spawn(shell);
dup2(p_stdout[0], 0);   // terminal 读 shell 输出
dup2(p_stdin[1], 1);     // terminal 写键盘输入
```

### 新架构（PTY pair）

```c
master_fd = open("/dev/ptmx");          // 内核创建 PTY 对 + /dev/pts0
slave_fd  = open("/dev/pts0");          // Terminal 拿到 slave fd

dup2(slave_fd, 0);
dup2(slave_fd, 1);
dup2(slave_fd, 2);                      // Shell 的 stdin/stdout/stderr 同一 slave
close(slave_fd);                         // Terminal 不再持有 slave（Shell 继承）

spawn(shell);                            // Shell 继承 fd 0/1/2 = slave
// Shell 启动后可选: setsid() + ioctl(0, TIOCSCTTY, 0)

close(0); close(1); close(2);            // Terminal 关闭继承的 slave fds

// Terminal 主循环:
struct termios t;
while (1) {
    ioctl(master_fd, TCGETS, &t);       // 检查 termios 变化

    // 键盘输入处理（ldisc）
    if (kbd_ring_has_data) {
        char ch = read_kbd();
        if ((t.c_lflag & ISIG) && ch == t.c_cc[VINTR]) {
            sys_kill(-pty_pgid, SIGINT); // Ctrl-C → SIGINT
        } else if (t.c_lflag & ICANON) {
            linebuf_push(ch);            // 行编辑
            if (ch == '\n' || ch == t.c_cc[VEOF]) {
                write(master_fd, linebuf, linebuf_len);
                linebuf_reset();
            }
        } else {
            write(master_fd, &ch, 1);    // raw 模式：立即写入
        }
    }

    // Shell 输出渲染
    int n = read(master_fd, buf, 256);   // Shell 输出 + echo
    if (n > 0) {
        vt100_feed(buf, n);              // VT100 解析
        display_client_flush();          // 渲染到屏幕
    } else if (n == 0) {
        // Shell 退出 → 重新 spawn
    }
}
```

## 初始化流程

```
kernel_main → sig_init() → devtmpfs_init() → 注册 /dev/ptmx (ptmx_ops)
→ 加载 init.elf → init spawn kbd_driver/terminal
→ terminal open("/dev/ptmx") → open("/dev/pts0") → dup2 slave → spawn shell
→ shell 运行
```

内核注册 /dev/ptmx 后不参与 PTY 创建决策 — Terminal 进程自行决定何时创建 PTY、spawn Shell。

## sys_kill 扩展（未来）

| pid 值 | 语义 |
|--------|------|
| pid > 0 | 发信号到指定进程 |
| pid == 0 | 发信号到同进程组 |
| pid == -1 | 发信号到所有进程（暂不实现，返回 -EPERM） |
| pid < -1 | 发信号到进程组 abs(pid) — pgsignal(abs(pid), sig) |

本阶段只实现 pid > 0。Terminal 用 sys_kill(shell_pid, SIGINT) 替代 kill(-pgid, SIGINT)。

## 新增/修改的文件

| 文件 | 变更 |
|------|------|
| `kernel/pty.h` | **新增** — struct pty, struct termios, struct winsize, PTY 常量 |
| `kernel/pty.c` | **新增** — ptmx_open, slave_open, pty read/write/close, TCGETS/TCSETS ioctl, pty_alloc |
| `kernel/devtmpfs.c` | 注册 /dev/ptmx (ptmx_ops)，支持 "ptsN" slave 设备节点动态创建/删除 |
| `kernel/proc.h` | FD_TTY=8, fd_table union 加 pty*, proc_t 加 sid/pgid/ctty（预留） |
| `kernel/trap.c` | sys_read/sys_write/sys_close/sys_dup2/sys_ioctl 新增 FD_TTY 分支, sys_poll FD_TTY 支持 |
| `kernel/proc.c` | proc_reap 新增 FD_TTY 分支 |
| `common/dev.h` | 新增 DEV_PTMX, DEV_PTS_SLAVE 设备类型 |
| `driver/terminal.cc` | 改为 PTY master holder，移除两条 pipe |
| `user/include/termios.h` | **新增** — struct termios, tcgetattr/tcsetattr/isatty |
| `user/include/sys/ioctl.h` | **新增** — ioctl() 声明 |

## 实现步骤

### Step 1: 内核 pty 数据结构 + /dev/ptmx + slave fd

- 新建 `kernel/pty.h` + `kernel/pty.c`
- 实现 struct pty 初始化、ring buffer 读写辅助函数
- 注册 /dev/ptmx 到 devtmpfs，实现 ptmx_open（创建 PTY 对 + /dev/ptsN slave 节点）
- FD_TTY 类型，sys_read/sys_write/sys_close/sys_dup2 FD_TTY 分支
- proc_t 加 sid/pgid/ctty 预留字段（初始化为 0）

**验证**: Terminal open("/dev/ptmx") + open("/dev/pts0") + spawn Shell → Shell 输出到屏幕

### Step 2: termios + ioctl

- TCGETS/TCSETS/TCSETSW/TCSETSF ioctl
- TIOCGWINSZ/TIOCSWINSZ
- TIOCGPTN (返回 PTY index)
- OPOST+ONLCR 在 slave write 路径

**验证**: Shell tcgetattr(0) + tcsetattr(0, raw) → Terminal 检查 termios → 切换到 raw 模式

### Step 3: Terminal 改造

- terminal.cc 改为 PTY master holder
- 移除两条 pipe + dup2 重排逻辑
- 添加 ldisc 逻辑：canonical/raw 模式切换、Ctrl-C→sys_kill
- 设置 TIOCSWINSZ

**验证**: Ctrl-C → Shell 子进程收到 SIGINT 退出；vi 类程序 raw 模式正常

### Step 4: libc 封装

- user/include/termios.h
- user/include/sys/ioctl.h
- tcgetattr/tcsetattr/isatty/ttyname

**验证**: 用户程序 isatty(0) 返回 1；tcgetattr 获取 termios 正确

### Step 5: Poll 扩展 + close 语义

- sys_poll FD_TTY 支持（POLLIN/POLLOUT/POLLHUP）
- master close → SIGHUP to slave session
- slave close → master read EOF

**验证**: Terminal poll(master_fd) 阻塞等待 Shell 输出；Shell 退出 → Terminal 检测到重新 spawn

### Step 6: session/pgid（未来）

- sys_setsid/sys_setpgid/sys_getpgid/sys_getsid
- pgsignal(pgid, sig)
- sys_kill 扩展 pid<0
- TIOCSCTTY/TIOCSPGRP/TIOCGPGRP

## 与其他系统的关系

| 系统 | TTY 位置 | 说明 |
|------|----------|------|
| Linux | 内核 | line discipline + termios + signal 全在内核 tty_struct |
| macOS | 内核 BSD 层 | 类似 Linux，XNU 的 BSD 部分 |
| MINIX 3 | 用户态 TTY server | 纯微内核，TTY 作为用户态服务 |
| 本系统 | 内核管道 + 用户态 ldisc | 折中：内核提供机制（PTY 管道 + termios 存储），用户态提供策略（line discipline） |

## 与 Wayland 的关系

Wayland compositor 运行 terminal emulator → open("/dev/ptmx") → 拿 master fd → 渲染终端窗口。完全复用 PTY 机制，无需额外修改。PTY 和 Wayland 是解耦的：

- PTY 解决"进程输出如何到达 terminal emulator 的输入"
- Wayland 解决"terminal emulator 如何把渲染结果送到屏幕"

## 未来扩展

| 项目 | 说明 | 优先级 |
|------|------|--------|
| /dev/pts/N 子目录 | devtmpfs 加目录层级支持，路径从 /dev/pts0 改为 /dev/pts/0 | 中 |
| session/pgid/controlling terminal | setsid/TIOCSCTTY/TIOCSPGRP | 中 |
| sys_kill pid<0 | pgsignal(pgid, sig) | 中 |
| SIGWINCH | 窗口大小变化投递 | 中 |
| pgsignal | 按 pgid 遍历 procs[] 投递信号 | 中 |
| Serial console tty | COM1 作为 console tty | 低 |
| 多终端 | 多个 PTY 对，多个 Terminal session | 低 |
