# 内核异常基础设施

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | syscall 返回值 | `int64_t`，成功≥0，失败返回 `-errno` | 内核侧统一，用户态 libc 翻译为 -1+errno |
| 2 | 内核函数返回值 | `int` 0/-errno 或指针 NULL=失败 + `out_errno` | fat32_open 等指针返回函数用 out_errno 输出错误码，避免双重判读 |
| 3 | 原子/引用计数 | `atomic_t`/`refcount_t`（ACQ_REL）+ underflow BUG_ON | SMP 安全 + 数据腐败自动捕获 |
| 4 | 断言门控 | BUG_ON/WARN_ON release 保留；ASSERT NDEBUG 控制 | 正常路径零成本（条件 false 一跳）；ASSERT 仅 debug 开销 |
| 5 | slab 并发 | 全路径持 `cache->lock` | 简单可靠，消除 freelist data race；后续可优化为 per-CPU slab |
| 6 | inode 创建竞态 | `inode_get_or_create` 原子 lookup-or-create | 单次 `inode_hash_lock` 保护查找+创建，无窗口 |
| 7 | fd_table 并发 | per-process `fd_lock` | socket peer scan/SCM_RIGHTS/proc_reap 均加锁；锁序 `scheduler_lock → fd_lock` |
| 8 | spinlock 递归检测 | NDEBUG 门控 `cpu_id` 字段 + `BUG_ON(locked && same_cpu)` | release 零开销；debug 每次 spin_lock rdmsr 读 cpu_id（~30 cycles） |
| 9 | 阻塞超时 | `wait_deadline` + `timer_queue_insert` 机制已就绪，多数路径仍设 0 | 机制存在但未全面接入；sys_recv 有 timeout_ms 参数 |

### 内核错误传播

syscall 返回值语义：所有 syscall 返回 `int64_t`，成功≥0（fd/pid/字节数/0），失败返回 `-errno`。用户态 libc 封装统一模式：`r = sys_xxx(args); if (r < 0) { errno = -r; return -1; } return r;`

内核函数返回值约定（部分已落地）：
- `blk_read/blk_write`：返回 `int`（0/-EIO）
- `fat32_read/write/mkdir/unlink/rmdir`：返回 `int`（0/-errno）
- `fat32_open`：返回 `struct inode*`，NULL=失败，通过 `out_errno` 参数输出错误码
- `inode_create`：未标注 `__must_check`

实现：`kernel/bsd/syscall.c` : syscall_dispatch（rax 承载 int64_t 返回值）

### 原子与引用计数

`atomic_t`/`refcount_t` 定义在 `kernel/xcore/atomic.h`。8 个 ref_count/pin_count 字段已替换：

| 位置 | 旧类型 | 新类型 |
|------|--------|--------|
| `struct inode::i_count` | `int ref_count` | `refcount_t` |
| `struct pipe::p_count` | `int ref_count` | `refcount_t` |
| `struct shm::s_count` | `int ref_count` | `refcount_t` |
| `struct file::f_count` | `int ref_count` | `refcount_t` |
| `struct unix_sock::u_count` | `int ref_count` | `refcount_t` |
| `struct files_t::f_count` | `int ref_count`（SEQ_CST） | `refcount_t`（ACQ_REL + underflow 检测） |
| `struct mm_t::m_count` | `int ref_count`（SEQ_CST） | `refcount_t`（ACQ_REL + underflow 检测） |
| `struct cache_page::pin_count` | `int pin_count` | `atomic_t` |

`refcount_inc` ACQ_REL ≈ SEQ_CST 成本，underflow 时 BUG_ON 捕获数据腐败。

### slab 并发

kmalloc/kfree 全路径持 `cache->lock`，消除 freelist data race。

实现：`kernel/xcore/mem/slab.c` : kmalloc/kfree

### inode 创建竞态

`inode_get_or_create` 在单次 `inode_hash_lock` 保护下完成 lookup-or-create：查找不存在则创建并插入 hash 表，无并发窗口。`inode_lookup`/`inode_create` 不再有串联调用。

实现：`kernel/bsd/inode.c` : inode_get_or_create；`kernel/bsd/fat32.c` : fat32_open 调用点

### fd_table 并发

per-process `fd_lock`（spinlock_t）保护所有 fd_table 操作：sys_close/dup2/open/pipe/install_fd/socket/shm。socket sendmsg/recvmsg 读 peer fd_table 时加 peer 的 `fd_lock`。SCM_RIGHTS 读 sender fd_table 时加 sender 的 `fd_lock`。

锁序：`scheduler_lock → fd_lock`

实现：`kernel/bsd/proc.h` : files_t（fd_lock 字段）；`kernel/bsd/syscall.c` 各 fd 操作入口

### spinlock 增强

Debug 构建（NDEBUG 未定义）`spinlock_t` 含 `cpu_id` 字段：`spin_lock` 时 rdmsr 读 `MSR_GS_BASE + 8` 获取当前 cpu_id 并写入，递归死锁检测 `BUG_ON(lk->locked && lk->cpu_id == current_cpu_id)`。Release 构建零开销（不读写 cpu_id）。

实现：`kernel/xcore/spinlock.h` : spinlock_t；`kernel/xcore/sched.c` : current_cpu_id_debug

### 锁协议

15 层锁层级表（完整表见 `kernel_lock.md`）：

1. tasks_lock → 2. scheduler_lock → 3. fd_lock → 4. inode_hash_lock → 5. i_lock → 6. fat_lock → 7. ahci_lock → 8. recv_lock → 9. cache->lock → 10. serial_tx/rx_lock → 11. bfc_lock

特例：`socket_lock → fd_lock`（socket 操作需同时持有 socket 内部锁和 fd_lock，顺序固定）。

### 日志与断言基础设施

代码已实现但未全面集成到调用点：

- `printk`：分级日志（LOG_DEBUG~LOG_PANIC）+ `log_level` 过滤。实现：`kernel/xcore/log.h/log.c`
- `BUG_ON`/`WARN_ON`/`ASSERT`：宏已定义。BUG_ON/WARN_ON release 保留；ASSERT NDEBUG 门控。实现：`kernel/xcore/log.h:27-42`
- `panic()`：打印原因 + 寄存器 + dump_stack_trace + halt。实现：`kernel/xcore/log.c:22-36`
- `dump_stack_trace()`：独立 RBP 链回溯函数。实现：`kernel/xcore/log.c:38-51`

当前状态：内核约 142 处仍用 `serial_printf`（无分级）；`trap_dispatch` 内核态异常仍调 `halt()` 而非 `panic()`；BUG_ON/WARN_ON/ASSERT 零调用点；`dump_stack_trace` 被 `panic()` 调用但 `trap_dispatch` 仍有独立内联回溯代码未复用。

### 用户态指针保护

`copy_from_user/copy_to_user` 已广泛使用（`kernel/xcore/mem/copy_user.c`）。`validate_user_buf/validate_user_ptr` 存在（`kernel/driver/user_check.h`）。残余直接访问：`sys_pipe` 直接写用户指针；`sys_getdents` 无 len 上限。

### 与其他模块的关系

| 模块 | 关系 | 文档 |
|------|------|------|
| 锁协议 | 锁层级表 + spinlock 增强 | [kernel_lock.md](kernel_lock.md) |
| 调度器 | timer_queue 超时机制 | [schedule.md](schedule.md) |
| 进程管理 | process_create_elf 回滚 | [proc.md](proc.md) |
| VFS | inode 竞态 + FAT chain | [vfs.md](vfs.md) |
| IPC | sys_req 超时 | [ipc.md](ipc.md) |
| 内存管理 | slab 并发 | [mem.md](mem.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| syscall 返回值语义统一 | `trap.h` 签名仍为 `uint64_t`，需改为 `int64_t`；sys_mmap 等 0=失败需改为 -errno | 高 |
| 内核函数 `__must_check` | `inode_create` 等不可忽略返回值的函数未标注 | 低 |
| 分配失败回滚 | `process_create_elf` 错误路径泄漏栈页（5 处），需 goto cleanup 集中回滚 | 高 |
| 用户态指针加固 | `sys_pipe` 直接写用户指针；`sys_getdents` 无 len 上限；`sys_mmap MAP_PHYSICAL` 未验证 offset 范围 | 高 |
| printk 调用迁移 | 142 处 `serial_printf` 需逐步迁移为 `printk(LOG_XX, ...)` | 中 |
| panic() 接入异常处理 | `trap_dispatch` 内核态异常仍调 `halt()`，需改为 `panic()` | 中 |
| 断言插入 | BUG_ON/WARN_ON/ASSERT 零调用点，需插入首批高价值位置（spin_lock 递归、kmalloc 返回值、inode_get/put ref_count） | 中 |
| 栈回溯复用 | `trap_dispatch` 仍有独立内联回溯代码，需复用 `dump_stack_trace()` | 低 |
| IPC 阻塞超时 | sys_req/sys_msg_to/sys_ioctl 均设 `wait_deadline=0`（永久等待），需加默认超时 | 中 |
| socket 阻塞超时 | accept/recvmsg 阻塞无 `wait_deadline` | 中 |
| FAT chain 循环检测 | walk_chain/free_chain/write tail loop 均无循环计数器，磁盘损坏时内核无限循环 | 中 |
| pipe 阻塞超时 | pipe read/write 阻塞 `wait_deadline=0` | 中 |
| 内核内存记账 | 无 `kernel_mem_stats`，kmalloc 失败静默返回 NULL 无诊断 | 低 |
| syscall 参数上限 | `sys_getdents/mmap/read/write` 无大小上限 | 中 |
| fd 数量可调 | `MAX_FD=32` 硬常量，alloc_fd 返回 -EMFILE 时无警告日志 | 低 |
| proc_reap 竞态 | `mm_release` 扫描 `tasks[]` 无锁，与 `task_reap` 持 `tasks_lock` 存竞态窗口 | 中 |
