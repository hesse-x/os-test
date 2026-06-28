# IPC 机制统一设计

## 概述

微内核四层 IPC + SHM 共享内存 + 信号机制，所有进程间通信统一设计。

| 机制 | 载荷 | 传输方式 | 用途 |
|------|------|---------|------|
| sys_req / sys_resp | ≤56B | 内联，零分配 | 控制信令（bind/unbind） |
| sys_msg / sys_msg_resp | 变长（≤64KB） | 内核 kmalloc 中转拷贝 | 数据传输（文件 I/O） |
| AF_UNIX SOCK_STREAM socket | 双向字节流 + SCM_RIGHTS | skb 链表 | Wayland、通用 IPC |
| pipe | 匿名单向 | ring buffer（4KB） | terminal ↔ shell |
| SHM | 共享内存页 | fd + mmap | 批量数据零拷贝（键盘、display buffer） |
| signal | 异步通知 | sigframe 栈帧投递 | Ctrl+C、异常翻译、SIGCHLD |

## IPC 选择指南

| 场景 | 推荐路径 |
|------|---------|
| Wayland compositor ↔ client | socket（sendmsg/recvmsg + SCM_RIGHTS） |
| 通用双向字节流 + fd 传递 | socket |
| 小载荷同步请求-响应（≤56B） | sys_req/sys_resp |
| 纯通知 | sys_notify |
| 批量数据 IPC（键盘事件、display buffer） | SHM + notify |
| 驱动间控制信令（bind/unbind） | sys_req/sys_resp |
| 文件 I/O（fs_driver ↔ 客户端） | 当前 sys_msg → 待迁移到 socket |

---

# 一、REQ/RESP + MSG/MSG_RESP

双层同步请求-响应 IPC 机制，所有事件（IRQ/REQ/NOTIFY/MSG）统一通过 per-process recv 队列接收，`sys_recv` 一次调用可多路复用。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IPC 模型 | 同步请求-响应 + 统一 recv | 微内核标准模式（seL4/QNX），一次 wait 收 IRQ+REQ+NOTIFY+MSG |
| 2 | sys_wait 处置 | 删除 | sys_recv 完全覆盖其功能 |
| 3 | 内联消息大小 | 64 字节 | 一个缓存行，内核 memcpy 开销可忽略 |
| 4 | 消息格式 | 内核透明（raw buffer） | 微内核原则，内核只搬运不解析 |
| 5 | recv_msg 布局 | `struct recv_msg { type, src, data[56] }` | 结构化头部 + payload；RECV_MSG 用 union 存 kmaddr/len |
| 6 | sys_req 超时 | 不加 | 目标崩溃时 proc_reap 唤醒调用者 |
| 7 | sys_resp 目标 | 内核自动关联 | 严格请求/响应配对，req_caller_pid 自动记录 |
| 8 | recv 队列 | per-process 固定 16 槽环形缓冲区 | 无动态分配，1KB/process，FIFO |
| 9 | sys_notify 语义 | 消息入队替代直接唤醒 | notify 消息进 recv 队列，recv 端统一消费 |
| 10 | 变长消息 | 内核 kmalloc 中转拷贝 | 数据必须跨地址空间，内核拷贝保证隔离；≤64KB 上限防止滥用 |
| 11 | recv 对 RECV_MSG | 扩展签名加 data_buf 参数 | 64B recv_msg 放不下变长数据，双缓冲：msg 头 + data_buf 存变长数据 |

## syscall 接口

### sys_recv(msg, data_buf, data_buf_len, timeout_ms) — syscall #2

```c
int sys_recv(void *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms);
```

- 从当前进程的 recv 队列取出一条消息，拷贝 64 字节到 msg（消息头）
- RECV_MSG 时：将变长数据 copy_to_user 到 data_buf（必须 `data_buf_len >= msg.len`，否则返回 -EINVAL），并 kfree 内核缓冲区
- 非 RECV_MSG 消息（IRQ/REQ/NOTIFY）忽略 data_buf 参数，现有调用者传 `NULL, 0`
- 队列为空：设 `WAIT_RECV` 阻塞，设 timeout（0=无限）
- 返回 0=成功，负 errno=-ETIMEDOUT/-EINVAL/-EFAULT

### sys_req(pid, request, reply) — syscall #3

```c
int sys_req(pid_t pid, void *request, void *reply);
```

- 向目标进程发送 56 字节请求（从 request 拷贝到 recv_msg.data），请求入目标 recv 队列（type=RECV_REQ, src=调用者 PID）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 调用者设 `WAIT_REQ_REPLY` 阻塞，等待目标 `sys_resp`
- 目标 `sys_resp` 时，56 字节回复拷贝到 reply，唤醒调用者
- 返回 0=成功，负 errno=-ESRCH/-EBUSY/-EFAULT
- 崩溃清理：目标进程 exit 时，`proc_reap` 扫描 `procs[]`，所有 `wait_event==WAIT_REQ_REPLY && req_target==目标PID` 的进程被唤醒，返回 -ESRCH

### sys_resp(reply) — syscall #4

```c
int sys_resp(void *reply);
```

- 将 56 字节回复从 reply 拷贝到当前请求调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- 唤醒调用者（从 `WAIT_REQ_REPLY` → `READY`）
- 清除 `req_caller_pid`
- 返回 0=成功，负 errno=-EINVAL/-ESRCH/-EFAULT

### sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall #20

```c
int sys_msg(int32_t target_pid, void *msg_buf, size_t msg_len,
            void *reply_buf, size_t reply_len);
```

- 向目标进程发送变长消息（≤64KB），内核 kmalloc 中转拷贝
- 调用者设 `WAIT_MSG_REPLY` 阻塞，等待目标 `sys_msg_resp`
- `msg_len ∈ [1, 65536]`，kmalloc 失败返回 -ENOMEM
- 返回 0=成功，负 errno=-ESRCH/-ENOMEM/-EINVAL/-EFAULT

### sys_msg_resp(resp_buf, resp_len) — syscall #21

```c
int sys_msg_resp(void *resp_buf, size_t resp_len);
```

- 将变长回复拷贝到当前 msg 调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- copy 长度 = `min(resp_len, msg_reply_len)`
- 唤醒调用者（从 `WAIT_MSG_REPLY` → `READY`）
- 清除 `msg_caller_pid`
- 返回 0=成功，负 errno=-EINVAL/-ESRCH/-EFAULT

### sys_notify(pid) — syscall #17

```c
int sys_notify(pid_t pid);
```

- 向目标进程的 recv 队列入一条消息（type=RECV_NOTIFY, src=调用者 PID, data=空）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 队列满时返回 -EBUSY

## recv_msg 结构

```c
#define RECV_IRQ    0   // IRQ 通知
#define RECV_REQ    1   // 内联请求（≤56B）
#define RECV_NOTIFY 2   // 异步通知
#define RECV_MSG    3   // 变长消息（≤64KB）

struct recv_msg {
    uint32_t type;       // RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG
    uint32_t src;        // IRQ 号 或 发送者 PID
    union {
        uint8_t data[56];       // RECV_IRQ / RECV_REQ / RECV_NOTIFY
        struct {                 // RECV_MSG only（内核内部，用户态不可见）
            void  *kmaddr;      // 内核 kmalloc 缓冲区地址
            size_t len;         // 数据长度
        } msg;
    };
};
```

## 内核数据结构

### proc_t IPC 相关字段

```c
// === recv 队列 ===
uint8_t  recv_buf[16][64];  // 16 槽 × 64 字节 = 1KB 固定缓冲区
uint32_t recv_head;         // 生产者写入位置
uint32_t recv_tail;         // 消费者读取位置
spinlock_t recv_lock;       // 保护 recv_buf/head/tail

// === req 状态（内联载荷，≤56B） ===
pid_t    req_caller_pid;    // 当前正在处理的请求调用者 PID（-1=无）
void    *req_reply_buf;     // 调用者 reply buffer 的用户态地址
int32_t  req_result;        // 请求返回结果（0=成功，负 errno=失败）
pid_t    req_target_pid;    // 请求目标 PID（用于崩溃清理）

// === msg 状态（变长载荷，≤64KB） ===
void    *msg_reply_buf;     // caller's reply buffer user-space address
size_t   msg_reply_len;     // caller's reply buffer size
pid_t    msg_caller_pid;    // server side: who sent the msg (-1 = none)
int32_t  msg_result;        // 0 = success, negative errno on error
pid_t    msg_target_pid;    // caller side: who we're waiting on (crash cleanup)
```

### wait_event_t

```c
enum wait_event_t {
    WAIT_NONE,       // 未等待
    WAIT_RECV,       // sys_recv 等待
    WAIT_REQ_REPLY,  // sys_req 等待回复
    WAIT_MSG_REPLY,  // sys_msg 等待回复
    WAIT_CHILD,      // sys_waitpid 等待子进程
    WAIT_PIPE,       // pipe 读写阻塞
    WAIT_POLL,       // poll 等待事件
};
```

## 关键实现路径

### sys_req 流程

```
调用者                              内核                              服务端
-------                              ------                              -------
1. sys_req(pid, request, reply)
                                      2. copy_from_user 请求到 recv_msg.data
                                      3. 入队 target->recv_buf (type=RECV_REQ)
                                      4. 唤醒 target (WAIT_RECV → READY)
                                      5. 阻塞调用者 (WAIT_REQ_REPLY)
                                                                          6. 服务端 recv() 返回 RECV_REQ
                                                                          7. 处理请求
                                                                          8. sys_resp(reply)
                                                                              9. CR3 切换 → copy_to_user
                                                                              10. 唤醒调用者 (WAIT_REQ_REPLY → READY)
11. 调用者恢复，返回 0 或负 errno
```

### sys_msg 流程

```
调用者                              内核                              服务端
-------                              ------                              -------
1. sys_msg(pid, msg_buf, msg_len,
           reply_buf, reply_len)
                                      2. 验证 target_pid/msg_len/指针
                                      3. kmalloc(msg_len)，失败→-ENOMEM
                                      4. copy_from_user(kbuf, msg_buf, msg_len)
                                      5. 入队 RECV_MSG(kmaddr=kbuf, len=msg_len)
                                      6. 唤醒 target (WAIT_RECV → READY)
                                      7. 阻塞调用者 (WAIT_MSG_REPLY)
                                                                          8. 服务端 recv() 返回 RECV_MSG
                                                                              内核: copy_to_user(data_buf, kmaddr, len)
                                                                              内核: kfree(kmaddr) ← 请求缓冲区释放
                                                                              内核: msg_caller_pid = src
                                                                          9. 服务端处理请求
                                                                          10. sys_msg_resp(resp_buf, resp_len)
                                                                              11. copy_from_user(kbuf_resp, resp_buf, resp_len)
                                                                              12. CR3 切换 → copy_to_user(msg_reply_buf, ...)
                                                                              13. kfree(kbuf_resp)
                                                                              14. 唤醒调用者 (WAIT_MSG_REPLY → READY)
15. 调用者恢复，返回 0 或负 errno
```

### IRQ 分发

IRQ 到达时向绑定进程的 recv 队列入 RECV_IRQ 消息，然后唤醒（如果进程在 WAIT_RECV）。详见 `kernel/trap.cc` trap_dispatch。

### sys_notify

向目标 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_RECV 进程。

### wake_process（内核内部，pipe 用）

向 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_PIPE 进程。pipe 阻塞使用 `WAIT_PIPE`，与 `WAIT_RECV` 隔离。

### proc_reap 崩溃清理

进程退出时清理所有 IPC 状态：

1. **req 调用者**：扫描所有进程，找到 `wait_event==WAIT_REQ_REPLY && req_target_pid==exiting_pid` 的进程，唤醒并设 `req_result=-ESRCH`
2. **msg 调用者**：扫描所有进程，找到 `wait_event==WAIT_MSG_REPLY && msg_target_pid==exiting_pid` 的进程，唤醒并设 `msg_result=-ESRCH`
3. **req 服务端**：`req_caller_pid >= 0` 时清除为 -1
4. **msg 服务端**：`msg_caller_pid >= 0` 时清除为 -1
5. **recv 队列中的 RECV_MSG**：遍历 recv_buf，对 type==RECV_MSG 的项 kfree(kmaddr)

## libc 封装

```c
// user/include/sys/ipc.h
int recv(struct recv_msg *msg, void *data_buf, size_t data_buf_len, uint32_t timeout_ms);
int req(int32_t pid, void *req_buf, void *resp_buf);
int resp(void *resp_buf);
int msg(int32_t pid, void *req_buf, size_t req_len, void *resp_buf, size_t resp_len);
int msg_resp(void *resp_buf, size_t resp_len);
```

高阶封装 `req_call()` 定义在 `user/include/sys.h`。

## ~~sys_msg/sys_msg_resp~~ 已标记废弃

`sys_msg/sys_msg_resp` 在 socket 稳定后由 socket 替代：

- fs_driver 当前通过 sys_msg 与客户端通信（文件 open/read/write/close）
- 迁移后：fs_driver 变为 listen socket `/dev/fs` → `accept` 每客户端独立连接
- 每个客户端通过 `sendmsg(fs_fd, req)` / `recvmsg(fs_fd, reply)` 进行文件 I/O
- session 路由（当前多客户端复用 fs_msg 号区分 session_id）被 accept 原生多连接替代
- **优势**：更干净的每客户端状态隔离 + SCM_RIGHTS fd 传递支持（未来）

迁移时机：socket 实现稳定并通过验证后。当前不阻塞 Wayland 开发。

---

# 二、AF_UNIX SOCK_STREAM Socket

## 概述

为支持 Wayland 合成器/客户端通信及通用进程间双向字节流通信，在内核态实现 AF_UNIX SOCK_STREAM socket。
接口语义与 Linux 对齐（`struct msghdr` + `struct iovec` + SCM_RIGHTS），便于复用 Linux 生态（libwayland 移植）。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | Socket 类型 | AF_UNIX, SOCK_STREAM | Wayland 核心需求，双向字节流 |
| 2 | 实现层级 | 内核态（`kernel/socket.cc`） | SCM_RIGHTS fd 传递只能内核做；skb 链表天然支持消息粒度元数据 |
| 3 | API 范围 | `socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown` + `poll` | 完整 Unix domain socket 生命周期，与 POSIX 对齐 |
| 4 | 数据结构 | skb 链表（参考 Linux `sk_buff`） | 可变大小 skb，每个 sendmsg 对应一个 skb，天然支持 SCM_RIGHTS 附属数据；ring buffer 无法关联按消息粒度的 fd 数组 |
| 5 | msghdr/iovec | 100% 兼容 Linux `struct msghdr` + `struct iovec` + `struct cmsghdr` | 直接复用 Linux UAPI 定义（含 padding 和位宽精确匹配），libwayland 无需大改 |
| 6 | syscall 接口 | 每操作独立 syscall 号（30-39），语义对齐 Linux | fd 操作同类，独立 syscall 更清晰 |
| 7 | 连接模型 | 无三次握手，`connect` 直接建立（参考 Linux AF_UNIX） | Unix domain socket 在同一内核，无需复杂握手 |
| 8 | 消息分帧 | SOCK_STREAM 不保留消息边界（与 Linux 一致） | libwayland 自己处理消息分帧 |
| 9 | SCM_RIGHTS 安装时机 | **Lazy**（recvmsg 时安装 fd） | 与 Linux 对齐。避免 fd 泄露（目标进程不 recv 则不占 fd slot）。skb 在队列中持有 fd 的引用，proc_reap 清理未消费的 skb fd |
| 10 | skb 大小 | 动态 kmalloc，一个 sendmsg 对应一个 skb | 与 Linux AF_UNIX 一致。软上限 64KB（同现有 sys_msg 最大载荷），防恶意 OOM |
| 11 | 锁 | 全局 `socket_lock`（spinlock_t，no irqsave），待升级 per-socket | 初期简单，Unix domain socket 无硬中断路径参与。锁顺序：`procs_lock` → `socket_lock` → `scheduler_lock` |
| 12 | Socket 命名 | 内核名字空间表（hash map: path → `struct unix_sock*`） | 不依赖文件系统（FAT32 无 socket inode），路径格式兼容 Linux `sockaddr_un.sun_path[108]` |

## 数据结构

### sk_buff（socket buffer）

参考 Linux 设计，可变大小 skb，每个 `sendmsg` 调用产生一个 skb。

```c
// struct sk_buff — 一个 sendmsg 对应一个 skb
struct sk_buff {
    struct sk_buff *next;       // 链表下一节点（多 skb 链式等待消费）
    uint32_t len;               // 数据长度（可能为 0，仅传递 fd）
    uint8_t  data[];            // 灵活数组成员，紧跟 skb 结构体后面
    // SCM_RIGHTS 附属数据（跟随此 skb 的 fd 数组）
    int      num_fds;           // 附属 fd 数量（≤ 8）
    int      fds[8];            // 要传递的 fd 数组（接收方 recvmsg 时安装）
    // 状态
    int      consumed;          // SOCK_STREAM 模式下已经读取的偏移量
};

// 分配: skb = kmalloc(sizeof(struct sk_buff) + data_len)
// 释放: kfree(skb)
```

- sendmsg 先遍历 msg_iov 算出总数据量，kmalloc sizeof(sk_buff) + len
- 逐个 iov copy_from_user 到 `skb->data`
- SCM_RIGHTS fd 数组保存原始 fd 号（lazy 安装，recvmsg 时才复制到目标进程）
- `consumed` 字段用于 SOCK_STREAM 模式下部分读：skb 留在队列中直到 `consumed == len`
- skb 可带 len=0 + num_fds>0（纯 SCM_RIGHTS 传递）

### unix_sock（per-socket 内核结构）

```c
#define UNIX_MAX_BACKLOG 8

enum unix_sock_state {
    UNIX_FREE,          // 初始状态
    UNIX_LISTEN,        // server: listen 后
    UNIX_CONNECTED,     // 已连接（client + accepted server）
    UNIX_CLOSED,        // 已关闭
};

struct unix_sock {
    // === 通用 ===
    int      state;                  // UNIX_* 状态
    pid_t    peer;                   // 对端 PID（CONNECTED 时有效）
    int      ref_count;              // fd 引用计数（dup2 共享时 >1）

    // === 接收队列（skb 链表）===
    struct sk_buff *recv_queue_head; // 队头
    struct sk_buff *recv_queue_tail; // 队尾
    int    recv_queue_len;           // 队列中 skb 数量
    pid_t  blocked_reader;           // 阻塞在读的进程（-1 = 无）
    pid_t  blocked_writer;           // 阻塞在写的进程（-1 = 无）

    // === listen 用 ===
    struct unix_sock *backlog_head;  // 待 accept 连接链表
    struct unix_sock *backlog_tail;
    int    backlog_len;              // 当前 backlog 数量

    // === 状态 ===
    int    shutdown_read;            // 已 shutdown 读方向
    int    shutdown_write;           // 已 shutdown 写方向

    // === 绑定路径 ===
    char   sun_path[108];            // bind 时设置的路径（空串表示未绑定/匿名）
};
```

## 连接建立流程

```
socket(AF_UNIX, SOCK_STREAM, 0):
    分配 struct unix_sock
    state = UNIX_FREE
    ref_count = 1
    peer = -1
    返回 fd (process->fd_table[fd].type = FD_SOCKET)

bind(fd, addr, addrlen):
    拷贝 sockaddr_un.sun_path（支持抽象路径和文件系统路径）
    注册到内核名字空间表（hash map）
    返回 0

listen(fd, backlog):
    state → UNIX_LISTEN
    记录 backlog 上限
    返回 0

connect(fd, addr, addrlen):
    根据 sun_path 从名字空间表查找 listener（UNIX_LISTEN 状态）
    如未找到：返回 -ENOENT
    如 backlog 满：返回 -ECONNREFUSED
    创建新 struct unix_sock child（state = UNIX_CONNECTED）
    unix_peer(child) = client
    unix_peer(client) = child
    将 child 挂到 listener 的 backlog 链表
    唤醒 listener 的 blocked_reader（accept 阻塞时）
    返回 0

accept(fd, addr, addrlen):
    从 backlog 链表头部取一个 child
    为 child 分配新的 fd
    child->ref_count++
    返回新 fd
```

## socketpair

```c
socketpair(AF_UNIX, SOCK_STREAM, 0, sv):
    分配 socket A（state = UNIX_CONNECTED）
    分配 socket B（state = UNIX_CONNECTED）
    unix_peer(A) = B
    unix_peer(B) = A
    sv[0] = A 的 fd
    sv[1] = B 的 fd
    返回 0
```

可用于替换现有 terminal ↔ shell 的两 pipe 拓扑（一个 socketpair 搞定双向）。

## 数据收发（sendmsg/recvmsg）

```c
sendmsg(fd, msg, flags):
    1. 验证 fd 是 FD_SOCKET，state = UNIX_CONNECTED
    2. 遍历 msg->msg_iov，计算总数据量 len
    3. len > MAX_SOCKET_DATA (64KB) 返回 -EMSGSIZE
    4. 分配 skb = kmalloc(sizeof(struct sk_buff) + len)
    5. 逐个 iov copy_from_user 到 skb->data
    6. 处理 msg->msg_control：
       - 解析 struct cmsghdr（CMSG_FIRSTHDR/CMSG_NXTHDR）
       - cmsg_level == SOL_SOCKET && cmsg_type == SCM_RIGHTS：
         验证发送方 fd 有效性（检查 fd_table 中对应 fd 合法）
         保存 fd 号到 skb->fds[]（lazy 安装，不跨进程复制）
       - 其他 cmsg_type 返回 -EINVAL
    7. 将 skb 挂到对端（peer sock）的 recv_queue 尾部
    8. 唤醒对端的 blocked_reader（wake_process，在 socket_lock 外调用）
    9. 返回总数据长度

recvmsg(fd, msg, flags):
    1. 验证 fd 是 FD_SOCKET
    2. 从 recv_queue 头部取第一个 skb
    3. recv_queue 空：
       - 对端 shutdown_write 或已 CLOSED → 返回 0 (EOF)
       - 否则阻塞（设 blocked_reader = self，WAIT_POLL → schedule）
    4. 遍历 msg->msg_iov，逐个 copy_to_user：
       - SOCK_STREAM：可消费部分 skb 数据（更新 consumed 偏移量）
       - consumed == skb->len → 弹出 skb 并 kfree
    5. SCM_RIGHTS（lazy 安装发生在 recvmsg 时）：
       - 遍历 skb->fds[]，在当前进程 fd_table 找空闲 slot
       - 为每个 fd 增加对应资源的引用计数（pipe->ref_count++、shm ref++ 等）
       - msg_control 中以 struct cmsghdr 格式写入（SOL_SOCKET, SCM_RIGHTS）
       - msg_controllen 不够时设 MSG_CTRUNC
    6. msg->msg_flags 输出（MSG_EOR 等）
    7. 消费完毕后唤醒对端的 blocked_writer
    8. 返回读取的字节数
```

## poll

```c
poll(fds, nfds, timeout_ms):
    1. 遍历 pollfd 数组，对每个 fd：
       - pipe: buf 不空 → POLLIN；未满 → POLLOUT
       - socket (FD_SOCKET):
         recv_queue 不空或 shutdown_read → POLLIN
         未 shutdown_write 且 peer 未 CLOSED → POLLOUT
         peer 已 CLOSED 且队列空 → POLLHUP
       - 其他 fd 类型：根据状态判断
    2. 有就绪 fd → 立即返回
    3. 全部未就绪且 timeout=0 → 返回 0
    4. 全部未就绪且 timeout>0 → 设 WAIT_POLL + wait_deadline → schedule
    5. 被唤醒后重新遍历 pollfd → 更新 revents → 返回
    6. timeout 到期 → 返回 0
```

**阻塞模型**：`WAIT_POLL` 状态。`wake_process` 扩展为同时唤醒 `WAIT_POLL` 和 `WAIT_PIPE` 的进程。任何 fd 事件（pipe write/read、socket send/recv/close/shutdown）都调用 `wake_process(pid)`。进程恢复后从用户空间 pollfd 数组重新检查就绪状态（必要时 spurious wakeup，但正确）。

**不存储 pollfd 在内核**：pollfd 数组始终在用户空间，每次 wake 后用户态 re-read。

**复用现有 timer_queue**：`wait_deadline` 已支持超时唤醒，poll 直接复用。

## shutdown

```c
shutdown(fd, how):
    - SHUT_RD (0): 设置 shutdown_read = 1，清空 recv_queue（kfree 所有 skb）
    - SHUT_WR (1): 设置 shutdown_write = 1，对端读 socket 看到 EOF（队列空后返回 0）
    - SHUT_RDWR (2): 两者都做
    — 唤醒 blocked_reader / blocked_writer
```

## SCM_RIGHTS fd 传递（Lazy 安装）

SCM_RIGHTS 是 Wayland 的核心需求（共享 buffer 通过 fd 传递）。采用 lazy 方案：sendmsg 时验证 fd 有效性但不安装，recvmsg 时才在接收方 fd_table 中分配 slot。

**发送方流程（sendmsg 中 — 验证阶段）：**

```c
for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
     cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        if (cmsg->cmsg_len < CMSG_ALIGN(sizeof(struct cmsghdr)) + sizeof(int))
            return -EINVAL;
        int *fds = (int *)CMSG_DATA(cmsg);
        int num_fds = (cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int);
        if (num_fds > 8) return -EINVAL;

        // 验证发送方拥有这些 fd（不安装到目标进程）
        for (int i = 0; i < num_fds; i++) {
            struct file *f = &current_proc->fd_table[fds[i]];
            if (f->type == FD_NONE) return -EBADF;
            // 只存 fd 号，延迟安装
            skb->fds[skb->num_fds++] = fds[i];
        }
    }
}
```

**接收方流程（recvmsg 中 — 安装阶段）：**

```c
if (skb->num_fds > 0) {
    for (int i = 0; i < skb->num_fds; i++) {
        int orig_fd = skb->fds[i];
        int new_fd = find_unused_fd(current_proc);
        if (new_fd < 0) {
            msg->msg_flags |= MSG_CTRUNC;
            break;
        }
        // 复制发送方的 struct file 到接收方
        // 增加引用计数（pipe->ref_count++ 等）
    }

    // 写入 msg_control
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_len = CMSG_ALIGN(sizeof(struct cmsghdr)) + skb->num_fds * sizeof(int);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    int *out_fds = (int *)CMSG_DATA(cmsg);
    memcpy(out_fds, skb->fds, skb->num_fds * sizeof(int));
}
```

**注意**：Lazy 安装的代价是在 skb 从 sendmsg 到 recvmsg 期间，发送方 close 了原始 fd 后，skb 中保存的 `fds[]` 需要持有对应 fd 资源（如 pipe 的 ref_count）的引用，防止释放。但当前实现中 fd 资源销毁是 `proc_reap` 时的最后一步——只要发送方进程还活着，它的 fd_table 中 fd 就算 close 也只影响本进程的 slot（pipe ref_count 递减但不一定归零）。更严格的做法是 skb 入队时增加对应资源的 ref_count，出队时释放。**初步实现可以先简单：在发送方 close fd 和 exit 时不销毁 skb 中引用到的资源**（借助 ref_count 机制）。后续按 Linux 的 `unix_inflight`/`unix_notinflight` 模式完善。

## EOF / EPIPE 行为

| 场景 | 行为 |
|------|------|
| 对端 close，本端 recv | 剩余数据先返回 → recv_queue 空时返回 0 (EOF) |
| 对端 close，本端 send | 对端 socket 已 CLOSED → 返回 -EPIPE |
| listen socket close | backlog 中未 accept 的连接全部关闭（通知对端 EOF） |
| 本端 close | shutdown_write=1，清空 recv_queue，唤醒 blocked_reader/writer |
| connect 找不到 listener | -ENOENT |
| backlog 满 | -ECONNREFUSED |

## 内核 fd_table 扩展

```c
#define FD_NONE   0
#define FD_PIPE   1
#define FD_SHM    2
#define FD_DEV    3
#define FD_FILE   4
#define FD_SOCKET 5
#define FD_TTY    6

struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV / FD_FILE / FD_SOCKET / FD_TTY
    int flags;           // O_RDONLY / O_WRONLY / O_RDWR / O_NONBLOCK
    struct pipe *pipe;    // FD_PIPE
    struct shm  *shm;     // FD_SHM
    pid_t target_pid;     // FD_DEV
    struct {              // FD_FILE
        pid_t   fs_pid;
        int32_t fs_fd;
        uint64_t offset;
        uint64_t file_size;
        int      ref_count;
    } file_data;
    struct unix_sock *sock; // FD_SOCKET
};
```

### sys_write/sys_read 对 FD_SOCKET 的支持

```c
static int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len) {
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    return sock_sendmsg_internal(sock, &iov, 1, NULL, 0);
}

static int64_t sock_read(struct unix_sock *sock, void *buf, size_t len) {
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    return sock_recvmsg_internal(sock, &iov, 1, NULL, 0);
}
```

SCM_RIGHTS 必须走 `sendmsg`/`recvmsg` 接口（附属数据从 msg_control 传递），`write`/`read` 不传递 fd。

## 与其他 IPC 机制的关系

| 特性 | pipe | socket (SOCK_STREAM) | sys_req/sys_resp | sys_msg/sys_msg_resp |
|------|------|---------------------|-----------------|---------------------|
| 方向 | 单向 | 双向 | REQ→RESP | MSG→MSG_RESP |
| 数据结构 | ring buffer（4KB） | skb 链表（动态大小） | 56B recv slot | kmalloc 中转（≤64KB） |
| SCM_RIGHTS | 不支持 | 支持 | 不支持 | 不支持 |
| 命名 | 匿名 | 匿名 + 命名（bind+connect） | PID 直连 | PID 直连 |
| 应用 | terminal ↔ shell | Wayland、通用 IPC | 驱动 bind/unbind | fs_driver 文件 I/O（待迁移）|

### socketpair 对 pipe 的可替代性

当前 pipe 使用场景（terminal ↔ shell）可以用 `socketpair(AF_UNIX, SOCK_STREAM, 0)` 直接替换：

```
当前（两个 pipe）:
  terminal fd 1(write)  →  stdin pipe  →  shell fd 0(read)
  shell fd 1(write)      →  stdout pipe →  terminal fd 0(read NONBLOCK)

替换后（一个 socketpair）:
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
  sv[0] → 给 shell（作为 stdin/stdout 的读写 fd）
  sv[1] → 给 terminal（作为 stdin/stdout 的读写 fd）
  双向，一个 fd pair 搞定
```

socket 稳定后替换。现有 pipe 继续保留（作为 socketpair 的一种简化特例）。

## syscall 接口定义（与 Linux 对齐）

```c
#define SYS_SOCKET    30   // -> socket(int domain, int type, int protocol)
#define SYS_BIND      31   // -> bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_LISTEN    32   // -> listen(int fd, int backlog)
#define SYS_ACCEPT    33   // -> accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
#define SYS_CONNECT   34   // -> connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_SOCKETPAIR 35  // -> socketpair(int domain, int type, int protocol, int sv[2])
#define SYS_SENDMSG   36   // -> sendmsg(int fd, const struct msghdr *msg, int flags)
#define SYS_RECVMSG   37   // -> recvmsg(int fd, struct msghdr *msg, int flags)
#define SYS_SHUTDOWN  38   // -> shutdown(int fd, int how)
#define SYS_POLL      39   // -> poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
```

### 数据结构定义（Linux UAPI 兼容）

```c
struct sockaddr_un {
    uint16_t sun_family;         // AF_UNIX = 1
    char     sun_path[108];      // 文件系统路径或抽象路径（\0 开头）
};

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#define CMSG_ALIGN(len)     (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg)     ((void *)(((char *)(cmsg)) + sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(msg, cmsg)  /* 下一个 cmsg，无则 NULL */
#define CMSG_FIRSTHDR(msg)      /* 第一个 cmsg */

struct msghdr {
    void       *msg_name;
    socklen_t   msg_namelen;     // 4-byte
    unsigned    __pad0;          // 填充 4-byte
    struct iovec *msg_iov;
    size_t      msg_iovlen;
    void       *msg_control;
    size_t      msg_controllen;
    int         msg_flags;
};

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_SEQPACKET 5

#define SOL_SOCKET  1
#define SCM_RIGHTS  1

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
```

## 与 Wayland 的关系

Wayland 协议的核心 socket 需求：

1. **`wl_display_connect()`** → `socket(AF_UNIX, SOCK_STREAM, 0)` + `connect()` 到 `$XDG_RUNTIME_DIR/wayland-0`
2. **事件循环** → `poll()` 等待 socket 可读
3. **消息收发** → `sendmsg()`/`recvmsg()` 传递序列化协议数据
4. **Buffer 共享** → `SCM_RIGHTS` fd 传递（wayland 客户端通过 fd 引用共享内存/DRM buffer）

## 锁设计

### 全局 socket_lock

初期使用一把全局 `spinlock_t socket_lock`（no irqsave），覆盖：
- `struct unix_sock` 所有字段的读写
- bind 名字空间表的插入/查找/删除
- skb 链表的入队/出队
- SCM_RIGHTS fd 安装时的 fd_table 操作

**锁顺序**：`procs_lock` → `socket_lock` → `scheduler_lock`

**`wake_process` 在 socket_lock 外调用**：sendmsg/recvmsg 操作完 skb 队列后，先释放 socket_lock，再调 `wake_process(blocked_reader)`。避免 socket_lock → scheduler_lock 的逆序。

### 后续升级

后续升级为 **per-socket spinlock** + **per-hash-bucket spinlock**（bind 名字空间）。当全局 socket_lock 成为性能瓶颈时执行。

## libc 封装

```c
// user/include/sys/socket.h
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int socketpair(int domain, int type, int protocol, int sv[2]);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int shutdown(int fd, int how);
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);
```

## 数据结构定义位置

| 定义 | 位置 | 使用方 |
|------|------|--------|
| `struct sockaddr_un` | `common/socket.h` | 内核 + libc |
| `struct iovec` | `common/syscall.h` | 内核 + libc |
| `struct msghdr` | `common/socket.h` | 内核 + libc |
| `struct cmsghdr` + CMSG_* 宏 | `common/socket.h` | 内核 + libc |
| `struct pollfd` + POLLIN/OUT | `common/syscall.h` | 内核 + libc |
| `struct sk_buff` + `struct unix_sock` | `kernel/socket.h`（内核私有） | 内核 |

---

# 三、Pipe

pipe 为匿名单向字节流，ring buffer 大小 4KB，通过 `sys_pipe` 创建一对 fd（读端 + 写端）。

libc 的 `pipe()` 标记 fd 类型：

```c
int pipe(int fd[2]) {
    int r = sys_pipe(fd);
    if (r < 0) { errno = -r; return -1; }
    fd_table[fd[0]].type = FD_PIPE;
    fd_table[fd[1]].type = FD_PIPE;
    return 0;
}
```

内核 `sys_read`/`sys_write`/`sys_close` 根据 fd_type 分发：
- FD_PIPE → pipe ring buffer
- FD_REGULAR → VFS/FAT32（inode + page cache）
- FD_SOCKET → sock_read/sock_write/sock_close
- FD_DEV → dev_ops callback（用户态驱动 via req）
- FD_TTY → pty_read/pty_write（PTY master/slave）

pipe 未来可被 socketpair 替代（参见 socket 部分），当前保留作为简化特例。

---

# 四、SHM 共享内存

## 概述

> **fd 化重构已完成**。共享内存已从硬编码 vaddr + `shm_regions[]` 数组模型重构为 fd 模型，对齐 Linux `memfd_create` + `mmap` + `SCM_RIGHTS` 语义。

## 设计

### 数据结构

```c
// kernel/proc.h

// SHM 底层对象（kmalloc 分配，引用计数管理）
struct shm {
    uint64_t phys;          // 物理页起始地址（或链表头）
    size_t   npages;        // 连续页数（离散页时=0，由 page_list 管理）
    size_t   file_size;     // ftruncate 设的逻辑大小（≤ npages * PAGE_SIZE）
    int      ref_count;     // 引用计数：fd + 1，mmap vma + 1
    int      flags;         // SHM_KERNEL | SHM_SEALED
    uint32_t seals;         // F_SEAL_SHRINK / F_SEAL_GROW / F_SEAL_WRITE / F_SEAL_SEAL
    char     name[32];      // memfd_create 传入的调试名
    // 离散页支持（当 bfc_alloc 无法分配连续大块时）
    uint64_t *page_list;    // NULL 表示 phys 连续，否则每个 entry 是 4K 页物理地址
    int      num_pages;     // page_list 长度
};

// fd_table 扩展
#define FD_SHM   2

struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV
    int flags;
    struct pipe *pipe;
    struct shm  *shm;    // if type == FD_SHM
    pid_t target_pid;
};

// mmap_region 扩展
struct mmap_region {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;           // MAP_PHYSICAL 专用（旧方式）
    struct shm *shm_obj;     // mmap(SHM fd) 时非 NULL，其 phys/npages 覆盖 phys/size
    mmap_region *next;
};
```

### 引用计数生命周期

```
                   ┌─────────────┐
                   │  struct shm │
                   │  ref_count  │
                   └──────┬──────┘
                          │
            ┌─────────────┼─────────────┐
            │ +1          │ +1          │ +1 (未来)
            ▼             ▼             ▼
      fd_table[fd]   mmap_region     SCM_RIGHTS 传 fd
      (FD_SHM)       (.shm_obj)      (新进程 fd)
            │             │
            │ close(fd)   │ munmap / proc_reap
            │ ref_count-- │ ref_count--
            └─────────────┘
                          │
                   ref_count == 0
                          │
                          ▼
                  kfree(shm) + bfc_alloc.free_page(phys, npages)
```

**规则：**

| 操作 | ref_count 变化 | 说明 |
|------|---------------|------|
| `sys_shm_create(size)` | =1（fd 持有） | fd 本身计 1 引用 |
| `sys_mmap(fd, ...)` | +1 | vma 映射计 1 |
| `close(fd)` | -1 | fd 释放 |
| `munmap(vaddr)` | -1 | vma 释放 |
| SCM_RIGHTS 发送 fd | +1 | 接收端 fd 计 1 |
| `dup2(old_fd, new_fd)` | +1 | 复制 fd 计 1 |
| 进程 exit：close all fds | -1 per FD_SHM | fd_table 遍历 |
| 进程 exit：unmap all vmas | -1 per shm_obj | mmap_regions 遍历 |

### kernel SHM（USB HID）特殊处理

```c
#define SHM_KERNEL 1  // 页由内核管理，ref_count==0 时不释放物理页

// 内核初始化时
struct shm *kshm = kmalloc(sizeof(struct shm));
kshm->phys = usb_hid_shm_phys;
kshm->npages = 1;
kshm->ref_count = 0;   // kernel 本身不计数
kshm->flags = SHM_KERNEL;
register_kernel_shm(USB_HID_SHM_ID, kshm);

// kbd_driver 请求时
int fd = sys_shm_attach(USB_HID_SHM_ID, 1);  // → kshm->ref_count++ (变 1)
void *vaddr = sys_mmap(fd, ...);             // → kshm->ref_count++ (变 2)
// close(fd) → ref_count=1, munmap → ref_count=0 → SHM_KERNEL 标记 → 不释放页
```

### syscall 接口

```c
// sys_shm_create(size) → 返回 fd（类似 memfd_create）
int sys_shm_create(size_t size);

// sys_shm_attach(id, mode) → 返回 fd（过渡用，等 SCM_RIGHTS 就绪后删除）
int sys_shm_attach(int32_t id, int mode);

// sys_mmap 6 参对齐 Linux
void *sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset);
// flags 含 MAP_SHARED 且 fd >= 0：映射 SHM fd
// flags 含 MAP_ANONYMOUS：匿名映射
// flags 含 MAP_PHYSICAL：物理地址映射
```

### proc_reap 清理路径

```c
// 新路径:
proc_reap:
    // 遍历 fd_table: type==FD_SHM → shm_put(shm) { ref_count--; 归零时 kfree+释放页 }
    // 遍历 mmap_regions: shm_obj != NULL → shm_put(shm_obj)
    //
    // 非 SHM 页（匿名 mmap、MAP_PHYSICAL）正常释放 PTE 和物理页
    // SHM 页（mmap_region.shm_obj != NULL）只 unmap PTE，不释放物理页
```

### 驱动适配

```
// 旧
vaddr = shm_create(4096);
vaddr = shm_attach(PID, 0);
vaddr = shm_attach_kernel(USB_HID_SHM_ID, &addr);

// 新
int fd = sys_shm_create(4096);
void *vaddr = sys_mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
sys_close(fd);   // 可选：关闭 fd，mmap 映射仍有效

int fd = sys_shm_attach(target_pid, 0);    // 过渡期
void *vaddr = sys_mmap(fd, ...);

int fd = sys_shm_attach(USB_HID_SHM_ID, 1);
void *vaddr = sys_mmap(fd, ...);
```

## Linux memfd_create 对齐

### 动机

当前 `sys_shm_create(size)` 返回可 mmap 的 fd，功能上等价于 Linux `memfd_create` + `ftruncate` 两步。为简化未来移植 libwayland 等依赖 Linux API 的代码，决定对齐 `memfd_create` 完整签名：

```c
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

int sys_memfd_create(const char *name, unsigned int flags);
// 返回 fd，size=0（需调 ftruncate 设大小）

int sys_ftruncate(int fd, off_t size);
// 扩大：分配新物理页 + 插入 shm.phys 页表链
// 缩小：释放超出的物理页（受 F_SEAL_SHRINK 限制）
```

### sealing 语义

| Seal | 含义 | 影响的操作 |
|------|------|-----------|
| `F_SEAL_SHRINK` | 禁止缩小 | `ftruncate(new_size < old_size)` → `-EPERM` |
| `F_SEAL_GROW` | 禁止扩大 | `ftruncate(new_size > old_size)` → `-EPERM` |
| `F_SEAL_WRITE` | 禁止写入 | `mmap(PROT_WRITE)` → `-EPERM`；已有 writable mmap 不受影响 |
| `F_SEAL_SEAL` | 禁止再设 seal | `fcntl(F_ADD_SEALS)` → `-EPERM` |

seal 一经设置不可撤销（`F_SEAL_SEAL` 锁定后连自身都不能再设）。

```c
#define F_ADD_SEALS  1033   // Linux 兼容
#define F_GET_SEALS  1034

int sys_fcntl(int fd, int cmd, uint64_t arg);
// cmd=F_ADD_SEALS, arg=bitmask of F_SEAL_*
// cmd=F_GET_SEALS → 返回当前 seal bitmask
```

### 生命周期变化

```
旧: shm_create(size) → fd(→mmap(→vaddr 直接读写)
    close(fd) → ref--, munmap → ref--, 归零→释放

新: memfd_create(name, flags) → fd(size=0)
    ftruncate(fd, size) → 分配物理页
    mmap(fd, ...) → 映射到地址空间
    close(fd) → ref-- (不影响已 mmap)
    munmap(vaddr) → ref--, 归零→释放
```

### 兼容性

- `sys_shm_create(size)` — 保留作为快捷方式，等价于 `memfd_create(NULL, 0)` + `ftruncate(fd, size)` 两步
- `sys_mmap(MAP_SHARED, fd, 0)` — 不受影响，对 memfd fd 和 shm fd 行为一致
- 现有驱动代码不需要改（仍可调 `sys_shm_create`），新增 Wayland 代码走 `memfd_create` 路径

### memfd_create 实现状态

- [ ] `struct shm` 扩展：file_size/seals/name/page_list 字段
- [ ] `SYS_MEMFD_CREATE` — 新 syscall，分配空 shm 对象 + fd
- [ ] `SYS_FTRUNCATE` — 新 syscall，扩大/缩小 shm 物理页
- [ ] `SYS_FCNTL` F_ADD_SEALS / F_GET_SEALS — sealing 管理
- [ ] `MFD_CLOEXEC` — 存入 fd flags，exec 时自动关闭
- [ ] `MFD_ALLOW_SEALING` — 允许后续加 seal
- [ ] resize 的 page_list 离散页支持
- [ ] `sys_mmap` 已映射区域在 ftruncate 扩大后的可见性

---

# 五、信号机制

## 动机

信号实现为 Linux 标准信号机制，为 PTY（Ctrl-C → SIGINT）和 Wayland 验收提供基础。

## 设计范围

| 特性 | 状态 | 说明 |
|------|------|------|
| sigframe 栈帧投递 | ✅ | pretcode + siginfo_t + ucontext_t，保存全部 GP 寄存器 |
| SA_SIGINFO | ✅ | handler(int, siginfo_t*, ucontext_t*) |
| EINTR | ✅ | 阻塞 syscall 被信号中断返回 -EINTR |
| blocked mask 内部机制 | ✅ | handler 执行期间 block sa_mask + 当前信号，sigreturn 恢复 |
| 内核异常翻译 | ✅ | #PF→SIGSEGV, #GP→SIGSEGV, #UD→SIGILL, #DE→SIGFPE |
| force_sig | ✅ | 同步信号绕过 SIG_IGN（避免故障指令无限重入） |
| SIGCHLD | ✅ | 替代 sys_exit 中 RECV_NOTIFY 子进程退出通知 |
| sys_sigprocmask | ❌ 不做 | blocked 初始为 0，仅内核在信号投递/sigreturn 时修改 |
| SA_RESTART | ❌ 不做 | EINTR 后由用户态 libc 或应用决定是否重启 |
| sigaltstack | ❌ 不做 | handler 在用户栈上执行 |
| SIGPIPE | ❌ 不做 | 保持现有 -EPIPE 返回值 |
| 作业控制 (SIGTSTP/Ctrl+Z) | ❌ 不做 | 需要 session/controlling terminal |

## 数据结构

### sigset_t

```c
typedef uint64_t sigset_t;  // 64 个信号，1 个 uint64_t 足够
```

### sigaction

```c
struct sigaction {
    union {
        void   (*sa_handler)(int);
        void   (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;       // handler 执行期间额外阻塞的信号集
    int      sa_flags;      // SA_SIGINFO, SA_RESTART, SA_NODEFER 等
    void   (*sa_restorer)(void);  // Linux 历史遗留，内核忽略
};
```

SIG_DFL = 0, SIG_IGN = 1，handler 地址 > 1。

### 信号编号

与 Linux x86-64 对齐（common/signal.h 已定义 SIGHUP..SIGTSTP 1-20，NSIG=32）。

| 信号 | default action | 用途 |
|------|---------------|------|
| SIGINT (2) | Terminate | Ctrl+C 终止前台进程 |
| SIGILL (4) | Terminate | 非法指令 |
| SIGABRT (6) | Terminate | abort() |
| SIGFPE (8) | Terminate | 算术异常 |
| SIGKILL (9) | Terminate（不可捕获/不可屏蔽） | 强制终止 |
| SIGSEGV (11) | Terminate | 段错误/页错误 |
| SIGTERM (15) | Terminate | 优雅终止 |
| SIGCHLD (17) | Ignore | 子进程退出通知 |
| SIGSTOP (19) | Stop（暂不实现，走 Terminate） | 暂停进程 |

### rt_sigframe（栈帧结构）

```c
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, ss, ds, es, fs, gs;
    uint64_t fs_base, gs_base;
    uint64_t cr2;              // #PF 地址（SIGSEGV 时 si_addr 来源）
    uint64_t _pad1;
};

struct ucontext_t {
    uint64_t          uc_flags;
    struct ucontext_t *uc_link;    // NULL（无 sigaltstack 链）
    sigset_t          uc_sigmask;
    struct sigcontext  uc_mcontext;
};

struct siginfo_t {
    int si_signo;
    int si_errno;    // 清零
    int si_code;     // SI_USER / SI_KERNEL / SI_QUEUE
    union {
        struct { pid_t si_pid; int si_uid; } _kill;
        void *si_addr;                         // SIGSEGV 崩溃地址
    } _sifields;
    char _pad[128 - 3*sizeof(int) - sizeof(union)];
};

struct rt_sigframe {
    uint64_t pretcode;           // 返回地址 = SIG_TRAMPOLINE_ADDR
    struct siginfo_t info;
    struct ucontext_t uc;
};
```

### proc_t signal_state

```c
struct signal_state {
    uint64_t      pending;           // bitmask: pending signals
    sigset_t      blocked;           // 当前阻塞信号集
    struct sigaction action[NSIG];   // per-signal handler 注册
};
```

### vdso sigreturn trampoline

固定映射到 `SIG_TRAMPOLINE_ADDR = 0x50000000`，每个进程创建时映射同一物理页：

```asm
sig_trampoline:
    mov rax, SYS_SIGRETURN
    syscall
```

未来扩展为完整 vdso ELF（含 clock_gettime 等符号），通过 AT_SYSINFO_EHDR 传递。

## syscall 接口

### sys_kill(pid_t pid, int sig) — syscall 43

```
pid > 0:  发送给指定进程
pid == 0: 发送给同进程组（预留）
pid < 0:  发送给进程组 -pid（预留）
sig == 0: 存在性检查，不发信号
```

### sys_sigaction(int sig, const sigaction *act, sigaction *oldact) — syscall 44

1. 验证 sig（SIGKILL/SIGSTOP → -EINVAL）
2. oldact 非 NULL → 拷贝当前 action 到用户空间
3. act 非 NULL → 从用户空间拷贝新 action
4. 清除该信号 pending bit（POSIX：注册 handler 时丢弃未决信号）

### sys_sigreturn() — syscall 45

1. 从当前 trapframe 的 rsp 定位用户栈上的 sigframe
2. 恢复 sigframe.uc.uc_mcontext 中的全部 GP 寄存器到 trapframe
3. 恢复 rip/rsp/rflags/cs/ss 到 trapframe
4. 恢复 blocked mask = sigframe.uc.uc_sigmask
5. 返回 0

## 信号投递路径

### 投递时机：trap return 前

```c
void check_pending_signals(trapframe_t *tf) {
    if (tf->cs != USER_CS) return;
    proc_t *proc = current_proc;

    while (1) {
        uint64_t pending = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        uint64_t deliverable = pending & ~proc->sig.blocked;
        if (!deliverable) return;

        int sig = __builtin_ctzll(deliverable);
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);

        sigaction_t *sa = &proc->sig.action[sig];

        if (sa->sa_handler == SIG_DFL) {
            // default action
        } else if (sa->sa_handler == SIG_IGN) {
            continue;
        } else {
            deliver_signal(proc, tf, sig, sa);
            return;
        }
    }
}
```

### deliver_signal — 构造 sigframe 推到用户栈

```c
void deliver_signal(proc_t *proc, trapframe_t *tf, int sig, sigaction_t *sa) {
    struct rt_sigframe frame = {0};
    frame.pretcode = SIG_TRAMPOLINE_ADDR;
    frame.info.si_signo = sig;
    frame.info.si_code = SI_KERNEL;

    // sigcontext — 从 trapframe 填充全部 16 个 GP 寄存器 + rip/rsp/rflags/cs/ss
    frame.uc.uc_sigmask = proc->sig.blocked;

    // 更新 blocked: handler 执行期间阻塞 sa_mask + 当前信号
    proc->sig.blocked |= sa->sa_mask | (1ULL << sig);

    // 推 sigframe 到用户栈
    uint64_t user_rsp = tf->rsp - sizeof(struct rt_sigframe);
    user_rsp &= ~0xFULL;

    // CR3 切换，拷贝 sigframe 到用户栈
    // ...

    // 修改 trapframe
    tf->rip = (uint64_t)sa->sa_handler;
    tf->rsp = user_rsp;

    if (sa->sa_flags & SA_SIGINFO) {
        tf->rdi = (uint64_t)sig;
        tf->rsi = (uint64_t)(user_rsp + offsetof(rt_sigframe, info));
        tf->rdx = (uint64_t)(user_rsp + offsetof(rt_sigframe, uc));
    } else {
        tf->rdi = (uint64_t)sig;
    }
}
```

### 用户态执行流程

```
trap return → 跳到 handler(rdi=sig, rsi=&siginfo, rdx=&ucontext)
handler 执行
handler ret → pop pretcode → 跳到 SIG_TRAMPOLINE_ADDR
trampoline: mov rax, SYS_SIGRETURN; syscall
→ sys_sigreturn 读取栈帧，恢复全部寄存器 + blocked mask
→ SYSRET/IRET 回到被中断代码
```

## EINTR

阻塞 syscall 被信号中断时返回 `-EINTR`。进程回到用户态后 `check_pending_signals` 投递信号。

需要加 EINTR 检查的阻塞 syscall：

| syscall | 阻塞场景 |
|---------|---------|
| sys_recv | WAIT_RECV |
| sys_read (pipe) | WAIT_PIPE |
| sys_waitpid | WAIT_CHILD |
| sys_req | WAIT_REQ_REPLY |
| sys_msg | WAIT_MSG_REPLY |
| sys_poll | WAIT_POLL |
| sys_accept | WAIT_RECV |

**不清除 pending bit**。EINTR 只让进程退出阻塞，信号由 `check_pending_signals` 正常投递。

### sys_kill 唤醒

```c
// sys_kill 中，设置 pending 后:
if (target->state == BLOCKED) {
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->pid == pid && target->state == BLOCKED) {
        target->state = READY;
        target->wait_event = WAIT_NONE;
        list_push_back(&cpu_locals[target_cpu].run_queue, &target->run_node);
        cpu_locals[target_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
}
```

## 内核异常翻译

| 异常向量 | 信号 | siginfo.si_code |
|----------|------|-----------------|
| 0 (#DE) | SIGFPE | FPE_INTDIV |
| 6 (#UD) | SIGILL | ILL_ILLOPC |
| 13 (#GP) | SIGSEGV | SEGV_MAPERR |
| 14 (#PF) | SIGSEGV | present→SEGV_ACCERR, not-present→SEGV_MAPERR |

trap_dispatch 修改：

```c
if (tf->cs == USER_CS) {
    int sig = exception_to_signal(tf->trapno);
    force_sig(proc, sig, tf);
    return;  // 不杀进程，让 check_pending_signals 处理
}
```

### force_sig

同步信号（SIGSEGV/SIGILL/SIGFPE）必须投递，即使 SIG_IGN：

1. 设置 pending bit
2. 从 blocked 中清除该信号位
3. 设置 siginfo
4. sa_handler == SIG_IGN → 强制改为 SIG_DFL

## SIGCHLD 替代 RECV_NOTIFY

```
旧: 子进程 sys_exit → 父进程 recv 队列入队 RECV_NOTIFY → 唤醒

新: 子进程 sys_exit → 父进程 sig.pending |= (1ULL << SIGCHLD) → 唤醒
    不再入队 RECV_NOTIFY（仅 exit 场景移除，其他 notify 保持不变）
```

waitpid 唤醒不依赖 SIGCHLD handler。即使 SIGCHLD 被 ignore，`sys_waitpid(WAIT_CHILD)` 在子进程 ZOMBIE 时仍被唤醒。

| RECV_NOTIFY 用途 | 迁移 |
|------|------|
| 子进程 exit → 通知父进程 | SIGCHLD 替代 |
| 驱动间 notify（kbd→terminal） | 保持 RECV_NOTIFY |
| fs_driver block_async 完成回调 | 保持 RECV_NOTIFY |

## 未来扩展

| 特性 | 依赖 | 说明 |
|------|------|------|
| sys_sigprocmask | blocked mask | 用户显式控制信号阻塞 |
| SA_RESTART | EINTR | libc 自动重启被中断 syscall |
| SIGPIPE | EINTR + socket | 写已关闭 socket 时 kill 进程 |
| 作业控制 | session + PTY | SIGTSTP/Ctrl+Z, fg/bg |
| sigaltstack | 信号栈 | handler 在独立栈执行 |
| vdso ELF | trampoline 页 | 扩展为完整 vdso（clock_gettime 等） |
| real-time signal | 信号队列 | 32-64 号信号排队不丢 |
| sys_kill pgid | 进程组 | kill(-pgid, sig) 投递到整组 |

---

# syscall 编号总表

| # | 名称 | 说明 |
|---|------|------|
| 0 | sys_getpid | 返回当前 PID |
| 1 | sys_yield | 让出 CPU |
| 2 | sys_recv | 统一事件接收 |
| 3 | sys_req | 同步内联请求（≤56B） |
| 4 | sys_resp | 回复当前 req 调用者 |
| 5 | sys_irq_bind | 绑定 IRQ |
| 6 | sys_exit | 退出进程 |
| 7 | sys_waitpid | 等待子进程 |
| 8 | — | sys_spawn 已删除，使用 fork+execve |
| 9 | sys_mmap | 匿名内存映射 |
| 10 | sys_munmap | 解除映射 |
| 11 | sys_shm_create | 创建共享内存 |
| 12 | sys_shm_attach | 附加共享内存 |
| 13 | sys_pipe | 创建管道 |
| 14 | sys_write | 写 fd |
| 15 | sys_read | 读 fd |
| 16 | sys_close | 关闭 fd |
| 17 | sys_notify | 异步通知 |
| 18 | sys_gettime | 全局单调时钟 |
| 19 | sys_clock | per-process CPU 时间 |
| 20 | sys_msg | 同步变长消息（≤64KB） |
| 21 | sys_msg_resp | 回复当前 msg 调用者 |
| 22 | sys_ioperm | I/O 端口权限 |
| 23 | sys_dup2 | 复制 fd |
| 24 | sys_fcntl | 文件控制 |
| 25 | sys_dma_alloc | DMA 分配 |
| 26 | sys_dma_free | 释放 DMA |
| 27 | sys_pci_dev_info | PCI 设备信息 |
| 28 | sys_block_async | 异步块 I/O |
| 29 | sys_install_fd | 注册 FD_FILE fd |
| 30-39 | socket 系列syscall | socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown/poll |
| 40 | sys_lseek | 文件偏移设置 |
| 41 | sys_memfd_create | 创建 memfd |
| 42 | sys_ftruncate | 截断文件 |
| 43 | sys_kill | 发送信号 |
| 44 | sys_sigaction | 注册信号 handler |
| 45 | sys_sigreturn | 信号返回 |
| 46 | sys_debug_print | 内核调试打印 |
| 47-56 | VFS 系列syscall | open/stat/mkdir/unlink/rmdir/dev_create/getdents/ioctl/fstat/fdev_pid |
| 57 | sys_fork | fork |
| 58 | sys_execve | execve |

NR_SYSCALL = 59。
