# Unix Domain Socket 设计与实现

## 概述

为支持 Wayland 合成器/客户端通信及通用进程间双向字节流通信，在内核态实现 AF_UNIX SOCK_STREAM socket。接口语义与 Linux 对齐（`struct msghdr` + `struct iovec` + SCM_RIGHTS），便于复用 Linux 生态（libwayland 移植）。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | Socket 类型 | AF_UNIX, SOCK_STREAM | Wayland 核心需求，双向字节流 |
| 2 | 实现层级 | 内核态（`kernel/socket.cc`） | SCM_RIGHTS fd 传递只能内核做；socket 本质是 pipe 的超集，复用 pipe 阻塞/唤醒机制 |
| 3 | API 范围 | `socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown` + `poll` | 完整 Unix domain socket 生命周期 |
| 4 | 数据结构 | skb 链表（参考 Linux `sk_buff`） | 天然支持消息边界（`SOCK_SEQPACKET`）和 SCM_RIGHTS 附属数据；比 ring buffer 更灵活 |
| 5 | msghdr/iovec | 100% 兼容 Linux `struct msghdr` + `struct iovec` + `struct cmsghdr` | 直接复用 Linux 生态，libwayland 无需大改 |
| 6 | syscall 接口 | 每操作独立 syscall 号，语义对齐 Linux | fd 操作同类，独立 syscall 更清晰 |
| 7 | 连接模型 | 无三次握手，`connect` 直接建立（参考 Linux AF_UNIX） | Unix domain socket 在同一内核，无需复杂握手 |
| 8 | 消息分帧 | SOCK_STREAM 不保留消息边界（与 Linux 一致） | libwayland 自己处理消息分帧 |

## 数据结构

### sk_buff（socket buffer）

```c
#define MAX_SKB_DATA 4096

struct sk_buff {
    struct sk_buff *next;       // 链表下一节点
    uint32_t len;               // 数据长度
    uint8_t  data[MAX_SKB_DATA]; // 内联数据
    // SCM_RIGHTS 附属数据
    int      num_fds;           // 附属 fd 数量（≤8）
    int      fds[8];            // 要传递的 fd 数组
    // 状态
    int      consumed;          // stream 模式下已经读取的偏移量
};
```

- 内联数据 + 附属 fd，无协议头
- `MAX_SKB_DATA = 4096`（同 pipe buf 大小，系统 page size）
- `consumed` 用于 `SOCK_STREAM` 模式下的部分读

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
    int      ref_count;              // fd 引用计数
    
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
};
```

### 连接建立流程

```
socket(AF_UNIX, SOCK_STREAM, 0):
    分配 struct unix_sock
    state = UNIX_FREE
    ref_count = 1
    返回 fd

bind(fd, addr, addrlen):
    拷贝 sockaddr_un（sun_path）
    文件系统路径绑定（预留，当前可先支持匿名/抽象路径）
    返回 0

listen(fd, backlog):
    state → UNIX_LISTEN
    记录 backlog 上限
    返回 0

connect(fd, addr, addrlen):
    根据路径/抽象地址查找 listener
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

### 数据收发（sendmsg/recvmsg）

```c
sendmsg(fd, msg, flags):
    1. 验证 fd 是 SOCK_STREAM
    2. 遍历 msg->msg_iov，计算总数据量
    3. 分配 struct sk_buff
    4. 遍历 msg->msg_iov，逐个 copy_from_user 到 skb->data
    5. 处理 msg->msg_control：
       - 解析 struct cmsghdr
       - cmsg_level == SOL_SOCKET && cmsg_type == SCM_RIGHTS：
         拷贝用户态 fd 数组 → 对端进程安装 fd（get_unused_fd + fd_install）
         将 fd 数组保存到 skb->fds[]
       - 其他 cmsg_type 返回 -EINVAL
    6. 将 skb 挂到对端（peer）的 recv_queue
    7. 唤醒对端的 blocked_reader
    8. 如果对端 recv_queue 过长（> 64KB），当前 writer 可阻塞

recvmsg(fd, msg, flags):
    1. 验证 fd 是 SOCK_STREAM
    2. 从 recv_queue 头部取一个 skb
    3. 遍历 msg->msg_iov，逐个 copy_to_user 到用户 buffer
       - SOCK_STREAM：可消费部分 skb 数据（更新 consumed）
       - 全部消耗完则弹出 skb 并 kfree
    4. 处理 msg->msg_control：
       - 如果有附属 fd，cmsg 格式写入 msg_control
       - msg_controllen 不够时设 MSG_CTRUNC
    5. msg->msg_flags 输出（MSG_EOR/MSG_TRUNC 等）
    6. 写数据消耗完毕后唤醒对端的 blocked_writer
    7. recv 完成
```

### poll

```c
poll(fds, nfds, timeout_ms):
    1. 遍历 pollfd 数组
    2. 对每个 fd：
       - 检查 fd 类型（pipe/socket/其他）
       - socket: recv_queue 不空 → POLLIN；未 shutdown_write → POLLOUT
       - pipe: pipe buf 不空 → POLLIN；未满 → POLLOUT
       - 其他 fd 类型：根据 read/write 状态判断
    3. 有就绪 fd → 立即返回
    4. 全部未就绪 → 阻塞（复用 WAIT_RECV 机制 + timeout）
    5. timeout 到期返回 POLLIN/POLLOUT 仍然为 0
```

### shutdown

```c
shutdown(fd, how):
    - SHUT_RD (0):  设置 shutdown_read = 1, 清空 recv_queue（kfree skb）
    - SHUT_WR (1):  设置 shutdown_write = 1, 对端读 socket 看到 EOF
    - SHUT_RDWR (2): 两者都做
```

### SCM_RIGHTS fd 传递

SCM_RIGHTS 是 Wayland 的核心需求（共享 buffer 通过 fd 传递）。

**发送方流程（sendmsg 中）：**

```c
// 从 msg_control 中解析 cmsghdr
for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
     cmsg != NULL;
     cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        if (cmsg->cmsg_len < sizeof(struct cmsghdr) + sizeof(int))
            return -EINVAL;
        int *fds = (int *)(cmsg + 1);
        int num_fds = (cmsg->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);
        if (num_fds > 8) return -EINVAL;
        
        // 对端进程安装 fd（在目标进程的 fd_table 中分配）
        for (int i = 0; i < num_fds; i++) {
            // 验证 fds[i] 是发送方的有效 fd
            // 调用 proc_install_fd(peer_pid, file_ptr) 在接收进程安装
            skb->fds[skb->num_fds++] = new_fd_in_target;
        }
    }
}
```

**接收方流程（recvmsg 中）：**

```c
if (skb->num_fds > 0) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_len = sizeof(struct cmsghdr) + skb->num_fds * sizeof(int);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    int *fds = (int *)(cmsg + 1);
    for (int i = 0; i < skb->num_fds; i++) {
        fds[i] = skb->fds[i];
    }
}
```

**关键安全约束：**
- 发送方只能用 SCM_RIGHTS 传递自己拥有的 fd
- fd 安装到接收方进程时需要验证 fd 合法性
- 引用计数管理：pipe ref_count++ / SHM ref_count++ 等

## 内核 fd_table 扩展

当前 `struct file` 支持 FD_PIPE/FD_SHM/FD_DEV，新增 FD_SOCKET：

```c
#define FD_NONE   0
#define FD_PIPE   1
#define FD_SHM    2
#define FD_DEV    3
#define FD_SOCKET 4

struct file {
    int type;               // FD_NONE / FD_PIPE / FD_SHM / FD_DEV / FD_SOCKET
    int flags;              // O_RDONLY / O_WRONLY / O_RDWR / O_NONBLOCK
    struct pipe *pipe;      // FD_PIPE
    struct shm  *shm;       // FD_SHM
    pid_t target_pid;       // FD_DEV
    struct unix_sock *sock; // FD_SOCKET
};
```

### sys_write/sys_read 对 FD_SOCKET 的支持

为了兼容现有 fd 抽象（libc 的 `write()` → `sys_write()`），FD_SOCKET 也通过 `sys_write`/`sys_read` 访问：

```c
static int64_t sock_write(struct unix_sock *sock, const void *buf, size_t len) {
    // 内部构造 skb，走 sendmsg 路径
}

static int64_t sock_read(struct unix_sock *sock, void *buf, size_t len) {
    // 从 recv_queue 读，走 recvmsg 路径（单 iov）
}
```

但 SCM_RIGHTS 必须走 `sendmsg`/`recvmsg` 接口。

## 与现有 pipe 的关系

| 特性 | pipe | socket (SOCK_STREAM) |
|------|------|---------------------|
| 方向 | 单向 | 双向 |
| 数据结构 | ring buffer（4KB） | skb 链表 |
| 阻塞管理 | read_pid/write_pid | blocked_reader/blocked_writer |
| SCM_RIGHTS | 不支持 | 支持（Wayland 关键） |
| 命名 | 匿名（只能 socketpair） | 匿名 + 命名（bind+listen+connect） |
| 多客户端 | 不支持 | 支持（accept） |
| poll | 需单独实现 | 统一 poll |

### pipe 替换为 socketpair 的可行性

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

**不替换的机制：**
- `sys_req/sys_resp`（56B 内联 RPC）：socket 的流式开销大于内联零开销
- `sys_msg/sys_msg_resp`（fs_driver 文件 RPC）：同步请求-响应语义，socket 是流式，转换无意义
- `sys_notify`：纯通知无数据，保留
- 驱动 SHM + notify（kbd↔terminal, KMS↔terminal）：性能关键路径，保留

## syscall 接口定义（与 Linux 对齐）

### 新增 syscall 号

```c
#define SYS_SOCKET    36   // -> socket(int domain, int type, int protocol)
#define SYS_BIND      37   // -> bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_LISTEN    38   // -> listen(int fd, int backlog)
#define SYS_ACCEPT    39   // -> accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
#define SYS_CONNECT   40   // -> connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
#define SYS_SOCKETPAIR 41  // -> socketpair(int domain, int type, int protocol, int sv[2])
#define SYS_SENDMSG   42   // -> sendmsg(int fd, const struct msghdr *msg, int flags)
#define SYS_RECVMSG   43   // -> recvmsg(int fd, struct msghdr *msg, int flags)
#define SYS_SHUTDOWN  44   // -> shutdown(int fd, int how)
#define SYS_POLL      45   // -> poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
```

NR_SYSCALL = 46（从 36 开始，前面 0-35 保留给现有 syscall）。

### 数据结构定义（与 Linux 兼容）

```c
// === sockaddr_un ===
struct sockaddr_un {
    uint16_t sun_family;         // AF_UNIX = 1
    char     sun_path[108];      // 文件系统路径
};

// === iovec ===
struct iovec {
    void  *iov_base;    // 缓冲区地址
    size_t iov_len;     // 缓冲区长度
};

// === msghdr ===
struct msghdr {
    void       *msg_name;        // 地址（可选）
    socklen_t   msg_namelen;     // 地址长度
    struct iovec *msg_iov;       // scatter/gather 数组
    size_t      msg_iovlen;      // iovec 中元素个数
    void       *msg_control;     // 辅助数据（SCM_RIGHTS 放这里）
    size_t      msg_controllen;  // sizeof(struct cmsghdr) + n * sizeof(int)
    int         msg_flags;       // recvmsg 时输出 MSG_EOR/MSG_TRUNC/MSG_CTRUNC
};

// === cmsghdr ===
struct cmsghdr {
    size_t  cmsg_len;    // 包含头的数据总长
    int     cmsg_level;  // SOL_SOCKET
    int     cmsg_type;   // SCM_RIGHTS
    // 后面紧跟数据: int fd[]
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

// CMSG 辅助宏（与 Linux 兼容）
#define CMSG_ALIGN(len)     (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg)     ((void *)(((char *)(cmsg)) + sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(msg, cmsg)  /* 下一个 cmsg，无则 NULL */
#define CMSG_FIRSTHDR(msg)      /* 第一个 cmsg */
```

## 与 Wayland 的关系

Wayland 协议的核心 socket 需求：

1. **`wl_display_connect()`** → `socket(AF_UNIX, SOCK_STREAM, 0)` + `connect()` 到 `$XDG_RUNTIME_DIR/wayland-0`
2. **事件循环** → `poll()` 等待 socket 可读
3. **消息收发** → `sendmsg()`/`recvmsg()` 传递序列化协议数据
4. **Buffer 共享** → `SCM_RIGHTS` fd 传递（wayland 客户端通过 fd 引用共享内存/DRM buffer）

libwayland 对这些 syscall 的封装在 `src/wayland-client.c` / `src/wayland-server.c` 中。syscall 号不同时需要薄适配层。

## 影响分析

### 需要修改的文件

| 文件 | 修改内容 |
|------|---------|
| `common/syscall.h` | 新增 SYS_SOCKET ~ SYS_POLL 宏定义（36-45）；更新 NR_SYSCALL |
| `kernel/trap.cc` | syscall_dispatch 中新增 10 个 case |
| `kernel/` 新文件 `socket.cc`, `socket.h` | 核心 socket 实现 |
| `kernel/proc.h` | `struct file` 增加 FD_SOCKET + `struct unix_sock *sock` |
| `kernel/proc.cc` | proc_reap 中增加 FD_SOCKET 清理 |
| `common/dev.h` | 无需修改 |
| `arch/x64/utils.h` | 无需修改（`__syscallN` 已支持 6 参数） |

### libc 封装（后续扩展）

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

libc `user/lib/file.cc` 中 `read()/write()` 分发增加 FD_SOCKET 分支。

## 实现步骤

```
1. [基础设施] 定义 struct sk_buff / struct unix_sock → kernel/socket.h
2. [基础设施] 实现 skb 分配/释放/入队/出队 → kernel/socket.cc
3. [syscall] socket/bind/listen/accept/connect → kernel/socket.cc
4. [syscall] socketpair → kernel/socket.cc
5. [syscall] sendmsg/recvmsg（含 iovec 展开 + SCM_RIGHTS）→ kernel/socket.cc
6. [syscall] shutdown → kernel/socket.cc
7. [syscall] poll（pipe + socket 统一）→ kernel/trap.cc / kernel/socket.cc
8. [proc] FD_SOCKET 引用计数 + proc_reap 清理 → kernel/proc.cc
9. [syscall_dispatch] trap.cc 中注册所有新 syscall
10. [libc] socket.h 头文件 + libc 封装
11. [集成] terminal ↔ shell pipe → socketpair 替换（可选，Wayland 就绪后）
12. [验证] Wayland compositor + client 通信
```
