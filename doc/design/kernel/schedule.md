# 进程调度

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | idle 进程模型 | Linux 风格：每 CPU 一个 idle 内核线程 | schedule() 语义干净（永远返回），idle 是正规进程走 switch_to |
| 2 | `schedule()` 语义 | 纯挑选+切换，**总是返回** | 对 idle：返回后 hlt；对抢占：返回后 iretq 回用户态。不含 hlt 循环 |
| 3 | idle 入口 | `while(1) { schedule(); sti(); hlt; }` | hlt 期间中断允许，任何中断唤醒后重新 schedule |
| 4 | BSP idle 进程重构 | 分配独立内核栈 + switch_frame，统一创建流程 | 所有 idle 进程结构一致，消除 `prev == nullptr` 特殊路径 |
| 5 | `run_count` 语义 | 本 CPU run_queue 中 READY 任务数,**不含 RUNNING**(出队即 `--`) | scheduler_lock 下更新,跨 CPU 用 `__atomic_load_n` RELAXED |
| 6 | `pick_cpu()` / `pick_cpu_pref(pref)` | 遍历 `cpu_locals[].run_count` 选最小值;`pick_cpu_pref` 增加父进程亲和性倾向(差距 <= `AFFINITY_THRESHOLD` 时选偏好 CPU) | 含义是"队列最短的 CPU",**不反映 RUNNING 任务** |
| 7 | `assigned_cpu` | 创建时初始化 + work stealing 迁移时更新(唯一写入者:`try_steal_task` 持双锁);fork/clone 中 `assigned_cpu` 先于 `pid` 赋值(顺序约束) | 防 pid 生效后 assigned_cpu 仍是 -1 导致 wake 路径越界 |
| 8 | IRQ 路由 | 全部留在 BSP | AP 靠定时器轮询拾取 woken 进程；低延迟需 IPI |
| 9 | timer_handler 抢占条件 | `tf->cs == 0x2B`（从用户态来则抢占） | 用户态中断时可安全调 schedule()；内核态中断（idle hlt）不调 schedule()，仅 EOI |
| 10 | idle 不计入 `run_count` | idle 永远可运行但不计入 | idle 是"兜底"，不是负载；`pick_cpu()` 只看用户进程负载 |
| 11 | `process_entry` | `jmp __trapret` | 无需额外操作 |
| 12 | AP idle 栈 | 创建独立内核栈，ap_entry_c 末尾切换到 idle 栈后进入 idle_entry | smp_boot_aps 分配的临时栈仅用于 AP 初始化 |
| 13 | CPU 时间统计 | `sched_clock()` 基于 TSC | schedule() 入口和出口更新 `cpu_time_ns` 和 `last_sched` |

### PCB 结构

进程控制块拆分为三层：`task_t`（调度 + IPC + 信号）、`mm_t`（地址空间 + fd）、`files_t`（fd 表，引用计数支持 fork 共享）。

**task_t**（kernel/proc.h : task_t）
  pid : pid_t — 进程 ID
  state : proc_state_t — READY / RUNNING / BLOCKED / ZOMBIE / REAPING
  k_rsp : uint64_t — 内核栈保存的 RSP（switch_to 用）
  k_stack_top : uint64_t — 内核栈虚拟地址顶部（2 页 = 8KB）
  entry : uint64_t — 用户态入口 RIP
  assigned_cpu : int — 进程分配的 CPU
  iopm : uint8_t* — IOPM bitmap 指针（8KB，ioperm 用，NULL=无权限）
  tgid : pid_t — 线程组 ID（CLONE_VM 预留）
  exit_code : int32_t
  cpu_time_ns : uint64_t — per-process CPU 时间
  last_sched : uint64_t — 上次调度时间戳（sched_clock TSC）
  mm : mm_t* — 地址空间（独立引用计数，fork 共享）
  run_node : list_node_t — per-CPU run_queue 节点
  wait_node : list_node_t
  wait_deadline : uint64_t
  wait_timed_out : uint8_t
  wait_event : wait_event_t — WAIT_RECV / WAIT_REQ_REPLY / WAIT_MSG_REPLY / WAIT_CHILD / WAIT_PIPE / WAIT_POLL
  recv_lock : spinlock_t
  recv_buf[16][64] : uint8_t — 16 槽 × 64 字节 = 1KB 固定 recv 队列
  recv_head / recv_tail : uint32_t
  recv_intr : uint8_t — ISR 唤醒标志（wake_process 设，sys_recv 检查后返回 -EINTR）
  req_caller_pid : pid_t — 当前 req 调用者（-1=无）
  req_reply_buf : void* — 调用者 reply buffer 用户态地址
  req_reply_len : size_t — reply buffer 大小（sys_req 路径=RECV_MSG_SIZE，ioctl proxy 路径=56）
  req_result : int32_t
  req_target_pid : pid_t — 崩溃清理用
  msg_reply_buf : void* / msg_reply_len : size_t
  msg_caller_pid : pid_t（-1 = 无） / msg_result : int32_t / msg_target_pid : pid_t
  sig : signal_state — 信号子系统（pending/blocked/action[]）
  sig_force_info : siginfo_t — force_sig 同步信号信息
  sid : pid_t / pgid : pid_t / ctty : pty* — 会话/进程组/控制终端

**mm_t**（kernel/proc.h : mm_t）
  cr3 : uint64_t — PML4 物理地址
  ref_count : int — fork 共享引用计数
  parent_pid : pid_t
  mmap_brk : uint64_t — mmap 高水位
  mmap_regions : mmap_region* — 已映射区域链表
  files : files_t* — fd 表（独立引用计数）

**files_t**（kernel/proc.h : files_t）
  fd_table[MAX_FD] : struct file — 固定 32 项数组（MAX_FD=32）
  ref_count : int — fork 共享引用计数

最多 64 个进程（MAX_PROC=64）。

### 内核栈布局

**被抢占进程**（时钟中断自然产生）：

高地址 ← k_stack_top
  trapframe_t ← CPU + __alltraps 保存
  __alltraps / trap_dispatch / timer_handler / schedule() 调用返回地址
  callee-saved (rbx, rbp, r12-r15) ← switch_to 保存
低地址 ← k_rsp

**新建进程**（手工构建）：

高地址 ← k_stack_top
  trapframe_t（全 0，RIP=用户入口，CS=0x2B, RFLAGS=0x202, RSP=0x7FFFFFFFE000, SS=0x23）
  process_entry 地址 ← switch_to ret 目标
  rbp=0, rbx=0, r12-r15=0
低地址 ← k_rsp

**idle 进程**（无用户态）：

高地址 ← k_stack_top
  switch_frame: rbx=0, rbp=0, r12-r15=0, ret_addr=idle_entry ← k_rsp

### switch_to

保存 callee-saved 寄存器（rbx, rbp, r12-r15），切换 RSP 到 next->k_rsp（偏移 8，_Static_assert 验证），无条件写 next->cr3（偏移 24）到 CR3（同页表时为 no-op），恢复寄存器后 ret。

实现：arch/x64/trapentry.S : switch_to

### Idle 进程

每个 CPU 一个 idle 进程（内核线程），特征：

- **无用户态**：永不 iretq/sysretq，纯内核循环
- **无独立 PML4**：`cr3 = PHY_ADDR(pml4)`（内核 PML4 物理地址），`mm = NULL`
- **无 trapframe**：内核栈上只有 switch_frame，ret_addr = idle_entry
- **不计入 `run_count`**：通过 `cpu_locals[cpu_id].idle_proc` 指针识别
- **状态**：始终可运行，schedule() 切换 FROM idle 时不设为 READY

入口：kernel/proc.c : idle_entry

### schedule()

实现：kernel/proc.c : schedule()

步骤：

1. 获取 per-CPU scheduler_lock（irqsave）
2. CPU 时间统计：`prev->cpu_time_ns += sched_clock() - prev->last_sched`（idle 除外）
3. **BLOCKED/ZOMBIE/REAPING 快速路径**：prev 不可运行且 run_queue 为空 → 直接切到 idle，return
4. 从 per-CPU run_queue 取下一个进程（FIFO，front 取出 remove）
5. run_queue 空：prev==idle 则 return（idle→idle 无需切换），否则 next=idle
6. prev==next：return
7. prev 重新入队：`prev->state = READY; list_push_back; run_count++`（idle 除外）
8. next 出队：`next->state = RUNNING; run_count--`
9. 更新 `next->last_sched = sched_clock()`
10. 更新 TSS RSP0 + `current_task = next`
11. `update_tss_iopm(next)` — 切换 IOPM bitmap
12. **spin_unlock（不恢复中断）→ switch_to → spin_lock_irqsave → spin_unlock_irqrestore**
    - switch_to 期间中断必须禁用（RSP/CR3 不一致窗口）
    - 切换完成后 lock+unlock 恢复原始中断状态

### timer_handler

实现：kernel/trap.c : timer_handler

步骤：

1. tick++，lapic_eoi()
2. xHCI 轮询：每 10 tick 调 `xhci_poll()`
3. 定时器队列到期处理（两阶段，避免跨 CPU 无锁写 run_queue）：
   - **阶段一**：持**本 CPU** scheduler_lock，遍历 `cpu_locals[cpu].timer_queue`，到期进程 `state→READY`、清 `wait_deadline`，`list_remove` 出 timer_queue 后挂到本地临时链表 `wakeup_list`（只动本 CPU 的 timer_queue 与任务自身字段）。
   - **阶段二**：释放本 CPU 锁，遍历 `wakeup_list`，对每个任务按其 `assigned_cpu` 加锁后 `list_push_back` 入该 CPU 的 run_queue 并 `run_count++`。
   - **为何不能在持本 CPU 锁时直接跨 CPU 投递**：run_queue/run_count 必须由所属 CPU 的 scheduler_lock 保护；若持 A 锁写 B CPU 的 run_queue，会与 B 的 `schedule()` 并发写同一链表/计数器，导致节点丢失、`run_count` 漂移，最终 `schedule()` 的一致性 `ASSERT(cnt == run_count)` 触发 panic。
4. 用户态抢占：`tf->cs == 0x2B` 时调 schedule()；内核态中断（idle hlt）不调

### check_pending_signals

在 `__trapret`（中断返回用户态前）和 `syscall_fast_entry`（syscall 返回用户态前）调用。

实现：kernel/trap.c : check_pending_signals

步骤：

1. `tf->cs != USER_CS` 则 return
2. 计算 `deliverable = pending & ~blocked`，为空则 return
3. 取最低位信号 `sig = __builtin_ctzll(deliverable)`，清除 pending 位
4. SIG_IGN → continue；SIG_DFL → 默认动作；有 handler → `deliver_signal(proc, tf, sig, sa)` 后 return（一次只投递一个）

详见 doc/design/ipc.md 信号机制部分。

### pick_cpu() / pick_cpu_pref()

实现：kernel/xcore/sched.c : pick_cpu() / pick_cpu_pref()

- `pick_cpu()` 等价于 `pick_cpu_pref(-1)`,选 `run_count` 最小的 CPU。
- `pick_cpu_pref(pref_cpu)` 在最小值基础上增加亲和性倾向:若偏好 CPU 的 `run_count` 与最小值差距 <= `AFFINITY_THRESHOLD`,则选偏好 CPU。`pref_cpu < 0` 表示无偏好。
- 含义是"队列最短的 CPU",**不反映 RUNNING 任务**(出队即 `--`)。
- 调用点:`process_create_elf` / `sys_fork` / `sys_clone`(CLONE_THREAD 亲和父 CPU)/ TOCTOU 重检。

### run_count 维护

所有修改在 scheduler_lock 保护下,跨 CPU 操作用 `__atomic_load_n` RELAXED。idle 进程不计入。`run_count` = 本 CPU run_queue 中 READY 任务数,**不含 RUNNING**(出队即 `--`)。Debug 构建(`NDEBUG` 未定义)下,`schedule()` 在所有 run_queue/run_count 变更完成后断言 `cnt == run_count`,用于捕获跨 CPU 路径未持本 CPU 锁写本 CPU 队列的竞态。

| 位置 | 操作 | 说明 |
|------|------|------|
| process_create / process_create_elf | run_count++ | 新进程 READY |
| sys_waitpid BLOCKED | schedule() 中不重新入队 | RUNNING → BLOCKED,不出队不计数 |
| sys_notify / sys_req / sys_resp 唤醒 | run_count++ | BLOCKED → READY,投递到 `assigned_cpu` 的队列 |
| IRQ owner 唤醒 | run_count++ | BLOCKED → READY,投递到 `assigned_cpu` 的队列 |
| timer_queue 到期 | run_count++ | BLOCKED → READY,两阶段投递:本 CPU 锁摘到临时链表,再按 `assigned_cpu` 加锁入队(详见 timer_handler 步骤 3) |
| pipe wake | run_count++ | BLOCKED → READY,投递到 `assigned_cpu` 的队列 |
| schedule() prev 重新入队 | run_count++(入队) | READY 重新入队 |
| schedule() next 出队 | run_count--(出队) | RUNNING 消费一个槽位 |
| 进程退出 | run_count-- | 移除一个可运行进程 |
| try_steal_task | victim `run_count--` + thief `run_count++` | work stealing 迁移,持双 CPU scheduler_lock |

### Work Stealing(运行时负载均衡)

实现:kernel/xcore/sched.c : try_steal_task(),在 idle_entry 循环内调用。

- **触发时机**:本 CPU idle_entry 即将 schedule() 前调用。idle 表示本 CPU 无可运行任务,尝试从繁忙 CPU 偷一个。
- **victim 选择**:线性扫描所有非本 CPU 的 `run_count`,选最大值;`max_run <= 1` 时放弃(偷了对方就空了)。
- **锁策略**:机会主义,用 `spin_trylock_irqsave` 抢目标 CPU 的 scheduler_lock,失败即放弃,绝不阻塞。持自己 CPU 的 scheduler_lock 防本 CPU 中断并发写 run_queue。
- **偷取对象**:victim 的 run_queue 尾部(`head->prev`),循环双向链表不变式。偷到则更新 `t->assigned_cpu = my_cpu`。
- **重检**:进临界区后重判 `list_empty`(外部 run_count 快照可能已过期)。
- **`assigned_cpu` 写入者**:仅 `try_steal_task` 持双锁时迁移更新;创建时初始化 + TOCTOU 重检时改写(尚未入队,安全)。`task_reap` 不清 `assigned_cpu`(清成 -1 会引入越界竞态:wake 路径在锁前无锁读)。
- **顺序约束**:fork/clone/proc_create 中 `assigned_cpu` 必须先于 `pid` 赋值,防 pid 生效后 assigned_cpu 仍是 -1 导致 `cpu_locals[-1]` 越界。

### 初始化序列

#### BSP（kernel_main）

实现：kernel/kernel.c : kernel_main

1. serial_init, init_mem, acpi_init, isr_init, kernel_init_finish
2. kasan_init, slab_init, sig_init, proc_init
3. smp_boot_aps()：对每个 AP 分配临时栈 + smp_init_cpu + create_idle_process(i) + 填充 trampoline + SIPI
4. pci_init, display_init, ahci_init, vfs_init, xhci_init
5. create_idle_process(0)（BSP idle，在 smp_boot_aps 之后）
6. 加载 init.elf from disk → process_create_elf
7. sti, current_task = bsp_idle, idle->state = RUNNING, 更新 TSS RSP0
8. 切换到 idle 内核栈，进入 idle_entry()（永不返回）

#### AP（ap_entry_c）

实现：arch/x64/smp.c : ap_entry_c

1. smp_apply_cpu(cpu_id)
2. pat_init(), idt_install(), setup_syscall()（MSR STAR/LSTAR/SFMASK）
3. enable LAPIC via MSR + software enable + mask LVT + start LAPIC timer
4. cpu_locals[cpu_id].lapic_base = LAPIC_BASE
5. current_task = idle, idle->state = RUNNING, 更新 TSS RSP0
6. 切换到 idle 内核栈，进入 idle_entry()

### sched_clock

基于 TSC 的单调时钟，用于 CPU 时间统计和定时器队列。实现：arch/x64/apic.c : sched_clock

当前直接返回 TSC cycle，未转换为纳秒。

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| IPI 唤醒 | IRQ 全留在 BSP,AP 靠定时器轮询拾取 woken 进程(~10ms 延迟);低延迟需 IPI 通知目标 CPU | 中 |
| 调度策略 | 当前 FIFO(run_queue 顺序),无优先级/时间片;需 O(1) 或 CFS 调度器 | 低 |
| per-CPU run_count 优化 | pick_cpu() 遍历所有 CPU;大核数时可用 cache line 对齐 + NUMA 感知 | 低 |
| sched_clock 精度 | 当前直接用 TSC cycle,未转换为纳秒;需校准 tsc_khz | 低 |
