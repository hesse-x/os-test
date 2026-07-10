# 系统调用

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 系统调用指令 | SYSCALL/SYSRET（MSR LSTAR） | x86-64 标准快速系统调用，比 int 0x80 快，不经过 IDT |
| 2 | syscall 与 trap 独立 | syscall_fast_entry 不走 __alltraps | 职责分离，SYSCALL 不自动 push SS/RSP/RFLAGS |
| 3 | trapframe 统一 | 手动构建与 __alltraps 相同布局的 trapframe | syscall_dispatch 共用 trapframe_t* 参数 |
| 4 | 用户栈保存 | SYSCALL 前 movq %rsp, %r10 | SYSCALL 不自动保存用户 RSP，R10 是 scratch 寄存器 |
| 5 | 内核栈切换 | swapgs → %gs:32 读 tss_rsp0 | per-CPU 安全，schedule() 更新 |
| 6 | swapgs 策略 | syscall 入口无条件 swapgs，出口无条件 swapgs | SYSCALL 只从 ring 3 来，无条件安全 |
| 7 | STAR 设置 | [47:32]=0x08, [63:48]=0x18 | SYSCALL CS=0x08/SS=0x10; SYSRET64 CS=0x2B/SS=0x23 |
| 8 | SFMASK | IF(bit 9) | SYSCALL 进入后 IF=0，与 interrupt gate 行为一致 |
| 9 | 调用约定 | RAX=syscall#, RDI/RSI/RDX/R10/R8/R9=args | Linux x86-64 syscall 约定 |
| 10 | 阻塞 syscall 返回 | SYSRET 路径也适用于被 reschedule 的进程 | switch_to 保存完整调用链，恢复后自然回到 SYSRET |

### SYSCALL/SYSRET 机制

syscall_fast_entry（arch/x64/trapentry.S）：
- SYSCALL 自动做：RCX=RIP, R11=RFLAGS，不 push 任何内容
- 保存用户 RSP 到 R10 → swapgs → %gs:32 读内核栈 → subq $176 构建 trapframe → 保存 CPU 自动字段（ss/rsp/rflags/cs/rip/err_code/trapno）→ 保存 16 个 GP 寄存器 → 设 DS/ES → call syscall_dispatch

SYSRET 返回（arch/x64/trapentry.S）：
- 恢复 GP 寄存器 → rcx→用户RIP, r11→用户RFLAGS, rsp→用户RSP → swapgs → sysretq

MSR 设置：kernel/trap.cc : setup_syscall() — STAR/LSTAR/CSTAR/SFMASK/EFER.SCE

### syscall_dispatch 与系统调用表

NR_SYSCALL=95，syscall_table[] 定义在 kernel/xcore/trap.c（Xcore 层，slot 0-19）+ kernel/bsd/syscall.c（BSD 层，slot 20-94）。

参数从 trapframe 提取：RDI/RSI/RDX/R10/R8/R9（6 参），返回值写回 tf->rax。超出范围返回 ENOSYS。

| 编号 | 名称 | 说明 |
|------|------|------|
| 0 | sys_getpid | 获取 PID（返回 tgid） |
| 1 | sys_yield | 主动让出 CPU |
| 2 | sys_recv | 统一事件接收（IRQ/REQ/NOTIFY/MSG） |
| 3 | sys_req | 同步 REQ（≤56B 内联载荷） |
| 4 | sys_resp | 回复当前 REQ 调用者 |
| 5 | sys_irq_bind | 绑定当前进程到指定 IRQ |
| 6 | sys_exit | 进程退出 |
| 7 | sys_waitpid | 等待子进程退出 |
| 8 | sys_mmap | 内存映射（6 参：匿名/SHM/设备/物理） |
| 9 | sys_munmap | 解除内存映射 |
| 10 | sys_pipe | 创建 pipe |
| 11 | sys_write | 写 fd |
| 12 | sys_read | 读 fd |
| 13 | sys_close | 关闭 fd |
| 14 | sys_notify | 异步通知 |
| 15 | sys_gettime | 全局单调时钟（纳秒） |
| 16 | sys_clock | per-process CPU 时间 |
| 17 | sys_msg | 变长消息请求 |
| 18 | sys_msg_resp | 变长消息回复 |
| 19 | sys_ioperm | I/O 端口权限 |
| 20 | sys_dup2 | 复制 fd |
| 21 | sys_fcntl | 文件控制（F_GETFL/F_SETFL/F_DUPFD/F_DUPFD_CLOEXEC） |
| 22 | sys_dma_alloc | 物理连续 DMA 分配 |
| 23 | sys_dma_free | 释放 DMA 缓冲区 |
| 24 | sys_pci_dev_info | PCI 设备查询 |
| 25 | sys_block_async | 异步块 I/O |
| 26 | sys_install_fd | 注册 FD_FILE fd |
| 27-36 | socket 系列 | socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown/poll（AF_UNIX + AF_NETLINK，netlink 详见 [netlink.md](netlink.md)） |
| 37 | sys_lseek | 文件偏移设置 |
| 38 | sys_memfd_create | 创建 memfd |
| 39 | sys_ftruncate | 截断文件（按 fd） |
| 40-42 | 信号基础 | sys_kill/sys_sigaction/sys_sigreturn |
| 43 | sys_debug_memstat | 内核内存统计 |
| 44-53 | VFS 系列 | sys_open/stat/mkdir/unlink/rmdir/dev_create/getdents/ioctl/fstat/fdev_pid |
| 54-59 | 进程管理 | sys_fork/execve/setsid/setpgid/getpgid/getsid |
| 60-68 | 线程/信号扩展 | sys_clone/futex/arch_prctl/tgkill/exit_group/set_tid_address/gettid/sigprocmask/pthread_set_cancel_handler（详见 [thread.md](thread.md)） |
| 69-79 | POSIX 身份与权限 | sys_getuid/euid/gid/egid/setuid/setgid/getppid/getpgrp/umask/gethostname/sethostname |
| 80-81 | alarm/pause | sys_alarm（定时 SIGALRM）/sys_pause（可中断睡眠） |
| 82-84 | 文件系统扩展 | sys_truncate（路径截断）/sys_fsync（单 inode 脏页写回）/sys_sync（全局脏页写回） |
| 85 | sys_sigpending | 返回未决信号集（per-task ∪ shared，不滤 blocked） |
| 86-94 | epoll + 事件 fd | epoll_create/create1/ctl/wait/pwait + eventfd2 + timerfd_create/settime + signalfd4（详见 [epoll.md](epoll.md)） |

### recv_msg 类型

common/syscall.h 定义：

| type | 名称 | src | data |
|------|------|-----|------|
| 0 | RECV_IRQ | IRQ 号 | 56 字节 |
| 1 | RECV_REQ | 发送者 PID | 56 字节请求载荷 |
| 2 | RECV_NOTIFY | 发送者 PID | 56 字节 |
| 3 | RECV_MSG | 发送者 PID | kmaddr + len（内核缓冲区指针） |

sys_recv 签名：sys_recv(buf, data_buf, data_buf_len, timeout_ms)，timeout_ms=0 无限等待。

### 返回值约定

- 状态型 syscall：0=成功，负数=errno
- 值返回型：sys_mmap 返回地址，sys_fork 返回子 PID（父）/0（子），sys_waitpid 返回 pid，sys_memfd_create 返回 fd

### 用户态封装

arch/x64/utils.h — __syscall0 至 __syscall6（内联汇编）。语义封装在 common/syscall.h。

### 与 __alltraps/__trapret 的差异

| 方面 | __alltraps | syscall_fast_entry |
|------|-----------|-------------------|
| 触发 | 硬件中断/异常 → IDT | SYSCALL 指令 → MSR LSTAR |
| CPU 自动保存 | SS/RSP/RFLAGS/CS/RIP/[err] | RCX=RIP, R11=RFLAGS（无栈操作） |
| 栈切换 | CPU 自动切 TSS RSP0 | 手动 swapgs + %gs:32 |
| swapgs | 条件性（检查 CS） | 无条件 |
| 返回 | iretq | sysretq |

### IPC 机制概览

详见 [ipc.md](ipc.md)。

| 层 | syscall | 用途 |
|----|---------|------|
| 控制信令 | req/resp | 短请求/回复 |
| 数据传输 | msg/msg_resp | ≤64KB 变长 |
| 双向字节流 | socket/sendmsg/recvmsg | AF_UNIX SOCK_STREAM + SCM_RIGHTS |
| 事件多路复用 | poll | 统一监听 pipe/socket/dev |
| 匿名管道 | pipe/write/read/close | stdin/stdout 单向 |

### 关键源码位置

- syscall 入口：arch/x64/trapentry.S : syscall_fast_entry
- syscall 分发：kernel/trap.cc : syscall_dispatch / syscall_table
- MSR 设置：kernel/trap.cc : setup_syscall
- trapframe 定义：arch/x64/trap.h : trapframe_t
- 用户态封装：arch/x64/utils.h : __syscall0-6
- syscall 编号和语义：common/syscall.h / common/syscall_nums.h

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| sigsuspend | 原子换掩码并阻塞到信号，详见 [thread.md](thread.md) POSIX 信号扩展 | 中 |
| sigwait / sigwaitinfo / sigtimedwait | 消费但不投递 handler + siginfo 返回，详见 [thread.md](thread.md) | 中 |
| sigaltstack | 信号备用栈，详见 [thread.md](thread.md) | 低 |
