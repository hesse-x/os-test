# 内核锁

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 原子操作实现 | GCC `__atomic` builtins | freestanding 可用，编译器自动 lowering 为 `xchg`，自动插入内存屏障 |
| 2 | spin_lock 是否内含 cli | 否 | 中断控制由调用者负责，锁保持纯自旋语义；irqsave/irqrestore 作为独立 API |
| 3 | 结构体字段 | `volatile uint32_t locked` + debug `int cpu_id` | Debug 构建追踪持锁 CPU + 递归死锁检测；Release 构建仅 locked，零性能损失 |
| 4 | unlock 内存序 | `__ATOMIC_RELEASE` | 与 lock 端 `__ATOMIC_ACQUIRE` 配对，精确表达 acquire-release |
| 5 | 自旋循环 | exchange + pause | 2-4 核无竞争瓶颈，简单优先 |
| 6 | 初始化 | `SPINLOCK_INIT` 条件编译宏 | NDEBUG: `{.locked = 0}`；debug: `{.locked = 0, .cpu_id = -1}` |
| 7 | locked 类型 | uint32_t | x86-64 上生成 `xchgll`，显式表达宽度意图 |
| 8 | schedule 持锁策略 | irqsave 持锁穿过 switch_to，unlock → switch_to → lock → irqrestore | per-CPU 锁争用极低；irqsave 保证 switch_to 期间中断关闭 |

### 自旋锁原语（kernel/spinlock.h）

spinlock_t : volatile uint32_t locked — release 构建仅此字段
             int cpu_id（debug 构建额外字段，记录持锁 CPU，递归死锁检测）

API：
- `spin_lock(lk)` — ACQUIRE exchange 自旋 + pause
- `spin_unlock(lk)` — RELEASE store
- `spin_lock_irqsave(lk, &flags)` — pushfq+cli → spin_lock
- `spin_unlock_irqrestore(lk, flags)` — spin_unlock → popfq

### 锁一览

| 锁 | 粒度 | 保护对象 | irqsave? |
|----|------|---------|----------|
| scheduler_lock | per-CPU（cpu_local_t 内） | run_queue + timer_queue + task.state + run_count + schedule() 逻辑 | **是**（timer_handler 会争） |
| tasks_lock | 全局 | tasks[] 空闲槽位分配（pid==-1）+ mm->parent_pid 修改 | 否 |
| ahci_lock | 全局 | AHCI DMA 命令队列 + 端口状态 | 否 |
| fat_lock | 全局 | FAT32 簇分配/释放 + 目录操作 | 否 |
| inode_hash_lock | 全局 | inode hash 表查找/插入 | 否 |
| i_lock（per-inode） | per-inode | inode 引用计数 + page cache 状态 | 否 |
| page_cache_lock | 全局 | page cache LRU 操作 | 否 |
| socket_lock | 全局 | AF_UNIX socket 状态 + 连接队列 | 否 |
| nl_group_lock | 全局 | netlink group 注册表 + 各 netlink_sock recv_queue/blocked_reader/wq | 否 |
| pty_alloc_lock | 全局 | pty_table 分配/释放 + next_pty_index | 否 |
| devtmpfs_lock | 全局 | /dev/ 设备节点注册/查找 | 否 |
| serial_tx_lock / serial_rx_lock | 全局 | 串口发送/接收缓冲区 | 否 |
| recv_lock（per-task） | per-task | recv_buf/head/tail 队列 | 否 |
| bfc_lock | 全局 | BFC 物理页分配器 | 否 |
| slab lock（per-cache） | per-cache | slab partial list + 全路径分配/释放 | 否 |
| fd_lock（per-files_t） | per-process（files_t 内） | fd_table **写路径**（open/close/dup2/socket/SCM_RIGHTS install）；读路径走 RCU | 否 |
| RCU read-side（rcu_read_lock） | per-CPU cli | fd_table **读路径**（sys_read/sys_write/peer scan/SCM_RIGHTS 验证/sys_poll/sys_getdents） | — |
| fat_cache_lock | 全局 | FAT 表缓存 | 否 |

锁获取顺序（防死锁）—— 全局层级（从外到内，必须按此顺序获取）：
  1. tasks_lock             — 进程表全局操作
  2. scheduler_lock[cpu]    — per-CPU 调度器
  3. fd_lock                — per-process fd 表
  4. socket_lock            — AF_UNIX socket 状态 + 连接队列
     nl_group_lock           — netlink group 注册表 + recv_queue（与 socket_lock 平级，无嵌套）
  5. inode_hash_lock        — inode hash 表
  6. i_lock                 — per-inode
  7. fat_lock               — FAT 表操作
  8. ahci_lock              — AHCI DMA
  9. recv_lock              — per-process recv 队列
  10. cache->lock           — per-slab-cache
  11. page_cache_lock       — page cache LRU
  12. devtmpfs_lock         — /dev/ 设备节点
  13. serial_tx/rx_lock     — 串口
  14. bfc_lock              — BFC 页分配
  15. pty_alloc_lock        — PTY 分配

不存在需要同时持 fat_lock + scheduler_lock、ahci_lock + scheduler_lock 等路径。
FAT32 内部顺序固定：i_lock → fat_lock → ahci_lock。
新增约束：scheduler_lock → fd_lock（proc_reap 先 scheduler_lock 再 fd_lock）。

tmpfs（/run）锁序：`socket_lock → mount_lock → inode_hash_lock → tmpfs_i_lock`
（bind→VFS 新嵌套，独立于 `i_lock → fat_lock → ahci_lock`，tmpfs 无块设备依赖）。
- `tmpfs_i_lock` = `struct tmpfs_inode_info.lock`（per-tmpfs-inode），保护 data/size/children。
- 调用链：sys_bind/connect 持 socket_lock → `vfs_mknod_socket`/`vfs_lookup_socket`
  → vfs_resolve（取 mount_lock）→ path_walk/path_walk_parent → i_op->create/lookup
  → inode_get/inode_put（取 inode_hash_lock）→ tmpfs_create/tmpfs_lookup（取 tmpfs_i_lock）。
- 单向无反向获取路径：mount_lock 仅 mount.c 用，inode_hash_lock 仅 inode.c 用，
  tmpfs_i_lock 仅 tmpfs.c 用，无任一在持锁后回取 socket_lock。
design1 锁序总表（调度锁模型定稿，§5.4）：
```
i_lock → fat_lock → ahci_lock          (FAT32 既有)
socket_lock → wq->lock → scheduler_lock (socket recv/send 阻塞，design0 模式 2)
pty 无锁 (SPSC ring，design0 模式 1，不走资源锁→wq->lock)
wq->lock → scheduler_lock              (资源唤醒通用，design1 定稿)
cmd_wq.lock → scheduler_lock           (virtio-gpu)
recv_lock  与 scheduler_lock 互不嵌套   (各自独立)
```
- socket 模式 2：读路径持 socket_lock 查条件 + 持锁 add_wait_queue（取 wq->lock）+ 标 BLOCKED → unlock(socket_lock) → schedule()（取 scheduler_lock，此时 socket_lock 已释放，二者不共持）。不变式：持 wq->lock 时绝不取 socket_lock（__wake_up 回调只取 scheduler_lock，无反向边）。
- pty 模式 1：SPSC ring 条件无锁可读，挂 wq 不持资源锁，故无「资源锁 → wq->lock」一层。
- evdev：HID 中断经 irqfd（`hid_irqfds[]` + `eventfd_signal_isr`，`xhci.c`）正规投递，唤醒全走 wq，无 ISR 直取 scheduler_lock 的例外。

**已完成**：sun_path hash（connect 通过 hash 直接获取 listener PID，消灭全进程 fd_table 扫描）+ RCU（fd_table 读走 rcu_read_lock + file_get/file_put 指针引用，消除所有 peer_fd_lock 读路径），socket_lock → fd_lock 嵌套彻底消除。详见下方 §RCU 设计 和 §sun_path hash 设计。
任何违反此顺序的代码路径必须标注原因和替代方案。

### irq_owner[] — 无锁 atomic

- sys_irq_bind 写：`__atomic_store_n(&irq_owner[irq], pid, __ATOMIC_RELEASE)`
- trap_dispatch 读：`__atomic_load_n(&irq_owner[trapno], __ATOMIC_ACQUIRE)`
- pick_cpu() 读 run_count：`__atomic_load_n(&cpu_locals[i].run_count, __ATOMIC_RELAXED)` — 近似值不影响正确性

### schedule() 锁协议（kernel/proc.c : schedule）

1. spin_lock_irqsave(&scheduler_lock[my_cpu], &flags)
2. run_queue 空 → 检查 prev 是否 BLOCKED/ZOMBIE/REAPING → 切 idle 或直接返回
3. dequeue next from run_queue front (FIFO round-robin)
4. prev==RUNNING → READY + push back; prev==BLOCKED/ZOMBIE/REAPING → 不入队
5. next=RUNNING, update current_task/TSS/IOPM
6. spin_unlock → switch_to(prev,next) → spin_lock_irqsave → spin_unlock_irqrestore
   （unlock 与 switch_to 之间保持 cli，防止中断在 RSP/CR3 切换期间破坏栈）

### 各操作的锁协议

| 操作 | 持锁顺序 | 说明 |
|------|---------|------|
| process_create_elf | tasks_lock → scheduler_lock[cpu] | 先分配槽位，再入队 |
| sys_exit 孤儿收养 | tasks_lock | 修改 mm->parent_pid |
| sys_exit 设 ZOMBIE | scheduler_lock[cpu] | 保护 state |
| sys_exit 唤醒父 | scheduler_lock[pcpu] | 入队父进程 |
| sys_waitpid 查找 | tasks_lock → scheduler_lock[cpu] | 扫描子进程 → 设 REAPING |
| sys_fork 分配 | tasks_lock → scheduler_lock[cpu] | 分配槽位 → 入队 |
| sys_connect listener PID lookup | socket_lock | sun_path hash 直接返回 listener + owner PID，无需 fd_table 扫描 |
| sys_accept peer 地址查找 | 无锁（child->peer_sock 直接指针） | peer_sock 在 connect 时已设置，直接读 sun_path 无需 fd_table 扫描 |
| sys_notify / wake_process | scheduler_lock[target_cpu] | 远程入队唤醒 |
| IRQ wake | scheduler_lock[target_cpu] | irq_owner atomic → 唤醒驱动 |
| sys_yield | scheduler_lock[my_cpu] | state=READY，不入队 |

### 与其他模块的关系

- 进程管理：scheduler_lock 保护 run_queue 和 state，详见 [proc.md](proc.md)
- 调度器：schedule() 持锁穿过 switch_to，详见 [schedule.md](schedule.md)
- VFS/FAT32：fat_lock + i_lock + ahci_lock 固定顺序，详见 [vfs.md](vfs.md)
- IPC：recv_lock 保护 recv 队列，详见 [ipc.md](ipc.md)
- PTY：pty_alloc_lock 保护 PTY 分配，详见 [terminal.md](terminal.md)

## RCU 设计

### 目标

消除 `fd_lock` 读路径——所有对 `fd_table[fd]` 的只读访问（sys_read/sys_write/sys_sendmsg/recvmsg/peer scan）不再需要持 `fd_lock`，彻底消除 `socket_lock → fd_lock` 嵌套。写路径（open/close/dup2/socket/SCM_RIGHTS install）仍持 `fd_lock`，但读路径无锁。

### 为什么选择 RCU

当前 `fd_lock` 的问题：

1. **socket_lock → fd_lock 嵌套**：`sys_connect` 需在 `socket_lock` 下扫描全进程 fd_table 找 listener socket 的 owner PID（`socket.c:1005-1019`），以及 `sock_sendmsg_internal`/`sock_recvmsg_internal` 的 peer 查找路径，构成已知死锁风险。
2. **SCM_RIGHTS 跨进程 fd 读取**：`sock_recvmsg_internal` 在 `socket_lock` 外逐进程取 `peer_fd_lock` 读 fd_table，虽然已拆开嵌套，但锁操作频繁。
3. **高频读路径**：`sys_read/sys_write` 每次系统调用都要取 `fd_lock` 读 `fd_table[fd]`，fd_table 读远多于写，适合 RCU。

### 适合本项目的 RCU 变体

Linux 内核的 SRCU/RCU 机制过于复杂。本项目 2-4 核、MAX_PROC=64、MAX_FD=32，选择**极简 RCU**：

| 特性 | Linux RCU | 本项目极简 RCU |
|------|-----------|---------------|
| 宽限期检测 | per-CPU quiescent state + ftrace | 全局 generation counter + per-CPU 位图 |
| 读者侧开销 | `__rcu_read_lock()` = preempt_disable | `rcu_read_lock()` = 禁止抢占（cli） |
| 写者侧回收 | call_rcu 延迟回调 | `synchronize_rcu()` 同步等待（spin 直到所有 CPU 过宽限期） |
| 适用场景 | 通用 | 内核不可抢占、2-4 核、写者极少 |

**核心约束**：当前内核不可抢占（schedule 仅在 syscall/irq 返回用户态时检查），因此 `rcu_read_lock()` = `cli` 即可保证读者在内核态期间不会发生上下文切换。

**quiescent state 推进触发点**（缺一不可，否则 synchronize_rcu 会 stall）：
1. `rcu_read_unlock()` — 退出显式 read-side CS 时推进
2. `idle_entry()` 循环 — idle 调用 `rcu_read_lock/unlock` 推进（CPU 无 runnable 进程时）
3. `timer_handler()` — 每次定时器中断调 `rcu_quiescent()`，让持续在用户态运行的 CPU 也能推进（关键：缺此项时，若某 CPU 上进程持续 runnable 不阻塞，idle 永不被调度，`cpu_gen` 永不推进 → stall）

### 数据结构

```c
// kernel/rcu.h

#define RCU_MAX_CPUS 4

typedef struct rcu_state {
    atomic_t global_gen;          // 全局 generation counter，写者递增
    atomic_t cpu_gen[RCU_MAX_CPUS]; // per-CPU 已观测到的 generation
    spinlock_t writer_lock;       // 串行化写者（同时只有一个 synchronize_rcu）
} rcu_state_t;

// 全局 RCU 状态
extern rcu_state_t rcu_state;
```

### API

```c
// 读者侧（读端临界区）
static inline void rcu_read_lock(void) {
    // 禁止抢占 = cli（内核不可抢占，cli 保证当前 CPU 不会调度出去）
    // 中断上下文天然在 rcu read-side（不会 schedule）
    __asm__ volatile("cli");
}

static inline void rcu_read_unlock(void) {
    // 记录当前 global_gen 到本 CPU cpu_gen（宽限期推进）
    int gen = atomic_read(&rcu_state.global_gen);
    int cpu = get_cpu_local()->cpu_id;
    atomic_set(&rcu_state.cpu_gen[cpu], gen);
    __asm__ volatile("sti");  // 或 pushfq/popfq 恢复原 IF
}

// 写者侧
void synchronize_rcu(void);  // 等待所有 CPU 过宽限期
void rcu_init(void);

// 中断上下文调用：若本 CPU 不在 read-side CS，推进 cpu_gen 到当前 global_gen
// 用于 timer_handler，让持续在用户态运行的 CPU 也能推进宽限期
static inline void rcu_quiescent(void);

// 受 RCU 保护的指针访问
#define rcu_dereference(p) \
    ({ typeof(p) ___p = __atomic_load_n(&(p), __ATOMIC_CONSUME); ___p; })

#define rcu_assign_pointer(p, v) \
    do { __atomic_store_n(&(p), (v), __ATOMIC_RELEASE); } while(0)
```

### 宽限期机制（synchronize_rcu）

```
写者:
  1. spin_lock(&rcu_state.writer_lock)     // 串行化多个写者
  2. new_gen = atomic_add_return(&rcu_state.global_gen, 1)
  3. for each cpu i:
       while (atomic_read(&rcu_state.cpu_gen[i]) < new_gen - 1)
           pause  // 自旋等待，最多几个 μs（2-4 核，不可抢占）
  4. spin_unlock(&rcu_state.writer_lock)

读者 rcu_read_unlock():
  cpu_gen[my_cpu] = global_gen  // 原子 store，宣告本 CPU 已过宽限期
```

**为什么安全**：
- `rcu_read_lock()` = cli 后，当前 CPU 不会调度，`cpu_gen[my_cpu]` 不会更新
- `synchronize_rcu()` 等 `cpu_gen[i] >= new_gen - 1`，即所有 CPU 至少完成了一次 `rcu_read_unlock()`
- 完成意味着：要么该 CPU 不在 read-side（cpu_gen 已是最新），要么在 read-side 但已退出
- 退出 read-side 后旧指针不再被访问，写者可安全释放

**性能**：2-4 核不可抢占内核，`synchronize_rcu()` 等待时间 < 10μs（所有 CPU 下一次 syscall/irq 返回即过宽限期）。

### fd_table RCU 化

#### 当前结构（files_t）

```c
typedef struct files_t {
    spinlock_t fd_lock;
    struct file fd_table[MAX_FD];
    refcount_t f_count;
} files_t;
```

#### 改造后

```c
typedef struct files_t {
    spinlock_t fd_lock;          // 写路径仍需（open/close/dup2/SCM_RIGHTS install）
    struct file fd_table[MAX_FD]; // RCU 保护：读者无锁，写者持 fd_lock
    refcount_t f_count;
} files_t;
```

**不变**：`fd_table` 仍是固定数组（MAX_FD=32），不引入指针间接层（Linux fdtable 双指针 `fd_array`/`fdtable` 对 32 个 slot 无意义）。

**改变**：读路径不再持 `fd_lock`，改为 `rcu_read_lock/unlock`。

#### 读路径改造

```c
// 改造前（sys_read 伪代码）
spin_lock(&files->fd_lock);
file_t *f = &files->fd_table[fd];
if (f->type == FD_NONE) { spin_unlock(...); return -EBADF; }
// ... 使用 f ...
spin_unlock(&files->fd_lock);

// 改造后
rcu_read_lock();
file_t *f = rcu_dereference(files->fd_table[fd]);  // array entry 本身是 RCU 保护的
if (f->type == FD_NONE) { rcu_read_unlock(); return -EBADF; }
local_entry = *f;  // 拷贝整个 file_t（32 字节），防止写者并发修改
rcu_read_unlock();
// ... 使用 local_entry ...
```

**关键**：读路径拷贝 `file_t` 到栈上局部变量（32 字节结构体，拷贝代价可忽略），退出 RCU read-side 后使用局部变量。这样写者持 `fd_lock` 写 `fd_table[fd]` 时，读者的局部拷贝不受影响。

#### 写路径（不变）

```c
// open/close/dup2/socket/SCM_RIGHTS install — 持 fd_lock 写
spin_lock(&files->fd_lock);
files->fd_table[fd] = new_entry;   // 原子 store（单 entry 写入，<=32B，x86 对齐写原子）
spin_unlock(&files->fd_lock);
```

写者持 `fd_lock` 保证多个写者互斥。`fd_table[fd]` 赋值是 x86 对齐写（`file_t` ≤ 32B，8-byte 对齐的 `struct` 赋值，编译器拆为 4 个 8-byte store），RCU 读者通过 `rcu_dereference` 读到旧值或新值，不会看到撕裂。

#### peer 扫描路径改造（消除 socket_lock → fd_lock 嵌套）

```c
// 改造前（sock_sendmsg_internal peer 查找，socket.c:294-308）
spin_lock(peer_fdlk);                    // ← 嵌套锁
for (int fd = 0; fd < MAX_FD; fd++) {
    if (peer_proc->mm->files->fd_table[fd].type == FD_SOCKET) { ... }
}
spin_unlock(peer_fdlk);

// 改造后
rcu_read_lock();
for (int fd = 0; fd < MAX_FD; fd++) {
    file_t f = rcu_dereference(peer_proc->mm->files->fd_table[fd]);
    if (f.type == FD_SOCKET && f.sock && f.sock != sock &&
        f.sock->peer == current_task->pid && f.sock->state == UNIX_CONNECTED) {
        peer_sock = f.sock;
        break;
    }
}
rcu_read_unlock();
```

**效果**：`sock_sendmsg_internal` / `sock_recvmsg_internal` 的 peer 查找不再需要 `peer_fd_lock`，`socket_lock → fd_lock` 嵌套完全消除。

#### 需改造的路径清单

| 路径 | 当前锁 | 改造后 | 说明 |
|------|--------|--------|------|
| sys_read/sys_write | fd_lock（读） | rcu_read_lock | 高频路径，最大收益 |
| sys_sendmsg/recvmsg fd 验证 | fd_lock（读） | rcu_read_lock | |
| sock_sendmsg_internal peer 查找 | peer_fd_lock（读） | rcu_read_lock | 消除嵌套 |
| sock_recvmsg_internal peer 查找 | peer_fd_lock（读） | rcu_read_lock | 消除嵌套 |
| sys_connect listener PID 查找 | peer_fd_lock（读，在 socket_lock 下） | rcu_read_lock | 消除已知特例嵌套 |
| sys_accept peer 地址查找 | peer_fd_lock（读） | rcu_read_lock | |
| SCM_RIGHTS 源 fd 验证 | sender_fd_lock（读） | rcu_read_lock | |
| sys_poll fd 检查 | fd_lock（读，隐式无锁读 fd_table） | rcu_read_lock | 当前已无锁读，加 RCU 保护 |
| sys_open/close/dup2/socket/SCM_RIGHTS install | fd_lock（写） | fd_lock（写）不变 | 写路径不变 |
| sys_getdents | 无锁读 fd_table[fd] | rcu_read_lock | 加保护 |

#### RCU 读者侧的引用计数安全

RCU read-side 退出后，读者持有的 `file_t` 局部拷贝中的指针（`inode`, `sock`, `pipe` 等）可能被写者释放。规则：

1. **RCU read-side 内做引用计数 bump**：如果在 read-side 内需要长期持有（如 `peer_sock` 跨 `rcu_read_unlock` 使用），必须在 read-side 内 bump refcount，确保对象不会被释放。
2. **局部拷贝仅用于瞬时检查**：如 `f.type == FD_SOCKET` 的类型检查、flags 检查等，不需要 bump refcount，因为检查和决策在 read-side 内完成。
3. **peer_sock 长期使用**：`sock_sendmsg_internal` 找到 `peer_sock` 后，通过 `unix_sock_acquire()` bump `u_count`（在 read-side 内），退出 read-side 后安全使用。

```c
// peer_sock 获取 — RCU read-side 内 bump refcount
rcu_read_lock();
for (int fd = 0; fd < MAX_FD; fd++) {
    file_t f = rcu_dereference(peer_proc->mm->files->fd_table[fd]);
    if (f.type == FD_SOCKET && f.sock && ...) {
        peer_sock = f.sock;
        unix_sock_acquire(peer_sock);  // bump u_count，防止 RCU 宽限期后释放
        break;
    }
}
rcu_read_unlock();
// peer_sock 安全使用（u_count ≥ 1）
// 使用后:
unix_sock_release(peer_sock);  // drop 引用
```

#### fd_table 写者的延迟释放

写者（close）将 `fd_table[fd]` 设为 `FD_NONE` 后，如果有其他 CPU 的读者仍在 RCU read-side 持有旧 `file_t` 拷贝，写者不能立即释放底层对象（inode/sock/pipe），因为读者的局部拷贝可能包含指向这些对象的指针，读者可能在 `rcu_read_unlock` 之前解引用。

**当前已安全**：close 路径在 `fd_lock` 内把 `fd_table[fd]` 设为 `FD_NONE` + `memset` 清零，然后调用 `inode_put`/`sock_close`/pipe refcount drop。这些操作基于引用计数，close 减的是 fd 持有的那一份引用。如果 RCU 读者在 read-side 内也 bump 了引用计数（如上 `unix_sock_acquire`），则 close 时 refcount 不会归零，对象不会被释放，直到读者 drop 引用。

**结论**：RCU 化不需要额外延迟释放机制——已有的 refcount 语义天然保证了安全。`synchronize_rcu()` 仅在需要确认旧版本内存可释放时使用（如 fd_table 整体替换，当前固定数组不涉及）。

### rcu_read_lock 与 irqsave 的关系

| 场景 | 中断状态 | RCU 行为 |
|------|---------|---------|
| syscall 路径（读 fd_table） | IF=1 → rcu_read_lock = cli | RCU 保护，禁止抢占 |
| IRQ handler 读 fd_table | IF=0（硬件自动 cli） | 天然在 RCU read-side，无需额外操作 |
| schedule() 持 scheduler_lock | irqsave 已 cli | 天然在 RCU read-side |
| rcu_read_unlock | 恢复 IF | 如果原 IF=0（IRQ context），不恢复 sti |

**改进**：`rcu_read_lock/unlock` 应保存/恢复 IF，而非无条件 cli/sti：

```c
static inline void rcu_read_lock(uint64_t *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags));
}

static inline void rcu_read_unlock(uint64_t flags) {
    int gen = atomic_read(&rcu_state.global_gen);
    int cpu = get_cpu_local()->cpu_id;
    atomic_set(&rcu_state.cpu_gen[cpu], gen);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}
```

与 `spin_lock_irqsave` 格式统一，避免在 IRQ context 内误开中断。

### RCU 初始化

```c
void rcu_init(void) {
    atomic_set(&rcu_state.global_gen, 0);
    for (int i = 0; i < MAX_CPUS; i++)
        atomic_set(&rcu_state.cpu_gen[i], 0);
    rcu_state.writer_lock = SPINLOCK_INIT;
}
```

在 `kernel_main` 中、调度器启动前调用。

### 锁一览更新（RCU 化后）

| 锁 | 粒度 | 保护对象 | irqsave? | 变化 |
|----|------|---------|----------|------|
| fd_lock（per-files_t） | per-process | fd_table **写路径**（open/close/dup2/socket/SCM_RIGHTS install） | 否 | 读路径改 RCU |
| socket_lock | 全局 | AF_UNIX socket 状态 + 连接队列 | 否 | peer scan 不再需 fd_lock |
| — | — | fd_table **读路径** | — | 改为 rcu_read_lock（cli） |

锁层级更新：`fd_lock` 降级为仅写路径使用，`socket_lock → fd_lock` 嵌套彻底消除，`sys_connect` 不再是已知特例。

---

## sun_path hash 设计

### 目标

`sys_connect` 当前在 `socket_lock` 下扫描全部进程的 fd_table（`socket.c:1005-1019`）寻找 listener socket 的 owner PID。这是 `socket_lock → fd_lock` 嵌套的根源。

**现状**：`unix_bind_table` 已有 hash 表（`socket.c:21`），但只存 `sun_path → unix_sock*` 映射。`sys_connect` 找到 `listener` 后仍需知道 **owner PID**（设置为 `client_sock->peer = listener_pid`），当前通过全进程 fd_table 扫描获取。

**方案**：在 `unix_bind_entry` 中直接存储 owner PID，`sys_connect` 通过 hash 查找一步获得 listener + owner PID，消灭全进程扫描。

### 数据结构变更

```c
// 改造前（socket.h）
typedef struct unix_bind_entry {
    char   sun_path[108];
    struct unix_sock *sock;
    struct unix_bind_entry *next;
} unix_bind_entry_t;

// 改造后
typedef struct unix_bind_entry {
    char   sun_path[108];
    struct unix_sock *sock;
    pid_t  owner_pid;           // 新增：bind/listen 时记录 owner PID
    struct unix_bind_entry *next;
} unix_bind_entry_t;
```

### API 变更

```c
// 改造前
int unix_bind_lookup(const char *sun_path, struct unix_sock **out);
int unix_bind_register(const char *sun_path, struct unix_sock *sock);

// 改造后
int unix_bind_lookup(const char *sun_path, struct unix_sock **out, pid_t *owner_pid);
int unix_bind_register(const char *sun_path, struct unix_sock *sock, pid_t owner_pid);
```

### sys_connect 改造

```c
// 改造前（socket.c:961-1048）
spin_lock(&socket_lock);
int ret = unix_bind_lookup(sun_path, &listener);
// ... 找到 listener 后，全进程扫描找 owner PID（1005-1019 行） ...
for (int i = 0; i < MAX_PROC; i++) {     // ← O(MAX_PROC × MAX_FD)
    spin_lock(peer_fdlk);                  // ← socket_lock → fd_lock 嵌套！
    for (int pf = 0; pf < MAX_FD; pf++) { ... }
    spin_unlock(peer_fdlk);
}

// 改造后
spin_lock(&socket_lock);
pid_t listener_pid = -1;
int ret = unix_bind_lookup(sun_path, &listener, &listener_pid);
if (ret != 0) { spin_unlock(&socket_lock); return -ret; }
// 直接使用 listener_pid，无需扫描 fd_table
client_sock->peer = listener_pid;
```

**效果**：
- `sys_connect` 时间复杂度：O(MAX_PROC × MAX_FD) → O(hash chain length)，通常 O(1)
- `socket_lock → fd_lock` 嵌套彻底消除
- `unix_bind_lookup` 返回的 `owner_pid` 在 `socket_lock` 保护下一致

### owner_pid 一致性保证

| 操作 | 说明 |
|------|------|
| `sys_bind` | 在 `socket_lock` 内调用 `unix_bind_register(path, sock, current_task->pid)`，记录 owner PID |
| `sys_listen` | 不改变 owner PID（bind 时已记录） |
| `sock_close` / `unix_sock_release` | 在 `socket_lock` 内调用 `unix_bind_unregister`，移除条目。关闭后 connect 查不到该路径 |
| 进程 exit | `files_put` → `sock_close` → `unix_bind_unregister`，自动清理 |

**竞态场景**：进程 A bind+listen 后 exit，进程 B connect 同一 sun_path。
- 进程 A exit 时 `sock_close` 在 `socket_lock` 内 unregister
- 进程 B connect 在 `socket_lock` 内 lookup，两者互斥
- 结果：B 看到 -ENOENT 或 A 的 listener（取决于时序），不会看到已释放的 `unix_bind_entry`

**不需要 RCU**：`unix_bind_table` 的所有读写都在 `socket_lock` 内，无需额外保护。

### sys_accept peer 地址查找改造

当前 `sys_accept` 在 `socket_lock` 释放后取 `peer_fd_lock` 查找 peer socket 的 `sun_path`（`socket.c:891-909`）。

改造后：
- `unix_sock` 已有 `sun_path[108]` 字段
- `sys_connect` 创建 child socket 时，child 的 `peer_sock = client_sock`，可直接从 `client_sock->sun_path` 获取
- 如果 `client_sock` 未 bind（匿名 socket），`sun_path[0] == '\0'`，与当前行为一致

```c
// 改造后（sys_accept peer 地址查找）
struct unix_sock *peer_sock = child->peer_sock;  // 直接指针
if (peer_sock && peer_sock->sun_path[0]) {
    // 直接读 sun_path，无需扫描 fd_table
    __memcpy(sa.sun_path, peer_sock->sun_path, 108);
}
```

**注意**：`peer_sock->sun_path` 读在 `socket_lock` 外。由于 `sun_path` 只在 `sys_bind` 时写入一次且不再修改，读是安全的（immutable after bind）。如果需要严格保护，可用 `rcu_dereference` 读取——但 `sun_path` 是内嵌数组而非指针，RCU 对数组无意义。直接读即可。

### sock_sendmsg_internal / sock_recvmsg_internal peer 查找改造

当前 `sock_sendmsg_internal` 在 `peer_sock == NULL` 时通过 `peer_fd_lock` 扫描 fd_table 找 peer socket（`socket.c:294-308`）。

改造后有两层优化：

1. **sun_path hash 提供 owner PID**：`sys_connect` 已正确设置 `client_sock->peer = listener_pid` 和 `child->peer = client_pid`。connected 后 `peer_sock` 直接指针已设置，sendmsg/recvmsg 的 peer 查找走直接指针，不需要 fd_table 扫描。
2. **极端路径（peer_sock 为 NULL 的 fallback）**：理论上 connected socket 的 `peer_sock` 必非 NULL。如果 `peer_sock` 丢失（bug），当前 fallback 扫 fd_table 可改为：用 `sock->peer` PID 反查 `tasks[peer_pid].mm->files`，RCU 读 fd_table 找 peer socket。

```c
// 改造后（sock_sendmsg_internal fallback，极低频）
if (!peer_sock) {
    pid_t peer_pid = sock->peer;
    if (peer_pid < 0 || peer_pid >= MAX_PROC) return -ENOTCONN;
    task_t *peer_proc = &tasks[peer_pid];
    if (peer_proc->pid != peer_pid) return -ENOTCONN;
    if (peer_proc->mm && peer_proc->mm->files) {
        rcu_read_lock(&rcu_flags);
        for (int fd = 0; fd < MAX_FD; fd++) {
            file_t f = rcu_dereference(peer_proc->mm->files->fd_table[fd]);
            if (f.type == FD_SOCKET && f.sock &&
                f.sock != sock && f.sock->peer == current_task->pid &&
                f.sock->state == UNIX_CONNECTED) {
                peer_sock = f.sock;
                unix_sock_acquire(peer_sock);
                break;
            }
        }
        rcu_read_unlock(rcu_flags);
    }
    if (!peer_sock) return -EPIPE;
}
```

---

## RCU + sun_path hash 实施路径

### 阶段 1：sun_path hash（独立，无 RCU 依赖）

1. `unix_bind_entry` 增加 `owner_pid` 字段
2. `unix_bind_register` 增加 `pid_t owner_pid` 参数，`sys_bind` 传 `current_task->pid`
3. `unix_bind_lookup` 增加 `pid_t *owner_pid` 输出参数
4. `sys_connect` 使用 `unix_bind_lookup` 返回的 `listener_pid`，删除全进程 fd_table 扫描（`socket.c:1005-1019`）
5. `sys_accept` peer 地址查找改用 `child->peer_sock->sun_path` 直接读，删除 peer_fd_lock 扫描（`socket.c:891-909`）

**验证**：`sys_connect` 不再有任何 `peer_fd_lock` 获取，`socket_lock → fd_lock` 嵌套消除。

### 阶段 2：RCU 基础设施

1. 新增 `kernel/rcu.h` + `kernel/rcu.c`（rcu_state_t, rcu_read_lock/unlock, synchronize_rcu, rcu_init）
2. `kernel_main` 中调用 `rcu_init()`
3. 验证：单元测试——多 CPU 上读者/写者并发，synchronize_rcu 正确等待

### 阶段 3：fd_table 读路径 RCU 化

1. `sys_read/sys_write` 的 fd_table 读改 `rcu_read_lock` + 局部拷贝
2. `sys_sendmsg/recvmsg` 的 fd 验证改 RCU
3. `sock_sendmsg_internal/sock_recvmsg_internal` 的 peer 查找改 RCU（fallback 路径）
4. `SCM_RIGHTS` 源 fd 验证改 RCU
5. `sys_poll` 的 fd 检查加 `rcu_read_lock` 保护
6. `sys_getdents` 加 RCU 保护

**验证**：所有 `fd_lock` 读路径消失，`fd_lock` 仅剩写路径（open/close/dup2/socket/SCM_RIGHTS install）。

### 阶段 4：锁层级文档更新

- 移除 `sys_connect listener PID lookup` 的已知特例标注
- `fd_lock` 在锁层级中的描述改为"写路径"
- 新增 `RCU read-side` 在锁一览中

### 风险与回退

| 风险 | 缓解 |
|------|------|
| RCU read-side 内 cli 时间过长 | fd_table 读路径短（拷贝 32B + 判断 type），cli < 1μs |
| `synchronize_rcu()` 在高频写场景延迟 | fd_table 写（open/close）频率远低于读，且 2-4 核不可抢占等待 < 10μs |
| `rcu_read_lock/unlock` 忘记配对 | Debug 构建加 ASSERT 检查 nesting depth |
| sun_path hash owner PID 过时（进程 exit 后 PID 重用） | `sock_close` → `unix_bind_unregister` 在 `socket_lock` 内移除条目，PID 重用前条目已清除 |
| 阶段 1 回退 | sun_path hash 改动独立，回退只需恢复 `unix_bind_entry` 和 `sys_connect` 旧代码 |

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| reschedule IPI | sys_notify 跨 CPU 入队后需发 IPI 唤醒目标 CPU（当前靠 timer ~10ms 轮询），延迟影响响应性 | 中 |
| TLB shootdown IPI | mm_release_pages 释放用户页后需通知其他 CPU flush stale TLB（当前 CR3 reload 自动 flush 本 CPU） | 中 |
| work stealing | idle CPU 从繁忙 CPU steal 进程，当前进程不迁移 CPU | 低 |
| wait_queue | wait_node 已预留，替代 WAIT_CHILD/WAIT_RECV 等线性扫描 tasks[] 的唤醒逻辑 | 低 |
| ~~RCU~~ | ~~fdtable 读走 RCU（无锁），消除所有 fd_lock 读路径，彻底避免 socket_lock/fd_lock 嵌套~~ → **已实现**，见 §RCU 设计 | 中→已完成 |
| ~~sun_path hash~~ | ~~AF_UNIX bind/listen 注册路径 hash 表，connect/accept 通过 hash 查找 listener，消灭 sys_connect 全进程 fd_table 扫描~~ → **已实现**，见 §sun_path hash 设计 | 中→已完成 |
| call_rcu 异步回调 | 当前 `synchronize_rcu()` 同步等待 grace period（4核约数十微秒），高频 close 场景下开销累积。引入 `call_rcu` + `struct file` 内嵌 `struct rcu_head`，close 时异步延迟释放，避免阻塞调用者 | 低 |
