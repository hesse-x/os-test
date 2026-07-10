# epoll 与事件 fd

## 当前架构设计

epoll I/O 多路复用 + eventfd / timerfd / signalfd 三类事件 fd，统一基于 wait_queue 基础设施实现。语义对齐 Linux epoll（LT + ET），满足 Wayland compositor 与通用用户态事件循环需求。

| 组件 | 职责 | 层级 |
|------|------|------|
| wait_queue | 回调式等待队列原语（多等待者唤醒） | Xcore |
| file_poll | per-type 就绪检测 helper（纯查询，不阻塞） | BSD |
| eventpoll | epoll 核心（interest 红黑树 + ready 链表 + LT/ET） | BSD |
| eventfd | 信号计数器 fd | BSD |
| timerfd | 定时器 fd（单次/周期） | BSD |
| signalfd | 信号 fd（消费信号替代 handler 交付） | BSD |

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 多等待者唤醒机制 | file 上挂 wait_queue 链表 | 天然支持"一个 fd 被 N 个 epoll 监听"，唤醒路径局部化无需全局注册表；未来 futex/inotify 可复用 |
| 2 | wq 内存策略 | 惰性分配（file->wq，NULL=无等待者） | 普通文件/目录/SHM 等恒就绪 fd 永不被 epoll 监听，惰性分配避免每个 file 背上 wait_queue_head 内存开销 |
| 3 | interest list 容器 | 红黑树（按 file* 排序去重） | epoll_ctl MOD/DEL 需按 file* 查找；O(log n) 且天然去重（同 file* 插入冲突→EEXIST） |
| 4 | 触发模式 | LT + ET | LT：就绪持续上报直到消费；ET：状态变化时仅通知一次，用户须读到 EAGAIN |
| 5 | epoll_wait 阻塞态 | 复用 WAIT_POLL（不新增 wait_event） | epoll_wait 与 sys_poll/eventfd 阻塞读同一语义，复用 wake_with_event 唤醒路径 |
| 6 | 唤醒点改造策略 | 单 PID 字段与 __wake_up 共存（双路径） | 先加 __wake_up 不删旧 read_pid/blocked_reader，降低回归风险；直接 read/write 阻塞仍走单 PID，epoll 走 wait_queue |
| 7 | epitem 持引用 | file_get 持 file 引用 | 被监听 fd 先于 epoll close 时 epitem 仍安全，file 不释放 |
| 8 | epoll fd 嵌套 | 禁止监听 FD_EPOLL 自身（ep_insert 返回 -EINVAL） | 防 epoll 嵌套死锁，对齐 Linux |
| 9 | timerfd 后端 | 全局 timerfd_list + timer IRQ tick hook 扫描 | 不依赖 per-task wait_deadline，独立到期回调 |
| 10 | signalfd 消费优先级 | signalfd 消费优先于 handler 交付 | 信号到达时 check_pending_signals 先查 signalfd，命中则保留 pending 位 + 唤醒，跳过 handler |

### 核心数据结构

**wait_queue_head**（kernel/xcore/wait_queue.h : wait_queue_head）
- lock : spinlock — 保护 wq 链表（irqsave）
- head : list_node — 挂 wait_queue_t.node

**wait_queue_t**（kernel/xcore/wait_queue.h : wait_queue_t）
- node : list_node — 挂入 wait_queue_head
- func : wait_queue_func_t — 唤醒回调（`void(*)(wait_queue_t*, unsigned long flags)`）
- data : void* — 通常指向 epitem 或阻塞进程

flags 是调用方透传的盲数据，Xcore 层不解释；poll 掩码语义由 BSD 层回调（ep_poll_callback）自行解释。

**file**（kernel/bsd/types.h : file）扩展字段
- wq : wait_queue_head* — 惰性分配，NULL 表示无等待者（types.h:66）
- union 新增：epoll* / eventfd* / timerfd* / signalfd*

**epitem**（kernel/bsd/eventpoll.h : epitem）
- rb_node : rb_node — 挂入 eventpoll.rbt（按 file* 排序）
- rdllist_node : list_node — 挂入 eventpoll.ready_list
- wait : wait_queue_t — 挂入被监听 fd 的 wq（func=ep_poll_callback, data=本 epitem）
- file : file* — 被监听 fd 的 file（file_get 持引用）
- events : __poll — 用户注册事件掩码（不含 EPOLLET）
- revents : __poll — 当前就绪事件
- user_data : uint64_t — epoll_event.data.u64
- is_ready : int — 是否在 ready_list（防重复入队）
- is_et : int — EPOLLET 模式标志
- ep : eventpoll* — 反向指针

**eventpoll**（kernel/bsd/eventpoll.h : eventpoll）
- lock : spinlock — 保护 rbt + ready_list
- wq : wait_queue_head — epoll_wait 阻塞在此
- rbt : rb_root — interest list（红黑树）
- ready_list : list_node — 就绪 epitem 链表
- nitems : int — interest list 当前数量（上限 EP_MAX_ITEMS=128）

**eventfd_ctx**（kernel/bsd/eventfd.h : eventfd_ctx）
- count : uint64_t — 信号计数器
- flags : uint32_t — EFD_CLOEXEC / EFD_NONBLOCK / EFD_SEMAPHORE
- lock : spinlock — 保护 count
- EVENTFD_MAX = 0xFFFFFFFFFFFFFFFEULL

**timerfd_ctx**（kernel/bsd/timerfd.h : timerfd_ctx）
- expiry : uint64_t — 到期绝对时间（sched_clock ns，0=未武装）
- interval : uint64_t — 周期间隔（0=单次）
- ticks : uint64_t — 未读到期次数
- lock : spinlock — 保护 expiry/interval/ticks
- node : list_node — 挂入全局 timerfd_list
- f : file* — 反向引用（tick 时取 wq 唤醒）

**signalfd_ctx**（kernel/bsd/signalfd.h : signalfd_ctx）
- sigmask : uint64_t — 此 fd 接受的信号集（SIGKILL/SIGSTOP 强制剥离）
- lock : spinlock — 保护 sigmask 更新
- 信号本体存进程 sig_pending/signal.shared_pending，signalfd 不另存队列

**signalfd_siginfo**（kernel/bsd/signalfd.h : signalfd_siginfo）— 128 字节，Linux UAPI 布局。填 ssi_signo/ssi_code=SI_USER，其余补 0。

**fd 类型常量**（kernel/bsd/types.h:37-40）：FD_EPOLL=9, FD_EVENTFD=10, FD_TIMERFD=11, FD_SIGNALFD=12

### 关键流程

#### epoll_ctl ADD/MOD/DEL

实现：kernel/bsd/eventpoll.c : ep_insert / ep_modify / ep_remove

- **ADD**：校验 fd 非 FD_EPOLL + interest 未满 + 红黑树无同 file* 项；分配 epitem，file_get 持引用，`ep_target_wq(f)` 解析被监听 fd 的 wq 并 `add_wait_queue`；插入红黑树 nitems++；**立即就绪检查** `file_poll(f, events)`，若就绪则入 ready_list + `__wake_up(&ep->wq, POLLIN)` 唤醒 epoll_wait
- **MOD**：红黑树查 file*，更新 events/is_et/user_data；重新 file_poll：就绪且不在 ready_list→入队+唤醒；不再就绪且在 ready_list→出队
- **DEL**：红黑树查 file*，remove_wait_queue 解除监听，从 rbt/ready_list 移除，file_put 释放引用，kfree epitem

#### ep_target_wq — 被监听 fd 的 wq 解析

实现：kernel/bsd/eventpoll.c : ep_target_wq

不同 fd 类型的 wq 存储位置不同，按类型惰性分配：
- FD_PIPE → pipe->close_wq（types.h:49）
- FD_SOCKET → unix_sock->wq（socket.h:73）
- FD_NETLINK → netlink_sock->wq（netlink.h:37，经 file_wq_get 惰性分配）
- FD_TTY → pty->wq（pty.h:157）
- 其余（FD_EVENTFD/FD_TIMERFD/FD_SIGNALFD 等）→ file->wq（经 file_wq_get 惰性分配）

#### 数据到达 → ep_poll_callback → 唤醒 epoll_wait

实现：kernel/bsd/eventpoll.c : ep_poll_callback

数据到达点（pipe write / socket sendmsg / netlink broadcast / pty 数据 / eventfd write / timerfd tick / signalfd 信号）调 `__wake_up(&target_wq, POLLIN)`，遍历 wq 上的 wait_queue_t 调其 func（即 ep_poll_callback）：
- **ET 模式**：仅首次转变（!is_ready）时 revents=mask&events，入 ready_list，is_ready=1，`__wake_up(&ep->wq, POLLIN)`
- **LT 模式**：每次刷新 revents，确保在 ready_list，`__wake_up(&ep->wq, POLLIN)`

#### epoll_wait 阻塞/唤醒循环

实现：kernel/bsd/eventpoll.c : sys_epoll_wait

1. 校验 epfd 是 FD_EPOLL + maxevents∈(0, EP_MAX_ITEMS]
2. 注册 wait_queue_t（func=ep_wait_callback→wake_with_event(proc, WAIT_POLL)）到 ep->wq
3. 循环：持 ep->lock 取 ready_list，对每个 epitem：
   - LT：`file_poll` 复查就绪，不再就绪则丢弃（stale），仍就绪则 re-enqueue 到 tail（LT 持续上报）
   - ET：直接报出，不复查（一次性）
   - pass_end 锚点防持续就绪 fd 在同一轮循环
4. 有事件→copy_to_user epoll_event 返回 n；无事件且 timeout=0→返回 0；否则 BLOCKED on WAIT_POLL + timer_queue 超时→schedule
5. 唤醒后 EINTR 检查（sig_pending & ~blocked | SIGKILL/SIGSTOP），优先于超时；wait_timed_out→返回 0

#### epoll_pwait 信号掩码

实现：kernel/bsd/eventpoll.c : sys_epoll_pwait

临时替换 proc->sig_blocked（强制或上 SIGKILL/SIGSTOP），调 sys_epoll_wait 主体，恢复原掩码。被屏蔽信号在 wait 期间不触发 EINTR。

#### eventfd 读写

实现：kernel/bsd/eventfd.c : eventfd_do_read / eventfd_do_write

- **read**：EFD_SEMAPHORE 模式 count>0 则 count-- 返回 1；普通模式 count>0 则返回 count 并清零；否则阻塞（WAIT_POLL + file->wq）/-EAGAIN
- **write**：count+val≤EVENTFD_MAX 则 count+=val + `__wake_up(&file->wq, POLLIN)`；否则阻塞/-EAGAIN

#### timerfd 到期与读

实现：kernel/bsd/timerfd.c : timerfd_tick_all / timerfd_do_read

- **到期扫描**：timer IRQ 调 `timerfd_tick_all`（bsd_init 注册为 timerfd_tick_hook），扫全局 timerfd_list，到期项 ticks++ + `__wake_up(file_wq_get(f), POLLIN)`
- **read**：ticks>0 返回 ticks（8 字节）并清零；否则阻塞/-EAGAIN
- **settime**：设 expiry/interval，支持 TFD_TIMER_ABSTIME（绝对时间）；it_value=0 取消

#### signalfd 读与信号消费

实现：kernel/bsd/signalfd.c : signalfd_do_read / signalfd_consumes

- **read**：从 sig_pending/shared_pending 取 sigmask 内首个可交付信号（~blocked，SIGKILL/SIGSTOP 旁路），填 signalfd_siginfo 返回并清除 pending 位；无则阻塞（WAIT_POLL + file->wq）/-EAGAIN
- **消费优先级**：check_pending_signals（signal.c:195）选定待交付信号后先调 `signalfd_consumes`：命中则**重新置位** pending（保留给 signalfd 读）+ `__wake_up(signalfd_wq, POLLIN)` + break（跳过 handler 交付）

### wait_queue 唤醒原语

实现：kernel/xcore/wait_queue.c : __wake_up

`__wake_up(wq, flags)` 遍历 wq 链表，但**遍历时不持 wq->lock**：先收集待唤醒项到本地数组（上限 32），解锁后逐个调 func。避免 ep_poll_callback 内取 eventpoll->lock + `__wake_up(&ep->wq)` 形成锁嵌套。

### file_poll 就绪检测

实现：kernel/bsd/file_poll.c : file_poll

纯查询函数，不阻塞、不加长锁，sys_poll / epoll_ctl / epoll_wait 共用。从 sys_poll 的 if-else 链抽取，语义不变：

| fd type | POLLIN 就绪 | POLLOUT 就绪 | 附加 |
|---------|------------|-------------|------|
| FD_PIPE | head!=tail \|\| p_count≤1（EOF） | 有空间 \|\| p_count≤1 | — |
| FD_SOCKET | recv_queue非空 \|\| shutdown_read | connected && !shutdown_write | POLLHUP: peer失效/CLOSED |
| FD_SOCKET(LISTEN) | backlog_len>0 | — | — |
| FD_NETLINK | recv_queue非空 | 恒就绪（广播不阻塞） | close→POLLHUP |
| FD_FILE | offset<file_size | 恒就绪 | — |
| FD_TTY | pty_poll(slave侧) | pty_poll | — |
| FD_DEV | dev_ops->poll（内核驱动） | 同 | 用户态驱动恒就绪 |
| FD_EPOLL | ready_list非空 | — | — |
| FD_EVENTFD | count>0 | count<EVENTFD_MAX | — |
| FD_TIMERFD | ticks>0 | — | — |
| FD_SIGNALFD | (pending\|shared)&sigmask&~blocked | — | — |
| FD_SHM等 | 恒就绪 | 恒就绪 | — |

FD_SOCKET 分支内取 socket_lock（锁序：eventpoll->lock → socket_lock）。

### 唤醒点（双路径共存）

每个数据到达点保留单 PID 唤醒（直接 read/write 阻塞）+ 追加 `__wake_up`（epoll/poll 等待者）：

| 对象 | 单 PID 字段（保留） | __wake_up 点 |
|------|---------------------|-------------|
| pipe | read_pid/write_pid | syscall.c:1231 close_wq POLLIN |
| socket | blocked_reader/blocked_writer | socket.c:249/257/376/647/1153 wq |
| netlink | blocked_reader | netlink.c:426 nl_group_broadcast wq POLLIN |
| pty | m/s_read_pid/write_pid | pty.c:285/347/360/375/434/474 wq |
| eventfd | — | eventfd.c:111/172 |
| timerfd | — | timerfd.c:159 |
| signalfd | — | signal.c:206 |

### 锁模型

| 锁 | 保护对象 | 持有者 |
|----|---------|--------|
| wq->lock (irqsave) | wait_queue 链表 | add/remove_wait_queue + __wake_up 收集阶段 |
| eventpoll->lock | rbt + ready_list | epoll_ctl/wait + ep_poll_callback |
| socket_lock | unix_sock 状态 | file_poll FD_SOCKET 分支 + socket I/O |
| timerfd_list_lock | 全局 timerfd_list | timerfd_tick_all + settime + file_put |

固定锁序：`fd_lock → eventpoll->lock → socket_lock → wq->lock → scheduler_lock`

关键约束：
- `__wake_up` 遍历**不持** wq->lock（先收集后回调），避免 ep_poll_callback 内取 eventpoll->lock + `__wake_up(&ep->wq)` 嵌套
- ep_poll_callback 持 eventpoll->lock 时不再取 wq->lock（wq->lock 仅 __wake_up 收集阶段持有）

### 系统调用

| 编号 | syscall | 签名 | 行为 |
|------|---------|------|------|
| 86 | sys_epoll_create | `sys_epoll_create(int64_t size)` | 忽略 size，转调 create1(0) |
| 87 | sys_epoll_create1 | `sys_epoll_create1(int64_t flags)` | flags 仅 EPOLL_CLOEXEC；分配 eventpoll + FD_EPOLL fd |
| 88 | sys_epoll_ctl | `sys_epoll_ctl(epfd, op, fd, ev*)` | ADD/MOD/DEL；ev 用户指针校验；fd≠epfd；fd 非 FD_EPOLL |
| 89 | sys_epoll_wait | `sys_epoll_wait(epfd, ev*, maxevents, timeout)` | maxevents∈(0,128]；timeout=0 非阻塞，-1 无限 |
| 90 | sys_epoll_pwait | `sys_epoll_pwait(epfd, ev*, maxevents, timeout, sigmask*, sigsetsize)` | 临时替换 sig_blocked（强制含 SIGKILL/SIGSTOP）后调 wait |
| 91 | sys_eventfd2 | `sys_eventfd2(initval, flags)` | flags: EFD_CLOEXEC/EFD_NONBLOCK/EFD_SEMAPHORE |
| 92 | sys_timerfd_create | `sys_timerfd_create(clockid, flags)` | clockid==CLOCK_MONOTONIC；flags: TFD_CLOEXEC/TFD_NONBLOCK |
| 93 | sys_timerfd_settime | `sys_timerfd_settime(fd, flags, new*, old*)` | flags: TFD_TIMER_ABSTIME；it_value=0 取消 |
| 94 | sys_signalfd4 | `sys_signalfd4(fd, sigmask*, sizemask, flags)` | fd==-1 创建；fd≥0 更新掩码；SIGKILL/SIGSTOP 强制剥离 |

分发：kernel/bsd/syscall.c:2899-2916。全部 BSD 层，Xcore 分发表无需改动。

### 与其他模块的关系

- 调度器：epoll_wait/eventfd/timerfd/signalfd 阻塞复用 WAIT_POLL，详见 [schedule.md](schedule.md)
- IPC：wait_queue + __wake_up 替代单 PID 唤醒的通用机制，详见 [ipc.md](ipc.md) AF_UNIX socket 章节
- 信号：signalfd 挂接 check_pending_signals 交付路径，详见 [ipc.md](ipc.md) 信号机制章节
- PTY：pty->wq 作为 epoll 监听 TTY 的唤醒通道，详见 [terminal.md](terminal.md)
- 系统调用编号：86-94，详见 [syscall.md](syscall.md) / [posix.md](posix.md)

### libc 封装

- user/include/sys/epoll.h / eventfd.h / timerfd.h / signalfd.h — 函数原型 + 常量
- user/lib/epoll.cc / eventfd.cc / timerfd.cc / signalfd.cc — thin syscall wrapper
- include/uapi/xos/epoll.h — epoll_event / EPOLLIN / EPOLLET / EPOLL_CTL_* 常量

### 测试

| ELF | 文件 | 覆盖 |
|-----|------|------|
| test_epoll | user/test/test_epoll.c | create1/ctl/wait/pwait + LT/ET + pipe/socket/eventfd 多路 + 多等待者 + 容错 |
| test_eventfd | user/test/test_eventfd.c | read/write/semaphore/poll/blocking wake |
| test_timerfd | user/test/test_timerfd.c | create/settime/单次/周期/read/abstime/disarm |
| test_signalfd | user/test/test_signalfd.c | create/read/消费优先于handler/blocked/poll |

注册：user/test/CMakeLists.txt Layer 16-19，test_runner 统一调度。

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| FD_TTY file_poll master 侧 | file_poll 对 FD_TTY 硬编码 pty_poll(pty, 0, events) 默认 slave 侧，无 fd 上下文区分 master/slave；test_poll 不覆盖 TTY | 低 |
| ET 多 epoll 共享 fd 数据竞争 | A epoll 读走数据使 fd 不再就绪，B epoll 的 ET 事件丢失；当前 ep_poll_callback 遍历 wq 所有 epitem 逐一通知，各 epoll 独立标 is_ready，但跨 epoll 读序未严格保证 | 低 |
| epoll fd close 自动 DEL 被监听 fd | 被监听 fd 先于 epoll close 时 epitem 持 file 引用保活，但未自动报 EPOLLHUP/DEL；用户 read 得 -EBADF | 低 |
| signalfd siginfo 完整填充 | 当前仅填 ssi_signo/ssi_code，ssi_pid/ssi_uid/等补 0；si_code 无 SI_KERNEL/SI_QUEUE 区分 | 低 |
| signalfd 队列信号 | rt signal (32-64) 排队不丢未实现，signalfd 依赖 pending 位图 | 低 |
| epoll fd 跨 fork | fork 深拷贝 fd table 指向同一 eventpoll，ready_list 竞争；文档注明不应跨 fork 共享需 CLOEXEC | 低 |
