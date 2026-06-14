# 统一 IPC 机制：sys_recv / sys_rpc / sys_reply

## 概述

用 `sys_recv` / `sys_rpc` / `sys_reply` 替换原 `sys_wait`，建立轻量级同步 RPC 和统一事件接收机制，使任意进程间可进行请求-响应式通信。所有事件（IRQ/RPC/notify）统一通过 per-process recv 队列接收，`sys_recv` 一次调用可多路复用。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IPC 模型 | 同步 RPC + 统一 recv | 微内核标准模式（seL4/QNX），一次 wait 收 IRQ+RPC+notify |
| 2 | sys_wait 处置 | 删除 | sys_recv 完全覆盖其功能 |
| 3 | 消息大小 | 64 字节 | 一个缓存行，内核 memcpy 开销可忽略 |
| 4 | 消息格式 | 内核透明（raw buffer） | 微内核原则，内核只搬运不解析 |
| 5 | recv_msg 布局 | `struct recv_msg { type, src, data[56] }` | 结构化头部 + payload |
| 6 | sys_rpc 超时 | 不加 | 目标崩溃时 proc_reap 唤醒调用者 |
| 7 | sys_reply 目标 | 内核自动关联 | 严格请求/响应配对，rpc_caller_pid 自动记录 |
| 8 | recv 队列 | per-process 固定 16 槽环形缓冲区 | 无动态分配，1KB/process，FIFO |
| 9 | sys_notify 语义 | 消息入队替代直接唤醒 | notify 消息进 recv 队列，recv 端统一消费 |
| 10 | 用户态封装 | sys.h/sys.cc + rpc_call() | 参见 [work.md](../../work.md) |

## syscall 接口

### sys_recv(buf, timeout_ms) — syscall #2

```c
int sys_recv(void *buf, uint32_t timeout_ms);
```

- 从当前进程的 recv 队列取出一条消息，拷贝 64 字节到 buf
- 队列为空：设 `WAIT_RECV` 阻塞，设 timeout（0=无限）
- 返回 0=成功，正 errno=ETIMEDOUT/EINVAL/EFAULT
- 被唤醒后再次检查队列（while 循环直到取到消息或超时）

### sys_rpc(pid, request, reply) — syscall #3

```c
int sys_rpc(pid_t pid, void *request, void *reply);
```

- 向目标进程发送 56 字节请求（从 request 拷贝到 recv_msg.data），请求入目标 recv 队列（type=RECV_RPC, src=调用者 PID）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 调用者设 `WAIT_RPC_REPLY` 阻塞，等待目标 `sys_reply`
- 目标 `sys_reply` 时，64 字节回复拷贝到 reply，唤醒调用者
- 返回 0=成功，正 errno=ESRCH/EBUSY/EFAULT
- 崩溃清理：目标进程 exit 时，`proc_reap` 扫描 `procs[]`，所有 `wait_event==WAIT_RPC_REPLY && rpc_target==目标PID` 的进程被唤醒，返回 ESRCH

### sys_reply(reply) — syscall #4

```c
int sys_reply(void *reply);
```

- 将 64 字节回复从 reply 拷贝到当前 RPC 调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- 唤醒调用者（从 `WAIT_RPC_REPLY` → `READY`）
- 清除 `rpc_caller_pid`
- 返回 0=成功，正 errno=EINVAL/ESRCH/EFAULT

### sys_notify(pid) — syscall #21（语义变更）

```c
int sys_notify(pid_t pid);
```

- 向目标进程的 recv 队列入一条消息（type=RECV_NOTIFY, src=调用者 PID, data=空）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 队列满时返回 EBUSY

## recv_msg 结构

```c
#define RECV_IRQ    0   // IRQ 通知
#define RECV_RPC    1   // RPC 请求
#define RECV_NOTIFY 2   // 异步通知

struct recv_msg {
    uint32_t type;       // RECV_IRQ / RECV_RPC / RECV_NOTIFY
    uint32_t src;        // IRQ 号 或 发送者 PID
    uint8_t  data[56];   // IRQ/NOTIFY: 空 / RPC: 请求 payload
};
```

## 内核数据结构

### proc_t 新增字段

```c
uint8_t  recv_buf[16][64];  // 16 槽 × 64 字节 = 1KB 固定缓冲区
uint32_t recv_head;         // 生产者写入位置
uint32_t recv_tail;         // 消费者读取位置
spinlock_t recv_lock;       // 保护 recv_buf/head/tail

pid_t    rpc_caller_pid;    // 当前正在处理的 RPC 调用者 PID（-1=无）
void    *rpc_reply_buf;     // 调用者 reply buffer 的用户态地址
int32_t  rpc_result;        // RPC 返回结果（0=成功，正 errno=失败）
pid_t    rpc_target_pid;    // RPC 目标 PID（用于崩溃清理）
```

### wait_event_t

```c
enum wait_event_t {
    WAIT_NONE,       // 未等待
    WAIT_RECV,       // sys_recv 等待
    WAIT_RPC_REPLY,  // sys_rpc 等待回复
    WAIT_CHILD,      // sys_waitpid 等待子进程
    WAIT_PIPE,       // pipe 读写阻塞
};
```

## 关键实现路径

### IRQ 分发

IRQ 到达时向绑定进程的 recv 队列入 RECV_IRQ 消息，然后唤醒（如果进程在 WAIT_RECV）。详见 `kernel/trap.cc` trap_dispatch。

### sys_notify

向目标 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_RECV 进程。

### wake_process（内核内部，pipe 用）

向 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_PIPE 进程。pipe 阻塞使用 `WAIT_PIPE`，与 `WAIT_RECV` 隔离。

### proc_reap 崩溃清理

扫描所有进程，唤醒等待此进程 reply 的 RPC 调用者（`wait_event==WAIT_RPC_REPLY && rpc_target==dying_pid`），设 `rpc_result=ESRCH`。

## syscall 编号

| # | 名称 | 说明 |
|---|------|------|
| 0 | sys_getpid | 返回当前 PID |
| 1 | sys_yield | 让出 CPU |
| 2 | sys_recv | 统一事件接收 |
| 3 | sys_rpc | 同步 RPC |
| 4 | sys_reply | 回复当前 RPC 调用者 |
| 5 | sys_irq_bind | 绑定 IRQ |
| 6 | sys_exit | 退出进程 |
| 7 | sys_waitpid | 等待子进程 |
| 8 | sys_spawn | 创建子进程 |
| 9 | sys_mmap | 匿名内存映射 |
| 10 | sys_munmap | 解除映射 |
| 11 | sys_serial_write | 串口输出 |
| 12 | sys_fb_info | framebuffer 信息 |
| 13 | sys_shm_create | 创建共享内存 |
| 14 | sys_shm_attach | 附加共享内存 |
| 15 | sys_pipe | 创建管道 |
| 16 | sys_write | 写 fd |
| 17 | sys_read | 读 fd |
| 18 | sys_close | 关闭 fd |
| 19 | sys_load_dev | 注册设备 |
| 20 | sys_lookup_dev | 查询设备 |
| 21 | sys_notify | 异步通知 |

NR_SYSCALL = 22。`sys_wait`（原 #2）已删除。
