# 进程调度

> **已实现**。每 CPU 有独立 idle 进程和 run_queue，AP 参与调度。`timer_handler` 用 `tf->cs == 0x2B` 判断抢占，`pick_cpu()` 按 run_count 分配进程。BKL 已移除，idle_entry 不再操作 BKL。以下描述当前实现；末尾附 x86-32 阶段历史方案供参考。

## 当前实现（x86-64）

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | idle 进程模型 | Linux 风格：每 CPU 一个 idle 内核线程 | `schedule()` 语义干净（永远返回），idle 是正规进程走 `switch_to`，未来扩展自然 |
| 2 | `schedule()` 语义 | 纯挑选+切换，**总是返回** | 对 idle：schedule() 返回后 hlt；对抢占：schedule() 返回后 iretq 回用户态。不含 hlt 循环 |
| 3 | idle 入口 | `while(1) { schedule(); sti(); hlt; }` | hlt 期间中断允许，任何中断唤醒后重新 schedule |
| 4 | BSP idle 进程重构 | 分配独立内核栈 + switch_frame，统一创建流程 | 所有 idle 进程结构一致，消除 `prev == nullptr` 特殊路径 |
| 5 | `run_count` 语义 | 动态可运行计数（READY + RUNNING 之和） | 大量 BLOCKED 驱动/服务进程下，静态计数严重失真；scheduler_lock 下更新，跨 CPU 用 `__atomic_add_fetch` RELAXED |
| 6 | `pick_cpu()` | 遍历 `cpu_locals[].run_count`，选最小值 | 简单有效，拆锁后可替换为更精细的策略 |
| 7 | `assigned_cpu` | 静态绑定，不迁移 | 步骤 3 目标是跑通多核调度；work stealing 是后续优化 |
| 8 | IRQ 路由 | 全部留在 BSP | AP 靠定时器轮询（~10ms 延迟）拾取 woken 进程；低延迟需 IPI |
| 9 | timer_handler 抢占条件 | `tf->cs == 0x2B`（从用户态来则抢占） | 用户态中断时可安全调 schedule()；内核态中断（idle hlt）不调 schedule()，仅 EOI |
| 10 | idle 不计入 `run_count` | idle 永远可运行但不计入 | idle 是"兜底"，不是负载；`pick_cpu()` 只看用户进程负载 |
| 11 | `process_entry` | `jmp __trapret` | BKL 移除后无需补偿 |
| 12 | AP idle 栈 | 创建独立内核栈，ap_entry_c 末尾切换到 idle 栈后进入 idle_entry | smp_boot_aps 分配的临时栈仅用于 AP 初始化 |

### PCB 结构

```c
struct proc_t {
    pid_t pid;
    proc_state_t state;          // READY / RUNNING / BLOCKED / ZOMBIE / REAPING
    uint64_t k_rsp;              // saved kernel RSP（switch_to 用）
    uint64_t k_stack_top;        // kernel stack 虚拟地址顶部（2 页 = 8KB）
    uint64_t cr3;                // PML4 物理地址
    uint64_t entry;              // 用户态入口 RIP
    int assigned_cpu;            // 进程分配的 CPU
    int iopl;                    // IOPL 级别（0=普通，3=驱动）
    pid_t parent_pid;
    int32_t exit_code;
    uint64_t mmap_brk;           // mmap 高水位
    list_node_t mmap_regions;    // 已映射区域链表
    list_node_t run_node;        // per-CPU run_queue 节点
    list_node_t wait_node;
    uint64_t wait_deadline;
    bool wait_timed_out;
    wait_event_t wait_event;     // WAIT_RECV / WAIT_REQ_REPLY / WAIT_MSG_REPLY / WAIT_CHILD / WAIT_PIPE
    struct file fd_table[MAX_FD];
    shm_region_t shm_regions[MAX_SHM_PER_PROC];
    uint64_t cpu_time_ns;        // per-process CPU 时间
    uint64_t last_sched;         // 上次调度时间戳
    // IPC 状态：recv_buf[16][64] + req_*/msg_* 字段
    spinlock_t recv_lock;
    // ...
};
```

最多 64 个进程（`MAX_PROC=64`）。

### 内核栈布局

**被抢占进程**（时钟中断自然产生）：

```
高地址 ← k_stack_top
  trapframe_t                ← CPU + __alltraps 保存
  ── __alltraps 调用返回地址
  ── trap_dispatch 调用返回地址
  ── timer_handler 调用返回地址
  ── schedule() 调用返回地址
  callee-saved (rbx, rbp, r12-r15)  ← switch_to 保存
低地址 ← k_rsp
```

**新建进程**（手工构建）：

```
高地址 ← k_stack_top
  trapframe_t（全 0，RIP=用户入口，CS=0x2B, RFLAGS=0x202, RSP=0x7FFFFFFFE000, SS=0x23）
  process_entry 地址            ← switch_to ret 目标
  rbp=0, rbx=0, r12-r15=0
低地址 ← k_rsp
```

**idle 进程**（无用户态）：

```
高地址 ← k_stack_top
  switch_frame: rbx=0, rbp=0, r12-r15=0, ret_addr=idle_entry  ← k_rsp
```

### switch_to

```asm
switch_to:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, proc_t.k_rsp_offset(%rdi)   # prev->k_rsp = RSP
    movq proc_t.k_rsp_offset(%rsi), %rsp   # RSP = next->k_rsp

    movq proc_t.cr3_offset(%rsi), %rax     # 切换 CR3（如果页表不同）
    movq %rax, %cr3

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
```

### Idle 进程

每个 CPU 一个 idle 进程（内核线程），特征：

- **无用户态**：永不 iretq/sysretq，纯内核循环
- **无独立 PML4**：`cr3 = PHY_ADDR(pml4)`（内核 PML4 物理地址）
- **无 trapframe**：内核栈上只有 switch_frame，ret_addr = idle_entry
- **不计入 `run_count`**：通过 `cpu_locals[cpu_id].idle_proc` 指针识别
- **状态**：始终可运行，schedule() 切换 FROM idle 时不设为 READY

```c
void idle_entry() {
    sti();
    while (1) {
        schedule();
        sti();
        __asm__ volatile("hlt");
    }
}
```

### schedule()

```c
void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    proc_t *idle = get_cpu_local()->idle_proc;
    proc_t *prev = current_proc;

    spin_lock_irqsave(&scheduler_lock);

    // 从 per-CPU run_queue 取下一个进程
    proc_t *next = nullptr;
    if (!list_empty(&cpu_locals[my_cpu].run_queue)) {
        next = LIST_ENTRY(list_front(&cpu_locals[my_cpu].run_queue), proc_t, run_node);
        list_remove(&next->run_node);
    }

    // 没有用户进程可运行，切到 idle
    if (next == nullptr) next = idle;

    // prev == next：无需切换
    if (prev == next) { spin_unlock_irqrestore(&scheduler_lock); return; }

    // 切换 FROM idle 时不改状态（idle 永远可运行）
    if (prev != idle && prev->state == RUNNING) {
        prev->state = READY;
        list_push_back(&cpu_locals[my_cpu].run_queue, &prev->run_node);
    }

    next->state = RUNNING;
    cpu_locals[my_cpu].run_count--;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;
    current_proc = next;

    spin_unlock_irqrestore(&scheduler_lock);
    switch_to(prev, next);
    spin_lock_irqsave(&scheduler_lock);
    spin_unlock_irqrestore(&scheduler_lock);
}
```

### timer_handler

```c
static void timer_handler(trapframe_t *tf) {
    tick++;
    lapic_eoi();
    if (tf->cs == 0x2B) {   // 从用户态来，可安全调 schedule()
        schedule();
    }
    // 从内核态来（idle hlt）：仅 EOI，idle 醒来后自行调 schedule()
}
```

### pick_cpu()

```c
static int pick_cpu() {
    int best = 0;
    for (int i = 1; i < ncpu; i++) {
        if (cpu_locals[i].run_count < cpu_locals[best].run_count) {
            best = i;
        }
    }
    return best;
}
```

### run_count 维护

所有修改在 scheduler_lock 保护下，跨 CPU 操作用 `__atomic_add_fetch` RELAXED。idle 进程不计入。

| 位置 | 操作 | 说明 |
|------|------|------|
| `process_create` | `cpu_locals[assigned_cpu].run_count++` | 新进程 READY |
| `process_create_elf` | `cpu_locals[assigned_cpu].run_count++` | 新进程 READY |
| `sys_wait` BLOCKED | `cpu_locals[assigned_cpu].run_count--` | RUNNING → BLOCKED |
| `sys_notify` 唤醒 | `cpu_locals[assigned_cpu].run_count++` | BLOCKED → READY |
| trap_dispatch IRQ owner 唤醒 | `cpu_locals[assigned_cpu].run_count++` | BLOCKED → READY |
| schedule() RUNNING↔READY | 不变 | run_count 计 READY+RUNNING 之和，两者转换不影响 |
| 进程退出 | `cpu_locals[assigned_cpu].run_count--` | 移除一个可运行进程 |

### 初始化序列

#### BSP（kernel_main）

```
1. init_mem, isr_init, kernel_init_finish, proc_init
2. create_idle_process(0)                          （BSP idle）
3. smp_boot_aps():
     对每个 AP (cpu_id 1..ncpu-1):
       - 分配临时栈 + smp_init_cpu
       - create_idle_process(i)                    （AP idle）
       - 填充 trampoline + SIPI
4. 加载用户进程 process_create_elf
5. current_proc = cpu_locals[0].idle_proc
6. idle->state = RUNNING
7. 更新 TSS RSP0
8. 切换到 idle 内核栈，进入 idle_entry()（永不返回）
```

#### AP（ap_entry_c）

```
1. smp_apply_cpu, idt_install, LAPIC, timer
2. current_proc = cpu_locals[cpu_id].idle_proc
3. idle->state = RUNNING
4. 更新 TSS RSP0
5. 切换到 idle 内核栈，进入 idle_entry()
```

---

## 历史：x86-32 阶段调度方案

### Ring 3 特权级切换（阶段一）

GDT 6 项（null/kcode/kdata/ucode/udata/TSS），TSS 静态全局变量，IDT 48+1 项（vector128=syscall，DPL=3，flags=0xEE）。trapframe_t 扩展增加 esp/ss（struct 尾部，eflags 之后）。`__alltraps` 不手动 push SS/ESP，iret 在特权级变化时自动 pop。syscall 独立入口 `syscall_entry`（不走 `__alltraps`）。USER_CS=0x1B, USER_DS=0x23, TSS_SEL=0x28。

ring 3 验证：内核栈上手动构造 iret 帧（EIP=not-present, CS=0x1B, ESP=假值, SS=0x23）→ iret → #PF 证明切换成功。

`kernel_init_finish`：清除 PD[0] identity map + 禁 bump 分配器。

### 进程与调度（阶段二）

PCB：32 位字段（k_esp, k_stack_top, cr3, entry 均为 uint32_t），状态仅 READY/RUNNING。switch_to 保存 callee-saved（ebx/esi/edi/ebp），进程创建时在内核栈顶构建 trapframe + switch_to 恢复帧（ret_addr=process_entry）。process_entry 为 `jmp __trapret`。

idle 进程使用 boot stack（不分配新栈），PID 0，schedule() 找不到 READY 进程时 idle 继续。调度器用 `procs[]` 数组环形扫描（2-3 个进程，无需链表）。TSS.esp0 在 schedule() 中更新。每进程独立 PD，拷贝内核 PDE（PD[768-1023]）共享内核映射。

用户代码来源：内核硬编码字节数组（阶段二验证目标为进程切换，非构建加载基础设施）。用户栈不分配，ESP 假值 0xBFFFFFFC（hlt 循环不访问栈）。
