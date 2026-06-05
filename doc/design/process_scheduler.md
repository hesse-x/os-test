# 进程与调度方案

> **历史文档**：这是 x86-32 阶段二的进程调度设计方案。当前代码已迁移至 x86-64。
>
> 当前实现概要：
> - proc_t: 全64位字段（k_rsp, k_stack_top, cr3, entry 均为 uint64_t）
> - switch_to: pushq rbx/rbp/r12-r15，System V AMD64 (rdi=prev, rsi=next)
> - trapframe: 全64位，process_entry → jmp __trapret → iretq
> - 用户栈: 0x00007FFFFFFFD000（canonical 低半区，非 0xBFFFE000）
> - 每进程独立 PML4（共享 PML4[511]），CR3 在 switch_to 中切换
> - tss.rsp0（非 esp0）在 schedule() 中更新
> - 参见 CLAUDE.md 和 kernel/proc.cc 了解当前实现

## 设计决策汇总

| # | 决策点 | 选择 | 理由 |
|---|--------|------|------|
| 1 | PCB 上下文存储 | 栈上（不内嵌 trapframe/context） | `__alltraps` 已把 trapframe 推到栈上，无需再拷；进程创建时在栈顶构建 trapframe，`__trapret` 直接恢复——零额外拷贝 |
| 2 | switch_to 新进程返回目标 | `process_entry()` 函数 | ebp 链完整、栈回溯可追踪；函数只一条 `jmp __trapret` |
| 3 | 每进程 PD 内核映射 | 创建时拷贝内核 PDE（PD[768-1023]） | 内核映射在 `init_mem` 后不变动，拷贝后无需同步；PDE 存 PT 物理地址，拷贝即共享 PT |
| 4 | 用户验证程序 | `hlt; jmp $`（3 字节），内核打印切换信息 | 验证目标是"进程能跑能切换"，不是"用户态能输出"；ring 3 不能直接 `outb`，输出留给阶段三 syscall |
| 5 | 进程状态 | 仅 READY / RUNNING | 阶段二无 IPC 和 I/O 阻塞，BLOCKED 用不上；加状态时再加枚举值 |
| 6 | 调度器寻找下一进程 | `procs[]` 数组环形扫描 | 只有 2-3 个进程，链表维护代码比扫描多；5 行搞定 vs 20 行 |
| 7 | 第一个进程过渡 | 创建 idle 进程（PID 0）兜底 | 系统始终有进程可运行；idle 用 boot stack + 全局 PD；无 READY 进程时 `hlt` 等中断唤醒 |
| 8 | 内核栈大小 | 8KB（2 页） | 和 boot stack 一致，足够容纳 trapframe + 调用链 + switch_to 保存区 |
| 9 | TSS.esp0 更新时机 | `schedule()` 中，switch_to 之前 | C 代码可读性好；中断门自动 IF=0，EOI 后新 IRQ 暂不响应，安全 |
| 10 | 用户代码来源 | 内核硬编码字节数组 | 现阶段目标验证进程切换，非构建加载基础设施；修改用户代码需重编译内核，但阶段二可接受 |
| 11 | 用户栈 | 不分配，ESP 假值 0xBFFFFFFC | `hlt` 循环不访问栈，假值不影响运行 |

## PCB 结构

```cpp
typedef int pid_t;

enum proc_state_t { READY, RUNNING };

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint32_t k_esp;         // saved kernel ESP（switch_to 用）
    uint32_t k_stack_top;   // kernel stack 虚拟地址顶部（8KB 区间高位端）
    uint32_t cr3;           // page directory 物理地址
    uint32_t entry;         // 用户态入口 EIP（构建 trapframe 用）
};
```

进程表：`proc_t procs[MAX_PROC=64]`，全局数组。`proc_t *current_proc` 指向当前运行进程。

**无 `next` 指针**——调度用数组环形扫描，不需要链表。
**无 trapframe 指针**——trapframe 在内核栈上，通过 `k_esp` 定位。
**无 user_stack_top**——阶段二用户程序不访问栈，ESP 为假值。

## 内核栈布局

### 被抢占进程（时钟中断自然产生）

```
高地址 ← k_stack_top
  trapframe_t                ← CPU + __alltraps 保存
  ── __alltraps 调用返回地址
  ── trap_dispatch 调用返回地址
  ── timer_handler 调用返回地址
  ── schedule() 调用返回地址
  callee-saved (ebx,esi,edi,ebp)  ← switch_to 保存
低地址 ← k_esp
```

恢复路径：switch_to pop callee-saved → ret 回 schedule → trap_dispatch → __trapret → iret。

### 新建进程（手工构建）

```
高地址 ← k_stack_top
  trapframe_t:
    pushregs_t (全0)
    gs=0x23, fs=0x23, es=0x23, ds=0x23
    trapno=0, err_code=0
    eip=用户入口
    cs=0x1B
    eflags=0x202 (IF=1)
    esp=0xBFFFFFFC (假值，hlt 不访问栈)
    ss=0x23
  process_entry 地址            ← switch_to ret 目标
  ebp=0                         ← 栈底标记
  edi=0
  esi=0
  ebx=0
低地址 ← k_esp
```

恢复路径：switch_to pop ebx/esi/edi/ebp → ret → `process_entry` → `jmp __trapret` → popal/pop段寄存器/skip trapno+err/iret → ring 3。

## switch_to 实现

```asm
# void switch_to(proc_t *prev, proc_t *next)
# C calling convention: prev on stack[0], next on stack[4]
switch_to:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp

    # 保存 prev 的 ESP 到 prev->k_esp
    movl 4(%esp), %eax          # prev (跳过 4 个 callee-saved + ret addr = 原始 esp+20)
    # 此时 %esp = 原始esp-16, 参数在原始esp+4 和原始esp+8
    # 需要计算参数位置：当前esp+20 是 prev, 当前esp+24 是 next
    movl %esp, k_esp_offset(%eax)   # prev->k_esp = current ESP

    # 加载 next->k_esp 到 ESP
    movl 24(%esp), %ecx         # next
    movl k_esp_offset(%ecx), %esp   # ESP = next->k_esp

    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret
```

**`k_esp_offset`** 需根据 `proc_t` 结构体布局计算（`k_esp` 在 `state` 之后的偏移量）。

## process_entry 实现

```asm
process_entry:
    jmp __trapret
```

switch_to ret 到此，ESP 指向 trapframe 起始位置（pushregs_t），直接跳转到 `__trapret` 恢复寄存器并 iret。

## 调度器

```cpp
void schedule() {
    proc_t *next = nullptr;
    // 从 current_proc 之后环形扫描，找第一个 READY 进程
    for (int i = current_proc->pid + 1; i < MAX_PROC + current_proc->pid + 1; i++) {
        int idx = i % MAX_PROC;
        if (procs[idx].state == READY) {
            next = &procs[idx];
            break;
        }
    }
    if (next == nullptr) {
        // 无可运行进程，idle 继续执行（hlt 等中断唤醒）
        return;
    }

    // 更新 TSS.esp0 为 next 的内核栈顶
    tss.esp0 = next->k_stack_top;

    // 更新进程状态
    current_proc->state = READY;
    next->state = RUNNING;

    proc_t *prev = current_proc;
    current_proc = next;

    switch_to(prev, next);
}
```

**EOI 在 `schedule()` 之前发送**：timer_handler 先 EOI，再调 schedule()。中断门自动 IF=0，EOI 后 PIC 可排队新 IRQ 但 CPU 不响应，直到 iret 恢复 IF=1。

**idle 进程无 READY 进程时**：schedule() 直接 return，idle 回到 `hlt` 循环等中断唤醒。

## 进程创建

```cpp
proc_t *process_create(uint32_t entry) {
    // 1. 从 procs[] 找空闲槽位（pid == -1 表示未使用）
    proc_t *proc = nullptr;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid < 0) { proc = &procs[i]; break; }
    }
    if (!proc) return nullptr;

    // 2. 分配内核栈（8KB = 2 页）
    Page *stack_pages = BFCAllocator::alloc_page(2);
    uint32_t k_stack_top = PHY_ADDR(stack_pages) + VMA_BASE + 2 * PAGE_SIZE;

    // 3. 分配 PD（1 页）
    Page *pd_page = BFCAllocator::alloc_page(1);
    uint32_t pd_phys = PHY_ADDR(pd_page);
    uint32_t pd_virt = pd_phys + VMA_BASE;

    // 4. 拷贝内核 PDE（PD[768-1023]）
    memcpy((void*)(pd_virt + 768 * 4), (void*)((uint32_t)page_directory + 768 * 4), 256 * 4);

    // 5. 分配并映射用户代码页
    Page *user_code_page = BFCAllocator::alloc_page(1);
    uint32_t user_code_phys = PHY_ADDR(user_code_page);
    uint32_t user_code_virt = user_code_phys + VMA_BASE;

    // 拷贝用户代码字节数组到物理页
    memcpy((void*)user_code_virt, init_code, sizeof(init_code));

    // 分配 PT 并映射用户代码到 0x400000
    // PD[1] (covers 0x400000-0x407FFFF) 指向新 PT
    Page *user_pt_page = BFCAllocator::alloc_page(1);
    uint32_t user_pt_phys = PHY_ADDR(user_pt_page);
    uint32_t user_pt_virt = user_pt_phys + VMA_BASE;

    memset((void*)user_pt_virt, 0, PAGE_SIZE);  // 清零 PT
    // PT[0] = user code page, Present + Writable + User (0x07)
    uint32_t *pt = (uint32_t*)user_pt_virt;
    pt[0] = user_code_phys | 0x07;

    // PD[1] = user PT, Present + Writable + User (0x07)
    uint32_t *pd = (uint32_t*)pd_virt;
    pd[1] = user_pt_phys | 0x07;

    // 6. 在内核栈上构建 trapframe + switch_to 恢复帧
    uint32_t *stack = (uint32_t*)k_stack_top;

    // trapframe（从高地址往低地址填写）
    stack[-1] = 0x23;           // ss
    stack[-2] = 0xBFFFFFFC;     // esp (假值)
    stack[-3] = 0x202;          // eflags (IF=1)
    stack[-4] = 0x1B;           // cs (USER_CS)
    stack[-5] = entry;          // eip (0x400000)
    stack[-6] = 0;              // err_code
    stack[-7] = 0;              // trapno
    stack[-8] = 0x23;           // ds
    stack[-9] = 0x23;           // es
    stack[-10] = 0x23;          // fs
    stack[-11] = 0x23;          // gs
    // pushregs_t (8 个 uint32, 全 0)
    for (int i = 0; i < 8; i++) stack[-12 - i] = 0;

    uint32_t tf_start = (uint32_t)&stack[-12];  // trapframe 起始地址

    // switch_to 恢复帧
    stack[-20] = (uint32_t)process_entry;  // return address
    stack[-21] = 0;                         // ebp (栈底标记)
    stack[-22] = 0;                         // edi
    stack[-23] = 0;                         // esi
    stack[-24] = 0;                         // ebx

    uint32_t k_esp = (uint32_t)&stack[-24];

    // 7. 填写 PCB
    proc->pid = allocated_index;
    proc->state = READY;
    proc->k_esp = k_esp;
    proc->k_stack_top = k_stack_top;
    proc->cr3 = pd_phys;
    proc->entry = entry;

    return proc;
}
```

## idle 进程创建

idle 进程代表内核启动上下文，不创建新栈，直接使用 boot stack：

```cpp
void init_idle_proc() {
    proc_t *idle = &procs[0];
    idle->pid = 0;
    idle->state = RUNNING;
    idle->k_esp = current_esp;           // 保存当前 ESP（boot stack 上）
    idle->k_stack_top = boot_stack_top;  // boot.cc 的 stack_bottom + 8192
    idle->cr3 = PHY_ADDR(page_directory);// 全局 PD 物理地址
    idle->entry = 0;                     // idle 不运行用户代码
    current_proc = idle;
}
```

**获取当前 ESP**：可用内联汇编 `movl %esp, var` 或在调用 `init_idle_proc` 前保存。关键是在 schedule 首次调用 switch_to 时，idle 的 k_esp 指向 boot stack 上 switch_to callee-saved 保存区的位置——这自然发生，因为 idle 首次被 switch_to 切出时，callee-saved 会保存在 boot stack 上，k_esp 被更新。

**idle 进程恢复后行为**：schedule() return → 回到 timer_handler → trap_dispatch → __trapret → iret。但 idle 不在 ring 3，iret 恢复到 idle 的内核代码继续执行（IF=1，等待下一个时钟中断再调 schedule）。

实际 idle 的执行流：idle 在内核态运行，每次时钟中断 → trap_dispatch → schedule → 如果无其他 READY 进程 → return → EOI → __trapret → iret 回到 idle 内核态 → 继续内核初始化后续代码或进入 hlt 循环。

**更精确的 idle 行为**：idle 进程的"用户态"实际是内核态（CS=0x08），它执行一个循环 `while(1) { schedule(); }`——每次 schedule 找到 READY 进程就切出去，找不到就继续循环等待时钟中断再来。或者更简单：idle 在内核态 `hlt`，时钟中断唤醒后 schedule 检查是否有新进程。

## 启动流程（阶段二后）

```
GRUB2 → _start → enable_page → kernel_main
→ init_mem → isr_init → kernel_init_finish
→ init_idle_proc()        // 创建 idle（PID 0），current_proc = idle
→ process_create(0x400000) // 创建 init 进程（PID 1），用户代码映射到 0x400000
→ process_create(0x400000) // 创建 proc A（PID 2），同入口但可区分
→ schedule()              // idle → init（第一次切换）
→ init 在 ring 3 执行 hlt; jmp $
→ 时钟中断 → trap_dispatch → timer_handler → schedule
→ init → proc A → init → proc A ...（轮转）
→ 串口输出 "schedule: proc 1 → proc 2" 等切换信息
```

## 前置清理

实现阶段二前需处理阶段一遗留：

1. **删除 `test_ring3()` 及 #PF ring 3 检测代码**：kernel.cc 中 `test_ring3()` 函数和 trap.cc 中 `#PF from ring 3` 特殊处理——由真正的进程系统替代。
2. **修复 `syscall_ret` 恢复顺序 bug**：当前 `syscall_ret` 的 `popal; addl $8; pop gs/fs/es/ds` 会跳过 gs/fs 而非 trapno/err_code。应改为与 `__trapret` 一致：`popal; pop gs/fs/es/ds; addl $8; iret`。

## 实现步骤

### 步骤 1：清理阶段一遗留

- 删除 `kernel/kernel.cc` 中 `test_ring3()` 函数及调用
- 删除 `kernel/trap.cc` 中 `#PF from ring 3` 特殊处理分支
- 修复 `arch/x86/trapentry.S` 中 `syscall_ret` 的恢复顺序

验证：QEMU 启动，内核正常运行（不再触发 ring 3 测试），时钟中断正常计数。

---

### 步骤 2：定义 PCB + 进程表

**新增文件：** `kernel/proc.h`

- `proc_t` 结构体、`proc_state_t` 枚举、`MAX_PROC=64`
- `procs[]` 数组声明、`current_proc` 指针声明
- 初始化函数 `proc_init()`：清零 procs[]，pid 设为 -1（表示未使用）

验证：编译通过。

---

### 步骤 3：实现 switch_to + process_entry

**修改文件：** `arch/x86/trapentry.S`

- 新增 `switch_to` 汇编函数（保存/恢复 callee-saved + 切换 ESP）
- 新增 `process_entry`（`jmp __trapret`）
- 在 `arch/x86/trap.h` 或 `kernel/proc.h` 中声明 `extern switch_to` 和 `extern process_entry`

验证：编译通过，switch_to 符号可见。

---

### 步骤 4：实现 schedule()

**新增文件：** `kernel/proc.cc`（或 `kernel/sched.cc`）

- `schedule()`：环形扫描找 READY 进程，更新 TSS.esp0，更新状态，调 `switch_to`
- `tss` 和 `page_directory` 需从 `arch/x86/paging.cc` 导出（extern）
- timer_handler 调 schedule() 之前先 EOI

验证：编译通过。

---

### 步骤 5：实现 process_create()

**同文件：** `kernel/proc.cc`

- `process_create(entry)`：分配 PCB/内核栈/PD/用户代码页/PT，拷贝内核 PDE 和用户代码，构建 trapframe 和 switch_to 恢复帧，设置 READY 状态
- 用户代码字节数组 `init_code[]` 定义在此文件或 `kernel/proc.h`
- 需要引入 BFC 分配器头文件

验证：编译通过。

---

### 步骤 6：集成到 kernel_main

**修改文件：** `kernel/kernel.cc`

- 删除 test_ring3 调用（步骤 1 已做）
- 调用 `proc_init()` 初始化进程表
- 调用 `init_idle_proc()` 创建 idle 进程
- 调用 `process_create(0x400000)` 创建 2 个用户进程
- 修改 timer_handler：EOI 后调 `schedule()`
- schedule() 中串口输出切换信息

验证：QEMU 串口输出 "schedule: proc 0 → proc 1"、"schedule: proc 1 → proc 2" 等轮转信息，两个进程交替运行。

---

## 阶段二省略 / 后续补充

| 省略内容 | 后续阶段 | 说明 |
|----------|----------|------|
| 用户栈分配 | 阶段三 | syscall 需要真实用户栈，当前 hlt 循环不访问栈 |
| BLOCKED 状态 | 阶段四 | IPC 需要阻塞/唤醒机制 |
| 进程销毁 | 阶段三+ | 退出/kill 需释放 PCB、栈、PD、用户页 |
| 用户程序独立加载 | 阶段五 | ELF loader + Multiboot2 module tag |
| syscall_ret 修复 | 阶段二步骤 1 | 与 __trapret 恢复顺序一致 |