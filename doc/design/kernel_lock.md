# 内核锁

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 原子操作实现 | GCC `__atomic` builtins | freestanding 可用，编译器自动 lowering 为 `xchg`，自动插入内存屏障 |
| 2 | spin_lock 是否内含 cli | 否 | 中断控制由调用者负责，锁保持纯自旋语义；irqsave/irqrestore 作为独立 API |
| 3 | 结构体字段 | 仅 `volatile uint32_t locked` | YAGNI — 2-4 核场景无需 debug 字段 |
| 4 | unlock 内存序 | `__ATOMIC_RELEASE` | 与 lock 端 `__ATOMIC_ACQUIRE` 配对，精确表达 acquire-release |
| 5 | 自旋循环 | exchange + pause | 2-4 核无竞争瓶颈，简单优先 |
| 6 | 初始化 | `SPINLOCK_INIT {0}` 宏 | 单字段无复杂初始化逻辑 |
| 7 | locked 类型 | uint32_t | x86-64 上生成 `xchgll`，显式表达宽度意图 |
| 8 | schedule 持锁策略 | irqsave 持锁穿过 switch_to，unlock → switch_to → lock → irqrestore | per-CPU 锁争用极低；irqsave 保证 switch_to 期间中断关闭 |

### 自旋锁原语（kernel/spinlock.h）

spinlock_t : volatile uint32_t locked — 仅此一个字段

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
| slab lock（per-cache） | per-cache | slab partial list | 否 |
| fat_cache_lock | 全局 | FAT 表缓存 | 否 |

锁获取顺序（防死锁）：
1. tasks_lock（低频，先锁）
2. scheduler_lock[cpu]（高频，后锁）

不存在需要同时持 fat_lock + scheduler_lock、ahci_lock + scheduler_lock 等路径。FAT32 内部顺序固定：i_lock → fat_lock → ahci_lock。

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
