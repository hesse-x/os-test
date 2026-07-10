# Netlink

## 当前架构设计

Netlink 多播事件通知机制，提供内核→用户态 + 用户态→用户态的广播通道。首要用途是 udev 设备事件（uevent）：内核 devtmpfs 设备创建/删除时广播到所有订阅进程，用户态服务（evdev 等）也可向 group 发送事件。客户端通过标准 POSIX socket API 访问，与 epoll 集成。

| 组件 | 职责 | 层级 |
|------|------|------|
| netlink_sock | AF_NETLINK socket 端点（recv_queue + group 订阅） | BSD |
| nl_group 注册表 | 全局 group 成员链表 + broadcast 遍历原语 | BSD |
| nl_uevent_broadcast | uevent payload 拼装 + nlmsghdr 封装的便捷封装 | BSD |
| devtmpfs | 设备创建/删除时触发 uevent 广播 | BSD |

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 通信模式 | datagram 多播（1:N），非 RPC | uevent 是 fire-and-forget 广播，不等回复；与 MSG（1:1 RPC 阻塞等回复）服务不同通信模式 |
| 2 | 队列容器 | sk_buff 链表（复用 AF_UNIX 的 sk_buff alloc/free） | datagram 每条消息独立完整，链表天然支持变长 + 多播复制；不另建管道 |
| 3 | group 标识 | uint32_t bitmask（32 group） | bind 时一次订阅多个 group，broadcast 按 bit 遍历，简洁 |
| 4 | 单锁策略 | nl_group_lock 同时保护 group 注册表和各 sock 的 recv_queue | broadcast 在锁内完成 enqueue+wake，与 cleanup 互斥，避免 sock 在遍历中途被 free；单锁避免锁序复杂度 |
| 5 | 队列溢出策略 | recv_queue 上限 256 条，满则 drop oldest | uevent 频率低，避免无上限导致 OOM；drop 旧消息符合"事件通知"语义（订阅者应尽快消费） |
| 6 | 消息复制 | 每个订阅者独立 kmalloc 一份 skb 拷贝 | uevent payload 通常 < 256B，走 slab 快速分配；成员数实际 1-3 个，复制开销可忽略，换取实现简洁 |
| 7 | fd 类型 | FD_NETLINK=13，独立于 FD_SOCKET | netlink 无连接/peer/backlog 概念，与 unix_sock 生命周期/分流逻辑不同，独立类型简化分发 |
| 8 | syscall 复用 | 无独立 syscall，复用 socket/bind/sendmsg/recvmsg/read/write | 用户态 API 与 Linux netlink 一致，移植 udev 客户端零改动 |
| 9 | 内核发送者标识 | nlmsghdr.nlmsg_pid = 0 表示内核 | 用户态 sendmsg 时填自身 PID，内核 nl_uevent_broadcast 填 0，客户端可区分来源 |

### 核心数据结构

**netlink_sock**（kernel/bsd/netlink.h : netlink_sock）
- groups : uint32_t — 已订阅 group bitmask
- portid : uint32_t — 绑定端口 ID（0 bind 时自动取 owner PID）
- protocol : int — NETLINK_KOBJECT_UEVENT 等
- recv_queue_head/tail : sk_buff* — 接收队列链表首尾
- recv_queue_len : int — 当前队列长度（上限 NL_RECV_QUEUE_LIMIT=256）
- blocked_reader : pid_t — recvmsg 阻塞的 PID（-1 = 无）
- wq : wait_queue_head* — 惰性分配，epoll 等待者挂此
- n_count : refcount_t — fd 引用计数（dup2 sharing）
- owner_pid : pid_t — 创建进程 PID

**nl_group_member**（kernel/bsd/netlink.h : nl_group_member）
- sock : netlink_sock* — 指向订阅此 group 的 socket
- next : nl_group_member* — 单链表下一项

**nl_groups[]**（kernel/bsd/netlink.c : nl_groups）— 全局数组，长度 NL_MAX_GROUPS=32，每项是该 group 的 nl_group_member 链表头。

**nlmsghdr**（include/uapi/xos/netlink.h : nlmsghdr）— Linux UAPI 兼容消息头：nlmsg_len / nlmsg_type / nlmsg_flags / nlmsg_seq / nlmsg_pid。配套宏 NLMSG_ALIGN/LENGTH/DATA/NEXT/OK。

**sockaddr_nl**（include/uapi/xos/netlink.h : sockaddr_nl）— nl_family / nl_pad / nl_pid / nl_groups。

**fd 类型常量**（kernel/bsd/types.h:41）：FD_NETLINK=13，file union 新增 `netlink_sock *nlsock`（types.h:86）。

### 关键流程

#### socket + bind 订阅

实现：kernel/bsd/socket.c : sys_socket / sys_bind

1. `socket(AF_NETLINK, SOCK_DGRAM, protocol)` → sys_socket 命中 AF_NETLINK 分支 → `netlink_sock_alloc(protocol)`（owner_pid = current PID，refcount=1）→ 安装 FD_NETLINK fd
2. `bind(fd, sockaddr_nl)` → sys_bind 命中 AF_NETLINK 分支 → `netlink_sock_bind`：设 portid（0 则取 owner_pid），遍历 nl_groups bitmask 对每个置位 bit 调 `nl_group_subscribe`（分配 nl_group_member 挂入 nl_groups[bit] 链表头）

#### 内核 uevent 广播

实现：kernel/bsd/devtmpfs.c → kernel/bsd/netlink.c : nl_uevent_broadcast → nl_group_broadcast

1. devtmpfs_create / devtmpfs_remove 末尾检查 `nl_is_initialized()`，调用 `nl_uevent_broadcast(action, devpath, subsystem)`
2. nl_uevent_broadcast 拼装 payload（`"action@devpath\0ACTION=...\0DEVPATH=...\0SUBSYSTEM=...\0"`，\0 分隔键值对），封装 nlmsghdr（nlmsg_pid=0 表内核，type 按 action 选 ADD/REMOVE/CHANGE），调 `nl_group_broadcast(0, msg, len, -1)`
3. nl_group_broadcast 持 nl_group_lock 遍历 nl_groups[0] 成员链表，对每个 sock（排除 exclude_pid）：
   - 队列满（≥256）则 drop oldest（dequeue + skb_free）
   - skb_alloc + memcpy payload + nl_skb_enqueue
   - blocked_reader≥0 则取 scheduler_lock 唤醒（READY + 入 run_queue + 取消 timer）
   - wq 非空则 `__wake_up(wq, POLLIN)` 触发 epoll

#### 用户态 sendmsg 广播

实现：kernel/bsd/netlink.c : netlink_sock_sendmsg

1. 校验 groups 非空（否则 -ENOTCONN），合并 iov 到连续 buffer（上限 MAX_SOCKET_DATA）
2. 对 sock->groups 每个置位 bit 调 `nl_group_broadcast(bit, buf, total, sock->owner_pid)`——排除发送者自身
3. 广播路径同上（持锁 enqueue + wake）

#### 用户态 recvmsg 接收

实现：kernel/bsd/netlink.c : netlink_sock_recvmsg

1. 持 nl_group_lock 检查 recv_queue_head：有数据则 copy_to_user 到 iov，skb 完全消费则 dequeue+free，填充 src_addr（从 nlmsghdr.nlmsg_pid 提取发送者），返回读取字节数
2. 无数据且 nonblock → -EAGAIN；无数据且阻塞 → 设 blocked_reader + WAIT_POLL + 30s 超时 + schedule
3. 唤醒后：超时则 -ETIMEDOUT；有 pending 信号（~blocked | SIGKILL/SIGSTOP）则 -EINTR；否则重试取数据

#### 进程退出清理

实现：kernel/bsd/proc.c → kernel/bsd/netlink.c : netlink_sock_close / nl_group_cleanup

1. files_put 关闭 FD_NETLINK 时调 `netlink_sock_close`：持 nl_group_lock 释放剩余 skb + 唤醒 blocked_reader（POLLHUP）+ `__wake_up(wq, POLLHUP|POLLIN)` 通知 epoll
2. `netlink_sock_release`：refcount 归零则 `nl_group_cleanup`（持 nl_group_lock 遍历所有 32 group 移除指向此 sock 的 member + kfree）+ `netlink_sock_free`

#### 初始化时序

实现：kernel/bsd/init.c → kernel/bsd/netlink.c : nl_init

bsd_init → vfs_init → devtmpfs_init → `nl_init()`（置 nl_initialized=true）。此后 dev_create 的 nl_uevent_broadcast 才会真正广播；启动期间 udevd 未启动时 broadcast 遍历空链表安全跳过（消息 drop，udevd 启动后可主动扫描 /dev 补偿）。

### 锁模型

| 锁 | 类型 | 保护对象 | 获取顺序 |
|----|------|---------|---------|
| nl_group_lock | spinlock | nl_groups[] 注册表 + 各 netlink_sock 的 recv_queue/blocked_reader/wq 访问 | 最外层 |
| scheduler_lock | spinlock (irqsave) | per-CPU run_queue/timer_queue | nl_group_lock 之后 |

固定锁序：`nl_group_lock → scheduler_lock (irqsave)`

关键约束：
- broadcast / recvmsg / subscribe / leave / cleanup 全部在 nl_group_lock 内操作 recv_queue，单锁互斥避免 sock 在遍历中途被 free
- 唤醒 blocked_reader 时在 nl_group_lock 内取目标 CPU 的 scheduler_lock（irqsave），与现有 `socket_lock → scheduler_lock` 锁序不冲突（netlink 不取 socket_lock）

### 系统调用/接口

Netlink 无独立 syscall 编号，复用现有 socket 系列调用，按 fd 类型（FD_NETLINK）分流：

| syscall | netlink 分流点 | 行为 |
|---------|---------------|------|
| sys_socket | socket.c:670 AF_NETLINK 分支 | 仅 SOCK_DGRAM；调 netlink_sock_alloc |
| sys_bind | socket.c:789 AF_NETLINK 分支 | 调 netlink_sock_bind（设 portid + subscribe groups） |
| sys_sendmsg / sys_write | socket.c:1420 / syscall.c:1085 FD_NETLINK 分支 | 调 netlink_sock_sendmsg（广播到订阅 groups，排除自身） |
| sys_recvmsg / sys_read | socket.c:1527 / syscall.c:1438 FD_NETLINK 分支 | 调 netlink_sock_recvmsg（阻塞/非阻塞取队列消息） |
| sys_ioctl | syscall.c:2106 | FD_NETLINK 返回 -ENOTTY |
| sys_lseek | syscall.c:2676 | FD_NETLINK 返回 -ESPIPE |

file_poll（kernel/bsd/file_poll.c:145）FD_NETLINK 分支：recv_queue 非空→EPOLLIN，POLLOUT 恒就绪（广播不阻塞）。epoll 集成经 ep_target_wq（eventpoll.c:108 FD_NETLINK 分支）惰性分配 sock->wq。

### uevent 消息格式

与 Linux uevent 兼容，payload 为 \0 分隔的键值对字符串：

```
"add@/dev/input0\0ACTION=add\0DEVPATH=/dev/input0\0SUBSYSTEM=input\0"
```

nlmsghdr.nlmsg_type 取 NLMSG_UEVENT_ADD(1) / NLMSG_UEVENT_REMOVE(2) / NLMSG_UEVENT_CHANGE(3)。

### 与其他模块的关系

- **devtmpfs**：dev_create / dev_remove 调用 nl_uevent_broadcast 触发 uevent
- **socket 层**：sys_socket/bind/sendmsg/recvmsg 按 domain/fd 类型分流到 netlink 路径，与 AF_UNIX 共享 sk_buff 基础设施
- **epoll**：netlink fd 通过 file_poll + wait_queue 集成，与 AF_UNIX socket/pipe/eventfd 共用同一机制，详见 [epoll.md](epoll.md)
- **IPC**：Netlink 与 MSG 服务不同通信模式（多播事件 vs 1:1 RPC），底层共享 sk_buff/wake_from_wait 基础设施，详见 [ipc.md](ipc.md)
- **进程管理**：bsd_proc_reap / files_put 关闭 netlink fd 时清理 group 订阅，详见 [proc.md](proc.md)

### libc 与用户态

- include/uapi/xos/netlink.h — nlmsghdr / sockaddr_nl / 常量 / 宏
- user/include/linux/netlink.h — Linux 兼容头文件（重定向到 xos/netlink.h + 补充 NETLINK_ROUTE 等协议常量），供移植的 Linux 程序使用
- user/udev/udevd.c — 最小 udevd 用户态进程（socket + bind + epoll + recvmsg 事件循环，打印 uevent）

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| NETLINK_ROUTE | 网络配置 group，需先有网络栈 | 低 |
| NETLINK_SELINUX | 安全策略 group，需先有 LSM 框架 | 低 |
| 引用计数 skb | 当前广播对每个订阅者独立 kmalloc 拷贝；如未来 payload 增大或订阅者增多，可引入一次 kmalloc + N 个引用的共享 skb | 低 |
