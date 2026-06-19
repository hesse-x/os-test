# 系统调用

## 概述

使用 SYSCALL/SYSRET 指令（MSR LSTAR）替代 int 0x80 进行系统调用，与 `__alltraps`/`__trapret` 独立。当前 NR_SYSCALL=47（编号 0-46 连续无空洞）。int 0x80 路径已移除。

## SYSCALL/SYSRET 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 系统调用指令 | SYSCALL/SYSRET（MSR） | x86-64 标准快速系统调用，不经过 IDT，无需 push 段寄存器，比 int 0x80 快 |
| 2 | syscall 与 trap 独立 | `syscall_fast_entry` 不走 `__alltraps` | 职责分离，SYSCALL 不自动 push SS/RSP/RFLAGS，需要手动构建 trapframe |
| 3 | trapframe 统一 | 手动构建与 `__alltraps` 相同布局的 trapframe | `syscall_dispatch` 共用 trapframe_t* 参数，无需额外分发表 |
| 4 | 用户栈保存 | SYSCALL 前先 movq %rsp, %r10 | SYSCALL 不自动保存用户 RSP，R10 是 scratch 寄存器（不保留） |
| 5 | 内核栈切换 | `swapgs` → `%gs:32` 读 `tss_rsp0` | tss_rsp0 由 `schedule()` 更新为当前进程内核栈顶，per-CPU 安全 |
| 6 | swapgs 策略 | syscall 入口无条件 swapgs，出口无条件 swapgs | SYSCALL 只从用户态进入（EFER.SCE 只允许 ring 3 触发） |
| 7 | STAR 设置 | `[47:32]=0x08, [63:48]=0x18` | SYSCALL: CS=0x08, SS=0x10; SYSRET64: CS=0x2B, SS=0x23 |
| 8 | SFMASK | IF(bit 9) | SYSCALL 进入后 IF=0，中断自动关闭，与 interrupt gate 行为一致 |
| 9 | 阻塞 syscall 返回 | SYSRET 路径也适用于被 reschedule 的进程 | `schedule()` → `switch_to` 保存完整调用链，恢复后自然回到 SYSRET 部分 |
| 10 | 调用约定 | RAX=syscall#, RDI/RSI/RDX/R10/R8/R9 = args | Linux x86-64 syscall 约定；R10 替代 RCX（RCX 被 SYSCALL 用作 return RIP） |

## MSR 设置（`setup_syscall()`，kernel/trap.cc）

```c
// STAR: SYSCALL CS=0x08/SS=0x10, SYSRET64 CS=0x2B/SS=0x23
uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x18 << 48);
wrmsr(MSR_STAR, star);

// LSTAR: SYSCALL 入口地址
wrmsr(MSR_LSTAR, (uint64_t)syscall_fast_entry);

// CSTAR: 32 位兼容入口（未使用，设为 0）
wrmsr(MSR_CSTAR, 0);

// SFMASK: 清除 IF(bit 9)
wrmsr(MSR_SFMASK, (1 << 9));

// EFER.SCE: 启用 SYSCALL/SYSRET
uint64_t efer = rdmsr(MSR_EFER);
efer |= EFER_SCE;
wrmsr(MSR_EFER, efer);
```

## syscall_fast_entry（arch/x64/trapentry.S）

SYSCALL 指令自动做：RCX = RIP, R11 = RFLAGS，然后加载 CS/SS（来自 STAR）。不自动 push 任何内容到栈、不自动切换栈。

```asm
syscall_fast_entry:
    movq %rsp, %r10           # 保存用户 RSP（R10 是 scratch）
    swapgs                    # GS_BASE ↔ KERNEL_GS_BASE → GS_BASE = cpu_local*
    movq %gs:32, %rsp         # 切内核栈（cpu_local.tss_rsp0，偏移 32）

    subq $176, %rsp           # 在内核栈构建 trapframe（176 bytes）

    # CPU 自动保存的字段
    movq $0x23, 168(%rsp)     # ss = USER_DS
    movq %r10, 160(%rsp)      # rsp = 用户 RSP
    movq %r11, 152(%rsp)      # rflags（SYSCALL 保存到 R11）
    movq $0x2B, 144(%rsp)     # cs = USER_CS
    movq %rcx, 136(%rsp)      # rip（SYSCALL 保存到 RCX）
    movq $0, 128(%rsp)        # err_code = 0
    movq $128, 120(%rsp)      # trapno = 128

    # 保存所有 16 个 GP 寄存器
    movq %r15, 0(%rsp) ... movq %rax, 112(%rsp)

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    movq %rsp, %rdi
    call syscall_dispatch
```

## SYSRET 返回

阻塞 syscall 也走此路径：被 reschedule 后恢复时，switch_to 返回到 syscall_dispatch 调用之后，继续执行 SYSRET 恢复逻辑。

```asm
    # 恢复 GP 寄存器（除 RCX/R11，它们用于 SYSRET）
    movq 112(%rsp), %rax ... movq 0(%rsp), %r15

    movq 136(%rsp), %rcx      # 用户 RIP（SYSRET 加载 RCX → RIP）
    movq 152(%rsp), %r11      # 用户 RFLAGS（SYSRET 加载 R11 → RFLAGS）
    movq 160(%rsp), %rsp      # 用户 RSP（必须在 swapgs 之前加载）

    swapgs                    # 换回用户 GS base
    sysretq                   # RCX→RIP, R11→RFLAGS, CS/SS 从 STAR 加载
```

## 与 __alltraps/__trapret 的差异

| 方面 | __alltraps/__trapret | syscall_fast_entry/SYSRET |
|------|---------------------|--------------------------|
| 触发方式 | 硬件中断/异常 → IDT | SYSCALL 指令 → MSR LSTAR |
| CPU 自动保存 | SS, RSP, RFLAGS, CS, RIP, [err] | RCX=RIP, R11=RFLAGS（无栈操作） |
| 栈切换 | CPU 自动切到 TSS RSP0 | 手动 swapgs + %gs:32 读 tss_rsp0 |
| swapgs | 条件性（检查 CS 判断来源） | 无条件（SYSCALL 只从 ring 3 来） |
| trapframe 构建 | push 寄存器到栈 | subq + movq 写入固定偏移 |
| 返回 | iretq（自动 pop SS/RSP/RFLAGS/CS/RIP） | sysretq（RCX→RIP, R11→RFLAGS, CS/SS 从 STAR） |

## syscall_dispatch 与系统调用表

```c
#define NR_SYSCALL 47
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_getpid,         // 0:  获取 PID
    sys_yield,          // 1:  主动让出 CPU
    sys_recv,           // 2:  统一事件接收（IRQ/REQ/NOTIFY/MSG）
    sys_req,            // 3:  同步 REQ（56 字节内联载荷）
    sys_resp,           // 4:  回复当前 REQ 调用者
    sys_irq_bind,       // 5:  绑定当前进程到指定 IRQ
    sys_exit,           // 6:  进程退出
    sys_waitpid,        // 7:  等待子进程退出
    sys_spawn,          // 8:  创建子进程
    sys_mmap,           // 9:  内存映射（6 参：addr/size/prot/flags/fd/offset）
    sys_munmap,         // 10: 解除内存映射
    sys_serial_write,   // 11: 串口输出
    sys_fb_info,        // 12: 获取 framebuffer 信息
    sys_shm_create,     // 13: 创建 SHM fd（返回 fd + struct shm）
    sys_shm_attach,     // 14: 附加 SHM（过渡期，返回 fd）
    sys_pipe,           // 15: 创建 pipe
    sys_write,          // 16: 写 fd（PIPE/FILE/SOCKET dispatch）
    sys_read,           // 17: 读 fd（PIPE/FILE/SOCKET dispatch）
    sys_close,          // 18: 关闭 fd（PIPE/FILE/SOCKET dispatch）
    sys_load_dev,       // 19: 注册驱动到 dev_table
    sys_dev_msg,        // 20: fd 版变长消息（替代 sys_lookup_dev）
    sys_notify,         // 21: 异步通知（消息入队）
    sys_gettime,        // 22: 全局单调时钟（纳秒）
    sys_clock,          // 23: per-process CPU 时间（纳秒）
    sys_msg,            // 24: 变长消息请求（≤64KB）
    sys_msg_resp,       // 25: 变长消息回复
    sys_ioperm,         // 26: I/O 端口权限
    sys_dup2,           // 27: 复制 fd（PIPE/FILE/SOCKET dispatch）
    sys_fcntl,          // 28: 文件控制
    sys_dma_alloc,      // 29: 物理连续 DMA 分配
    sys_dma_free,       // 30: 释放 DMA 缓冲区
    sys_pci_dev_info,   // 31: PCI 设备查询（bus/dev/func → info）
    sys_block_read,     // 32: 块设备读
    sys_block_write,    // 33: 块设备写
    sys_block_async,    // 34: 异步块 I/O（RECV_NOTIFY 回调）
    sys_open_dev,       // 35: 打开设备节点（返回 fd | target_pid<<32）
    sys_install_fd,     // 36: 注册 FD_FILE fd（libc open 用）
    sys_socket,         // 37: socket(AF_UNIX, SOCK_STREAM, 0) → fd
    sys_bind,           // 38: bind(fd, addr, addrlen)
    sys_listen,         // 39: listen(fd, backlog)
    sys_accept,         // 40: accept(fd, addr, addrlen)
    sys_connect,        // 41: connect(fd, addr, addrlen)
    sys_socketpair,     // 42: socketpair(domain, type, proto, sv[2])
    sys_sendmsg,        // 43: sendmsg(fd, msg, flags)
    sys_recvmsg,        // 44: recvmsg(fd, msg, flags)
    sys_shutdown,       // 45: shutdown(fd, how)
    sys_poll,           // 46: poll(fds, nfds, timeout_ms)
};
```

参数从 trapframe 提取：RDI/RSI/RDX/R10/R8/R9（Linux 约定，6 参 syscall），返回值写回 tf->rax。超出范围返回 ENOSYS。

## 用户态封装

底层 `__syscall0`-`__syscall6` 定义在 `arch/x64/utils.h`（内联汇编）。语义封装定义在 `common/syscall.h`：

```c
static inline uint64_t __syscall0(uint64_t n) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}
// syscall1..syscall5 类似，传入 1-5 个参数

// 6 参数 syscall（sys_mmap 需要，传递 r9）
static inline int64_t __syscall6(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                                 int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4), "r8"(a5), "r9"(a6)
        : "rcx", "r11", "memory");
    return ret;
}
```

### 返回值约定

- 状态型 syscall（recv/req/resp/notify/exit/irq_bind/munmap）：0=成功，正数=errno
- 值返回型 syscall：sys_mmap 返回地址（NULL 失败），sys_spawn/sys_waitpid 返回 pid（0 失败），sys_getpid 始终成功

### recv_msg 结构

sys_recv 接收的消息类型（定义在 `common/syscall.h`）：

| type | 名称 | src | data |
|------|------|-----|------|
| 0 | RECV_IRQ | IRQ 号 | 56 字节 |
| 1 | RECV_REQ | 发送者 PID | 56 字节请求载荷 |
| 2 | RECV_NOTIFY | 发送者 PID | 56 字节 |
| 3 | RECV_MSG | 发送者 PID | kmaddr + len（内核 kmalloc 缓冲区指针） |

sys_recv 签名：`sys_recv(buf, data_buf, data_buf_len, timeout_ms)`，timeout_ms=0 无限等待。

## IPC 机制

| 层 | syscall | 载荷 | 用途 |
|----|---------|------|------|
| 控制信令 | sys_req/sys_resp | ≤56B 内联 | 短请求/回复（kbd bind/unbind 等） |
| 数据传输 | sys_msg/sys_msg_resp | ≤64KB 变长 | 文件 I/O（待迁移 socket） |
| 双向字节流 | socket/sendmsg/recvmsg | skb 链表 ≤64KB | AF_UNIX SOCK_STREAM + SCM_RIGHTS fd 传递 |
| 事件多路复用 | poll | pollfd 数组 | 统一监听 pipe/socket/dev fd 事件 |
| 匿名管道 | pipe/write/read/close | 4KB ring buffer | stdin/stdout 单向数据传输 |

sys_req：同步阻塞，发送 56 字节请求到目标进程，目标进程 sys_recv 收到 RECV_REQ，处理完后 sys_resp 回复，发送方被唤醒。

sys_msg：同步变长，发送方 kmalloc 内核缓冲区拷贝数据，接收方 sys_recv 收到 RECV_MSG（含 kmaddr），接收方 sys_msg_resp 回复后内核 kfree 缓冲区。

IPC 机制设计详见 [rpc.md](rpc.md)。
