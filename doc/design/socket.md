# Unix Domain Socket 设计与实现

## 概述

为支持 Wayland 合成器/客户端通信及通用进程间双向字节流通信，在内核态实现 AF_UNIX SOCK_STREAM socket。
接口语义与 Linux 对齐（`struct msghdr` + `struct iovec` + SCM_RIGHTS），便于复用 Linux 生态（libwayland 移植）。

当前 NR_SYSCALL=37（syscall 编号 0-36）。新增 socket 10 个 syscall 后 NR_SYSCALL=47。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | Socket 类型 | AF_UNIX, SOCK_STREAM | Wayland 核心需求，双向字节流 |
| 2 | 实现层级 | 内核态（`kernel/socket.cc`） | SCM_RIGHTS fd 传递只能内核做；skb 链表天然支持消息粒度元数据 |
| 3 | API 范围 | `socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown` + `poll` | 完整 Unix domain socket 生命周期，与 POSIX 对齐 |
| 4 | 数据结构 | skb 链表（参考 Linux `sk_buff`） | 可变大小 skb，每个 sendmsg 对应一个 skb，天然支持 SCM_RIGHTS 附属数据；ring buffer 无法关联按消息粒度的 fd 数组 |
| 5 | msghdr/iovec | 100% 兼容 Linux `struct msghdr` + `struct iovec` + `struct cmsghdr` | 直接复用 Linux UAPI 定义（含 padding 和位宽精确匹配），libwayland 无需大改 |
| 6 | syscall 接口 | 每操作独立 syscall 号（37-46），语义对齐 Linux | fd 操作同类，独立 syscall 更清晰 |
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

### 连接建立流程

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

### socketpair

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

### 数据收发（sendmsg/recvmsg）

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

### poll

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

**阻塞模型**：新增 `WAIT_POLL` 状态（`enum wait_event_t`）。`wake_process` 扩展为同时唤醒 `WAIT_POLL` 和 `WAIT_PIPE` 的进程。任何 fd 事件（pipe write/read、socket send/recv/close/shutdown）都调用 `wake_process(pid)`。进程恢复后从用户空间 pollfd 数组重新检查就绪状态（必要时 spurious wakeup，但正确）。

**不存储 pollfd 在内核**：pollfd 数组始终在用户空间，每次 wake 后用户态 re-read。不需要 per-fd `poll_pid` 链表。

**复用现有 timer_queue**：`wait_deadline` 已支持超时唤醒，poll 直接复用 `wait_deadline` + `timer_queue` 机制实现 timeout。

### shutdown

```c
shutdown(fd, how):
    - SHUT_RD (0): 设置 shutdown_read = 1，清空 recv_queue（kfree 所有 skb）
    - SHUT_WR (1): 设置 shutdown_write = 1，对端读 socket 看到 EOF（队列空后返回 0）
    - SHUT_RDWR (2): 两者都做
    — 唤醒 blocked_reader / blocked_writer
```

### SCM_RIGHTS fd 传递（Lazy 安装）

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
    // 在接收进程 fd_table 中安装 fd（持 socket_lock 操作）
    // 注意：安装的是发送方进程的 fd 对应的资源
    // 但 fd 号是新的（接收方分配空闲 slot）
    for (int i = 0; i < skb->num_fds; i++) {
        int orig_fd = skb->fds[i];
        // 在接收方 fd_table 找空闲 slot
        int new_fd = find_unused_fd(current_proc);
        if (new_fd < 0) {
            msg->msg_flags |= MSG_CTRUNC;
            break;
        }
        // 复制发送方的 struct file 到接收方
        // 需要从 skb 或发送进程上下文获取原始 file 信息
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

### EOF / EPIPE 行为

| 场景 | 行为 |
|------|------|
| 对端 close，本端 recv | 剩余数据先返回 → recv_queue 空时返回 0 (EOF) |
| 对端 close，本端 send | 对端 socket 已 CLOSED → 返回 -EPIPE |
| listen socket close | backlog 中未 accept 的连接全部关闭（通知对端 EOF） |
| 本端 close | shutdown_write=1，清空 recv_queue，唤醒 blocked_reader/writer |
| connect 找不到 listener | -ENOENT |
| backlog 满 | -ECONNREFUSED |

## 内核 fd_table 扩展

当前 `struct file` 支持 FD_PIPE/FD_SHM/FD_DEV/FD_FILE，新增 FD_SOCKET：

```c
#define FD_NONE   0
#define FD_PIPE   1
#define FD_SHM    2
#define FD_DEV    3
#define FD_FILE   4
#define FD_SOCKET 5

struct file {
    int type;            // FD_NONE / FD_PIPE / FD_SHM / FD_DEV / FD_FILE / FD_SOCKET
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

为了兼容 POSIX（`write()`/`read()` 在 socket fd 上必须有效），内核的 `sys_write`/`sys_read` 增加 FD_SOCKET 分支。内部通过一个共享的 `sock_sendmsg_internal` 辅助函数实现，避免与 `sendmsg`/`recvmsg` 的 msghdr 解析代码重复：

```c
// sock_sendmsg_internal — sendmsg 和 sock_write 的公共路径
// 构造单 iovec 的 internal msghdr，调用 sendmsg 内部逻辑
static int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len) {
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    // 调用内部 sendmsg 核心函数（无 msghdr 解析开销）
    return sock_sendmsg_internal(sock, &iov, 1, NULL, 0);
}

static int64_t sock_read(struct unix_sock *sock, void *buf, size_t len) {
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    return sock_recvmsg_internal(sock, &iov, 1, NULL, 0);
}
```

SCM_RIGHTS 必须走 `sendmsg`/`recvmsg` 接口（附属数据从 msg_control 传递），`write`/`read` 不传递 fd。

## 与现有 IPC 机制的关系

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

### 不替换的机制

- `sys_req/sys_resp`（56B 内联 RPC）：socket 的 skb 分配/链式管理开销大于内联零开销，保留
- `sys_notify`：纯通知无数据，保留
- 驱动 SHM + notify（kbd↔terminal, KMS↔terminal）：性能关键路径的共享内存直接访问，保留

### ~~sys_msg/sys_msg_resp~~ 已标记废弃

`sys_msg/sys_msg_resp` 在 socket 稳定后由 socket 替代：

- fs_driver 当前通过 sys_msg 与客户端通信（文件 open/read/write/close）
- 迁移后：fs_driver 变为 listen socket `/dev/fs` → `accept` 每客户端独立连接
- 每个客户端通过 `sendmsg(fs_fd, req)` / `recvmsg(fs_fd, reply)` 进行文件 I/O
- session 路由（当前多客户端复用 fs_msg 号区分 session_id）被 accept 原生多连接替代
- **优势**：更干净的每客户端状态隔离 + SCM_RIGHTS fd 传递支持（未来）

迁移时机：socket 实现稳定并通过验证后。当前不阻塞 Wayland 开发。

### Socket vs 现有 IPC 选择指南

| 场景 | 推荐路径 |
|------|---------|
| Wayland compositor ↔ client | socket（sendmsg/recvmsg + SCM_RIGHTS）|
| 通用双向字节流 + fd 传递 | socket |
| 小载荷同步请求-响应（≤56B） | sys_req/sys_resp（保留） |
| 纯通知 | sys_notify（保留） |
| 批量数据 IPC（键盘事件、display buffer） | SHM + notify（保留） |
| 驱动间控制信令（bind/unbind） | sys_req/sys_resp（保留） |
| 文件 I/O（fs_driver ↔ 客户端） | 当前 sys_msg → 待迁移到 socket |

## syscall 接口定义（与 Linux 对齐）

### 新增 syscall 号

实际当前 syscall 已到 36（SYS_INSTALL_FD），因此 socket syscall 从 37 开始：

```c
#define SYS_SOCKET    37   // -> socket(int domain, int type, int protocol)
#define SYS_BIND      38   // -> bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_LISTEN    39   // -> listen(int fd, int backlog)
#define SYS_ACCEPT    40   // -> accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
#define SYS_CONNECT   41   // -> connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_SOCKETPAIR 42  // -> socketpair(int domain, int type, int protocol, int sv[2])
#define SYS_SENDMSG   43   // -> sendmsg(int fd, const struct msghdr *msg, int flags)
#define SYS_RECVMSG   44   // -> recvmsg(int fd, struct msghdr *msg, int flags)
#define SYS_SHUTDOWN  45   // -> shutdown(int fd, int how)
#define SYS_POLL      46   // -> poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
```

NR_SYSCALL = 47（从 0 到 46）。

### 数据结构定义（Linux UAPI 兼容）

以下定义直接对应 Linux x86-64 UAPI，包含精确的位宽和 padding。

```c
// === sockaddr_un ===
struct sockaddr_un {
    uint16_t sun_family;         // AF_UNIX = 1
    char     sun_path[108];      // 文件系统路径或抽象路径（\0 开头）
};

// === iovec ===
struct iovec {
    void  *iov_base;    // 缓冲区地址
    size_t iov_len;     // 缓冲区长度
};

// === cmsghdr（Linux UAPI x86-64 精确布局）===
// cmsg_len 为 socklen_t（32-bit），不是 size_t
#define CMSG_ALIGN(len)     (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg)     ((void *)(((char *)(cmsg)) + sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(msg, cmsg)  /* 下一个 cmsg，无则 NULL */
#define CMSG_FIRSTHDR(msg)      /* 第一个 cmsg */

// === msghdr（Linux UAPI x86-64 精确布局）===
struct msghdr {
    void       *msg_name;        // 地址（可选）
    socklen_t   msg_namelen;     // 4-byte
    unsigned    __pad0;          // 填充 4-byte
    struct iovec *msg_iov;       // scatter/gather 数组
    size_t      msg_iovlen;      // iovec 中元素个数
    void       *msg_control;     // 辅助数据（SCM_RIGHTS 放这里）
    size_t      msg_controllen;  // sizeof(struct cmsghdr) + n * sizeof(int)
    int         msg_flags;       // recvmsg 时输出 MSG_EOR/MSG_TRUNC/MSG_CTRUNC
};

// === pollfd ===
struct pollfd {
    int   fd;            // 要监听的文件描述符
    short events;        // 请求的事件位掩码（POLLIN/POLLOUT）
    short revents;       // 返回的事件位掩码
};

// === 常量 ===
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

libwayland 对这些 syscall 的封装在 `src/wayland-client.c` / `src/wayland-server.c` 中。
syscall 号不同时需要薄适配层（或在 `sys.h` 中映射）。

## 锁设计

### 全局 socket_lock

初期使用一把全局 `spinlock_t socket_lock`（no irqsave），覆盖：
- `struct unix_sock` 所有字段的读写
- bind 名字空间表的插入/查找/删除
- skb 链表的入队/出队
- SCM_RIGHTS fd 安装时的 fd_table 操作

**锁顺序**（与现有锁一致）：`procs_lock` → `socket_lock` → `scheduler_lock`

**反例（必须避免）**：持 scheduler_lock 后再拿 socket_lock — 会被迫逆序。

**`wake_process` 在 socket_lock 外调用**：sendmsg/recvmsg 操作完 skb 队列后，先释放 socket_lock，再调 `wake_process(blocked_reader)`。避免 socket_lock → scheduler_lock 的逆序。

### 后续升级

todo.md 中记录：后续升级为 **per-socket spinlock** + **per-hash-bucket spinlock**（bind 名字空间）。当全局 socket_lock 成为性能瓶颈时执行。

## 影响分析

### 需要修改的文件

| 文件 | 修改内容 |
|------|---------|
| `common/syscall.h` | 新增 SYS_SOCKET ~ SYS_POLL 宏定义（37-46）；更新 NR_SYSCALL |
| `kernel/trap.cc` | syscall_dispatch 中新增 10 个 case；扩展 NR_SYSCALL=47 |
| `kernel/socket.cc`, `kernel/socket.h` | 核心 socket 实现（新文件） |
| `kernel/proc.h` | `struct file` 增加 FD_SOCKET 类型 + `struct unix_sock *sock`；`enum wait_event_t` 增加 `WAIT_POLL` |
| `kernel/proc.cc` | proc_reap 中增加 FD_SOCKET 清理；wake_process 扩展 WAIT_POLL |
| `kernel/trap.cc`（wake_process） | 条件扩展为 `wait_event == WAIT_PIPE || wait_event == WAIT_POLL` |
| `arch/x64/utils.h` | 无需修改（`__syscallN` 已支持 6 参数）|

### libc 封装

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

libc `user/lib/file.cc` 中 `read()/write()` 分发增加 FD_SOCKET 分支 → 委托内核 `sys_write/read` 处理。

### 数据结构定义位置

| 定义 | 位置 | 使用方 |
|------|------|--------|
| `struct sockaddr_un` | `common/syscall.h` 或新版 `common/socket.h` | 内核 + libc |
| `struct iovec` | `common/syscall.h` | 内核 + libc |
| `struct msghdr` | `common/syscall.h` 或 `common/socket.h` | 内核 + libc |
| `struct cmsghdr` + CMSG_* 宏 | `common/syscall.h` 或 `common/socket.h` | 内核 + libc |
| `struct pollfd` + POLLIN/OUT | `common/syscall.h` | 内核 + libc |
| `struct sk_buff` + `struct unix_sock` | `kernel/socket.h`（内核私有） | 内核 |

## 实现步骤

```
1. [基础设施] 定义 struct sk_buff / struct unix_sock + skb 分配/释放/入队/出队 → kernel/socket.h/.cc
2. [数据定义] sockaddr_un / iovec / msghdr / cmsghdr / pollfd → common/socket.h
3. [syscall] socket/bind/listen/accept/connect（含 bind 名字空间表）→ kernel/socket.cc
4. [syscall] socketpair → kernel/socket.cc
5. [syscall] sendmsg/recvmsg（iovec 展开 + SCM_RIGHTS lazy 安装）→ kernel/socket.cc
6. [syscall] shutdown + EOF/EPIPE 边界处理 → kernel/socket.cc
7. [syscall] poll（pipe + socket 统一，WAIT_POLL 模型）→ kernel/trap.cc / kernel/socket.cc
8. [sys_write/read] FD_SOCKET 分支（sock_write/sock_read 内部辅助函数）→ kernel/trap.cc / kernel/socket.cc
9. [proc] WAIT_POLL + FD_SOCKET 引用计数 + proc_reap 清理 → kernel/proc.h / kernel/proc.cc
10. [wake_process] 扩展 WAIT_POLL 唤醒条件 → kernel/trap.cc
11. [syscall_dispatch] trap.cc 中注册所有新 syscall（37-46），NR_SYSCALL=47
12. [libc] socket.h 头文件 + libc 封装
13. [集成] terminal ↔ shell pipe → socketpair 替换（可选，Wayland 就绪后）
14. [验证] Wayland compositor + client 通信
```
