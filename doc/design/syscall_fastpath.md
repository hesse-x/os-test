# SYSCALL/SYSRET 快速系统调用

> **已实现**。当前代码使用 SYSCALL/SYSRET 指令（MSR LSTAR）替代 int 0x80 进行系统调用，与 `__alltraps`/`__trapret` 独立。int 0x80 路径已移除。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 系统调用指令 | SYSCALL/SYSRET（MSR） | x86-64 标准快速系统调用，不经过 IDT，无需 push 段寄存器，比 int 0x80 快 |
| 2 | syscall 与 trap 独立 | `syscall_fast_entry` 不走 `__alltraps` | 职责分离，SYSCALL 不自动 push SS/RSP/RFLAGS，需要手动构建 trapframe |
| 3 | trapframe 统一 | 手动构建与 `__alltraps` 相同布局的 trapframe | `syscall_dispatch` 共用 trapframe_t* 参数，无需额外分发表 |
| 4 | 用户栈保存 | SYSCALL 前先 movq %rsp, %r10 | SYSCALL 不自动保存用户 RSP，R10 是 scratch 寄存器（不保留） |
| 5 | 内核栈切换 | `swapgs` → `%gs:32` 读 `tss_rsp0` | tss_rsp0 由 `schedule()` 更新为当前进程内核栈顶，per-CPU 安全 |
| 6 | swapgs 策略 | syscall 入口无条件 swapgs，出口无条件 swapgs | SYSCALL 只从用户态进入（EFER.SCE 只允许 ring 3 触发），不存在内核态来 syscall 的问题 |
| 7 | STAR 设置 | `[47:32]=0x08, [63:48]=0x18` | SYSCALL: CS=0x08, SS=0x10; SYSRET64: CS=0x2B, SS=0x23; SYSRET32: CS=0x1B, SS=0x23 |
| 8 | SFMASK | IF(bit 9) | SYSCALL 进入后 IF=0，中断自动关闭，与 interrupt gate 行为一致 |
| 9 | 阻塞 syscall 返回 | SYSRET 路径也适用于被 reschedule 的进程 | `schedule()` → `switch_to` 保存完整调用链，恢复后自然回到 syscall_fast_entry 的 SYSRET 部分 |
| 10 | 调用约定 | RAX=syscall#, RDI/RSI/RDX/R10/R8/R9 = args | Linux x86-64 syscall 约定；R10 替代 RCX（RCX 被 SYSCALL 用作 return RIP） |

## MSR 设置（`setup_syscall()`，kernel/trap.cc）

```c
// STAR: SYSCALL CS=0x08/SS=0x10, SYSRET64 CS=0x2B/SS=0x23
uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x18 << 48);
wrmsr(MSR_STAR, star);

// LSTAR: SYSCALL 入口地址
wrmsr(MSR_LSTAR, (uint64_t)syscall_fast_entry);

// CSTAR: 32 位兼容入口（未使用，设为 0）
wrmsr(MSR_CSTAR, 0);

// SFMASK: 清除 IF(bit 9)
wrmsr(MSR_SFMASK, (1 << 9));

// EFER.SCE: 启用 SYSCALL/SYSRET
uint64_t efer = rdmsr(MSR_EFER);
efer |= EFER_SCE;
wrmsr(MSR_EFER, efer);
```

## syscall_fast_entry（arch/x64/trapentry.S）

SYSCALL 指令自动做：RCX = RIP, R11 = RFLAGS，然后加载 CS/SS（来自 STAR）。不自动 push 任何内容到栈、不自动切换栈。

```asm
syscall_fast_entry:
    movq %rsp, %r10           # 保存用户 RSP（R10 是 scratch）
    swapgs                    # GS_BASE ↔ KERNEL_GS_BASE → GS_BASE = cpu_local*
    movq %gs:32, %rsp         # 切内核栈（cpu_local.tss_rsp0，偏移 32）

    subq $176, %rsp           # 在内核栈构建 trapframe（176 bytes）

    # CPU 自动保存的字段
    movq $0x23, 168(%rsp)     # ss = USER_DS
    movq %r10, 160(%rsp)      # rsp = 用户 RSP
    movq %r11, 152(%rsp)      # rflags（SYSCALL 保存到 R11）
    movq $0x2B, 144(%rsp)     # cs = USER_CS
    movq %rcx, 136(%rsp)      # rip（SYSCALL 保存到 RCX）
    movq $0, 128(%rsp)        # err_code = 0
    movq $128, 120(%rsp)      # trapno = 128

    # 保存所有 16 个 GP 寄存器
    movq %r15, 0(%rsp) ... movq %rax, 112(%rsp)

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    movq %rsp, %rdi
    call syscall_dispatch
```

## SYSRET 返回（同文件，syscall_dispatch 返回后）

阻塞 syscall（如 sys_getc）也走此路径：被 reschedule 后恢复时，switch_to 返回到 syscall_dispatch 调用之后，继续执行 SYSRET 恢复逻辑。

```asm
    # 恢复 GP 寄存器（除 RCX/R11，它们用于 SYSRET）
    movq 112(%rsp), %rax ... movq 0(%rsp), %r15

    movq 136(%rsp), %rcx      # 用户 RIP（SYSRET 加载 RCX → RIP）
    movq 152(%rsp), %r11      # 用户 RFLAGS（SYSRET 加载 R11 → RFLAGS）
    movq 160(%rsp), %rsp      # 用户 RSP（必须在 swapgs 之前加载）

    swapgs                    # 换回用户 GS base
    sysretq                   # RCX→RIP, R11→RFLAGS, CS/SS 从 STAR 加载
```

## syscall_dispatch（kernel/trap.cc）

当前 20 个系统调用（编号 0-19 连续无空洞），分发表方式：

```c
#define NR_SYSCALL 20
static syscall_fn_t syscall_table[NR_SYSCALL] = {
    sys_getpid,       // 0
    sys_yield,        // 1
    sys_wait,         // 2
    sys_notify,       // 3
    sys_irq_bind,     // 4
    sys_exit,         // 5
    sys_waitpid,      // 6
    sys_spawn,        // 7
    sys_mmap,         // 8
    sys_munmap,       // 9
    sys_serial_write, // 10
    sys_fb_info,      // 11
    sys_shm_create,   // 12
    sys_shm_attach,   // 13
    sys_pipe,         // 14
    sys_write,        // 15
    sys_read,         // 16
    sys_close,        // 17
    sys_load_dev,     // 18
    sys_lookup_dev,   // 19
};

void syscall_dispatch(trapframe_t *tf) {
    if (tf->rax < NR_SYSCALL) {
        tf->rax = syscall_table[tf->rax](
            tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8);
    } else {
        tf->rax = (uint64_t)-1;
    }
}
```

参数从 trapframe 提取：RDI/RSI/RDX/R10/R8（Linux 约定），返回值写回 tf->rax。

## 与 __alltraps/__trapret 的差异

| 方面 | __alltraps/__trapret | syscall_fast_entry/SYSRET |
|------|---------------------|--------------------------|
| 触发方式 | 硬件中断/异常 → IDT | SYSCALL 指令 → MSR LSTAR |
| CPU 自动保存 | SS, RSP, RFLAGS, CS, RIP, [err] | RCX=RIP, R11=RFLAGS（无栈操作） |
| 栈切换 | CPU 自动切到 TSS RSP0 | 手动 swapgs + %gs:32 读 tss_rsp0 |
| swapgs | 条件性（检查 CS 判断来源） | 无条件（SYSCALL 只从 ring 3 来） |
| trapframe 构建 | push 寄存器到栈 | subq + movq 写入固定偏移 |
| 返回 | iretq（自动 pop SS/RSP/RFLAGS/CS/RIP） | sysretq（RCX→RIP, R11→RFLAGS, CS/SS 从 STAR） |

## 用户态封装（arch/x64/utils.h）

```c
// 当前编号（详见 common/syscall.h）
#define SYS_GETPID       0
#define SYS_YIELD        1
#define SYS_WAIT         2
#define SYS_NOTIFY       3
#define SYS_IRQ_BIND     4
// ... 完整列表见 common/syscall.h

static inline uint64_t __syscall0(uint64_t n) {
    uint64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}
// syscall1..syscall5 类似，传入 1-5 个参数
```

## 实现顺序

```
1. setup_syscall() MSR 设置
   → 验证: wrmsr 不触发 #GP，EFER.SCE=1

2. syscall_fast_entry 汇编
   → 验证: SYSCALL 进入内核，trapframe 布局正确

3. SYSRET 返回汇编
   → 验证: sysretq 正确返回用户态

4. syscall_dispatch + syscall_table
   → 验证: sys_putc/sys_getpid 通过 SYSCALL 正常工作

5. 移除 int 0x80 路径
   → 验证: 用户态只使用 SYSCALL 指令
```