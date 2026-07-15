# IPC 机制统一设计

## 当前架构设计

微内核四层 IPC + SHM 共享内存 + 信号机制，所有进程间通信统一设计。

| 机制 | 载荷 | 传输方式 | 用途 |
|------|------|---------|------|
| sys_req / sys_resp | ≤56B | 内联，零分配 | 控制信令（bind/unbind）、ioctl IPC proxy |
| sys_msg / sys_msg_resp | 变长（≤64KB） | 内核 kmalloc 中转拷贝 | FD_FILE 代理文件 I/O（过渡保留） |
| AF_UNIX SOCK_STREAM socket | 双向字节流 + SCM_RIGHTS | skb 链表 | Wayland、通用 IPC |
| AF_NETLINK socket | datagram 多播事件 | skb 链表 + group 注册表 | uevent 广播（udev），详见 [netlink.md](netlink.md) |
| pipe | 匿名单向 | ring buffer（4KB） | terminal ↔ shell |
| SHM | 共享内存页 | fd + mmap | 批量数据零拷贝（键盘、display buffer） |
| signal | 异步通知 | sigframe 栈帧投递 | Ctrl+C、异常翻译、SIGCHLD |

### IPC 选择指南

| 场景 | 推荐路径 |
|------|---------|
| Wayland compositor ↔ client | socket（sendmsg/recvmsg + SCM_RIGHTS） |
| 通用双向字节流 + fd 传递 | socket |
| 小载荷同步请求-响应（≤56B） | sys_req/sys_resp |
| 纯通知 | sys_notify |
| 批量数据 IPC（键盘事件、display buffer） | SHM + notify |
| 驱动间控制信令（bind/unbind） | sys_req/sys_resp |
| 用户态驱动 ioctl | sys_ioctl → 内核 IPC proxy 到 req/resp |

### Linux IPC 性能参考（本机实测）

| IPC 方式 | 单次往返延迟 | 大文件吞吐量 | 多客户端支持 | 同步 / 消息边界 |
|----------|-------------|-------------|-------------|----------------|
| mmap 共享内存 + 锁 | 0.3~0.8 μs | 90+ GB/s | 需自行实现 | 无，需手动分割 |
| Unix Socket STREAM | 1.2~3 μs | 40~60 GB/s | 原生 listen/accept | 流式 |
| Unix Socket DGRAM | 0.9~2.2 μs | 35~50 GB/s | 无连接多客户端 | 天然消息边界 |
| Pipe/FIFO | 1.8~4 μs | 25~40 GB/s | 不支持多客户端 | 流式 |
| 本地 TCP 127.0.0.1 | 3~8 μs | 15~25 GB/s | 原生支持 | 流式 |
| POSIX 消息队列 | 10~30 μs | <5 GB/s | 支持 | 天然消息 |

### 主流微内核 IPC 参考

| 系统 | 内核原生 IPC | 上层兼容 | 大块数据 |
|------|------------|---------|---------|
| Windows NT | LPC（端口式消息） | socket、管道 | Section（共享内存） |
| XNU (macOS/iOS) | Mach Port 消息 | BSD socket/pipe/signal | 共享内存 |
| 鸿蒙 | 快速 IPC 消息通道 | UDS | 共享内存零拷贝 |
| Minix/QNX | 端口消息传递（唯一 IPC） | 用户态模拟 pipe/socket | — |

---

# 一、REQ/RESP + MSG/MSG_RESP

双层同步请求-响应 IPC 机制，所有事件（IRQ/REQ/NOTIFY/MSG）统一通过 per-process recv 队列接收，`sys_recv` 一次调用可多路复用。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IPC 模型 | 同步请求-响应 + 统一 recv | 微内核标准模式（seL4/QNX），一次 wait 收 IRQ+REQ+NOTIFY+MSG |
| 2 | 内联消息大小 | 64 字节 | 一个缓存行，内核 memcpy 开销可忽略 |
| 3 | 消息格式 | 内核透明（raw buffer） | 微内核原则，内核只搬运不解析 |
| 4 | recv_msg 布局 | type + src + data[56] | 结构化头部 + payload；RECV_MSG 用 union 存 kmaddr/len |
| 5 | sys_req 超时 | 不加 | 目标崩溃时 proc_reap 唤醒调用者 |
| 6 | sys_resp 目标 | 内核自动关联 | 严格请求/响应配对，req_caller_pid 自动记录 |
| 7 | recv 队列 | per-process 固定 16 槽环形缓冲区 | 无动态分配，1KB/process，FIFO |
| 8 | sys_notify 语义 | 消息入队替代直接唤醒 | notify 消息进 recv 队列，recv 端统一消费 |
| 9 | 变长消息 | 内核 kmalloc 中转拷贝 | 数据必须跨地址空间，内核拷贝保证隔离；≤64KB 上限防止滥用 |
| 10 | recv 对 RECV_MSG | 扩展签名加 data_buf 参数 | 64B recv_msg 放不下变长数据，双缓冲：msg 头 + data_buf 存变长数据 |

### syscall 接口

#### sys_recv(msg, data_buf, data_buf_len, timeout_ms) — syscall #2

- 从当前进程的 recv 队列取出一条消息，拷贝 64 字节到 msg（消息头）
- RECV_MSG 时：将变长数据 copy_to_user 到 data_buf（必须 `data_buf_len >= msg.len`，否则返回 -EINVAL），并 kfree 内核缓冲区
- 非 RECV_MSG 消息（IRQ/REQ/NOTIFY）忽略 data_buf 参数，现有调用者传 `NULL, 0`
- 队列为空：设 `WAIT_RECV` 阻塞，设 timeout（0=无限）
- `recv_intr` 置位时返回 -EINTR（ISR 唤醒但无实际消息）
- 返回 0=成功，负 errno=-ETIMEDOUT/-EINTR/-EINVAL/-EFAULT

#### sys_req(pid, request, reply) — syscall #3

- 向目标进程发送 56 字节请求（从 request 拷贝到 recv_msg.data），请求入目标 recv 队列（type=RECV_REQ, src=调用者 PID）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 调用者设 `WAIT_REQ_REPLY` 阻塞，等待目标 `sys_resp`
- 目标 `sys_resp` 时，回复拷贝到 reply（长度 = `req_reply_len`，默认 RECV_MSG_SIZE），唤醒调用者
- 崩溃清理：目标进程 exit 时，`proc_reap` 扫描 `procs[]`，所有 `wait_event==WAIT_REQ_REPLY && req_target==目标PID` 的进程被唤醒，返回 -ESRCH

#### sys_resp(reply) — syscall #4

- 将回复从 reply 拷贝到当前请求调用者的 reply buffer（长度 = `caller->req_reply_len`）
- 需 CR3 切换到调用者地址空间
- 唤醒调用者（从 `WAIT_REQ_REPLY` → `READY`）
- 清除 `req_caller_pid`

#### sys_msg(target_pid, msg_buf, msg_len, reply_buf, reply_len) — syscall #20

- 向目标进程发送变长消息（≤64KB），内核 kmalloc 中转拷贝
- 调用者设 `WAIT_MSG_REPLY` 阻塞，等待目标 `sys_msg_resp`
- `msg_len ∈ [1, 65536]`，kmalloc 失败返回 -ENOMEM

#### sys_msg_resp(resp_buf, resp_len) — syscall #21

- 将变长回复拷贝到当前 msg 调用者的 reply buffer（需 CR3 切换到调用者地址空间）
- copy 长度 = `min(resp_len, msg_reply_len)`
- 唤醒调用者（从 `WAIT_MSG_REPLY` → `READY`）

#### sys_notify(pid) — syscall #17

- 向目标进程的 recv 队列入一条消息（type=RECV_NOTIFY, src=调用者 PID, data=空）
- 如果目标正在 `sys_recv` 等待（`WAIT_RECV`），唤醒它
- 队列满时返回 -EBUSY

### recv_msg 结构

定义：common/syscall_nums.h : recv_msg_t

类型常量：RECV_IRQ=0, RECV_REQ=1, RECV_NOTIFY=2, RECV_MSG=3

字段：
  type : uint32_t — RECV_IRQ / RECV_REQ / RECV_NOTIFY / RECV_MSG
  src : uint32_t — IRQ 号或发送者 PID
  union:
    data[56] : uint8_t — RECV_IRQ / RECV_REQ / RECV_NOTIFY 的载荷
    msg : { kmaddr : void*, len : size_t } — RECV_MSG 内核内部（用户态不可见）

### task_t IPC 相关字段

定义：kernel/proc.h : task_t

**recv 队列**：
  recv_buf[16][64] : uint8_t — 16 槽 × 64 字节 = 1KB 固定缓冲区
  recv_head / recv_tail : uint32_t
  recv_lock : spinlock_t
  recv_intr : uint8_t — ISR 唤醒标志（wake_process 设，sys_recv 检查后返回 -EINTR）

**req 状态（内联载荷，≤56B）**：
  req_caller_pid : pid_t — 当前请求调用者（-1=无）
  req_reply_buf : void* — 调用者 reply buffer 用户态地址
  req_reply_len : size_t — reply buffer 大小（sys_req 路径=RECV_MSG_SIZE，ioctl proxy 路径=56）
  req_result : int32_t
  req_target_pid : pid_t — 崩溃清理用

**msg 状态（变长载荷，≤64KB）**：
  msg_reply_buf : void* / msg_reply_len : size_t
  msg_caller_pid : pid_t（-1=无） / msg_result : int32_t / msg_target_pid : pid_t

### wait_event_t

定义：kernel/proc.h : wait_event_t

WAIT_NONE / WAIT_RECV / WAIT_REQ_REPLY / WAIT_MSG_REPLY / WAIT_CHILD / WAIT_PIPE / WAIT_POLL

### 关键实现路径

#### sys_req 流程

1. 调用者 sys_req(pid, request, reply)
2. 内核 copy_from_user 请求到 recv_msg.data
3. 入队 target->recv_buf (type=RECV_REQ)，唤醒 target (WAIT_RECV → READY)
4. 阻塞调用者 (WAIT_REQ_REPLY)
5. 服务端 recv() 返回 RECV_REQ，处理请求
6. 服务端 sys_resp(reply)：CR3 切换 → copy_to_user → 唤醒调用者 (WAIT_REQ_REPLY → READY)
7. 调用者恢复，返回 0 或负 errno

#### sys_msg 流程

1. 调用者 sys_msg(pid, msg_buf, msg_len, reply_buf, reply_len)
2. 内核验证参数，kmalloc(msg_len)，copy_from_user
3. 入队 RECV_MSG(kmaddr, len)，唤醒 target (WAIT_RECV → READY)，阻塞调用者 (WAIT_MSG_REPLY)
4. 服务端 recv() 返回 RECV_MSG：内核 copy_to_user(data_buf, kmaddr, len)，kfree(kmaddr)，记录 msg_caller_pid
5. 服务端 sys_msg_resp(resp_buf, resp_len)：copy_from_user → CR3 切换 → copy_to_user → kfree(kbuf_resp) → 唤醒调用者

#### IRQ 分发

IRQ 到达时向绑定进程的 recv 队列入 RECV_IRQ 消息，然后唤醒（如果进程在 WAIT_RECV）。详见 kernel/trap.c : trap_dispatch

#### sys_notify

向目标 recv 队列入 RECV_NOTIFY 消息 + 唤醒 WAIT_RECV 进程。

#### 唤醒原语（内核内部）

三个收敛在 `kernel/xcore/sched.h` 的 inline，统一"持 scheduler_lock + 检查 + wake_from_wait"语义，避免各处 open-code：

- **`wake_process(pid)`**（窄语义，pipe/socket/PTY 等 IPC 用）：仅处理 `WAIT_PIPE / WAIT_POLL / WAIT_RECV`，命中其它 event 触发 `ASSERT` panic（约束代码化：调用者若需唤醒其它 event 应改用 `wake_with_event`；若需打断任意阻塞态应改用 `wake_process_any`）。`WAIT_RECV` 时额外设 `recv_intr`，被 `wake_process` 唤醒的 `sys_recv` 检查后返回 `-EINTR`。
- **`wake_with_event(target, expected_event)`**（精确 event 匹配）：仅当 target 处于 `BLOCKED` 且 `wait_event == expected_event` 时唤醒。futex WAKE 路径用 `wake_with_event(t, WAIT_FUTEX)`。
- **`wake_process_any(target)`**（中断任意阻塞态）：不区分 event，无条件唤醒 `BLOCKED` 的 target。signal 投递路径（`deliver_signal_to`/`sys_kill`/`sys_tgkill`）及扫任意 BLOCKED 线程投递信号的循环（`proc.c` SIGHUP、`pty.c` SIGWINCH）用此——signal 需能打断 `WAIT_FUTEX`（pthread_cancel 路径）等任意等待。

#### proc_reap 崩溃清理

进程退出时清理所有 IPC 状态：

1. **req 调用者**：扫描所有进程，找到 `wait_event==WAIT_REQ_REPLY && req_target_pid==exiting_pid` 的进程，唤醒并设 `req_result=-ESRCH`
2. **msg 调用者**：扫描所有进程，找到 `wait_event==WAIT_MSG_REPLY && msg_target_pid==exiting_pid` 的进程，唤醒并设 `msg_result=-ESRCH`
3. **req 服务端**：`req_caller_pid >= 0` 时清除为 -1
4. **msg 服务端**：`msg_caller_pid >= 0` 时清除为 -1
5. **recv 队列中的 RECV_MSG**：遍历 recv_buf，对 type==RECV_MSG 的项 kfree(kmaddr)

### libc 封装

定义：user/include/sys/ipc.h

recv(req_msg, data_buf, data_buf_len, timeout_ms) / req(pid, req_buf, resp_buf) / resp(resp_buf) / msg(pid, req_buf, req_len, resp_buf, resp_len) / msg_resp(resp_buf, resp_len)

fd-based 变体：notify_fd(fd) — 从 fd_table 查 target_pid 后调 sys_notify；msg_fd(fd, ...) — 从 fd_table 查 target_pid 后调 sys_msg

---

# 二、AF_UNIX SOCK_STREAM Socket

### 概述

为支持 Wayland 合成器/客户端通信及通用进程间双向字节流通信，在内核态实现 AF_UNIX SOCK_STREAM socket。接口语义与 Linux 对齐（struct msghdr + struct iovec + SCM_RIGHTS），便于复用 Linux 生态。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | Socket 类型 | AF_UNIX, SOCK_STREAM | Wayland 核心需求，双向字节流 |
| 2 | 实现层级 | 内核态（kernel/socket.c） | SCM_RIGHTS fd 传递只能内核做 |
| 3 | API 范围 | socket/bind/listen/accept/connect/socketpair/sendmsg/recvmsg/shutdown + poll | 完整 Unix domain socket 生命周期 |
| 4 | 数据结构 | skb 链表 | 可变大小 skb，天然支持 SCM_RIGHTS 附属数据 |
| 5 | msghdr/iovec | 100% 兼容 Linux UAPI | 直接复用 Linux 定义，libwayland 无需大改 |
| 6 | syscall 接口 | 每操作独立 syscall 号（30-39） | fd 操作同类，独立 syscall 更清晰 |
| 7 | 连接模型 | 无三次握手，connect 直接建立 | Unix domain socket 在同一内核 |
| 8 | 消息分帧 | SOCK_STREAM 不保留消息边界 | 与 Linux 一致 |
| 9 | SCM_RIGHTS 安装时机 | Lazy（recvmsg 时安装 fd） | 与 Linux 对齐。skb 持有 fd 资源引用 |
| 10 | skb 大小 | 动态 kmalloc，软上限 64KB | 防恶意 OOM |
| 11 | 锁 | 全局 socket_lock（spinlock_t，no irqsave） | 锁顺序：procs_lock → socket_lock → scheduler_lock |
| 12 | Socket 命名 | bind 在 `/run` tmpfs 上 mknod 建 INODE_SOCKET inode（对齐 Linux `vfs_mknod(SOCKFS)`），sock 挂 inode->i_priv；`/run` 未挂载时降级哈希表占名 | VFS socket 文件 stat/ls 可见，connect 走 path 解析；哈希表保留作降级 |

### 数据结构

#### sk_buff（socket buffer）

定义：kernel/socket.h : sk_buff

字段：
  next : sk_buff* — 链表下一节点
  len : uint32_t — 数据长度（可能为 0，仅传递 fd）
  consumed : int — SOCK_STREAM 模式下已读取的偏移量
  num_fds : int — 附属 fd 数量（≤ MAX_SCM_FDS=8）
  fds[MAX_SCM_FDS] : int — 发送方 fd 号（recvmsg 时安装到接收方）
  data[] : uint8_t — 灵活数组成员（必须在末尾），kmalloc(sizeof(sk_buff) + data_len) 分配

consumed 字段用于 SOCK_STREAM 部分读：skb 留在队列中直到 consumed == len。skb 可带 len=0 + num_fds>0（纯 SCM_RIGHTS 传递）。

#### unix_sock（per-socket 内核结构）

定义：kernel/socket.h : unix_sock

状态常量：UNIX_FREE / UNIX_LISTEN / UNIX_CONNECTED / UNIX_CLOSED（UNIX_MAX_BACKLOG=8）

字段：
  state : int
  peer : pid_t — 对端 PID（CONNECTED 时有效）
  peer_sock : unix_sock* — 对端 socket 直接指针（sendmsg/recvmsg 热路径避免 PID 查找）
  ref_count : int — fd 引用计数（dup2 共享时 >1）
  recv_queue_head / recv_queue_tail : sk_buff*
  recv_queue_len : int
  blocked_reader / blocked_writer : pid_t（-1=无）
  backlog_head / backlog_tail : unix_sock* / backlog_len : int
  shutdown_read / shutdown_write : int
  sun_path[108] : char

**peer_sock**：connect/accept 时双向设置，热路径直接指针访问。

### 连接建立流程

- socket()：分配 unix_sock（state=UNIXFREE, ref_count=1, peer=-1），返回 FD_SOCKET fd
- bind()：拷贝 sun_path → `vfs_mknod_socket`（`path_walk_parent` + `i_op->create(S_IFSOCK)`）在 `/run` tmpfs 建 INODE_SOCKET inode → `ip->i_priv = sock` 挂 sock + 记 `sock->bind_inode`（bind 期间持有 inode 引用）；路径已存在返 -EADDRINUSE；`/run` 未挂载 / path 解析失败降级 `unix_bind_register_hash` 哈希表占名
- listen()：state → UNIX_LISTEN
- connect()：`vfs_lookup_socket`（path_walk 取 socket inode）→ 从 i_priv 取 listener；UNIX_LISTEN 命中建立连接，未 LISTEN 返 -ECONNREFUSED，未找到返 -ENOENT；backlog 满 -ECONNREFUSED；VFS 失败降级哈希查找。创建 child（UNIX_CONNECTED），双向设 peer/peer_sock，挂到 listener backlog，唤醒 blocked_reader
- accept()：从 backlog 取 child，分配 fd，ref_count++，返回新 fd

### socketpair

分配两个 unix_sock（UNIX_CONNECTED），双向设 peer/peer_sock，返回两个 fd。可用于替换 terminal ↔ shell 的两 pipe 拓扑。

### 数据收发（sendmsg/recvmsg）

**sendmsg**：
1. 验证 FD_SOCKET + UNIX_CONNECTED
2. 遍历 msg_iov 计算总数据量，>64KB → -EMSGSIZE
3. kmalloc(sizeof(sk_buff) + len)，copy_from_user 各 iov
4. 处理 msg_control：解析 cmsghdr，SOL_SOCKET+SCM_RIGHTS 验证 fd 有效性存 skb->fds[]
5. skb 挂到 peer_sock->recv_queue 尾部
6. 释放 socket_lock 后 wake_process 唤醒 blocked_reader
7. 返回总数据长度

**recvmsg**：
1. 验证 FD_SOCKET，从 recv_queue 取第一个 skb
2. recv_queue 空：对端 shutdown_write/CLOSED → 返回 0 (EOF)，否则阻塞（WAIT_POLL → schedule）
3. 遍历 msg_iov copy_to_user：SOCK_STREAM 可部分消费（更新 consumed），consumed==len 时弹出 kfree
4. SCM_RIGHTS lazy 安装：遍历 skb->fds[]，在接收方 fd_table 找空 slot，复制 struct file 并增加资源引用计数，写入 msg_control
5. 唤醒 blocked_writer，返回读取字节数

### poll

实现：kernel/bsd/socket.c : sys_poll（就绪判断委托 file_poll）

1. 遍历 pollfd 数组，对每个 fd 调 `file_poll(f, events)`（kernel/bsd/file_poll.c）得到 revents——per-type 就绪逻辑统一在 file_poll，sys_poll/epoll_ctl/epoll_wait 共用
2. 有就绪 fd → 立即返回；全部未就绪且 timeout=0 → 返回 0
3. 全部未就绪且 timeout>0 → 对每个 fd 在其 `file_wq_get(f)` 上注册 wait_queue_t（poll_wait_cb→wake_with_event(proc, WAIT_POLL)），设 WAIT_POLL + wait_deadline → schedule
4. 被唤醒后移除 wait 注册、重新遍历 pollfd 调 file_poll → 更新 revents → 返回

阻塞模型：WAIT_POLL 状态。数据到达点调 `__wake_up(&f->wq, POLLIN)` 触发 wait_queue 回调唤醒阻塞的 poll 进程。file_poll 与 wait_queue 机制详见 [epoll.md](epoll.md)。

### shutdown

SHUT_RD：shutdown_read=1，清空 recv_queue（kfree 所有 skb）
SHUT_WR：shutdown_write=1，对端读看到 EOF
SHUT_RDWR：两者都做
唤醒 blocked_reader / blocked_writer

### SCM_RIGHTS fd 传递

**发送方（sendmsg）**：验证 fd 有效性，存原始 fd 号到 skb->fds[]，不跨进程复制。

**接收方（recvmsg）**：lazy 安装——在接收方 fd_table 找空闲 slot，复制 struct file 并增加资源引用计数（pipe->ref_count++、shm_get、unix_sock_acquire 等）。

**资源保活**：skb 入队到 recvmsg 期间，发送方 close 或 exit 不释放 skb 引用到的资源（引用计数保证）。proc_reap 清理时遍历未消费 skb 释放引用。

### socket inode 生命周期 + unlink-while-open

bind 走 VFS 建 INODE_SOCKET inode（见上方 bind 流程），sock 挂 `inode->i_priv` 并 `inode_get` 持引用（i_count+1）→ socket inode 在 bind 期间不被释放。`unix_bind_unregister`（sock close）清 `bind_inode->i_priv = NULL` + `inode_put(bind_inode)` 平衡 bind 时的 get。

这天然对齐 Linux **unlink-while-open** 语义：`tmpfs_unlink` 只摘目录项（从父 children 链表移除）不释放 socket inode（仍有 bind 持有的 i_count 引用）；client connect 此时若已 path_walk 拿到 inode（短暂 inode_get），仍能从 i_priv 取到 sock；直到 unregister 清 i_priv + inode_put，i_count 归 0 才 kfree。socket inode 落点与 mknod 细节见 [vfs.md](vfs.md) AF_UNIX socket inode 节。

### EOF / EPIPE 行为

| 场景 | 行为 |
|------|------|
| 对端 close，本端 recv | 剩余数据先返回 → recv_queue 空时返回 0 (EOF) |
| 对端 close，本端 send | 对端 socket 已 CLOSED → 返回 -EPIPE |
| listen socket close | backlog 中未 accept 的连接全部关闭 |
| 本端 close | shutdown_write=1，清空 recv_queue，唤醒 blocked_reader/writer |
| connect 找不到 listener | -ENOENT |
| backlog 满 | -ECONNREFUSED |

### sys_write/sys_read 对 FD_SOCKET 的支持

便捷读写（不传 SCM_RIGHTS）：sock_write → sock_sendmsg_internal（单 iov，无 control）；sock_read → sock_recvmsg_internal。SCM_RIGHTS 必须走 sendmsg/recvmsg。

实现：kernel/socket.c : sock_write / sock_read

### 与其他 IPC 机制的关系

| 特性 | pipe | socket | sys_req/sys_resp | sys_msg/sys_msg_resp |
|------|------|--------|-----------------|---------------------|
| 方向 | 单向 | 双向 | REQ→RESP | MSG→MSG_RESP |
| 数据结构 | ring buffer（4KB） | skb 链表（动态大小） | 56B recv slot | kmalloc 中转（≤64KB） |
| SCM_RIGHTS | 不支持 | 支持 | 不支持 | 不支持 |
| 命名 | 匿名 | 匿名 + 命名（bind+connect） | PID 直连 | PID 直连 |
| 应用 | terminal ↔ shell | Wayland、通用 IPC | 驱动 bind/unbind | FD_FILE 代理 I/O（过渡保留） |

### syscall 编号

30=socket, 31=bind, 32=listen, 33=accept, 34=connect, 35=socketpair, 36=sendmsg, 37=recvmsg, 38=shutdown, 39=poll

### 数据结构定义位置

| 定义 | 位置 |
|------|------|
| sockaddr_un / iovec / msghdr / cmsghdr + CMSG_* 宏 / pollfd + POLLIN/OUT | common/socket.h |
| recv_msg_t + RECV_* 常量 | common/syscall_nums.h |
| sk_buff / unix_sock | kernel/socket.h（内核私有） |

### 锁设计

全局 spinlock_t socket_lock（no irqsave），覆盖：unix_sock 字段读写、bind 名字空间表、skb 队列操作、SCM_RIGHTS fd 安装。

锁顺序：procs_lock → socket_lock → scheduler_lock

wake_process 在 socket_lock 外调用：sendmsg/recvmsg 操作完 skb 队列后，先释放 socket_lock，再调 wake_process。避免 socket_lock → scheduler_lock 逆序。

**bind→VFS 锁嵌套**：bind/connect 走 VFS 建/查 socket inode 时，`unix_bind_register`/`unix_bind_lookup` 在持 `socket_lock` 下调 `vfs_mknod_socket`/`vfs_lookup_socket`，内部取 `mount_lock`（vfs_resolve）→ `inode_hash_lock`（inode_create/get）→ `tmpfs_i_lock`（tmpfs children 链表），锁序单向 `socket_lock → mount_lock → inode_hash_lock → tmpfs_i_lock`，无反向获取路径。详见 [kernel_lock.md](kernel_lock.md)。

### libc 封装

定义：user/include/sys/socket.h（static inline）

socket / bind / listen / accept / connect / socketpair / sendmsg / recvmsg / shutdown / poll

---

# 三、Pipe

pipe 为匿名单向字节流，ring buffer 大小 4KB，通过 sys_pipe 创建一对 fd（读端 + 写端）。

内核 sys_read/sys_write/sys_close 根据 fd_type 分发：
- FD_PIPE → pipe ring buffer（4KB，byte-by-byte 拷贝）
- FD_REGULAR → VFS/FAT32（inode + page cache）
- FD_SOCKET → sock_read/sock_write/sock_close
- FD_DEV → dev_ops callback（内核设备直接回调，用户态驱动 via req）
- FD_TTY → pty_read/pty_write（PTY master/slave）

pipe 未来可被 socketpair 替代（terminal ↔ shell 场景），当前保留作为简化特例。

---

# 四、SHM 共享内存

### 概述

共享内存对齐 Linux memfd 模型。用户态通过 `memfd_create` + `ftruncate` + `mmap` 创建和映射 SHM；内核内部通过 `shm_create_internal` 分配。用户态驱动 SHM 通过 `dev_create` 关联到设备 inode，consumer 通过 `open` → `mmap` 访问，与 Linux 设备 mmap 语义一致。

### 三层 SHM 分配 API

对齐 Linux shmem 分层设计（底层页分配 → SHM 结构创建 → syscall 层）：

#### shm_alloc_pages(npages) — 纯物理页分配

```c
uint64_t shm_alloc_pages(uint64_t npages);
```

- 调用 `bfc_alloc_pages(npages)` 分配连续物理页，`memset` 清零
- 返回物理地址（`phys_addr_t`）
- `shm_create_internal` 和 `sys_ftruncate` 都调用此函数分配页

#### shm_create_internal(npages) — 内核内部 SHM 创建

```c
struct shm *shm_create_internal(uint64_t npages);
```

- 调用 `shm_alloc_pages(npages)` 分配连续物理页
- 创建 `page_list`（kmalloc 数组，指向每个连续页），`phys` = 首页物理地址作为 mmap 快捷路径
- `kmalloc` 分配 `struct shm`，初始化：phys, npages, page_list, file_size=npages\*PAGE_SIZE, s_count=1, seals=0
- 不支持 npages=0——zero-size 是 `memfd_create` syscall 层的策略
- xHCI 等内核内部调用者总是知道大小，直接用此函数。page_list 中的页恰好连续，mmap 可用 `phys` 一次映射

#### syscall 层

- `sys_memfd_create(name, size)`：`size > 0` 时调用 `shm_create_internal(size/PAGE_SIZE)`，`size == 0` 时创建 file_size=0 的空 shm（kmalloc struct shm，phys=0, npages=0, page_list=NULL）
- `sys_ftruncate(fd, new_size)`：扩展时逐页调用 `shm_alloc_pages(1)`，追加到 `page_list`（对齐 Linux shmem 逐页分配）；收缩时释放尾部多余页

### 数据结构

**struct shm**（kernel/proc.h : shm）
  phys : uint64_t — 物理页起始地址（连续分配时）
  npages : size_t — 连续页数
  file_size : size_t — ftruncate 设的逻辑大小（≤ npages * PAGE_SIZE）
  s_count : refcount_t — 引用计数：fd +1，mmap vma +1
  seals : uint32_t — F_SEAL_SHRINK / F_SEAL_GROW / F_SEAL_WRITE / F_SEAL_SEAL
  name[32] : char — memfd_create 传入的调试名
  page_list : uint64_t* — 每个 entry 是 4K 页物理地址
  num_pages : int — page_list 长度

fd_table 中 FD_SHM(=6) 类型指向 struct shm。

**mmap_region**（kernel/proc.h : mmap_region）
  vaddr / size : uint64_t
  phys : uint64_t — MAP_PHYSICAL 专用
  shm_obj : shm* — mmap(SHM fd) 或 mmap(设备 fd with inode->shm) 时非 NULL
  next : mmap_region*

### 引用计数生命周期

创建时 s_count=1（fd 持有），mmap +1，close(fd) -1，munmap -1，SCM_RIGHTS 发送 +1，dup2 +1。进程 exit 遍历 fd_table 和 mmap_regions 各减一次。s_count=0 时 kfree(shm) + 释放物理页。

| 操作 | s_count 变化 | 说明 |
|------|-------------|------|
| memfd_create + ftruncate | =1（fd 持有） | fd 本身计 1 引用 |
| sys_mmap(fd, ...) | +1 | vma 映射计 1 |
| close(fd) | -1 | fd 释放 |
| munmap(vaddr) | -1 | vma 释放 |
| SCM_RIGHTS 发送 fd | +1 | 接收端 fd 计 1 |
| dup2(old_fd, new_fd) | +1 | 复制 fd 计 1 |
| 进程 exit | -1 per FD_SHM + -1 per shm_obj | fd_table 和 mmap_regions 各遍历 |

### shm_put 统一释放

删除 `SHM_KERNEL` 标志后，统一用 page_list 释放路径：refcount_dec_and_test 归零时逐页 bfc_free_page(page_list[i]) → kfree(page_list) → kfree(shm)。实现：kernel/bsd/types.h : shm_put

### 内核 SHM（USB HID）注册方式

内核初始化时 `shm_create_internal(1)` 分配 USB HID SHM，通过 `devtmpfs_create("usb_hid_kbd", DEV_USB_HID, &usb_hid_ops, shm)` 注册到 devtmpfs。kbd_driver 通过 `open("/dev/usb_hid_kbd")` + `mmap` 访问，不再使用 `register_kernel_shm` / `sys_shm_attach`。

### 用户态驱动 SHM 关联

用户态驱动通过 `sys_dev_create` 将 memfd 关联到设备 inode：

```c
int sys_dev_create(const char *name, int shm_fd);
```

- `shm_fd == -1`：无 SHM 关联
- `shm_fd >= 0`：校验 fd 类型为 FD_SHM，从 fd_table 取出 struct shm，`shm_get(shm)` 增加引用，设置 `inode->shm = shm`
- 内核自动填充 `dev_ops.driver_pid = current_task->pid`（调用者即 driver），callbacks 全 NULL（用户态驱动通过 ioctl REQ 代理）

consumer 通过 `open("/dev/xxx")` + `mmap(fd, ...)` 访问，mmap 从 `inode->shm` 取物理页映射。这是微内核下 `.mmap` 回调的替代——用户态驱动无法提供内核回调，通过 shm_fd 声明"mmap 时映射这块内存"。

### sys_mmap FD_DEV 路径

mmap 设备 fd 时 fd 类型不变（仍为 FD_DEV），用户态驱动的 mmap 从 `inode->shm` 取物理页映射。引用计数：mmap 时 `shm_get(inode->shm)`，VMA 记录 shm 指针；munmap/进程 exit 时 `shm_put`。shm 的生命周期独立于 inode：驱动退出后 inode 可立即释放，shm 被 VMA 持有直到最后一个映射消失。

### syscall 接口

- sys_memfd_create(name, flags)（syscall #38）— 返回 fd，size=0；MFD_CLOEXEC 存入 fd flags；MFD_ALLOW_SEALING 允许后续加 seal
- sys_ftruncate(fd, size)（syscall #39）— 扩大：逐页分配新物理页；缩小：释放超出的物理页（受 F_SEAL_SHRINK 限制）
- sys_mmap(addr, size, prot, flags, fd, offset)（syscall #8）— MAP_SHARED+fd≥0 映射 SHM fd；MAP_ANONYMOUS 匿名映射；MAP_PHYSICAL 物理地址映射；设备 fd 从 inode->shm 映射
- sys_dev_create(name, shm_fd)（syscall #49）— 创建设备节点，shm_fd=-1 无 SHM，shm_fd≥0 关联 SHM；内核自动填充 driver_pid

### memfd_create + ftruncate + sealing

| Seal | 含义 | 影响的操作 |
|------|------|-----------|
| F_SEAL_SHRINK | 禁止缩小 | ftruncate(new < old) → -EPERM |
| F_SEAL_GROW | 禁止扩大 | ftruncate(new > old) → -EPERM |
| F_SEAL_WRITE | 禁止写入 | mmap(PROT_WRITE) → -EPERM |
| F_SEAL_SEAL | 禁止再设 seal | fcntl(F_ADD_SEALS) → -EPERM |

seal 一经设置不可撤销。

### proc_reap 清理路径

遍历 fd_table：type==FD_SHM → shm_put（s_count--，归零时 kfree+释放页）。遍历 mmap_regions：shm_obj != NULL → shm_put。SHM 页只 unmap PTE，不释放物理页。

---

# 五、信号机制

### 概述

信号实现为 Linux 标准信号机制，为 PTY（Ctrl-C → SIGINT）和 Wayland 验收提供基础。

### 设计范围

| 特性 | 状态 | 说明 |
|------|------|------|
| sigframe 栈帧投递 | ✅ | pretcode + siginfo_t + ucontext_t，保存全部 GP 寄存器 |
| SA_SIGINFO | ✅ | handler(int, siginfo_t*, ucontext_t*) |
| EINTR | ✅ | 阻塞 syscall 被信号中断返回 -EINTR |
| blocked mask 内部机制 | ✅ | handler 执行期间 block sa_mask + 当前信号，sigreturn 恢复 |
| 内核异常翻译 | ✅ | #PF→SIGSEGV, #GP→SIGSEGV, #UD→SIGILL, #DE→SIGFPE |
| force_sig | ✅ | 同步信号绕过 SIG_IGN（避免故障指令无限重入） |
| SIGCHLD | ✅ | 替代 sys_exit 中 RECV_NOTIFY 子进程退出通知 |
| sys_kill pgid | ✅ | pid>0 单进程，pid==0 同 pgid，pid<-1 进程组 -pid |
| sys_sigprocmask | ✅ | syscall #67，用户显式控制信号阻塞掩码（SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK） |
| sys_sigpending | ✅ | syscall #85，返回未决信号集（per-task ∪ shared，不滤 blocked） |
| sys_alarm/sys_pause | ✅ | syscall #80/#81，定时 SIGALRM + 可中断睡眠，详见 [posix.md](posix.md) alarm/pause 章节 |
| SA_RESTART | ❌ 不做 | EINTR 后由用户态 libc 或应用决定是否重启 |
| sigaltstack | ❌ 不做 | handler 在用户栈上执行 |
| SIGPIPE | ❌ 不做 | 保持现有 -EPIPE 返回值 |
| 作业控制 | ❌ 不做 | 需要 session/controlling terminal |

### 数据结构

**sigset_t**：uint64_t（64 个信号，1 个 uint64_t 足够）

**sigaction**（common/signal.h）
  sa_handler / sa_sigaction : union — SIG_DFL=0, SIG_IGN=1, handler 地址 > 1
  sa_mask : sigset_t — handler 执行期间额外阻塞的信号集
  sa_flags : int — SA_SIGINFO 等
  sa_restorer : void(*)() — Linux 历史遗留，内核忽略

**信号编号**：与 Linux x86-64 对齐（common/signal.h : SIGHUP..SIGTSTP 1-20, NSIG=32）

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
| SIGSTOP (19) | Stop（暂走 Terminate） | 暂停进程 |

**rt_sigframe 栈帧结构**（定义：kernel/trap.c 相关头文件）

sigcontext — 16 个 GP 寄存器 + rip/eflags + 段寄存器 + fs_base/gs_base + cr2（#PF 地址）
ucontext_t — uc_flags + uc_link(NULL) + uc_sigmask + uc_mcontext(sigcontext)
siginfo_t — si_signo + si_errno(清零) + si_code(SI_USER/SI_KERNEL/SI_QUEUE) + union(_kill: si_pid/si_uid, si_addr: SIGSEGV 崩溃地址)
rt_sigframe — pretcode(SIG_TRAMPOLINE_ADDR) + info + uc

**信号字段**（proc 内嵌，不是独立 signal_state struct）：
  sig_pending : uint64_t — per-task 私有 pending（tgkill/pthread_kill）
  sig_blocked : sigset_t — per-task 阻塞掩码
  sig_force_info : siginfo_t — force_sig 临时数据
  signal : signal_struct* — 线程组共享（shared_pending + action[] + group_exit + parent_pid，见 signal.h）

**vdso sigreturn trampoline**：固定映射到 SIG_TRAMPOLINE_ADDR=0x50000000，内容为 `mov rax, SYS_SIGRETURN; syscall`。每个进程创建时映射同一物理页。

### syscall 接口

#### sys_kill(pid, sig) — syscall #43

pid > 0：发送给指定进程；pid == 0：发送给同 pgid（pgsignal 遍历）；pid < -1：发送给进程组 -pid；sig == 0：存在性检查

#### sys_sigaction(sig, act, oldact) — syscall #44

1. 验证 sig（SIGKILL/SIGSTOP → -EINVAL）
2. oldact 非 NULL → 拷贝当前 action 到用户空间
3. act 非 NULL → 从用户空间拷贝新 action
4. 清除该信号 pending bit（POSIX：注册 handler 时丢弃未决信号）

#### sys_sigreturn() — syscall #45

1. 从当前 trapframe 的 rsp 定位用户栈上的 sigframe
2. 恢复 sigframe.uc.uc_mcontext 中全部 GP 寄存器到 trapframe
3. 恢复 rip/rsp/rflags/cs/ss
4. 恢复 blocked mask = sigframe.uc.uc_sigmask

### 信号投递路径

#### 投递时机

check_pending_signals(trapframe_t *tf) 在 `__trapret`（中断返回用户态前）和 `syscall_fast_entry`（syscall 返回用户态前）调用。

实现：kernel/trap.c : check_pending_signals

#### deliver_signal — 构造 sigframe 推到用户栈

1. 构建 rt_sigframe：pretcode = SIG_TRAMPOLINE_ADDR, si_signo = sig, si_code = SI_KERNEL
2. 保存当前 blocked 到 uc.uc_sigmask
3. 更新 blocked：`sig.blocked |= sa_mask | (1ULL << sig)`（handler 期间阻塞）
4. 推 sigframe 到用户栈：`user_rsp = tf->rsp - sizeof(rt_sigframe)`，16 字节对齐
5. CR3 切换，拷贝 sigframe 到用户栈
6. 修改 trapframe：rip = sa_handler, rsp = user_rsp
7. SA_SIGINFO 时设 rdi=sig, rsi=&info, rdx=&uc；否则只设 rdi=sig

#### 用户态执行流程

trap return → handler(rdi=sig, rsi=&siginfo, rdx=&ucontext) → handler ret → pop pretcode → SIG_TRAMPOLINE_ADDR → sys_sigreturn → 恢复全部寄存器 + blocked mask → 回到被中断代码

### EINTR

阻塞 syscall 被信号中断时返回 -EINTR。进程回到用户态后 check_pending_signals 投递信号。不清除 pending bit。

| syscall | 阻塞场景 |
|---------|---------|
| sys_recv | WAIT_RECV |
| sys_read (pipe) | WAIT_PIPE |
| sys_waitpid | WAIT_CHILD |
| sys_req | WAIT_REQ_REPLY |
| sys_msg | WAIT_MSG_REPLY |
| sys_poll | WAIT_POLL |
| sys_accept | WAIT_RECV |

### sys_kill 唤醒

设置 pending 后，如果 target 状态为 BLOCKED，在 scheduler_lock 下将其设为 READY、wait_event=WAIT_NONE、入 run_queue、run_count++。

### 内核异常翻译

| 异常向量 | 信号 | si_code |
|----------|------|---------|
| 0 (#DE) | SIGFPE | FPE_INTDIV |
| 6 (#UD) | SIGILL | ILL_ILLOPC |
| 13 (#GP) | SIGSEGV | SEGV_MAPERR |
| 14 (#PF) | SIGSEGV | present→SEGV_ACCERR, not-present→SEGV_MAPERR |

trap_dispatch 中 `tf->cs == USER_CS` 时调 force_sig，不杀进程，由 check_pending_signals 处理。

### force_sig

同步信号（SIGSEGV/SIGILL/SIGFPE）必须投递，即使 SIG_IGN：设置 pending bit，从 blocked 清除该信号位，设置 sig_force_info，sa_handler==SIG_IGN 时强制改为 SIG_DFL。

### SIGCHLD 替代 RECV_NOTIFY

子进程 sys_exit 时：父进程 sig.pending |= (1ULL << SIGCHLD) + 唤醒。不再入队 RECV_NOTIFY。waitpid 唤醒不依赖 SIGCHLD handler——即使 SIGCHLD 被 ignore，sys_waitpid(WAIT_CHILD) 在子进程 ZOMBIE 时仍被唤醒。

驱动间 notify（kbd→terminal）保持 RECV_NOTIFY 不变。

---

# syscall 编号总表

NR_SYSCALL=95（编号 0-94 连续）。完整编号表见 [syscall.md](syscall.md) 当前架构设计的 syscall_dispatch 表和 [posix.md](posix.md) Syscall 编号表。IPC 相关 syscall 分布：

| 区间 | 类别 |
|------|------|
| 0-19 | Xcore 层（调度/IPC/内存/设备） |
| 20-26 | fd 操作 + DMA + PCI |
| 27-36 | AF_UNIX socket 系列 |
| 40-42, 67, 85 | 信号（kill/sigaction/sigreturn/sigprocmask/sigpending） |
| 80-81 | alarm/pause（信号驱动定时/可中断睡眠） |
| 86-94 | epoll + eventfd/timerfd/signalfd（详见 [epoll.md](epoll.md)） |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| socket per-socket lock | 全局 socket_lock → per-socket spinlock + per-hash-bucket spinlock（bind 名字空间），消除无关 socket pair 间锁竞争 | 中 |
| socketpair 替代 pipe | terminal ↔ shell 当前用两个 pipe，可用一个 socketpair 双向通信替代 | 低 |
| sys_msg/sys_msg_resp 废弃 | FD_FILE 消除后，sys_msg 仅剩用户消失，届时可删除 | 低 |
| MSG_CTRUNC 标志 | recvmsg 在 controllen 不够时应设 msg_flags |= MSG_CTRUNC，当前仅设 *controllen = 0 | 低 |
| ftruncate + mmap 可见性 | 已 mmap 的 SHM 区域在 ftruncate 扩大后，新页对已映射地址空间不可见（需刷新页表） | 中 |
| SA_RESTART | libc 自动重启被中断 syscall | 低 |
| SIGPIPE | 写已关闭 socket 时发送 SIGPIPE | 低 |
| 作业控制 | SIGTSTP/Ctrl+Z, fg/bg（需 session + PTY） | 低 |
| sigaltstack | handler 在独立栈执行 | 低 |
| vdso ELF | trampoline 页扩展为完整 vdso（clock_gettime 等），通过 AT_SYSINFO_EHDR 传递 | 低 |
| real-time signal | 32-64 号信号排队不丢 | 低 |
| recv/resp 降级为 driver-only | recv/resp/req/msg/msg_resp 从公共 libc 头移到 driver/ipc.h，公共 libc 只导出 POSIX 标准接口 | 低 |
| futex 跨进程同步原语 | SHM 场景必需，当前靠轮询标志位易死锁；无 PTHREAD_PROCESS_SHARED mutex/cond/POSIX 信号量 | 高 |
| AF_UNIX SOCK_DGRAM/SEQPACKET | sys_socket 对非 SOCK_STREAM 返回 EPROTONOSUPPORT | 低 |
| SCM_RIGHTS cross-process files_t UAF | 接收方读 sender fd_table 时，sender exit + kfree(files_t) 可导致 UAF。缓解：`files_put` 中 `synchronize_rcu()` 保证 grace period 后才 kfree；当前 `file_get` 在 RCU 内原子性验证+持引用，sender close 不会立即 free | 低 |
| 消息优先级/优先级继承 | recv 队列为 FIFO，实时场景可能优先级反转 | 低 |
| 寄存器直传小包快速路径 | req/resp 仍经 recv 队列拷贝，未做纯寄存器直达 bypass | 低 |
| 通用端口死亡通知 | REQ/MSG 等待者可获通知，但无通用 port death callback 机制 | 低 |
| FIFO 命名管道 | 有 UDS 可弱化需求 | 低 |
| AF_INET 网络 socket | 单机 OS 可裁剪 | 远期 |
