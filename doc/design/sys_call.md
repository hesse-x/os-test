# 阶段三：系统调用 + 用户分页 + 用户栈

> **历史文档**：这是 x86-32 阶段三的系统调用设计方案。当前代码已迁移至 x86-64。
>
> 当前实现概要：
> - syscall 使用 SYSCALL/SYSRET 指令（MSR LSTAR），不再使用 int 0x80。int 0x80 路径已移除
> - syscall_fast_entry: swapgs → %gs:32 读 tss_rsp0 切栈 → 手动构建 trapframe → call syscall_dispatch
> - SYSRET 返回：从 trapframe 恢复寄存器 → RCX=rip/R11=rflags → swapgs → sysretq
> - 详见 [syscall_fastpath.md](syscall_fastpath.md)
> - syscall_dispatch: `syscall_table[tf->rax](tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8)`（Linux x86-64 约定）
> - 所有 syscall 函数签名: `uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)`
> - 20 个系统调用（编号 0-19 连续无空洞）：getpid(0), yield(1), wait(2), notify(3), irq_bind(4), exit(5), waitpid(6), spawn(7), mmap(8), munmap(9), serial_write(10), fb_info(11), shm_create(12), shm_attach(13), pipe(14), write(15), read(16), close(17), load_dev(18), lookup_dev(19)
> - sys_putc/sys_getc/sys_sbrk 已删除，编号已紧凑重排为 0-17
> - sys_wait/sys_notify/sys_irq_bind 为用户态驱动 IPC 基础设施，详见 [user_driver.md](user_driver.md)
> - 用户地址空间: 代码 0x400000, 栈 0x00007FFFFFFFD000
> - 独立 PML4 + CR3 切换（switch_to 中 movq 24(%rsi), %rax; movq %rax, %cr3）
> - BLOCKED + WAIT_NOTIFY 阻塞/唤醒（WAIT_KBD 已移除，键盘由用户态驱动接管）
> - IRQ 绑定机制：irq_owner[] + trap_dispatch 查表唤醒绑定进程
> - IOPL 支持：驱动进程 IOPL=3，普通进程 IOPL=0
> - 参见 CLAUDE.md 和 kernel/trap.cc 了解当前实现

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| syscall 入口寄存器保存 | 保持 pushal，与 __alltraps 一致 | pushal 只多存4个寄存器，int 0x80 低频路径性能影响忽略不计；栈布局与 trapframe_t 完全一致，syscall_dispatch 直接用 trapframe_t* |
| 进程地址空间 | 独立 PD + CR3 切换 | 进程间内存隔离，shell 必需；之前 crash bug 边做边排查 |
| 用户地址空间布局 | 经典三段：代码 0x400000, 堆预留 0x600000, 栈 0xBFFFE000 | 间距充足，栈远离 VMA_BASE 边界 |
| syscall 分发方式 | trap_dispatch 检查 trapno==128 后调用 syscall_dispatch | 与硬件中断/异常共享 trap_dispatch 入口，统一分发路径 |
| syscall 参数传递 | 从 trapframe 的 pushregs_t 提取参数 | `uint32_t (*)(uint32_t,...)` 分发表，dispatch 提取 tf->regs.ebx/ecx/edx/esi/edi 作为5个参数，返回值写 tf->regs.eax |
| syscall 集合 | 20个（编号 0-19 连续无空洞） | 见 common/syscall.h |
| 阻塞机制 | wait_event 字段 + 扫描 procs[] | freestanding 无 std::list，64进程扫描成本忽略不计 |
| sys_yield 实现 | 直接调 schedule()，无特殊处理 | 与 timer IRQ 路径共享同一 schedule()，Linux 同方案 |
| CR3 切换策略 | 无条件写 CR3 | 每次都刷新 TLB，简单正确；避免 CR3 比较开销（需要读旧值+条件跳转），64 进程下切换频率不高 |

## 用户地址空间布局

```
每个进程独立 PD：

0x400000   ┌─ 代码区 (1 page+, PD[1])
           │
0x600000   ├─ 堆区 (预留, brk syscall 按需扩展)
           │   ...
0xBFFFE000 ├─ 栈区 (1 page, PD[767], PT[1023])
0xC0000000 └─ VMA_BASE (kernel, PD[768+])

ESP = 0xBFFFF000 (栈区起始，向下增长)
entry = 0x400000
```

## 7. 系统调用框架

### syscall_entry（保持 pushal，与 __alltraps 布局一致）

```asm
syscall_entry:                # int 0x80 从 ring 3 进入
    pushl %ds                 # 段寄存器
    pushl %es
    pushl %fs
    pushl %gs
    pushal                    # edi,esi,ebp,esp_ignored,ebx,edx,ecx,eax
    movl $0x10, %eax          # 加载内核数据段
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl %esp                # 传 trapframe_t* 指针
    call syscall_dispatch
    addl $4, %esp
    # 直接落入 syscall_ret（非 jmp）
```

### syscall_ret

```asm
syscall_ret:
    popal                     # 恢复 pushal 保存的寄存器
    popl %gs                  # 恢复段寄存器
    popl %fs
    popl %es
    popl %ds
    addl $8, %esp             # 跳过 trapno + err_code
    iret                      # 弹出 EIP, CS, EFLAGS; 特权级变化时还弹出 ESP, SS
```

栈布局与 __alltraps 完全一致，syscall_dispatch 收到 trapframe_t*：
- `tf->regs.eax` = syscall 号
- `tf->regs.ebx` = arg1, `tf->regs.ecx` = arg2, `tf->regs.edx` = arg3, `tf->regs.esi` = arg4, `tf->regs.edi` = arg5
- 返回值写回 `tf->regs.eax`，syscall_ret 的 popal 自动恢复

注意：trapframe_t 的通用寄存器嵌套在 `pushregs_t regs` 子结构中，因此访问路径为 `tf->regs.eax` 而非 `tf->eax`。

### syscall 分发路径

trap_dispatch 检查 `tf->trapno == 128`（int 0x80 对应向量 128），直接调用 syscall_dispatch：

```c
void trap_dispatch(trapframe_t *tf) {
    if (tf->trapno == 128) {
        syscall_dispatch(tf);
        return;
    }
    // ... IRQ 注册表、默认处理 ...
}
```

### syscall_dispatch 实现

```c
void syscall_dispatch(trapframe_t *tf) {
    if (tf->regs.eax < NR_SYSCALL) {
        tf->regs.eax = syscall_table[tf->regs.eax](
            tf->regs.ebx, tf->regs.ecx, tf->regs.edx,
            tf->regs.esi, tf->regs.edi);
    } else {
        tf->regs.eax = (uint32_t)-1;  // 无效 syscall 号
    }
}
```

## 8. 系统调用（x86-64 当前实现）

### 概览

| # | 名称 | 功能 | 参数（RDI, RSI, RDX, R10, R8） | 返回值 (RAX) |
|---|------|------|-------------------------------|-------------|
| 0 | sys_getpid | 获取当前进程 PID | 无 | pid |
| 1 | sys_yield | 主动让出 CPU，触发调度 | 无 | 0 |
| 2 | sys_wait | 阻塞当前进程，等待 WAIT_NOTIFY | 无 | 0（被唤醒后） |
| 3 | sys_notify | 唤醒指定 PID 的阻塞进程 | arg1=(pid_t)target_pid | 0 |
| 4 | sys_irq_bind | 绑定当前进程到指定 IRQ 向量 | arg1=(int)irq | 0 成功 / -1 失败 |

### ABI

- **入口**：SYSCALL 指令 → MSR_LSTAR → `syscall_fast_entry`
- **调用约定**：Linux x86-64 syscall 约定
  - RAX = syscall 编号
  - RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5
  - 返回值写回 RAX
- **函数签名**：`uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)`
- **分发**：`syscall_table[tf->rax](tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8)`
- **无效 syscall 号**：返回 `(uint64_t)-1`

### sys_getpid() — syscall 0

返回当前进程的 PID（从 `current_proc->pid` 读取）。

```c
uint64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return (uint64_t)current_proc->pid;
}
```

### sys_yield() — syscall 1

主动让出 CPU，调用 `schedule()` 将当前进程重新入队，切换到下一个就绪进程。

```c
uint64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    schedule();
    return 0;
}
```

### sys_wait() — syscall 2

阻塞当前进程（state=BLOCKED, wait_event=WAIT_NOTIFY），递减 run_count，调用 `schedule()` 切走。进程在被 `sys_notify` 或 IRQ 唤醒后从 `schedule()` 返回点继续执行。

```c
uint64_t sys_wait(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    current_proc->state = BLOCKED;
    current_proc->wait_event = WAIT_NOTIFY;
    __atomic_add_fetch(&cpu_locals[current_proc->assigned_cpu].run_count, -1, __ATOMIC_RELAXED);
    schedule();
    return 0;
}
```

典型用法：用户态驱动在空闲循环中 `sys_wait()` 阻塞，等待事件（键盘中断、磁盘请求等）唤醒后处理。

### sys_notify(pid_t target_pid) — syscall 3

唤醒指定 PID 的阻塞进程。遍历 `procs[]` 查找匹配 PID 且 state==BLOCKED + wait_event==WAIT_NOTIFY 的进程，获取目标 CPU 的 `scheduler_lock` 后将状态改为 READY、入队 run_queue、递增 run_count。

```c
uint64_t sys_notify(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    pid_t target_pid = (pid_t)arg1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid == target_pid &&
            procs[i].state == BLOCKED &&
            procs[i].wait_event == WAIT_NOTIFY) {
            int target_cpu = procs[i].assigned_cpu;
            spin_lock(&cpu_locals[target_cpu].scheduler_lock);
            procs[i].state = READY;
            procs[i].wait_event = WAIT_NONE;
            list_push_back(&cpu_locals[target_cpu].run_queue, &procs[i].run_node);
            cpu_locals[target_cpu].run_count++;
            spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
            break;
        }
    }
    return 0;
}
```

典型用法：disk_driver 完成磁盘操作后 `sys_notify(shell_pid)` 通知 shell；fs_driver 请求磁盘后由 disk_driver 唤醒。

### sys_irq_bind(int irq) — syscall 4

将当前进程绑定到指定硬件 IRQ 向量。绑定后，该 IRQ 发生时 `trap_dispatch` 查 `irq_owner[irq]`，发现已绑定用户进程则直接唤醒（获取 scheduler_lock → READY → 入队 → EOI），不再走内核 ISR 注册表。

```c
uint64_t sys_irq_bind(uint64_t arg1, uint64_t, uint64_t, uint64_t, uint64_t) {
    int irq = (int)arg1;
    if (irq < 0 || irq >= MAX_IRQ_HANDLERS) return (uint64_t)-1;
    __atomic_store_n(&irq_owner[irq], current_proc->pid, __ATOMIC_RELEASE);
    return 0;
}
```

典型用法：kbd_driver 启动时 `sys_irq_bind(33)` 绑定键盘中断，之后 `sys_wait()` 阻塞，IRQ 到来时内核自动唤醒 kbd_driver。

### 用户态封装

定义在 `common/syscall.h`，底层内联汇编在 `arch/x64/utils.h`：

```c
// arch/x64/utils.h — 底层 syscall 指令封装
static inline int64_t __syscall0(int64_t nr) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory");
    return ret;
}
static inline int64_t __syscall1(int64_t nr, int64_t a1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

// common/syscall.h — 语义封装
static inline int64_t sys_getpid()         { return __syscall0(SYS_GETPID); }
static inline void sys_yield()             { __syscall0(SYS_YIELD); }
static inline void sys_wait()              { __syscall0(SYS_WAIT); }
static inline void sys_notify(int32_t pid) { __syscall1(SYS_NOTIFY, (int64_t)pid); }
static inline void sys_irq_bind(int irq)   { __syscall1(SYS_IRQ_BIND, (int64_t)irq); }
```

## 9. 独立 PD + CR3 切换

`process_create` 已删除，当前仅保留 `process_create_elf`。详见 `kernel/proc.cc`。

    Page *user_code_pt_page = bfc_alloc.alloc_page(1);
    uint32_t *code_pt = (uint32_t *)phys_to_virt(page_to_phys(user_code_pt_page));
    for (int i = 0; i < 1024; i++) code_pt[i] = 0;
    code_pt[0] = user_code_phys | 0x07;   // Present + Writable + User
    new_pd[1] = page_to_phys(user_code_pt_page) | 0x07;

    // 7. 分配用户栈页 + PT（PD[767], PT[1023] → 0xBFFFE000）
    Page *user_stack_page = bfc_alloc.alloc_page(1);
    uint32_t user_stack_phys = page_to_phys(user_stack_page);

    Page *user_stack_pt_page = bfc_alloc.alloc_page(1);
    uint32_t *stack_pt = (uint32_t *)phys_to_virt(page_to_phys(user_stack_pt_page));
    for (int i = 0; i < 1024; i++) stack_pt[i] = 0;
    stack_pt[1023] = user_stack_phys | 0x07;  // PT[1023] → 0xBFFFF000
    new_pd[767] = page_to_phys(user_stack_pt_page) | 0x07;

    // 8. 在内核栈上构建 trapframe + switch_to 帧
    // 9. proc->cr3 = pd_phys（物理地址，CR3 需要）
}
```

辅助函数：
```c
static uint32_t page_to_phys(Page *p) {
    return (uint32_t)(p - BFCAllocator::frames) * PAGE_SIZE;
}
static uint32_t phys_to_virt(uint32_t phys) {
    return phys + VMA_BASE;
}
```

### switch_to 实现

```asm
# void switch_to(proc_t *prev, proc_t *next)
# proc_t layout: pid(0) state(4) k_esp(8) k_stack_top(12) cr3(16) entry(20) wait_event(24)
switch_to:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp

    movl 20(%esp), %eax          # prev (callee-saved push 后偏移 +16)
    movl %esp, 8(%eax)           # prev->k_esp = current ESP

    movl 24(%esp), %ecx          # next
    movl 8(%ecx), %esp           # ESP = next->k_esp

    # 无条件切 CR3（每次都 flush TLB）
    movl 16(%ecx), %eax          # next->cr3
    movl %eax, %cr3              # 写 CR3 自动 flush TLB

    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret
```

注意：idle 进程（PID 0）仍用全局 page_directory 的物理地址作为 cr3。

### trapframe 构建

在内核栈顶构建完整的 trapframe + switch_to 恢复帧：

```c
uint32_t *stack = (uint32_t *)k_stack_top;

// trapframe (从高地址向下)
stack[-1]  = 0x23;           // ss (USER_DS)
stack[-2]  = 0xBFFFF000;     // esp (用户栈顶)  ⚠️ 当前代码误写为 0xC0000000，需修复
stack[-3]  = 0x202;          // eflags (IF=1)
stack[-4]  = 0x1B;           // cs (USER_CS)
stack[-5]  = entry;          // eip
stack[-6]  = 0;              // err_code
stack[-7]  = 0;              // trapno
stack[-8]  = 0x23;           // ds
stack[-9]  = 0x23;           // es
stack[-10] = 0x23;           // fs
stack[-11] = 0x23;           // gs
// pushregs_t: edi, esi, ebp, esp_ignored, ebx, edx, ecx, eax
stack[-12] = 0;              // eax
stack[-13] = 0;              // ecx
stack[-14] = 0;              // edx
stack[-15] = 0;              // ebx
stack[-16] = 0;              // esp_ignored
stack[-17] = 0;              // ebp
stack[-18] = 0;              // esi
stack[-19] = 0;              // edi

// switch_to 恢复帧 (trapframe 下方)
stack[-20] = (uint32_t)process_entry;  // return address
stack[-21] = 0;                         // ebp
stack[-22] = 0;                         // edi
stack[-23] = 0;                         // esi
stack[-24] = 0;                         // ebx

uint32_t k_esp = (uint32_t)&stack[-24];
```

process_entry 是新进程首次被 switch_to 返回后的入口，跳转到 __trapret 开始 iret 回用户态：
```asm
process_entry:
    jmp __trapret
```

> **已知 bug**：`proc.cc:155` 中 `stack[-2]` 被写为 `0xC0000000`（VMA_BASE），这是内核空间地址，ring 3 无法访问。应改为 `0xBFFFF000`（用户栈页顶部）。

## 10. 进程状态扩展

```c
typedef int32_t pid_t;

enum proc_state_t { READY, RUNNING, BLOCKED };
enum wait_event_t { WAIT_NONE, WAIT_KBD };

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint32_t k_esp;         // saved kernel ESP (for switch_to)
    uint32_t k_stack_top;   // kernel stack top (8KB region high end)
    uint32_t cr3;           // page directory physical address
    uint32_t entry;         // user entry EIP
    wait_event_t wait_event; // 阻塞原因
};
```

### 进程初始化

```c
void proc_init() {
    for (int i = 0; i < MAX_PROC; i++) {
        procs[i].pid = -1;          // 标记槽位空闲
        procs[i].state = READY;
        procs[i].wait_event = WAIT_NONE;
    }
    current_proc = nullptr;
}

void init_idle_proc() {
    proc_t *idle = &procs[0];
    idle->pid = 0;
    idle->state = RUNNING;
    idle->k_esp = <current ESP>;    // 读取当前 ESP（引导栈位置）
    idle->k_stack_top = (uint32_t)&stack_bottom + 8192;
    idle->cr3 = PHY_ADDR((uintptr_t)page_directory);  // 使用全局 PD
    idle->wait_event = WAIT_NONE;
    current_proc = idle;
    tss.esp0 = idle->k_stack_top;
}
```

### schedule() 实现

```c
void schedule() {
    if (current_proc == nullptr) return;

    proc_t *next = nullptr;
    // 环形扫描，只找 state==READY 的进程，跳过 BLOCKED
    for (int i = current_proc->pid + 1; i < MAX_PROC + current_proc->pid + 1; i++) {
        int idx = i % MAX_PROC;
        if (procs[idx].pid >= 0 && procs[idx].state == READY) {
            next = &procs[idx];
            break;
        }
    }
    if (next == nullptr) return;     // 无就绪进程，当前继续运行

    tss.esp0 = next->k_stack_top;   // 更新 TSS.esp0

    // 只有 RUNNING 进程才设为 READY（被抢占/yield）
    // BLOCKED 进程保持 BLOCKED
    if (current_proc->state == RUNNING) {
        current_proc->state = READY;
    }
    next->state = RUNNING;

    proc_t *prev = current_proc;
    current_proc = next;
    switch_to(prev, next);

    // switch_to 返回后（prev 被恢复），确保中断开启
    // 从 syscall 路径切出时 int 0x80 门自动 CLI，IF=0
    __asm__ volatile("sti");
}
```

### kbd_handler 唤醒逻辑

```c
static void keyboard_handler(trapframe_t *tf) {
    kbd_handle();                // 读 scancode 转 ASCII 入缓冲区
    // 唤醒等待键盘的进程
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].pid >= 0 && procs[i].state == BLOCKED &&
            procs[i].wait_event == WAIT_KBD) {
            procs[i].state = READY;
            procs[i].wait_event = WAIT_NONE;
            break;               // 只唤醒一个
        }
    }
    outb(0x20, 0x20);            // EOI
}
```

## 验证计划

1. **sys_yield 验证**：用户进程调用 sys_yield 后进程轮转正常
4. **独立 PD 验证**：CR3 切换后进程正常运行，内核空间访问正常

## 实现顺序

```
1. proc_t 扩展（BLOCKED + wait_event）
   → 验证: 编译通过 ✅

2. 独立 PD + CR3 切换 + 用户栈分配
   → 验证: QEMU 轮转正常运行 ✅（ESP bug 待修复）

3. syscall_dispatch + syscall_table 实现
   → 验证: syscall stub 输出正确 ✅

4. sys_getpid + sys_yield 实现
   → 验证: 用户态 syscall 正常工作 ✅
```
