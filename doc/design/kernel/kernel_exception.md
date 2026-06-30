# 内核异常基础设施（Kernel Exception Infrastructure）

当前内核"加点东西就 hang"的根本原因不是某个 syscall 的 bug，而是缺少几个系统性基础设施。本文档规划 5 个大模块的补建工作，每个模块列出：现状、目标、具体工作项、验证标准。

## 实现进度总览

| 工作项 | 状态 | 说明 |
|--------|------|------|
| 1.1 统一 syscall 返回值 | ❌ | `trap.h` 仍为 `uint64_t`；`syscall.h` 仍为旧约定 |
| 1.2 内核函数返回值约定 | 🔶 | `blk_read/blk_write` 已返回 `int`；`fat32_read/write/mkdir/unlink/rmdir` 已返回 `int`（0/-errno）；`fat32_open` 返回 `struct inode*`（通过 `out_errno` 输出错误码）；`inode_create` 未标 `__must_check` |
| 1.3 分配失败回滚 | ❌ | `process_create_elf` 错误路径泄漏 `stack_pages`（`proc.c:899,905,912,917,924`）；`mm_put(mm)` 正确释放 PML4；进程槽位未泄漏（错误路径 slot 仍为 UNUSED） |
| 1.4 用户态指针加固 | 🔶 | `copy_from_user/copy_to_user` 已广泛使用；`sys_pipe` 仍直接写用户指针（`trap.c:2221-2222`）；`sys_getdents` 无 `len` 上限（`vfs.c:205`） |
| 2.1 printk | ✅❌ | `log.h/log.c` 已实现，零调用点；内核全用 `serial_printf` |
| 2.2 BUG_ON/WARN_ON/ASSERT | ✅❌ | 宏已定义（`log.h:27-42`），零调用点 |
| 2.3 panic() | ✅❌ | `log.c:22-36` 已实现，`trap_dispatch` 内核态异常仍调 `halt()` 而非 `panic()` |
| 2.4 栈回溯通用化 | ✅❌ | `dump_stack_trace()` 已提取为独立函数（`log.c:38-51`），`trap_dispatch` 仍有独立回溯代码未复用 |
| 3.1 atomic_t/refcount_t | ✅ | `kernel/atomic.h` 已创建，包含 atomic_t/refcount_t 全部定义 |
| 3.2 替换 plain ref_count | ✅ | 8 个 ref_count/pin_count 字段已替换为 refcount_t/atomic_t |
| 3.3 slab 并发修复 | ✅ | kmalloc/kfree 全路径持 cache->lock |
| 3.4 inode lookup/create 竞态 | ✅ | `inode_get_or_create` 已实现并被 `fat32_open` 使用；`inode_lookup/inode_create` 不再有串联调用 |
| 3.5 fd_table 并发 | ✅ | `fd_lock` 已添加并保护所有 fd_table 操作（sys_close/dup2/open/pipe/install_fd/socket/shm） |
| 3.6 spinlock 增强 | ✅ | `spinlock_t` debug 构建含 `cpu_id` 字段 + 递归死锁检测 |
| 3.7 锁协议文档化 | ✅ | 完整 15 层锁层级表 + fd_lock 条目 + socket_lock→fd_lock 特例标注 |
| 4.1 IPC 阻塞超时 | ❌ | `sys_req/sys_msg_to/sys_ioctl` 均设 `wait_deadline=0`（`trap.c:797,2744,1683`） |
| 4.2 socket 阻塞超时 | ❌ | `accept/recvmsg` 阻塞无 `wait_deadline` |
| 4.3 FAT chain 循环检测 | ❌ | `walk_chain/free_chain/write tail loop` 均无循环计数器 |
| 4.4 pipe 阻塞超时 | ❌ | pipe read/write 阻塞 `wait_deadline=0` |
| 5.1 内核内存记账 | ❌ | 无 `kernel_mem_stats`；kmalloc 失败静默返回 NULL |
| 5.2 process_create_elf 回收 | ❌ | 同 1.3 |
| 5.3 syscall 参数上限 | ❌ | `sys_getdents/mmap/read/write` 无上限 |
| 5.4 fd 数量可调 | ❌ | `MAX_FD=32` 硬常量，无 EMFILE 警告 |
| 5.5 proc_reap 竞态 | 🔶 | `task_reap` 持 `tasks_lock`；`mm_release` 扫描 `tasks[]` 无锁 |

✅ = 已实现 ❌ = 未实现 🔶 = 部分实现

---

## 1. 内核错误传播体系

### 现状

- `errno.h` 定义了 38 个错误码，但内核函数缺乏统一的返回值约定
- syscall 返回值语义不统一：有的返回 0=成功/正值=errno，有的返回非零=成功/0=失败（见 `syscall.h` 注释）
- 下层失败（kmalloc NULL、blk_write 失败）被上层静默忽略，代码继续往下跑
- 部分分配成功、部分失败时没有回滚，资源永久泄漏（`process_create_elf` 所有错误路径泄漏栈页/PML4/进程槽位）
- `validate_user_buf/validate_user_ptr` 存在但大量 syscall 不使用；`copy_from_user/copy_to_user` 存在但多处用 `__memcpy` 绕过

### 目标

建立从底层到 syscall 出口的统一错误传播链：**任何失败都必须被发现、被传递、被处理**。

### 工作项

#### 1.1 统一 syscall 返回值语义

**规则**：
- 所有 syscall 返回 `int64_t`
- 成功：返回 >= 0 的值（具体值含义由各 syscall 定义，如 fd、pid、字节数、0）
- 失败：返回 `-errno`（负数，如 `-EINVAL`、`-ENOMEM`）

**改动**：
- `trap.h` 中所有 `uint64_t sys_xxx(...)` 签名改为 `int64_t sys_xxx(...)`
- `syscall_dispatch` 调整：rax 直接承载 int64_t 返回值
- `common/syscall.h` 用户态 wrapper 适配新语义
- 清理现有不一致：`sys_mmap` 当前 0=失败改为 `-ENOMEM`，`sys_spawn/sys_waitpid` 当前 0=失败改为 `-ECHILD/-ENOMEM`

#### 1.2 内核函数返回值约定

**规则**：
- 返回 `int`：0 = 成功，负值 = `-errno`（与 syscall 对齐）
- 返回指针：`NULL` = 失败（调用方必须检查）
- 返回 `bool`：`false` = 失败
- `__must_check` 标注所有不可忽略返回值的函数

**改动**：
- `fat32.c`：`fat32_read/fat32_write/fat32_mkdir/fat32_unlink/fat32_rmdir` 返回值已改为 `int`（0/-errno） ✅；`fat32_open` 返回 `struct inode*`，通过 `out_errno` 输出错误码（不需再改）
- `vfs.c`：各 syscall 入口检查 fat32 返回值，失败时直接返回错误
- `blk_write/blk_read`：~~当前返回 void，改为返回 `int`（0/-EIO）~~ ✅ 已返回 `int`
- `inode.c`：`inode_create` 返回值标注 `__must_check`
- `map_user_page_direct/map_user_pages`：已有 `__must_check`，确认所有调用方都检查了返回值

#### 1.3 分配失败回滚（cleanup-on-fail）

> **未实现**。`process_create_elf`（`proc.c:884-993`）错误路径泄漏 `stack_pages`：`mm_create` 失败（:905）、`elf_load` 失败（:912）、user_stack_page 失败（:917）、`map_user_page_direct` 失败（:924）均未释放已分配的栈页（2页 `bfc_alloc_page`）。`mm_put(mm)` 在各错误路径已正确调用，PML4 不泄漏。进程槽位不泄漏（错误路径未修改 slot 状态，仍为 UNUSED）。`sys_open` 已正确处理（`inode_put` on fd alloc failure）。

**模式**：采用 goto cleanup 集中式回滚（Linux 内核惯例）。

**改动**：
- `process_create_elf`：重写为 goto cleanup 模式，失败路径释放所有已分配资源（栈页 `bfc_free_page(stack_pages, 2)`、PML4 页、用户栈页、进程槽位重置）
- `sys_mmap`：MAP_PHYSICAL 路径失败时释放已映射的页
- `sys_open`：fat32_open 失败时释放已 alloc_fd 的槽位

#### 1.4 用户态指针/参数检查加固

> **部分实现**。`copy_from_user/copy_to_user` 已广泛使用，`validate_user_buf/ptr` 已存在。但：`sys_pipe` 仍直接写用户指针（`trap.c:2221-2222` `((__force int *)fd_ptr)[0] = read_fd`）；`sys_getdents` 无 `len` 上限（`vfs.c:205` `kmalloc(len)` 无限制）；`sys_open` 固定 256 字节 `strncpy_from_user`；`sys_mmap MAP_PHYSICAL` 未验证 offset 范围（可映射任意物理地址，严重提权漏洞）。

**改动**：
- 所有 syscall 入口添加参数范围检查（fd 范围、pid 范围、size 上限）
- 全部用户态内存访问走 `copy_from_user/copy_to_user`，禁止 `__memcpy` 直接访问用户指针
- `sys_pipe`：`((__force int *)fd_ptr)[0] = read_fd` 改为 `copy_to_user`
- `sys_getdents`：添加 `len` 上限检查（如 `len <= 4096`）
- `sys_mmap MAP_PHYSICAL`：验证 `offset` 在合法物理内存范围（防止用户映射内核空间/设备 MMIO）
- `sys_open`：当前 `strncpy_from_user(path, upath, 256)` 已合理（匹配 PATH_MAX），暂不改；远期可优化为先 `strlen` 再按实际长度拷贝

### 验证

- 所有 syscall 在非法参数下返回 `-errno` 而不是 hang/静默继续
- `process_create_elf` 在 PML4 分配失败后 kmalloc 总量不增长（无泄漏）
- 用户态程序收到明确 errno，能正确处理错误
- `sys_mmap MAP_PHYSICAL` 传入越界 offset → 返回 `-EINVAL`（而非映射内核物理内存）

---

## 2. 内核可观测性（Logging + Panic + Assert）

### 现状

> **基础设施已实现但未集成**。`kernel/log.h` + `kernel/log.c` 已包含 `printk`（含 LOG_DEBUG~LOG_PANIC 分级 + `log_level` 过滤）、`BUG_ON`/`WARN_ON`/`ASSERT` 宏、`panic()`（含 CPU ID + `dump_stack_trace` + halt）、`dump_stack_trace()`（独立 RBP 链回溯函数）。但**零调用点**：内核约 142 处仍全用 `serial_printf`，`trap_dispatch` 内核态异常仍调 `halt()` 而非 `panic()`（`trap.c:367`），`BUG_ON`/`WARN_ON`/`ASSERT` 无任何调用。

- `serial_printf` 为唯一日志手段（142 处调用点），无分级、无过滤；且非 DEBUG 构建时全部编译为 `((void)0)`
- `BUG_ON/WARN_ON/ASSERT` 宏已定义但无调用点，内核违反不变量时静默继续
- `panic()` 已实现但未接入异常处理路径，内核态异常仍调 `halt()`（`trap.c:367`）
- `dump_stack_trace()` 已为独立函数（`log.c:38`），但 `trap_dispatch` 仍有独立的内联回溯代码（`trap.c:259-294`）未复用
- 异常时 trap_dispatch 对用户态进程发送信号，对内核态异常调 `halt()`（缺 kernel panic 路径）

### 目标

开发阶段内核违反不变量时**主动停下并报告**，而不是静默继续跑坏状态。

### 工作项

#### 2.1 日志分级系统（printk）

> **已实现**（`kernel/log.h` + `kernel/log.c`），但未集成：内核零 `printk()` 调用点，全用 `serial_printf`。

**设计**：
```
#define LOG_DEBUG   0  // 开发调试（默认关闭）
#define LOG_INFO    1  // 正常流程关键节点
#define LOG_WARN    2  // 可恢复异常（不应发生但已处理）
#define LOG_ERROR   3  // 严重错误（功能受损但系统可继续）
#define LOG_PANIC   4  // 不可恢复（系统必须停）
```

- `printk(level, fmt, ...)` = `serial_printf(fmt, ...)` + level 过滤
- 全局 `log_level` 变量控制最低输出级别（默认 `LOG_WARN`，debug 构建设为 `LOG_DEBUG`）
- `LOG_PANIC` 级别无条件输出并调用 `panic()`
- 现有 `serial_printf` 调用逐步迁移为 `printk(LOG_INFO/WARN, ...)`，保留 `serial_printf` 作为早期启动（printk 未初始化前）的底层输出

**文件**：新增 `kernel/log.h` + `kernel/log.c`

#### 2.2 断言宏（BUG_ON / WARN_ON / ASSERT）

> **已实现**（`kernel/log.h:27-42`），但零调用点。待插入首批高价值位置。

**NDEBUG 门控原则**（与 Linux `CONFIG_DEBUG_SPINLOCK` 对齐）：

**Release 构建（NDEBUG 定义）永远保留的检查**：
- `BUG_ON`：正常路径零成本（一次比较，条件为 false 就跳过）；触发时系统必须停
- `WARN_ON`：正常路径零成本；触发时输出警告但继续运行
- `ASSERT`：**NDEBUG 控制** — release 下编译为 `((void)0)`，debug 下等同 `BUG_ON`
- `printk` + log_level：零额外成本（只是替 serial_printf 加 level tag），用户可感知错误信息
- `panic()` 替代 `halt()`：仅内核异常时触发，从"静默挂死"变为"报告原因后停机"
- `refcount_t` atomic 操作 + underflow BUG_ON：ACQ_REL ≈ SEQ_CST 成本相同，underflow 捕获数据腐败

**NDEBUG 控制（仅 debug 构建启用）**：
- `spin_lock` cpu_id 跟踪 + 递归死锁检测：每次 spin_lock 跑 `rdmsr` (~30 cycles)，纯内核内部不变量
- 其他每次调用有不可忽略开销、仅检查内核内部不变量的检测

**判断标准**：检查在正常路径（条件为 false）时是否零成本 → release 保留。每次调用是否有不可忽略开销 → NDEBUG 控制。

**设计**：
```
// BUG_ON(cond): cond 为真 → panic（不可恢复的不变量违反）
#define BUG_ON(cond)  do { if (cond) panic("BUG_ON: %s at %s:%d", #cond, __FILE__, __LINE__); } while(0)

// WARN_ON(cond): cond 为真 → printk(LOG_WARN) + 返回（可恢复的异常，不应发生）
#define WARN_ON(cond) ({ int __ret = !!cond; if (__ret) printk(LOG_WARN, "WARN_ON: %s at %s:%d", #cond, __FILE__, __LINE__); __ret; })

// ASSERT(cond): debug 构建等同 BUG_ON，release 构建编译时移除
#ifdef NDEBUG
#define ASSERT(cond) ((void)0)
#else
#define ASSERT(cond) BUG_ON(cond)
#endif
```

**插入点**（首批高价值位置）：
- `spin_lock`：NDEBUG 门控的递归死锁检测 `BUG_ON(lk->locked && lk->cpu_id == current_cpu_id_debug())`（需给 spinlock_t 加 `cpu_id` 字段，详见 3.6）
- `kmalloc` 返回值：所有调用方添加 `ASSERT(ptr != NULL)` 或 `BUG_ON(!ptr)`
- `inode_get`：`ASSERT(ip->ref_count > 0)`（防止 ref_count 为 0 时 get）
- `inode_put`：`BUG_ON(ip->ref_count < 0)`（防止 ref_count 负数）
- `sys_req/sys_msg`：`WARN_ON(target_pid >= MAX_PROC || procs[target_pid].state == UNUSED)`
- `proc_reap`：`ASSERT(proc->state == ZOMBIE)`
- `page_cache_release`：`WARN_ON(cp->pin_count <= 0)`（替代当前静默忽略）
- `fat32_free_chain`：`WARN_ON(cluster >= 2 && 循环检测)`（详见 3.2）

#### 2.3 panic() 函数

> **已实现**（`kernel/log.c:22-36`），但 `trap_dispatch` 内核态异常仍调 `halt()` 而非 `panic()`。需替换 `trap.c:367` 的 `halt()` 为 `panic()`。

**设计**：
```
void panic(const char *fmt, ...) {
    printk(LOG_PANIC, fmt, ...);          // 打印 panic 原因
    dump_registers();                      // 打印当前 CPU 寄存器
    dump_stack_trace();                    // 栈回溯（debug 构建已有，需通用化）
    // SMP: 向所有其他 CPU 发 IPI halt
    for (;;) { hlt(); }                    // 永久停机
}
```

- 替代 `trap_dispatch` 中内核态异常（#PF/#GP in kernel）的当前行为（直接继续 → 损坏状态），改为 `panic()`
- `trap_dispatch` 中检测 `tf->cs & 3 == 0`（内核态异常）→ panic；`tf->cs & 3 == 3`（用户态异常）→ kill 进程（保持现有行为）

#### 2.4 栈回溯通用化

> **已实现**（`kernel/log.c:38-51` `dump_stack_trace()` 已为独立函数，`panic()` 已调用它）。但 `trap_dispatch` 仍有独立的内联回溯代码（`trap.c:262-294`），未复用 `dump_stack_trace()`。

**现状**：debug 构建已有 RBP 链栈回溯（`-g -fno-omit-frame-pointer`），但仅在 trap handler 中触发。

**改动**：
- `dump_stack_trace()` 提为独立函数，`panic()` 和 `WARN_ON/BUG_ON` 都可调用
- 非 debug 构建提供有限回溯（通过 `.eh_frame` 或 `ORC unwinder`，远期目标；近期依赖 debug 构建即可）

### 验证

- `BUG_ON(1)` 触发 panic：串口输出原因 + 寄存器 + 栈回溯 + 系统停机
- `WARN_ON(1)` 触发 warn 日志：串口输出警告但系统继续运行
- 内核态 #PF（如访问 NULL）触发 panic 而非静默继续
- 调整 `log_level` 后低级别日志消失

---

## 3. 并发安全基础设施

### 现状

- 无 `atomic_t/refcount_t` 类型，ref_count 用 plain `int`，SMP 下 data race
- slab 分配器：kmalloc 快速路径无锁读写 freelist，kfree 跨 CPU 持锁写同一字段 → freelist 损坏
- `inode_get`：`ref_count++` 是 plain C 增量，无原子操作
- `inode_lookup/inode_create`：两个进程并发 open 同一文件创建重复 inode
- `fd_table`：per-process 无锁保护，跨进程访问（socket peer scan、SCM_RIGHTS）是竞态
- spinlock 无递归检测、无死锁检测、无超时；spin_lock_irqsave 在锁争用时中断禁用时间过长
- 锁获取顺序未文档化、未强制

### 目标

提供 SMP 安全的原子原语 + 引用计数基础设施 + 锁协议文档化。

### 工作项

#### 3.1 atomic_t / refcount_t 类型

**设计**：新增 `kernel/atomic.h`

```
typedef struct { int counter; } atomic_t;

static inline int atomic_read(atomic_t *v)        { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }
static inline void atomic_set(atomic_t *v, int i)  { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline int atomic_add_return(atomic_t *v, int i) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_ACQ_REL); }
static inline int atomic_sub_return(atomic_t *v, int i) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_ACQ_REL); }
static inline int atomic_inc_return(atomic_t *v)   { return atomic_add_return(v, 1); }
static inline int atomic_dec_return(atomic_t *v)   { return atomic_sub_return(v, 1); }
static inline bool atomic_dec_and_test(atomic_t *v) { return atomic_sub_return(v, 1) == 0; }

// refcount_t: 0 = free, >0 = in-use, BUG_ON underflow
typedef struct { atomic_t refs; } refcount_t;

static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline int  refcount_read(refcount_t *r)       { return atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r)        { int old = atomic_inc_return(&r->refs); BUG_ON(old <= 0); }
static inline bool refcount_dec_and_test(refcount_t *r) { int old = atomic_dec_return(&r->refs); BUG_ON(old < 0); return old == 1; }
```

#### 3.2 替换所有 plain ref_count 为 refcount_t

| 位置 | 当前 | 改为 |
|------|------|------|
| `struct inode::ref_count` | `int ref_count` | `refcount_t i_count` |
| `struct pipe::ref_count` | `int ref_count` | `refcount_t p_count` |
| `struct shm::ref_count` | `int ref_count` | `refcount_t s_count` |
| `struct file::file_data.ref_count` | `int ref_count` | `refcount_t f_count` |
| `struct unix_sock::ref_count` | `int ref_count` | `refcount_t u_count` |
| `struct files_t::ref_count` | `int ref_count`（SEQ_CST atomic） | `refcount_t f_count` |
| `struct mm_t::ref_count` | `int ref_count`（SEQ_CST atomic） | `refcount_t m_count` |
| `struct cache_page::pin_count` | `int pin_count` | `atomic_t pin_count` |

- `inode_get`：`ip->ref_count++` → `refcount_inc(&ip->i_count)`
- `inode_put`：`ip->ref_count--` → `if (refcount_dec_and_test(&ip->i_count)) ...`
- `shm_get/shm_put`：同理（当前 `__atomic_add_fetch/__atomic_fetch_sub` 但用 RELAXED，改为 ACQ_REL）
- `files_put/mm_put`：当前已用 `__atomic_sub_fetch`（SEQ_CST），改为 `refcount_dec_and_test`（ACQ_REL，语义等价但统一接口 + 带 underflow 检测）
- `unix_sock ref_count`：当前 plain `++`/`--`，改为 `refcount_inc`/`refcount_dec_and_test`
- `page_cache_release`：`pin_count--` → `atomic_dec_return`，underflow 时 `WARN_ON`

#### 3.3 slab 分配器并发修复

**方案 A（近期，推荐）**：移除快速路径无锁优化，所有 kmalloc/kfree 都走 `cache->lock`。

- 简单可靠，消除 freelist data race
- 性能损失可接受（当前系统进程数少，锁争用低）
- 后续可优化为 per-CPU slab（Linux 方案）

**方案 B（远期优化）**：per-CPU active slab + fast path，但要求：
- active slab 的 `freelist` 和 `inuse` 只由本 CPU 修改（本地 CPU 中断禁用即可保护）
- 跨 CPU free 时走 slow path（加 cache->lock → 将对象加入 remote slab 的 freelist）
- 需 `Page::slab.cpu_id` 字段标记本 CPU active slab

**近期选方案 A**：kfree 直接加 `cache->lock`，kmalloc 也加 `cache->lock`。

#### 3.4 inode_lookup/inode_create 竞态修复

> **部分实现**：`inode_hash_lock` 已存在（`inode.c:7`），`inode_lookup` 和 `inode_create` 各自加锁。但 `fat32_open` 中 `inode_lookup(cluster)` 返回 NULL 后到 `inode_create(...)` 之间仍有窗口（`fat32.c:676-680`），两个进程并发 open 同一文件可创建重复 inode。需合并为 `inode_lookup_create` 原子操作。

**方案**：`inode_hash_lock`（全局 hash 表锁）保护 lookup + create 的原子性。

- ~~当前 `inode.c` 的 hash 表操作无锁（`inode_hash` 链表插入/查找是 plain C）~~ 已有 `inode_hash_lock`
- `inode_lookup`：加锁 → 查找 → 解锁
- `inode_create`：加锁 → 查找（确认不存在）→ 创建 → 插入 → 解锁
- 或者：`inode_lookup + inode_create` 合并为 `inode_lookup_create(ino, type, ...)`，单次加锁完成 lookup-or-create

#### 3.5 fd_table 并发保护

**现状**：`files_t::fd_table[MAX_FD]` 无锁，跨进程访问场景：
- socket sendmsg/recvmsg 读取 peer proc 的 fd_table（查找 peer socket）
- SCM_RIGHTS 读取 sender proc 的 fd_table（查找传递的 fd）
- proc_reap 遍历 fd_table 关闭所有 fd

**方案**：per-process `fd_lock`（spinlock_t）

- `sys_close/sys_dup2/sys_install_fd/sys_open/sys_pipe/sys_socket`：修改 fd_table 前加 `fd_lock`
- socket 代码访问 peer fd_table：加 peer 的 `fd_lock`
- SCM_RIGHTS：加 sender 的 `fd_lock`
- proc_reap：已持 `scheduler_lock`，需确认与 `fd_lock` 的嵌套顺序

**锁顺序**：`scheduler_lock → fd_lock`（proc_reap 先 scheduler_lock 再 fd_lock）

#### 3.6 spinlock 增强（NDEBUG 门控）

- 给 `spinlock_t` 加 `int cpu_id` 字段：`spin_lock` 时记录当前 CPU，`spin_unlock` 时清零
- **Release 构建**（NDEBUG）：`spin_lock/spin_unlock` 与原版完全一致，不读写 `cpu_id`，零性能损失
- **Debug 构建**（非 NDEBUG）：`spin_lock` 设置 `cpu_id` + 递归死锁检测 `BUG_ON(lk->locked && lk->cpu_id == current_cpu_id)`
- `current_cpu_id_debug()`：用 `rdmsr(MSR_GS_BASE) + 8` 硬编码偏移量读取 `cpu_local_t.cpu_id`（offset 8，在 `saved_r10` uint64_t 之后）。不 `#include smp.h`（smp.h ↔ spinlock.h 循环依赖 + cpu_local_t 嵌入 spinlock_t 字段导致定义顺序冲突）
- `spin_lock_irqsave/spin_unlock_irqrestore` 调用 `spin_lock/spin_unlock`，自动适配 NDEBUG 分支
- 未来可选：spin_lock_timeout（超时打印锁持有者 + 等待者 + panic）

#### 3.7 锁协议文档化

**改动**：在 `doc/design/kernel_lock.md` 中添加锁获取顺序表：

```
全局锁层级（从外到内，必须按此顺序获取）：
  1. tasks_lock             — 进程表全局操作
  2. scheduler_lock         — per-CPU 调度器
  3. fd_lock                — per-process fd 表
  4. inode_hash_lock        — inode hash 表
  5. i_lock                 — per-inode
  6. fat_lock               — FAT 表操作
  7. ahci_lock              — AHCI DMA
  8. recv_lock              — per-process recv 队列
  9. cache->lock            — per-slab-cache
  10. serial_tx/rx_lock     — 串口
  11. bfc_lock              — BFC 页分配
```

- 任何违反此顺序的代码路径必须标注原因和替代方案
- `BUG_ON` 检查：debug 构建中 `spin_lock` 时检测 `lk->cpu_id` 确保不递归（NDEBUG 门控，详见 3.6）

### 验证

- SMP 下两个进程并发 `sys_open` 同一文件 → 不创建重复 inode（`inode_lookup_create` 原子性）
- `kmalloc/kfree` 多核并发 → slab freelist 不损坏（无 double-alloc 或 lost object）
- `refcount_dec_and_test` 在 ref_count=0 时触发 `BUG_ON`（panic）
- 并发 `sys_close` + peer `sys_sendmsg` → fd_table 不 race（fd_lock 保护）

---

## 4. 阻塞超时框架

### 现状

- `wait_deadline` 字段存在但绝大多数阻塞操作设为 0（永久等待）
- 14 处 `wait_deadline = 0`：sys_req(:797)、sys_msg_to(:2744)、sys_ioctl(:1683)、sys_kill(:1483/:1667/:2164/:2668/:2729)、sys_waitpid(:934) 等；`sys_recv` 有 `timeout_ms` 参数（:669 设置 deadline），其余 `wait_deadline=0`（:674）
- `sys_recv` 有 `timeout_ms` 参数但部分路径（WAIT_REQ_REPLY/WAIT_MSG_REPLY）不使用它
- 定时器队列（`timer_queue_insert`）机制存在（proc.h），但只在 sys_recv 的 timeout_ms 路径使用
- FAT chain walker 无循环检测 → 磁盘损坏时内核无限循环

### 目标

所有阻塞操作都有超时保底：**对方进程崩溃、磁盘损坏、网络断开 → 调用方不会永久挂起**。

### 工作项

#### 4.1 IPC 阻塞超时

**改动**：所有 WAIT_REQ_REPLY / WAIT_MSG_REPLY 路径添加默认超时。

**规则**：
- `sys_req(target_pid, request, reply)`：添加 `timeout_ms` 参数（或使用 sys_recv 的 timeout）。默认 5s
- `sys_msg_to(target_pid, ...)`：`wait_deadline` 设置为 `sched_clock() + 5s * 1e9`（或调用方提供的超时）
- `sys_ioctl` 用户态驱动路径：REQ 等待超时 = 3s（驱动响应不应太慢）
- 超时到期 → `wait_timed_out = 1`，进程被唤醒 → 返回 `-ETIMEDOUT`

**实现**：
- 已有 `timer_queue_insert/remove` 机制（proc.c），只需在各阻塞路径设置 `wait_deadline = sched_clock() + timeout_ns`
- sys_req 签名可能需要扩展：`sys_req(pid, request, reply, timeout_ms)` 或复用 `sys_recv(timeout_ms)` 的语义

#### 4.2 socket 阻塞超时

**改动**：
- `sys_accept`：添加 backlog timeout（如 30s），超时返回 `-ETIMEDOUT`
- `sock_recvmsg_internal`：while(1) 循环添加 `wait_deadline`，超时返回 `-ETIMEDOUT`
- `sys_connect`：阻塞等待 accept 时添加超时（如 10s）
- 未来：`SO_RCVTIMEO/SO_SNDTIMEO` socket option 设置 per-socket 超时

#### 4.3 FAT chain 循环检测

**改动**：
- `fat32_walk_chain`：添加 `max_walk` 计数器（如 `page_index * 2`），超过时 `WARN_ON + return -EIO`
- `fat32_free_chain`：添加 `max_free` 计数器（如 1024 簇），超过时 `WARN_ON + return -EIO`（标记磁盘损坏）
- `fat32_write` tail-finding loop：添加 `max_tail` 计数器，超过时 `return -EIO`

#### 4.4 pipe 阻塞超时

- 当前 pipe read/write 阻塞在 `WAIT_PIPE`，`wait_deadline = 0`
- 改为：pipe write 阻塞时设 `wait_deadline = sched_clock() + 5s`（或 `SO_SNDTIMEO`）
- pipe read 阻塞同理
- 超时 → 返回 `-ETIMEDOUT` 或部分读/写结果

### 验证

- 目标进程 crash 后，调用 `sys_req` 的进程 5s 后返回 `-ETIMEDOUT`（而非永久挂起）
- FAT 磁盘镜像损坏（循环链）→ fat32 操作返回 `-EIO`（而非内核无限循环）
- socket accept 无连接 → 30s 后返回 `-ETIMEDOUT`

---

## 5. 资源限额与回收机制

### 现状

- `MAX_FD=32`、`MAX_PROC=64` 是硬上限但无软限制/警告
- `process_create_elf` 错误时进程槽位不释放 → 实际可用数递减
- `kmalloc` 无总量追踪，不知道内核还剩多少内存
- `sys_getdents` 用户可请求任意大 buffer → 内核 kmalloc 用户控制大小
- 无 per-process 资源计数（打开 fd 数、mmap 内存量、shm 数量）
- `proc_reap` 扫描 procs[] 不持 `procs_lock` → PID recycling 竞态

### 目标

内核能感知资源余量，在接近耗尽时主动报告而非静默失败后 hang。

### 工作项

#### 5.1 内核内存记账

**设计**：全局 `kernel_mem_stats`

```
struct {
    size_t total_pages;      // 物理总页数
    size_t used_pages;       // 已分配页数（BFC + Slab + kernel）
    size_t slab_used;        // slab 分配器已用字节
    size_t slab_peak;        // slab 峰值
    size_t kmalloc_calls;    // kmalloc 调用计数
    size_t kfree_calls;      // kfree 调用计数
} kernel_mem_stats;
```

- `kmalloc` 成功时 `slab_used += size`，`kfree` 时 `slab_used -= size`
- `bfc_alloc_page` 时 `used_pages += n`，`bfc_free_page` 时 `used_pages -= n`
- `printk(LOG_WARN, "kmalloc(%zu) failed, slab_used=%zu, free_pages=%zu", ...)` — 分配失败时打印内存余量
- 新增 `sys_debug_memstat` 或通过 `sys_debug_print` 扩展，用户态可查询内核内存状态

#### 5.2 process_create_elf 资源回收

**改动**（与 1.3 合并）：
- 错误路径释放进程槽位：`proc->pid = -1`、`proc->state = UNUSED`
- 释放已分配的栈页、PML4 页、用户栈页
- 使用 goto cleanup 集中回滚

#### 5.3 syscall 参数上限

| syscall | 当前 | 改为 |
|---------|------|------|
| `sys_getdents(len)` | 无上限，`kmalloc(len)` | `len <= 4096`，超出返回 `-EINVAL` |
| `sys_read(len)` | 无上限 | `len <= 65536`（64KB），超出截断 |
| `sys_write(len)` | 无上限 | 同 sys_read |
| `sys_msg(msg_len)` | 64KB 上限 | 保持，但内核 kmalloc 失败时返回 `-ENOMEM` 而非 hang |
| `sys_mmap(size)` | 无上限 | `size <= 128MB`（或按可用内存比例），超出返回 `-ENOMEM` |

#### 5.4 per-process fd 数量可调

- 当前 `MAX_FD=32` 硬上限
- `alloc_fd` 返回 `-EMFILE` 时 `printk(LOG_WARN, "pid %d: fd table full (MAX_FD=%d)", ...)`
- 后续可改为动态 fd_table（kmalloc 分配 + 按需扩展），近期保持 32 硬上限 + 警告即可

#### 5.5 proc_reap 竞态修复

> **部分实现**：`task_reap` 已持 `tasks_lock` 清理 PCB slot。但 `mm_release` 扫描 `tasks[]` 查找 `WAIT_REQ_REPLY/WAIT_MSG_REPLY` 阻塞进程时无锁（`proc.c:276-306`），存在竞态窗口。

- `proc_reap` 扫描 `tasks[]` 时加 `tasks_lock`（或至少在读取 pid/state 时加锁）
- PID recycling 风险：进程 A 等待 pid=X，X 退出 → proc_reap 清理 → pid=X 分配给新进程 B → A 可能误操作 B
- 近期方案：ZOMBIE 进程不立即回收 slot，等 waitpid 后才回收（当前逻辑已如此，但需确保 tasks_lock 保护完整）

### 验证

- 内核内存不足时 `kmalloc` 失败 → `printk(LOG_WARN)` 打印余量 → 调用方返回 `-ENOMEM`（而非 hang）
- 连续创建 64 个进程（达到 MAX_PROC）→ 第 65 个返回 `-ENOMEM` + 进程槽不泄漏
- `sys_getdents(len=100000)` → 返回 `-EINVAL`（而非 kmalloc 100KB）

---

## 实施顺序

基础设施有依赖关系，必须按层构建：

```
Phase 0（1天）: atomic.h + refcount_t          ← 3.1
                   ↓ 所有后续模块依赖此原语

Phase 1（1-2天）: 集成已有 log 基础设施         ← 2.1-2.4
                   serial_printf → printk 迁移
                   trap_dispatch halt() → panic()
                   插入首批 BUG_ON/WARN_ON/ASSERT
                   ↓ 错误可见性，后续改动都能被验证

Phase 2（3-4天）: slab 并发修复                   ← 3.3
                   + inode/fd/refcount 并发修复    ← 3.2, 3.4, 3.5
                   + spinlock 递归检测              ← 3.6
                   + 锁协议文档化                    ← 3.7

Phase 3（2-3天）: syscall 返回值语义统一           ← 1.1
                   + 内核函数返回值约定              ← 1.2
                   + process_create_elf goto cleanup ← 1.3, 5.2
                   + 用户态指针检查加固              ← 1.4
                   + syscall 参数上限                ← 5.3

Phase 4（2-3天）: IPC 阻塞超时                    ← 4.1
                   + socket 阻塞超时                 ← 4.2
                   + FAT chain 循环检测              ← 4.3
                   + 内核内存记账                    ← 5.1

总计：约 9-13 天
```

Phase 0 和 Phase 1 是最底层依赖，必须最先完成。**Phase 1 工作量已缩减**：`printk`/`panic`/`BUG_ON`/`WARN_ON`/`ASSERT`/`dump_stack_trace` 代码已存在（`log.h/log.c`），只需迁移 `serial_printf` 调用点 + 替换 `trap_dispatch` 中 `halt()` + 插入首批断言。Phase 2 并发修复是 Phase 3 参数检查的前提（SMP 下参数检查本身也需要锁保护）。Phase 4 超时机制和记账相对独立，可最后做。

### 分轮执行计划

实际执行建议分 3 轮，每轮独立可测、有明确验收标志：

| 轮次 | Phase 范围 | 主题 | 预估行数 | 风险 | 验收标志 |
|------|-----------|------|---------|------|---------|
| **R1** | P0+P1 | 内核可观测 | ~243 | 低 | `BUG_ON(1)` 触发 panic+栈回溯+停机；`WARN_ON` 输出警告但继续运行；142处日志有分级 |
| **R2** | P2 | 并发安全 | ~193 | **高** | SMP 并发 open 不创建重复 inode；kmalloc/kfree 多核不损坏；spin_lock 递归检测触发 BUG_ON |
| **R3** | P3+P4 | 健壮性 | ~476 | 中 | 所有 syscall 非法参数返回 -errno 不 hang；process_create_elf 失败无泄漏；IPC 5s 超时返回 ETIMEDOUT |

各轮详细拆分：

**R1（1-2天）：内核可观测**
- 新建 `kernel/atomic.h`（+60行）
- 142处 `serial_printf` → `printk(LOG_XX, ...)` 迁移（最大单项，每处需判定 log level）
- `trap_dispatch` 中 `halt()` → `panic()`，删除独立回溯代码改调 `dump_stack_trace()`
- 插入首批 BUG_ON/WARN_ON/ASSERT（~15处：spin_lock递归、kmalloc返回值、inode_get/put、proc_reap等）

**R2（2-3天）：并发安全**
- 8个 `ref_count` 字段改为 `refcount_t` + 所有使用点；`pin_count` → `atomic_t`
- slab 快路径加锁（kmalloc fast path + kfree same-CPU path 各加 lock/unlock）
- `inode_lookup_create` 合并为原子操作；fat32.c 调用点适配
- `proc.h` 加 `fd_lock` + trap.c/socket.c/proc.c 约11处加锁
- `spinlock_t` 加 `cpu_id` + 递归死锁检测
- 锁协议文档化
- **关键：每改一个模块（3.2→3.3→3.4→3.5→3.6）就构建验证，不要攒到最后**

**R3（3-4天）：健壮性**
- trap.h/trap.c 55个 sys_xxx 签名 `uint64_t` → `int64_t`（机械替换）
- syscall.h 52个 wrapper 适配；syscall_dispatch rax 适配
- `sys_mmap/sys_spawn/sys_waitpid` 返回值语义修正（0=失败 → -errno）
- `process_create_elf` goto cleanup 重写（110行重写，核心难点）
- `sys_pipe` copy_to_user；`sys_getdents` len上限；`sys_mmap MAP_PHYSICAL` offset 验证
- syscall 参数上限（getdents/read/write/mmap）
- sys_req/sys_msg_to/sys_ioctl 加 wait_deadline；socket accept/recvmsg/connect 加超时
- FAT chain 循环检测；pipe 阻塞超时
- kernel_mem_stats + kmalloc 失败日志 + sys_debug_memstat

---

## 与现有代码的关系

| 现有设计文档 | 本文涉及 | 说明 |
|-------------|---------|------|
| `kernel_lock.md` | 3.6, 3.7 | 扩展 spinlock_t（加 cpu_id）+ 扩展锁协议，添加层级文档 |
| `schedule.md` | 4.1-4.2 | 使用已有 timer_queue 超时机制 |
| `proc.md` | 1.3, 5.2 | process_create_elf 重写 |
| `vfs.md` | 4.3 | FAT chain 循环检测 |
| `vfs.md` | 3.4 | inode lookup/create 竞态 |
| `ipc.md` | 4.1 | sys_req 超时 |
| `ipc.md` | 4.2 | socket 阻塞超时 |
| `mem.md` | 3.3 | slab 并发修复 |
