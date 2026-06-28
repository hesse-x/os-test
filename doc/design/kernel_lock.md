# 内核锁设计

## 1. 自旋锁原语

> **已实现**。`spin_lock_irqsave` / `spin_unlock_irqrestore` 已添加，用于 per-CPU `scheduler_lock`。

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 原子操作实现 | GCC `__atomic` builtins | freestanding 可用，编译器自动 lowering 为 `xchg`，自动插入内存屏障 |
| 2 | `spin_lock` 是否内含 `cli` | 否 | 中断控制由调用者负责，锁本身保持纯自旋语义 |
| 3 | 结构体字段 | 仅 `volatile uint32_t locked` | YAGNI |
| 4 | 放置文件 | `kernel/spinlock.h` | 锁是内核同步原语，非架构特定 |
| 5 | unlock 内存序 | `__ATOMIC_RELEASE` | 与 lock 端 `__ATOMIC_ACQUIRE` 配对，精确表达 acquire-release 同步语义 |
| 6 | 自旋循环 | `exchange` + `pause` | 2-4 核场景无竞争瓶颈，简单优先 |
| 7 | 初始化 | `SPINLOCK_INIT {0}` 宏 | 单字段无复杂初始化逻辑，无需 `spin_init()` |
| 8 | `locked` 类型 | `uint32_t` | x86-64 上生成 `xchgll`，显式表达宽度意图 |

### 实现

```c
struct spinlock_t {
    volatile uint32_t locked;
};

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *lk) {
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) == 1)
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *lk) {
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
}

static inline void spin_lock_irqsave(spinlock_t *lk, uint64_t *flags) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    *flags = f;
    spin_lock(lk);
}

static inline void spin_unlock_irqrestore(spinlock_t *lk, uint64_t flags) {
    spin_unlock(lk);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}
```

## 2. BKL → 细粒度锁拆分

> **已实现**。BKL 已完全移除，替换为 per-CPU `scheduler_lock` + 全局 `procs_lock` + `fb_lock` + `irq_owner[]` atomic。`kernel/list.h` 已添加（`list_node_t` + `LIST_ENTRY`），`proc_t` 新增 `run_node`/`wait_node`，`cpu_local_t` 新增 `scheduler_lock`/`run_queue`。汇编入口/出口不再有 `kernel_lock_acquire/release`。

### 目标

将 BKL（大内核锁）拆分为 per-CPU `scheduler_lock` + 全局 `procs_lock` + `fb_lock` + `irq_owner` atomic 化，最终完全移除 BKL。

策略：**渐进拆分** — 先加细粒度锁（BKL 兜底），验证后再删 BKL。

### 当前 BKL 保护范围

BKL 在所有用户态内核入口 acquire、返回用户态 release，串行化以下操作：

| 共享状态 | 操作 | 频率 | 跨 CPU |
|---------|------|------|--------|
| `procs[]` 全部字段 | process_create 扫描/填充、schedule 扫描、sys_wait/notify 修改 state | 高 | 是 |
| `cpu_locals[].run_count` | pick_cpu 读、create/wait/notify/wake 写 | 高 | 是（跨 CPU 写） |
| `irq_owner[]` | sys_irq_bind 写、trap_dispatch 读 | 写低/读高 | 是 |
| `fb_putc` cursor + serial | sys_putc | 中 | 是 |
| `current_proc` | schedule 写、syscall handler 读 | 高 | 否（per-CPU） |

**已知隐患**：`trap_dispatch` 在 kernel-mode IRQ 路径读 `irq_owner[]` 不持 BKL，存在数据竞争。

### 新锁方案

#### 锁一览

| 锁 | 粒度 | 保护对象 | irqsave? |
|----|------|---------|----------|
| `scheduler_lock` | per-CPU（`cpu_local_t` 内） | run_queue + `procs[i].state`（assigned_cpu==本 CPU） + `run_count` + `schedule()` | **是**（timer_handler 会争） |
| `procs_lock` | 全局 | `procs[]` 空闲槽位分配（pid==-1） + PCB 初始化 | 否 |
| `fb_lock` | 全局 | `fb_putc` cursor 位置 + VGA 文本缓冲区 | 否 |
| `irq_owner[]` | 无锁 | `__atomic_store_n` 写 / `__atomic_load_n` 读 | N/A |

#### 锁协议

```
scheduler_lock[cpu] 保护：
  - run_queue[cpu]（per-CPU 就绪队列链表）
  - procs[i].state（where procs[i].assigned_cpu == cpu）
  - cpu_locals[cpu].run_count
  - schedule() 的选择逻辑

procs_lock 保护：
  - procs[] 数组的空闲槽位查找（pid == -1）
  - PCB 字段初始化（pid, cr3, k_stack_top, entry, assigned_cpu, iopl, run_node, wait_node）
  - 不保护 state（state 归 scheduler_lock）

fb_lock 保护：
  - fb_putc 的 cursor 位置（fb_row, fb_col）
  - VGA 文本缓冲区写入
  - 不保护 serial_putc（outb 原子，字符交错可接受）

irq_owner[]：
  - sys_irq_bind 写：__atomic_store_n(&irq_owner[irq], pid, __ATOMIC_RELEASE)
  - trap_dispatch 读：__atomic_load_n(&irq_owner[trapno], __ATOMIC_ACQUIRE)
```

#### 锁获取顺序（防死锁）

当需要同时持多把锁时，按以下顺序获取：

1. `procs_lock`（低频，先锁）
2. `scheduler_lock[cpu]`（高频，后锁）

不存在需要同时持 `fb_lock` + `scheduler_lock` 或 `fb_lock` + `procs_lock` 的路径。

### 数据结构变更

#### list_node_t — 内嵌双向链表节点

```c
// kernel/list.h（新文件）
struct list_node_t {
    list_node_t *prev;
    list_node_t *next;
};

// 初始化哨兵节点
static inline void list_init(list_node_t *head) {
    head->prev = head;
    head->next = head;
}

// 在 head 前插入 node（FIFO：入队到尾部）
static inline void list_push_back(list_node_t *head, list_node_t *node) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

// 从链表中移除 node
static inline void list_remove(list_node_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = node;  // 防止误用
}

// 链表是否为空
static inline bool list_empty(list_node_t *head) {
    return head->next == head;
}

// 获取头部节点（FIFO：出队从头部）
static inline list_node_t *list_front(list_node_t *head) {
    return head->next;
}

// node 所在的 proc_t（container_of 风格）
#define LIST_ENTRY(node, type, member) \
    ((type *)((char *)(node) - offsetof(type, member)))
```

#### proc_t 变更

```c
struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint64_t k_rsp;
    uint64_t k_stack_top;
    uint64_t cr3;
    uint64_t entry;
    wait_event_t wait_event;
    int assigned_cpu;
    uint8_t iopl;
    list_node_t run_node;    // 嵌入 per-CPU run_queue
    list_node_t wait_node;   // 嵌入 wait_queue（预留）
};
```

#### cpu_local_t 变更

```c
struct cpu_local_t {
    int cpu_id;
    uint32_t apic_id;
    proc_t *_cur_proc;
    uint64_t lapic_base;
    uint64_t kernel_stack;
    uint64_t tss_rsp0;
    int run_count;
    proc_t *idle_proc;
    spinlock_t scheduler_lock;  // per-CPU 调度器锁
    list_node_t run_queue;      // per-CPU 就绪队列（哨兵节点）
};
```

### schedule() 新设计

#### 当前流程（BKL 下）

```
schedule():
    // BKL 已被调用者持有
    扫描 procs[] 找 READY && assigned_cpu==my_cpu
    if 找到:
        设 prev=RUNNING/READY, next=RUNNING
        更新 current_proc, TSS rsp0
        kernel_lock_release()
        switch_to(prev, next)
        kernel_lock_acquire()
    // BKL 仍被持有
```

#### 新流程

```
schedule():
    spin_lock_irqsave(&scheduler_lock[my_cpu], &flags)

    if run_queue 为空:
        spin_unlock_irqrestore(&scheduler_lock[my_cpu], flags)
        return  // 无可运行进程，idle 继续 hlt

    prev = current_proc
    next = LIST_ENTRY(list_front(&run_queue), proc_t, run_node)
    list_remove(&next->run_node)   // 出队

    // 状态转换
    if prev->state == RUNNING:
        prev->state = READY
        list_push_back(&run_queue, &prev->run_node)  // prev 入队尾部（round-robin）
        run_count++
    // if prev->state == BLOCKED: 不入队，run_count 不变（已在 sys_wait 中 --）

    next->state = RUNNING
    run_count--  // 从队列移出
    current_proc = next
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top

    // 持锁穿过 switch_to（per-CPU 锁，争用极低）
    switch_to(prev, next)
    // switch_to 返回后在新进程栈上，flags 是新进程保存的 IF

    spin_unlock_irqrestore(&scheduler_lock[my_cpu], flags)
```

**关键设计**：per-CPU scheduler_lock 持锁穿过 `switch_to`。这是安全的，因为：

1. per-CPU 锁，本 CPU 是主要争用者，跨 CPU 入队（sys_notify）极少
2. `irqsave` 保证 switch_to 期间中断关闭，timer_handler 不会在本 CPU 重入 schedule
3. switch_to 返回后在新进程的栈上，`flags` 是新进程调用 schedule() 时保存的 IF，irqrestore 正确恢复

### 各操作的新锁协议

#### process_create_elf(path)

```
lock(procs_lock)
    扫描 procs[] 找空闲槽（pid == -1）
    填充 PCB 字段（pid, cr3, k_stack_top, entry, assigned_cpu, iopl）
    list_init(&proc->run_node)
    list_init(&proc->wait_node)
    proc->state = READY
unlock(procs_lock)

lock(scheduler_lock[assigned_cpu])     // 可能跨 CPU
    list_push_back(&run_queue[assigned_cpu], &proc->run_node)
    run_count[assigned_cpu]++
unlock(scheduler_lock[assigned_cpu])
```

#### sys_wait()

```
lock(scheduler_lock[my_cpu])
    current_proc->state = BLOCKED
    run_count[my_cpu]--
unlock(scheduler_lock[my_cpu])
// schedule() 将切走当前进程，不入队（state != RUNNING）
```

#### sys_notify(target_pid)

```
lock(scheduler_lock[target_cpu])       // 可能跨 CPU
    if procs[target_pid].state == BLOCKED:
        procs[target_pid].state = READY
        list_push_back(&run_queue[target_cpu], &procs[target_pid].run_node)
        run_count[target_cpu]++
unlock(scheduler_lock[target_cpu])
```

#### trap_dispatch() wake（IRQ → 用户态驱动）

```
pid = __atomic_load_n(&irq_owner[trapno], __ATOMIC_ACQUIRE)
if pid >= 0:
    target_cpu = procs[pid].assigned_cpu
    lock(scheduler_lock[target_cpu])   // 可能跨 CPU
        if procs[pid].state == BLOCKED:
            procs[pid].state = READY
            list_push_back(&run_queue[target_cpu], &procs[pid].run_node)
            run_count[target_cpu]++
    unlock(scheduler_lock[target_cpu])
    // EOI 已由调用者完成
```

**注意**：`trap_dispatch` 在 kernel-mode IRQ 路径也安全 — `irq_owner` atomic 读无数据竞争，`scheduler_lock` 远程加锁是 spinlock 标准操作。

#### sys_yield()

```
lock(scheduler_lock[my_cpu])
    current_proc->state = READY
    // 不入队 — schedule() 会将 RUNNING→READY 的进程入队
unlock(scheduler_lock[my_cpu])
schedule()
```

#### sys_putc()

```
lock(fb_lock)
    fb_putc(ch)
unlock(fb_lock)
serial_putc(ch)  // 无需锁
```

#### sys_irq_bind(irq)

```
// procs_lock 不需要 — 只写 irq_owner，不改 procs[] 槽位
__atomic_store_n(&irq_owner[irq], current_proc->pid, __ATOMIC_RELEASE)
```

#### pick_cpu()

```
// 读所有 CPU 的 run_count，用 __atomic_load_n 读近似值即可
best_cpu = 0
for i in 0..ncpu:
    r = __atomic_load_n(&cpu_locals[i].run_count, __ATOMIC_RELAXED)
    if r < min:
        min = r
        best_cpu = i
return best_cpu
```

**说明**：`pick_cpu()` 只在 `process_create` 时调用，用于选择负载最低的 CPU。用 `RELAXED` 读 `run_count` 是安全的 — 选到的 CPU 可能在读后 run_count 变化了，但这只影响负载均衡质量，不影响正确性。

### BKL 移除

#### 汇编变更

**`__alltraps`（trapentry.S）**：
```asm
    # 删除：
    #   cli
    #   call kernel_lock_acquire
    # 保留：
    #   swapgs（用户态入口仍需）
```

**`__trapret`（trapentry.S）**：
```asm
    # 删除：
    #   call kernel_lock_release
    # 保留：
    #   swapgs（返回用户态仍需）
```

**`syscall_fast_entry`（trapentry.S）**：
```asm
    # 删除：
    #   call kernel_lock_acquire
```

**SYSCALL 返回路径（trapentry.S）**：
```asm
    # 删除：
    #   call kernel_lock_release
```

**`process_entry`（trapentry.S）**：
```asm
process_entry:
    # 删除：
    #   call kernel_lock_acquire
    jmp __trapret
```

#### C 代码变更

**`idle_entry()`（proc.cc）**：
```c
void idle_entry() {
    sti();
    while (1) {
        schedule();
        __asm__ volatile("hlt");
    }
}
```

**删除**：
- `kernel/trap.cc` 中的 `kernel_lock` 定义 + `kernel_lock_acquire/release` 函数
- `kernel/trap.h` 中的声明
- `kernel/spinlock.h` 中的 `kernel_lock` extern（如有）
- `proc.cc` 中所有 `kernel_lock_acquire/release` 调用

### idle_entry 与中断安全

BKL 移除后，`idle_entry` 的 `hlt` 指令在 IF=1 时执行。中断唤醒 CPU 后：
1. CPU 清 IF，切内核栈，进入 `__alltraps`
2. `trap_dispatch` 处理中断（可能唤醒进程：锁 scheduler_lock + 入队）
3. `__trapret` → `iretq` 恢复 IF=1
4. 回到 `idle_entry` 循环，调用 `schedule()` 检查 run_queue

**timer_handler 在内核态不调 schedule**：保持现有逻辑 — `tf->cs != 0x2B` 时只做 EOI。idle 进程回到循环后调 `schedule()` 检查是否有新就绪进程。最差延迟 ~10ms（100Hz timer），对当前场景可接受。

### 实现步骤

#### 步骤 4.1：spin_lock_irqsave + scheduler_lock + per-CPU run queue

**新增**：
- `kernel/list.h`：`list_node_t` + 内联操作
- `spinlock.h`：`spin_lock_irqsave` / `spin_unlock_irqrestore`
- `cpu_local_t`：新增 `scheduler_lock` + `run_queue`
- `proc_t`：新增 `run_node` + `wait_node`

**修改**：
- `proc.cc`：
  - `create_idle_process()`：创建后入队到本 CPU run_queue
  - `schedule()`：改用 run_queue + scheduler_lock，移除 procs[] 线性扫描
  - `sys_wait()`：改用 scheduler_lock
  - `sys_yield()`：改用 scheduler_lock
- `trap.cc`：
  - `sys_notify()`：改用 scheduler_lock[target_cpu]
  - `trap_dispatch()` wake 路径：改用 scheduler_lock[target_cpu]
  - `pick_cpu()`：改用 `__atomic_load_n` RELAXED 读 run_count
- `smp.cc`：`smp_init_cpu()` 中初始化 `scheduler_lock` + `list_init(&run_queue)`

**验证**：`-smp 2`，多进程调度正常，BKL 仍兜底。

#### 步骤 4.2：procs_lock

**修改**：
- `proc.cc`：
  - `process_create()` / `process_create_elf()`：先锁 `procs_lock` 分配槽位 + 填 PCB，再锁 `scheduler_lock[cpu]` 入队

**验证**：`-smp 2`，进程创建正常。

#### 步骤 4.3：irq_owner atomic 化

**修改**：
- `trap.cc`：
  - `isr_init()`：`irq_owner[]` 初始化改用 `__atomic_store_n`（或保持普通写，单线程初始化无竞争）
  - `trap_dispatch()`：读 `irq_owner[]` 改用 `__atomic_load_n(ACQUIRE)`
  - `sys_irq_bind()`：写 `irq_owner[]` 改用 `__atomic_store_n(RELEASE)`

**验证**：`-smp 2`，kbd/disk 驱动正常。

#### 步骤 4.4：fb_lock

**新增**：
- `kernel/spinlock.h` 或 `kernel/fb.cc`：`spinlock_t fb_lock = {0}`

**修改**：
- `trap.cc`：`sys_putc()` 中 `fb_putc` 前后加 `spin_lock(&fb_lock)` / `spin_unlock(&fb_lock)`

**验证**：`-smp 2`，输出正常，无 cursor 错乱。

#### 步骤 4.5：移除 BKL

**删除**：
- `kernel/trap.cc`：`kernel_lock` 定义 + `kernel_lock_acquire/release` 函数
- `kernel/trap.h`：`kernel_lock_acquire/release` 声明
- `arch/x64/trapentry.S`：所有 `call kernel_lock_acquire` / `call kernel_lock_release` + `cli`（仅 `__alltraps` 用户态路径的 `cli`）
- `kernel/proc.cc`：`idle_entry()` 中的 `kernel_lock_acquire/release`，`schedule()` 中残留的 BKL 操作（如果步骤 4.1 已移除则跳过）

**验证**：`-smp 2`，全系统压力测试 — 多进程调度 + shell 交互 + kbd/disk 驱动 + 串口输出。

### 未覆盖项（后续步骤）

| 项目 | 原因 |
|------|------|
| reschedule IPI | 独立步骤，当前 timer 轮询 ~10ms 延迟可接受 |
| TLB shootdown IPI | BKL 下 CR3 reload 自动 flush；移除 BKL 后若需运行时修改页表（extend_mapping 等）才需要 |
| per-CPU run queue + work stealing | 当前进程不迁移 CPU，未来扩展 |
| wait_queue 替代 wait_event 线性扫描 | `wait_node` 已预留，实现待需求驱动 |
