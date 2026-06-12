# AP 参与调度设计（步骤 3）

> **已实现**。每 CPU 有独立 idle 进程和 run_queue，AP 参与调度。`timer_handler` 用 `tf->cs == 0x2B` 判断抢占，`pick_cpu()` 按 run_count 分配进程。BKL 已在步骤 4 中移除，idle_entry 不再操作 BKL。

## 概述

在步骤 2 BKL 基础上，让 AP 参与用户进程调度。采用 Linux 风格 idle 进程模型：每个 CPU 拥有独立的 idle 内核线程，`schedule()` 简化为纯挑选+切换+返回，idle 进程通过 `hlt + schedule()` 循环睡眠等待。实现 `pick_cpu()` 负载均衡，新进程按 `run_count`（动态可运行计数）分配到最空闲的 CPU。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | idle 进程模型 | Linux 风格：每 CPU 一个 idle 内核线程 | `schedule()` 语义干净（永远返回），idle 是正规进程走 `switch_to`，未来扩展自然（per-CPU runqueue、调度类等） |
| 2 | `schedule()` 语义 | 纯挑选+切换，**总是返回** | 对 idle：schedule() 返回后 hlt；对抢占：schedule() 返回后 iretq 回用户态。不含 hlt 循环 |
| 3 | idle 入口 | `while(1) { schedule(); release BKL; hlt; acquire BKL; }` | hlt 期间释放 BKL 允许其他 CPU 进入内核；任何中断唤醒后重新 acquire + schedule |
| 4 | BSP idle 进程重构 | 分配独立内核栈 + switch_frame，统一创建流程（选项 B） | 所有 idle 进程结构一致，消除 `prev == nullptr` 特殊路径，未来维护简单 |
| 5 | `run_count` 语义 | 动态可运行计数（READY + RUNNING 之和） | 大量 BLOCKED 驱动/服务进程下，静态计数严重失真；所有更新在 BKL 下，无需原子操作 |
| 6 | `pick_cpu()` | 遍历 `cpu_locals[].run_count`，选最小值 | 简单有效，步骤 4 拆锁后可替换为更精细的策略 |
| 7 | `assigned_cpu` | 静态绑定，不迁移 | 步骤 3 目标是跑通多核调度；work stealing 是后续优化 |
| 8 | IRQ 路由 | 不改，全部留在 BSP | AP 靠定时器轮询（~10ms 延迟）拾取 woken 进程，对键盘/磁盘足够；低延迟需 IPI（步骤 5+） |
| 9 | timer_handler 抢占条件 | `tf->cs == 0x2B`（从用户态来则抢占） | 用户态中断时 BKL 已持有，可安全调 schedule()；内核态中断（idle hlt）BKL 未持有，不调 schedule()，仅 EOI |
| 10 | idle 不计入 `run_count` | idle 永远可运行但不计入 | idle 是"兜底"，不是负载；`pick_cpu()` 只看用户进程负载 |
| 11 | `process_entry` BKL 补偿 | 加 `call kernel_lock_acquire` 后 `jmp __trapret` | 修现有 bug：新进程首次运行需 acquire BKL 以平衡 `__trapret` 中 release |
| 12 | AP idle 栈 | 创建独立内核栈，ap_entry_c 末尾切换到 idle 栈后进入 idle_entry | smp_boot_aps 分配的临时栈仅用于 AP 初始化，idle 进程有独立栈避免混淆 |

## 详细设计

### 1. Idle 进程

每个 CPU 一个 idle 进程（内核线程），特征：

- **无用户态**：永不 iretq/sysretq，纯内核循环
- **无独立 PML4**：`cr3 = PHY_ADDR(pml4)`（内核 PML4 物理地址），包含 identity map + higher-half
- **无 trapframe**：内核栈上只有 switch_frame，ret_addr = idle_entry
- **不计入 `run_count`**：通过 `cpu_locals[cpu_id].idle_proc` 指针识别
- **状态**：始终可运行，schedule() 切换 FROM idle 时不设为 READY

```
idle 内核栈布局:

[k_stack_top]
[switch_frame: rbx=0, rbp=0, r12-r15=0, ret_addr=idle_entry]  ← k_rsp
```

### 2. cpu_local_t 扩展

```c
struct cpu_local_t {
    int cpu_id;
    uint32_t apic_id;
    proc_t *_cur_proc;
    uint64_t lapic_base;
    uint64_t kernel_stack;
    uint64_t tss_rsp0;
    int run_count;
    proc_t *idle_proc;    // NEW: 本 CPU 的 idle 进程指针
};
```

### 3. create_idle_process()

```c
// 为指定 CPU 创建 idle 进程
static proc_t *create_idle_process(int cpu_id) {
    // 1. 在 procs[] 中找空闲槽
    // 2. 分配 8KB 内核栈
    // 3. 构建 switch_frame（ret_addr = idle_entry），无 trapframe
    // 4. cr3 = PHY_ADDR(pml4)（内核 PML4）
    // 5. assigned_cpu = cpu_id
    // 6. state = READY
    // 7. 存入 cpu_locals[cpu_id].idle_proc
    // 8. 返回 proc_t*
}
```

### 4. idle_entry()

```c
void idle_entry() {
    kernel_lock_acquire();
    sti();
    while (1) {
        schedule();
        kernel_lock_release();
        __asm__ volatile("hlt");
        kernel_lock_acquire();
    }
}
```

BKL 平衡：
- 入口 acquire 一次
- 每轮循环：schedule() 内部 release→acquire（1次），循环体 release→hlt→acquire（1次）
- schedule() 返回时 BKL 持有，hlt 前 release，hlt 后 acquire，下一轮 schedule() 时 BKL 持有
- 永不退出，无泄露

### 5. schedule() 改造

```c
void schedule() {
    int my_cpu = get_cpu_local()->cpu_id;
    proc_t *idle = get_cpu_local()->idle_proc;
    proc_t *prev = current_proc;

    // 扫描 READY 的非 idle 进程
    proc_t *next = nullptr;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid >= 0 && procs[i].state == READY &&
            procs[i].assigned_cpu == my_cpu && &procs[i] != idle) {
            next = &procs[i];
            break;
        }
    }

    // 没有用户进程可运行，切到 idle
    if (next == nullptr) next = idle;

    // prev == next：无需切换
    if (prev == next) return;

    // 切换 FROM idle 时不改状态（idle 永远可运行）
    if (prev != idle && prev->state == RUNNING) {
        prev->state = READY;
    }

    next->state = RUNNING;
    per_cpu_tss[my_cpu].rsp0 = next->k_stack_top;
    get_cpu_local()->tss_rsp0 = next->k_stack_top;
    current_proc = next;

    kernel_lock_release();
    switch_to(prev, next);
    kernel_lock_acquire();
}
```

**关键变化**：
- 移除 `prev == nullptr` 特殊路径（idle 是正规进程，switch_to 正常工作）
- idle 进程指针从 `cpu_locals` 获取，用于识别和跳过
- 切换 FROM idle 时不设 READY
- 无 READY 用户进程时切换到 idle（而非 return 不做任何事）

### 6. timer_handler 改造

```c
static void timer_handler(trapframe_t *tf) {
    tick++;
    lapic_eoi();
    if (tf->cs == 0x2B) {   // 从用户态来，BKL 已持有
        schedule();
    }
    // 从内核态来（idle hlt）：仅 EOI，idle 醒来后自行调 schedule()
}
```

**关键变化**：
- 移除 `cpu_id != 0` 守卫（AP 现在也参与调度）
- 用 `tf->cs` 检查判断是否可安全调 schedule()，而非硬编码 CPU ID
- 从用户态来：BKL 已由 `__alltraps` acquire，可安全调度
- 从内核态来：BKL 未持有（idle 在 hlt 前已 release），不能调 schedule()

### 7. pick_cpu() 实现

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

### 8. run_count 维护

所有修改在 BKL 保护下，无需原子操作。idle 进程不计入。

| 位置 | 操作 | 说明 |
|------|------|------|
| `process_create` | `cpu_locals[assigned_cpu].run_count++` | 新进程 READY |
| `process_create_elf` | `cpu_locals[assigned_cpu].run_count++` | 新进程 READY |
| `sys_wait` BLOCKED | `cpu_locals[assigned_cpu].run_count--` | RUNNING → BLOCKED |
| `sys_notify` 唤醒 | `cpu_locals[assigned_cpu].run_count++` | BLOCKED → READY |
| trap_dispatch IRQ owner 唤醒 | `cpu_locals[assigned_cpu].run_count++` | BLOCKED → READY |
| schedule() RUNNING↔READY | 不变 | run_count 计 READY+RUNNING 之和，两者转换不影响 |
| 进程退出（未来） | `cpu_locals[assigned_cpu].run_count--` | 移除一个可运行进程 |

### 9. 初始化序列

#### BSP（kernel_main）

```
1. init_mem, isr_init, kernel_init_finish, proc_init   （不变）
2. create_idle_process(0)                                         （NEW：BSP idle）
3. smp_boot_aps():
     对每个 AP (cpu_id 1..ncpu-1):
       - 分配临时栈 + smp_init_cpu（不变）
       - create_idle_process(i)                                   （NEW：AP idle）
       - 填充 trampoline + SIPI（不变）
4. 加载用户进程 process_create_elf（不变）
5. current_proc = cpu_locals[0].idle_proc                         （NEW）
6. idle->state = RUNNING                                          （NEW）
7. 更新 TSS RSP0                                                  （NEW）
8. 切换到 idle 内核栈，进入 idle_entry()（inline asm，永不返回）    （NEW）
```

步骤 8 的栈切换（类似当前 schedule 中 `prev == nullptr` 路径）：

```c
uint64_t idle_rsp = bsp_idle->k_rsp;
__asm__ volatile(
    "movq %0, %%rsp\n"
    "popq %%rbx\n"
    "popq %%rbp\n"
    "popq %%r12\n"
    "popq %%r13\n"
    "popq %%r14\n"
    "popq %%r15\n"
    "retq\n"
    :: "r"(idle_rsp)
    : "memory");
```

BSP 引导栈自此废弃，idle 进程栈接管。

#### AP（ap_entry_c）

```
1. smp_apply_cpu, idt_install, LAPIC, timer（不变）
2. current_proc = cpu_locals[cpu_id].idle_proc           （NEW）
3. idle->state = RUNNING                                 （NEW）
4. 更新 TSS RSP0                                         （NEW）
5. 切换到 idle 内核栈，进入 idle_entry()（同 BSP 栈切换） （NEW）
```

移除原有的 `while(1) hlt` 循环。

### 10. process_entry BKL bug 修复

当前 `process_entry` 缺少 `kernel_lock_acquire`，导致 `__trapret` 中 release 失衡：

```asm
# 修复前
process_entry:
    jmp __trapret

# 修复后
process_entry:
    call kernel_lock_acquire
    jmp __trapret
```

背景：schedule() 在 switch_to 前已 release BKL。新进程首次运行时 BKL 未持有，需 acquire 以平衡 `__trapret` 的 release。

## BKL 平衡验证

| 路径 | acquire | release |
|------|---------|---------|
| 用户态中断 → trap_dispatch → __trapret | `__alltraps` (CS==0x2B) | `__trapret` (CS==0x2B) |
| 用户态中断 → trap_dispatch → schedule → __trapret | `__alltraps` | schedule unlock + `__trapret` |
| syscall → syscall_dispatch → sysretq | `syscall_fast_entry` | syscall 返回路径 |
| syscall → schedule → sysretq | `syscall_fast_entry` | schedule unlock + syscall 返回路径 |
| 新进程首次运行 → process_entry → __trapret | `process_entry`（修复后） | `__trapret` |
| idle → schedule → switch_to → 用户进程 | idle_entry | schedule unlock |
| 用户进程 → schedule → switch_to → idle | schedule 中 switch_to 后 acquire | idle 循环体 release |
| idle hlt → timer 中断 → EOI → idle 继续 | idle 循环体 acquire | （无，hlt 前已 release） |
| AP timer 中断（idle hlt） | 不 acquire | 不 release（仅 EOI） |
| 内核态异常 | 不 acquire | 不 release（直接 halt） |

关键不变量：**schedule() 调用时 BKL 持有，返回时 BKL 持有**。

## 代码改动清单

| # | 文件 | 改动 |
|---|------|------|
| 1 | `arch/x64/smp.h` | `cpu_local_t` 新增 `idle_proc` 字段 |
| 2 | `kernel/proc.h` | 声明 `idle_entry()`、`create_idle_process()` |
| 3 | `kernel/proc.cc` | 实现 `create_idle_process()`：分配栈+switch_frame+内核PML4；实现 `idle_entry()`；`pick_cpu()` 遍历 run_count 选最小；`schedule()` 移除 `prev==nullptr` 特殊路径，加入 idle 判断逻辑；`process_create`/`process_create_elf` 调 `pick_cpu()` 并 `run_count++`；`build_idle_kstack()` |
| 4 | `kernel/trap.cc` | `timer_handler`：移除 `cpu_id != 0` 守卫，改用 `tf->cs == 0x2B` 检查；`sys_wait`：`run_count--`；`sys_notify`：`run_count++`；trap_dispatch IRQ owner 唤醒：`run_count++` |
| 5 | `arch/x64/trapentry.S` | `process_entry`：加 `call kernel_lock_acquire` |
| 6 | `kernel/kernel.cc` | kernel_main：创建 BSP idle 进程，设 current_proc，栈切换进入 idle_entry，移除旧 idle 循环 |
| 7 | `arch/x64/smp.cc` | smp_boot_aps：为每个 AP 创建 idle 进程；ap_entry_c：设 current_proc，栈切换进入 idle_entry，移除 `while(1) hlt` |

## 验证

1. 编译通过
2. `-smp 2` 启动：BSP + AP 均进入 idle_entry
3. 用户进程在不同 CPU 上调度运行（串口输出可见 CPU ID）
4. 键盘/磁盘驱动正常工作（IRQ 路由 BSP，AP 进程被 notify 后在下次定时器唤醒时调度）
5. 多次运行稳定，无死锁/panic
