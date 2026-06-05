# 阶段三：系统调用 + 用户分页 + 用户栈

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| syscall 入口寄存器保存 | 保持 pushal，与 __alltraps 一致 | pushal 只多存4个寄存器，int 0x80 低频路径性能影响忽略不计；栈布局与 trapframe_t 完全一致，syscall_dispatch 直接用 trapframe_t* |
| 进程地址空间 | 独立 PD + CR3 切换 | 进程间内存隔离，shell 必需；之前 crash bug 边做边排查 |
| 用户地址空间布局 | 经典三段：代码 0x400000, 堆预留 0x600000, 栈 0xBFFFE000 | 间距充足，栈远离 VMA_BASE 边界 |
| syscall 分发方式 | 从 trapframe 提取参数传入 | `uint32_t (*)(uint32_t,...)` 分发表，dispatch 提取 tf->ebx/ecx/edx/esi/edi 作为5个参数，返回值写 tf->eax |
| syscall 集合 | 4个：putc(0), getpid(1), yield(2), getc(3) | shell 最小依赖；getc 阻塞等待键盘输入 |
| 阻塞机制 | wait_event 字段 + 扫描 procs[] | freestanding 无 std::list，64进程扫描成本忽略不计 |
| sys_yield 实现 | 直接调 schedule()，无特殊处理 | 与 timer IRQ 路径共享同一 schedule()，Linux 同方案 |

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

### syscall_entry（保持 pushal，现有代码不变）

```asm
syscall_entry:                # int 0x80 从 ring 3 进入
    pushl %ds/%es/%fs/%gs     # 段寄存器
    pushal                    # edi,esi,ebp,esp_ignored,ebx,edx,ecx,eax
    movl $0x10, segment regs  # 加载内核数据段
    pushl %esp                # 传 trapframe_t* 指针
    call syscall_dispatch
    addl $4, %esp
    jmp syscall_ret           # popal + 恢复段寄存器 + skip trapno/err_code + iret
```

栈布局与 __alltraps 完全一致，syscall_dispatch 收到 trapframe_t*：
- `tf->eax` = syscall 号
- `tf->ebx` = arg1, `tf->ecx` = arg2, `tf->edx` = arg3, `tf->esi` = arg4, `tf->edi` = arg5
- 返回值写回 `tf->eax`，syscall_ret 的 popal 自动恢复

### syscall_dispatch 实现

```c
typedef uint32_t (*syscall_fn_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

#define NR_SYSCALL 4
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_putc,    // 0: 输出字符
    sys_getpid,  // 1: 获取 PID
    sys_yield,   // 2: 主动让出 CPU
    sys_getc,    // 3: 读键盘输入（阻塞）
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->eax < NR_SYSCALL) {
        tf->eax = syscall_table[tf->eax](
            tf->ebx, tf->ecx, tf->edx, tf->esi, tf->edi);
    } else {
        tf->eax = -1;  // 无效 syscall 号
    }
}
```

## 8. 基础系统调用

### sys_putc(char c) — syscall 0

```c
uint32_t sys_putc(uint32_t arg1, ...) {
    fb_putc((char)arg1, 0xFFFFFF);
    serial_putc((char)arg1);
    return 0;
}
```

### sys_getpid() — syscall 1

```c
uint32_t sys_getpid(uint32_t, ...) {
    return current_proc->pid;
}
```

### sys_yield() — syscall 2

```c
uint32_t sys_yield(uint32_t, ...) {
    schedule();  // 直接调用，与 timer IRQ 共享同一路径
    return 0;    // 被唤醒后从 schedule() 返回点继续
}
```

执行流程：
1. sys_yield 调用 schedule()
2. schedule 把 current_proc 状态改为 READY，switch_to 切到下一个进程
3. 当前进程下次被轮转回来时，从 schedule() 返回点继续
4. syscall_dispatch 写 tf->eax = 0
5. syscall_ret 恢复回用户态

### sys_getc() — syscall 3

```c
uint32_t sys_getc(uint32_t, ...) {
    if (kbd_buffer_empty()) {
        current_proc->state = BLOCKED;
        current_proc->wait_event = WAIT_KBD;
        schedule();
        // 被唤醒后继续执行
    }
    return kbd_buffer_pop();
}
```

阻塞与唤醒：
- sys_getc 发现键盘缓冲区空 → 设 `state=BLOCKED, wait_event=WAIT_KBD` → schedule()
- kbd IRQ handler：将 scancode 转 ASCII 入缓冲区 → 扫描 procs[] 找 `BLOCKED+WAIT_KBD` 进程 → 设 `state=READY`
- 被唤醒进程下次轮转回来时，从 schedule() 返回点继续 → kbd_buffer_pop() 取字符

## 9. 独立 PD + CR3 切换

### process_create 改造

原来所有进程共享全局 `page_directory`，改为每个进程独立 PD：

```c
proc_t *process_create(uint32_t entry) {
    // 1. 分配 PCB + 内核栈（不变）
    // 2. 分配用户代码页 + 用户栈页 + 各自的 PT（不变）
    // 3. 分配独立 PD 页
    Page *pd_page = bfc_alloc.alloc_page(1);
    uint32_t pd_phys = page_to_phys(pd_page);
    uint32_t pd_virt = phys_to_virt(pd_phys);

    // 4. 清零 PD
    memset((void*)pd_virt, 0, PAGE_SIZE);

    // 5. 拷贝内核 PDE（PD[768..1023]），共享内核 PT
    uint32_t *new_pd = (uint32_t*)pd_virt;
    for (int i = 768; i < 1024; i++) {
        new_pd[i] = page_directory[i];  // 共享内核 PT，只拷贝 PDE 值
    }

    // 6. 设置用户 PDE（代码 PD[1], 栈 PD[767]）
    //    各进程有独立的 PT，映射各自的代码/栈页
    new_pd[1] = user_code_pt_phys | 0x07;   // User + Present + Writable
    new_pd[767] = user_stack_pt_phys | 0x07;

    // 7. proc->cr3 = pd_phys（物理地址，load_cr3 需要）
    proc->cr3 = pd_phys;
}
```

### switch_to 改造

```asm
switch_to:
    pushl %ebx/%esi/%edi/%ebp

    movl 20(%esp), %eax          # prev
    movl %esp, 8(%eax)           # prev->k_esp

    movl 24(%esp), %ecx          # next
    movl 8(%ecx), %esp           # ESP = next->k_esp

    # 切 CR3（仅在 PD 不同时才切，避免不必要的 TLB flush）
    movl 16(%ecx), %eax          # next->cr3
    movl %eax, %cr3              # 写 CR3 自动 flush TLB

    popl %ebp/%edi/%esi/%ebx
    ret
```

注意：idle 进程（PID 0）仍用全局 page_directory 的物理地址作为 cr3。

### 用户栈分配

每个进程分配 1 页用户栈：

```c
// 分配栈页
Page *stack_page = bfc_alloc.alloc_page(1);
uint32_t stack_phys = page_to_phys(stack_page);

// 分配栈 PT（PD[767]）
Page *stack_pt_page = bfc_alloc.alloc_page(1);
uint32_t stack_pt_phys = page_to_phys(stack_pt_page);
uint32_t *stack_pt = (uint32_t*)phys_to_virt(stack_pt_phys);
memset(stack_pt, 0, PAGE_SIZE);
stack_pt[1023] = stack_phys | 0x07;  // PT[1023] → 0xBFFFE000-0xBFFFFFFF

// trapframe 中 ESP 设为 0xBFFFF000
```

### trapframe 构建（更新 ESP）

```
stack[-2]  = 0xBFFFF000;     // esp（真实用户栈顶）
// 其余不变
```

## 10. 进程状态扩展

```c
enum proc_state_t { READY, RUNNING, BLOCKED };
enum wait_event_t { WAIT_NONE, WAIT_KBD };

struct proc_t {
    pid_t pid;
    proc_state_t state;
    uint32_t k_esp;
    uint32_t k_stack_top;
    uint32_t cr3;
    uint32_t entry;
    wait_event_t wait_event;   // 阻塞原因
};
```

schedule() 扫描时跳过 BLOCKED 进程（只找 READY）：
```c
void schedule() {
    // 环形扫描，只找 state==READY 的进程
    for (...) {
        if (procs[idx].pid >= 0 && procs[idx].state == READY) {
            next = &procs[idx];
            break;
        }
    }
}
```

kbd_handler 唤醒逻辑：
```c
static void keyboard_handler(trapframe_t *tf) {
    kbd_handle();                // 读 scancode 转 ASCII 入缓冲区
    // 唤醒等待键盘的进程
    for (int i = 0; i < MAX_PROC; i++) {
        if (procs[i].state == BLOCKED && procs[i].wait_event == WAIT_KBD) {
            procs[i].state = READY;
            procs[i].wait_event = WAIT_NONE;
            break;               // 只唤醒一个
        }
    }
    outb(0x20, 0x20);            // EOI
}
```

## 验证计划

1. **sys_putc 验证**：用户进程代码改为调用 `int 0x80` (eax=0, ebx='A')，屏幕输出字符 'A'
2. **sys_yield 验证**：用户进程调用 sys_yield 后进程轮转正常
3. **sys_getc 验证**：shell 进程调用 sys_getc 等待键盘输入，按键后字符回显
4. **独立 PD 验证**：CR3 切换后进程正常运行，内核空间访问正常

## 实现顺序

```
1. proc_t 扩展（BLOCKED + wait_event）
   → 验证: 编译通过

2. 独立 PD + CR3 切换 + 用户栈分配
   → 验证: QEMU 轮转正常运行（如果 CR3 crash 则排查）

3. syscall_dispatch + syscall_table 实现
   → 验证: syscall stub 输出正确

4. sys_putc + sys_getpid + sys_yield 实现
   → 验证: 用户态通过 int 0x80 打印字符

5. sys_getc + kbd 阻塞唤醒
   → 验证: shell 进程等待按键并回显
```