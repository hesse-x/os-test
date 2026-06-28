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
| pty_alloc_lock | 全局 | pty_table 分配/释放 + next_pty_index | 否 |
| devtmpfs_lock | 全局 | /dev/ 设备节点注册/查找 | 否 |
| serial_tx_lock / serial_rx_lock | 全局 | 串口发送/接收缓冲区 | 否 |
| recv_lock（per-task） | per-task | recv_buf/head/tail 队列 | 否 |
| bfc_lock | 全局 | BFC 物理页分配器 | 否 |
| slab lock（per-cache） | per-cache | slab partial list + 全路径分配/释放 | 否 |
| fd_lock（per-files_t） | per-process（files_t 内） | fd_table 修改 + 跨进程读取 | 否 |
| fat_cache_lock | 全局 | FAT 表缓存 | 否 |

锁获取顺序（防死锁）—— 全局层级（从外到内，必须按此顺序获取）：
  1. tasks_lock             — 进程表全局操作
  2. scheduler_lock[cpu]    — per-CPU 调度器
  3. fd_lock                — per-process fd 表
  4. socket_lock            — AF_UNIX socket 状态 + 连接队列
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
**过度方案**：尽量拆开 socket_lock/fd_lock 嵌套——sys_accept/sendmsg/recvmsg peer scan/SCM_RIGHTS 先拿 socket_lock 获取必要信息，释放后再拿 fd_lock。sys_connect 仍嵌套 socket_lock → peer_fd_lock（需原子查找+连接），标注为已知特例。
**长远目标**：RCU（fdtable 读无锁）+ sun_path hash（connect 不扫描全进程 fd_table），彻底消除嵌套。
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
| sys_connect listener PID lookup | socket_lock → peer_fd_lock（**已知特例**） | 需原子查找+连接，无法拆开；长远 TODO: sun_path hash 消灭全进程扫描 |
| sys_accept peer 地址查找 | peer_fd_lock（socket_lock 释放后） | 6.1 已拆开 backlog dequeue 与 fd 分配；peer 地址查找取 peer_fd_lock 无嵌套 |
| sys_notify / wake_process | scheduler_lock[target_cpu] | 远程入队唤醒 |
| IRQ wake | scheduler_lock[target_cpu] | irq_owner atomic → 唤醒驱动 |
| sys_yield | scheduler_lock[my_cpu] | state=READY，不入队 |

### 与其他模块的关系

- 进程管理：scheduler_lock 保护 run_queue 和 state，详见 [proc.md](proc.md)
- 调度器：schedule() 持锁穿过 switch_to，详见 [schedule.md](schedule.md)
- VFS/FAT32：fat_lock + i_lock + ahci_lock 固定顺序，详见 [vfs.md](vfs.md)
- IPC：recv_lock 保护 recv 队列，详见 [ipc.md](ipc.md)
- PTY：pty_alloc_lock 保护 PTY 分配，详见 [terminal.md](terminal.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| reschedule IPI | sys_notify 跨 CPU 入队后需发 IPI 唤醒目标 CPU（当前靠 timer ~10ms 轮询），延迟影响响应性 | 中 |
| TLB shootdown IPI | mm_release_pages 释放用户页后需通知其他 CPU flush stale TLB（当前 CR3 reload 自动 flush 本 CPU） | 中 |
| work stealing | idle CPU 从繁忙 CPU steal 进程，当前进程不迁移 CPU | 低 |
| wait_queue | wait_node 已预留，替代 WAIT_CHILD/WAIT_RECV 等线性扫描 tasks[] 的唤醒逻辑 | 低 |
| **RCU** | fdtable 读走 RCU（无锁），消除所有 fd_lock 读路径，彻底避免 socket_lock/fd_lock 嵌套。过度方案拆开嵌套后 RCU 是最终解 | 中 |
| **sun_path hash** | AF_UNIX bind/listen 注册路径 hash 表，connect/accept 通过 hash 查找 listener，消灭 sys_connect 全进程 fd_table 扫描 | 中 |
