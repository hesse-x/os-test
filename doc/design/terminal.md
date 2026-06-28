# Terminal / PTY

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | PTY 策略位置 | 用户态 Terminal（ldisc） | 微内核原则：内核只提供机制（字节管道），策略（行编辑/信号映射）在用户态 |
| 2 | PTY 数据结构 | 内核 struct pty（双向 ring buffer + termios + winsize） | termios 是进程间共享状态（Shell 设置 raw → Terminal 检查），必须内核存储 |
| 3 | master/slave 区分 | 不在 fd 里区分，由 ref_count（master_refs/slave_refs）隐式表达 | Terminal 持 master，Shell 持 slave，不需要 fd 层面的 master/slave 标记 |
| 4 | 数据流路径 | 键盘→Terminal(ldisc)→master write→m_to_s_buf→slave read→Shell | Ctrl-C 等 ISIG 处理在 Terminal 用户态完成，不进入 PTY 管道 |
| 5 | Shell fd 0/1/2 | 同一 PTY slave fd（dup2 复制） | PTY 是双向管道，stdin/stdout 指向同一 slave 符合 TTY 语义 |
| 6 | OPOST+ONLCR | 内核 slave write 路径做 '\n'→'\r\n' | 简单输出处理开销极小，内核做比 Terminal 做 latency 更低 |
| 7 | 进程树 | init→fork(kbd_driver)→fork(terminal)→fork(shell) | terminal 是 PTY master holder + ldisc 处理器；init 不分配 PTY |
| 8 | master close 语义 | 发 SIGHUP 到 slave session 前台 pgid | 与 Linux 一致：terminal 关闭时 Shell 收到 SIGHUP |
| 9 | slave close 语义 | 清空缓冲，master read 返回 EOF | Shell 退出后 Terminal 检测到重新 fork |
| 10 | 串口驱动位置 | 内核态（driver_pid=0） | 串口在内核初始化早期就需要用于 debug 输出；16550 UART 驱动足够简单 |
| 11 | 串口角色 | 双重：内核 debug 输出 + 用户交互 I/O | 内核 serial_printf 直接调用 serial_putc，用户进程通过 open("/dev/serial") 使用 FD_DEV |

### 数据流

```
VGA 路径：
键盘 → kbd_driver SHM → Terminal 用户态 line discipline 处理：
  · ISIG 开: Ctrl-C → sys_kill(-pgid, SIGINT)，不写入 master
  · ISIG 开: Ctrl-Z → sys_kill(-pgid, SIGTSTP)，不写入 master
  · ICANON 开: 积累字符直到 Enter → write(master, line)
  · ICANON 关: 每字符立即 write(master, ch)
  → write(master_fd) → pty.m_to_s_buf → slave sys_read(slave_fd)

Shell sys_write(slave_fd) → pty.s_to_m_buf → master sys_read(master_fd)
  → Terminal VT100 解析 → cell buffer → KMS 渲染

串口路径（debug 输出，当前无 PTY 包装）：
内核 serial_printf → serial_putc → COM1 TX → QEMU stdout
用户 read("/dev/serial") → serial_irq_handler → ring buf → serial_dev_read
用户 write("/dev/serial") → serial_dev_write → serial_putc → COM1 TX
```

### 内核：PTY 机制

#### struct pty（kernel/pty.h : struct pty）

  m_to_s_buf[4096] : uint8_t — master→slave ring buffer
  m_to_s_head / m_to_s_tail : uint32_t — 写/读位置
  s_to_m_buf[4096] : uint8_t — slave→master ring buffer
  s_to_m_head / s_to_m_tail : uint32_t — 写/读位置
  m_read_pid / m_write_pid / s_read_pid / s_write_pid : pid_t — 阻塞等待进程（-1=无）
  t_termios : struct termios — 内核存储，Terminal 通过 TCGETS 读取调整 ldisc
  t_winsize : struct winsize — 窗口大小（row/col/xpixel/ypixel）
  index : int — PTY 编号（对应 /dev/ptsN）
  master_refs / slave_refs : int — master/slave fd 引用数
  slave_opened : int — slave 端是否已被打开
  t_sid : pid_t — session ID（0=无 session）
  t_pgid : pid_t — 前台进程组 ID
  eof_pending : int — Ctrl-D 标志：master write len=0 → slave read 返回 0
  pts_priv : pts_dev_priv* — slave 设备私有数据（cleanup 用）

#### struct termios / struct winsize（kernel/pty.h）

termios 与 Linux x86-64 ABI 对齐：c_iflag/c_oflag/c_cflag/c_lflag + c_cc[19]。默认值：ICRNL|IXON / OPOST|ONLCR / CS8|CLOCAL / ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN。c_cc[VINTR]=^C, [VERASE]=DEL, [VKILL]=^U, [VEOF]=^D, [VSUSP]=^Z。

winsize 与 Linux ABI 对齐：ws_row/ws_col/ws_xpixel/ws_ypixel。

#### PTY 全局表

`pty_table[MAX_PTY=16]` + `pty_alloc_lock` — 最多 16 个 PTY 对。devtmpfs MAX_DEV_ENTRIES=32 预留一半给其他设备。

#### /dev/ptmx 创建流程

1. open("/dev/ptmx") → ptmx_open 回调
2. pty_alloc 分配 index，kmalloc(struct pty)，初始化 ring buffers + default_termios
3. devtmpfs 注册 "ptsN" slave 设备节点
4. 修改 fd_table[fd]：type=FD_TTY, pty=new_pty, master_refs=1

#### slave 打开

open("/dev/ptsN") → devtmpfs_lookup → pts_open 回调 → fd_table[fd] type=FD_TTY, slave_refs++, slave_opened=1

#### PTY fd 操作

master read：从 s_to_m_buf 读，空则阻塞 m_read_pid on WAIT_PIPE；slave_refs==0 且缓冲空返回 EOF
master write：写入 m_to_s_buf，唤醒 s_read_pid；写入 len=0 设 eof_pending
slave read：从 m_to_s_buf 读，空则阻塞 s_read_pid on WAIT_PIPE；eof_pending 时返回 0
slave write：写入 s_to_m_buf（OPOST+ONLCR 处理），唤醒 m_read_pid

close：master_refs-- 或 slave_refs--；slave_refs==0 时清 m_to_s_buf 剩余数据 + 唤醒 m_read_pid(EOF) + 删除 devtmpfs "ptsN"；master_refs==0 时向 slave session 发 SIGHUP + slave write 返回 -EPIPE；两者归零则 kfree(pty)

dup2：对应 refs++（master_refs 或 slave_refs）

#### ioctl 命令（kernel/pty.c : pty_ioctl）

| 命令 | 编号 | 行为 |
|------|------|------|
| TCGETS | 0x5401 | 拷贝 pty->t_termios 到用户空间 |
| TCSETS | 0x5402 | 拷贝用户 termios 到 pty->t_termios |
| TCSETSW | 0x5403 | 同 TCSETS（简化实现） |
| TCSETSF | 0x5404 | 同 TCSETS + 清空双向 ring buffer |
| TIOCGPGRP | 0x540F | 返回 pty->t_pgid |
| TIOCSPGRP | 0x5410 | 设置 pty->t_pgid |
| TIOCSCTTY | 0x540E | 设置进程 ctty=pty（session leader 才能调用） |
| TIOCGWINSZ | 0x5413 | 拷贝 pty->t_winsize 到用户空间 |
| TIOCSWINSZ | 0x5414 | 拷贝用户 winsize → pty->t_winsize，大小变化则向 slave session 发 SIGWINCH |
| TIOCGPTN | 0x5406 | 返回 pty->index |
| TIOCSPTLCK | 0x5407 | stub，返回 0 |

#### pty_poll（kernel/pty.c : pty_poll）

接入 sys_poll FD_TTY 分支，支持 POLLIN/POLLOUT/POLLHUP 事件。

### 内核串口驱动

16550 UART（COM1 0x3F8, 115200 baud），注册为 `/dev/serial`（FD_DEV + dev_ops 回调），对齐 Linux serial8250 模型。

#### TX 路径

serial_putc（arch/x64/utils.h 门控 NSERIAL）：spin_lock_irqsave(serial_tx_lock) → LSR 等待 THR 空 → outb(COM1, c) → unlock。内核 debug 输出直接调用 serial_printf→serial_puts→serial_putc，不经过 fd。

#### RX 路径

serial_irq_handler（IRQ vector 36，GSI 4 edge-triggered）：spin_lock_irqsave(serial_rx_lock) → while(LSR_DR) drain FIFO → 写入 serial_rx_buf ring（256B）→ wake serial_read_waiter → unlock。

serial_dev_read（FD_DEV ops.read）：spin_lock_irqsave(serial_rx_lock) → ring 空 → 设 serial_read_waiter + BLOCKED + WAIT_PIPE → schedule → 有数据则拷贝到用户 buf。

#### /dev/serial 生命周期

- open：serial_fd_count++，首次 open 时 register_irq(36) + I/O APIC 路由 + IER RX enable
- close：serial_fd_count--，清除 serial_read_waiter，最后一次 close 时 mask IRQ + unregister
- dup2：serial_fd_count++（dev_ops 不单独处理，sys_dup2 统一走 inode ref bump）
- poll：serial_dev_poll — POLLIN=ring 有数据，POLLOUT=always ready
- ioctl：TCGETS 返回 0（serial 是 tty）

#### 锁协议

| 锁 | 保护范围 | 获取者 |
|---|---|---|
| serial_tx_lock | UART THR 写入（outb COM1） | serial_putc（内核 printf + sys_write FD_DEV serial） |
| serial_rx_lock | RX ring buffer + serial_read_waiter + serial_fd_count | ISR + sys_read/close/poll |

锁不与 scheduler_lock 嵌套（serial_rx_lock 在 schedule 前释放）。serial_tx_lock 不与任何锁嵌套。

关键源码位置：kernel/serial.c / kernel/serial.h

### 内核：fd / session 支持

FD_TTY=8 — fd_table type，master 和 slave 都用此类型。file_t union 含 `pty*`。

task_t 新增 sid/pgid/ctty 字段：fork 时拷贝，setsid 时重设。

sys_kill 扩展：pid>0 发指定进程；pid==0 发同 pgid（pgsignal）；pid<-1 发 abs(pid) pgid；pid==-1 返回 -EPERM。

### 用户态：Terminal 进程（driver/terminal.cc）

Terminal 是 PTY master holder + VT100 渲染器 + ldisc 处理器。

初始化流程：
1. attach kbd_driver SHM（读键盘输入）
2. open("/dev/ptmx") → 创建 PTY 对，获取 master_fd
3. TIOCGPTN 获取 pty_idx，构造 pts_path "/dev/ptsN"
4. fork → 子进程 open slave + dup2 0/1/2 + execve("/usr/bin/shell")
5. 父进程 fcntl(master_fd, F_SETFL, O_NONBLOCK)
6. TIOCSWINSZ 设置真实窗口大小

主循环：
1. TCGETS 检查 termios 变化
2. 读 kbd_ring → ldisc 处理 → master write / sys_kill(-pgid, SIGINT)
3. master read → VT100 解析 → cell buffer → KMS 渲染 → serial echo
4. Shell 退出（master read 返回 0）→ close master → 重新 open ptmx → fork shell

ldisc 处理：
- ISIG 开 + Ctrl-C → sys_kill(-pgid, SIGINT)
- ISIG 开 + Ctrl-Z → sys_kill(-pgid, SIGTSTP)
- ICANON 开 → 行编辑（Backspace 删末字节、Ctrl-U 清行、Enter 提交）
- ICANON 关 → 每字符立即 write(master, ch)
- ECHO 开 → 输入字符回显到 VT100
- Ctrl-D → 行缓冲有内容写 master 不带 \n；空缓冲 write len=0 触发 EOF

VT100 状态机（vt100_feed）：NORMAL → ESC → CSI → 参数累积 → 派发指令。支持光标定位、清屏、清行、颜色设置、重置属性。

### 用户态：Shell（shell/shell.cc）

Shell 启动后 setsid() + ioctl(0, TIOCSCTTY, 0)，成为 session leader 并设置控制终端。

命令：ls/cat/cd/pwd/touch/mkdir/rmdir/unlink/help + 路径执行。

路径执行：未匹配内置命令时 spawn(abs_path) + waitpid。

TEST 模式：fork → execve test_runner.elf + waitpid。

### 进程树与 fd 分配

```
init (PID 2, fd 0/1/2 = /dev/serial)
  ├── fork → kbd_driver (fd 0/1/2 = /dev/serial, 继承)
  ├── fork → terminal (fd 0/1/2 = /dev/serial, 继承; master_fd = /dev/ptmx)
  │     ├── fork → shell (fd 0/1/2 = /dev/ptsN slave)
  │     │     ├── spawn → user_program (fd 0/1/2 继承 slave)
  │     │     └── fork → test_runner (fd 0/1/2 继承 slave, TEST 模式)
```

init 调 waitpid(-1) 兜底回收所有孤儿子进程。

### 与其他模块的关系

- 进程管理：task_t.sid/pgid/ctty 用于 session/job control，详见 [proc.md](proc.md)
- IPC：sys_kill(-pgid, SIGINT) 使用 pgsignal，详见 [ipc.md](ipc.md)
- VFS：/dev/ptmx 和 /dev/ptsN 注册在 devtmpfs，详见 [vfs.md](vfs.md)
- KMS：Terminal 通过 KMS req FLIP 渲染 cell buffer，详见 [kms.md](kms.md)
- 调度：pty read/write 阻塞 on WAIT_PIPE，详见 [schedule.md](schedule.md)

### 系统调用

| 编号 | syscall | 签名 | 行为 |
|------|---------|------|------|
| 13 | sys_pipe | `sys_pipe(int *fd_ptr)` | 创建 pipe（匿名单向，非 PTY） |
| 14 | sys_write | `sys_write(int fd, const void *buf, size_t len)` | FD_TTY: pty master/slave write |
| 15 | sys_read | `sys_read(int fd, void *buf, size_t len)` | FD_TTY: pty master/slave read |
| 16 | sys_close | `sys_close(int fd)` | FD_TTY: refs--, 归零发 SIGHUP/EPIPE |
| 23 | sys_dup2 | `sys_dup2(int old, int new)` | FD_TTY: refs++ |
| 24 | sys_fcntl | `sys_fcntl(int fd, int cmd, ...)` | F_SETFL 支持 O_NONBLOCK |
| 39 | sys_poll | `sys_poll(...)` | FD_TTY: pty_poll 接入 |
| 54 | sys_ioctl | `sys_ioctl(int fd, uint32_t cmd, ...)` | FD_TTY: pty_ioctl 处理 |
| 43 | sys_kill | `sys_kill(pid_t pid, int sig)` | pid<0: pgsignal; pid==0: 同 pgid |
| 59 | sys_setsid | `sys_setsid()` | 设置 sid=pid, pgid=pid |
| 60 | sys_setpgid | `sys_setpgid(pid_t pid, pid_t pgid)` | 设置进程组 |
| 61 | sys_getpgid | `sys_getpgid(pid_t pid)` | 获取进程组 |
| 62 | sys_getsid | `sys_getsid(pid_t pid)` | 获取 session |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| /dev/pts/N 子目录 | devtmpfs 加目录层级支持，路径从 /dev/pts0 改为 /dev/pts/0 | 低 |
| 多终端会话 | 多个 PTY 对 + 多个 Terminal session（Ctrl+Fn 切换） | 低 |
| Serial console PTY | 当前 /dev/serial 是裸 FD_DEV（无 ldisc），需将串口纳入 PTY 模型：Terminal ldisc 处理 serial 输入，serial shell 运行在 PTY slave 上，与 VGA shell 并行（对齐 Linux agetty ttyS0） | 低 |
| TIOCSPGRP pgid 验证 | 当前未验证 pgid 属于 pty 的 session | 中 |
| job control（SIGTSTP/bg/fg） | Shell 支持 Ctrl-Z 挂起、bg/fg 命令恢复 | 中 |
