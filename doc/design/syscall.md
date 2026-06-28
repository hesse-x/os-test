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

NR_SYSCALL=59，syscall_table[] 定义在 kernel/trap.cc。

参数从 trapframe 提取：RDI/RSI/RDX/R10/R8/R9（6 参），返回值写回 tf->rax。超出范围返回 ENOSYS。

| 编号 | 名称 | 说明 |
|------|------|------|
| 0 | sys_getpid | 获取 PID |
| 1 | sys_yield | 主动让出 CPU |
| 2 | sys_recv | 统一事件接收（IRQ/REQ/NOTIFY/MSG） |
| 3 | sys_req | 同步 REQ（≤56B 内联载荷） |
| 4 | sys_resp | 回复当前 REQ 调用者 |
| 5 | sys_irq_bind | 绑定当前进程到指定 IRQ |
| 6 | sys_exit | 进程退出 |
| 7 | sys_waitpid | 等待子进程退出 |
| 8 | — | slot 已空（原 sys_spawn，已改用 fork+execve） |
| 9 | sys_mmap | 内存映射（6 参） |
| 10 | sys_munmap | 解除内存映射 |
| 11 | sys_shm_create | 创建 SHM fd |
| 12 | sys_shm_attach | 附加 SHM |
| 13 | sys_pipe | 创建 pipe |
| 14 | sys_write | 写 fd |
| 15 | sys_read | 读 fd |
| 16 | sys_close | 关闭 fd |
| 17 | sys_notify | 异步通知 |
| 18 | sys_gettime | 全局单调时钟（纳秒） |
| 19 | sys_clock | per-process CPU 时间 |
| 20 | sys_msg | 变长消息请求 |
| 21 | sys_msg_resp | 变长消息回复 |
| 22 | sys_ioperm | I/O 端口权限 |
| 23 | sys_dup2 | 复制 fd |
| 24 | sys_fcntl | 文件控制 |
| 25 | sys_dma_alloc | 物理连续 DMA 分配 |
| 26 | sys_dma_free | 释放 DMA 缓冲区 |
| 27 | sys_pci_dev_info | PCI 设备查询 |
| 28 | sys_block_async | 异步块 I/O |
| 29 | sys_install_fd | 注册 FD_FILE fd |
| 30 | sys_socket | socket |
| 31 | sys_bind | bind |
| 32 | sys_listen | listen |
| 33 | sys_accept | accept |
| 34 | sys_connect | connect |
| 35 | sys_socketpair | socketpair |
| 36 | sys_sendmsg | sendmsg |
| 37 | sys_recvmsg | recvmsg |
| 38 | sys_shutdown | shutdown |
| 39 | sys_poll | poll |
| 40 | sys_lseek | 文件偏移设置 |
| 41 | sys_memfd_create | 创建 memfd |
| 42 | sys_ftruncate | 截断文件 |
| 43 | sys_kill | 发送信号 |
| 44 | sys_sigaction | 注册信号 handler |
| 45 | sys_sigreturn | 信号返回 |
| 46 | sys_debug_print | 内核调试打印 |
| 47 | sys_open | 打开文件 |
| 48 | sys_stat | 获取文件状态 |
| 49 | sys_mkdir | 创建目录 |
| 50 | sys_unlink | 删除文件 |
| 51 | sys_rmdir | 删除目录 |
| 52 | sys_dev_create | 创建设备节点 |
| 53 | sys_getdents | 读取目录项 |
| 54 | sys_ioctl | 设备控制 |
| 55 | sys_fstat | 基于 fd 获取文件状态 |
| 56 | sys_fdev_pid | 获取设备驱动 PID |
| 57 | sys_fork | fork |
| 58 | sys_execve | execve |

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
- 值返回型：sys_mmap 返回地址，sys_fork 返回子 PID（父）/0（子），sys_waitpid 返回 pid

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
| clone | 线程创建（CLONE_VM 等），需完整 pthread 支持 | 中 |
| futex | 线程同步原语，pthread mutex 底层 | 中 |
| arch_prctl | 线程本地存储（FS/GS base 管理） | 中 |
| set_tid_address / gettid | 线程 ID 管理 | 低 |
| tgkill | 线程级信号发送 | 低 |
| exit_group | 进程组退出 | 低 |
