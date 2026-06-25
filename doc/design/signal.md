# 信号机制设计

## 动机

当前系统信号实现为简化版：`deliver_signal` 只保存 rip/rsp/rflags 到 PCB（3 字段），不支持嵌套信号、SA_SIGINFO、EINTR、内核异常翻译。Ctrl+C 作为 0x03 字节写入 pipe，不产生 SIGINT。本阶段重构为 Linux 标准信号机制，为 PTY（Ctrl-C → SIGINT）和 Wayland 验收提供基础。

## 设计范围

| 特性 | 状态 | 说明 |
|------|------|------|
| sigframe 栈帧投递 | ✅ 本阶段 | pretcode + siginfo_t + ucontext_t，保存全部 GP 寄存器 |
| SA_SIGINFO | ✅ 本阶段 | handler(int, siginfo_t*, ucontext_t*) |
| EINTR | ✅ 本阶段 | 阻塞 syscall 被信号中断返回 -EINTR |
| blocked mask 内部机制 | ✅ 本阶段 | handler 执行期间 block sa_mask + 当前信号，sigreturn 恢复 |
| 内核异常翻译 | ✅ 本阶段 | #PF→SIGSEGV, #GP→SIGSEGV, #UD→SIGILL, #DE→SIGFPE |
| force_sig | ✅ 本阶段 | 同步信号绕过 SIG_IGN（避免故障指令无限重入） |
| SIGCHLD | ✅ 本阶段 | 替代 sys_exit 中 RECV_NOTIFY 子进程退出通知 |
| sys_sigprocmask | ❌ 不做 | blocked 初始为 0，仅内核在信号投递/sigreturn 时修改 |
| SA_RESTART | ❌ 不做 | EINTR 后由用户态 libc 或应用决定是否重启 |
| sigaltstack | ❌ 不做 | handler 在用户栈上执行 |
| SIGPIPE | ❌ 不做 | 保持现有 -EPIPE 返回值 |
| 作业控制 (SIGTSTP/Ctrl+Z) | ❌ 不做 | 需要 session/controlling terminal |

## 数据结构

### sigset_t

```c
typedef uint64_t sigset_t;  // 64 个信号，1 个 uint64_t 足够
```

### sigaction（跟 Linux 结构，去掉 sa_restorer — vdso 方案不需要）

```c
struct sigaction {
    union {
        void   (*sa_handler)(int);
        void   (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;       // handler 执行期间额外阻塞的信号集
    int      sa_flags;      // SA_SIGINFO, SA_RESTART, SA_NODEFER 等
    void   (*sa_restorer)(void);  // Linux 历史遗留，内核忽略
};
```

SIG_DFL = 0, SIG_IGN = 1，handler 地址 > 1。

### 信号编号

与 Linux x86-64 对齐（common/signal.h 已定义 SIGHUP..SIGTSTP 1-20，NSIG=32）。

本阶段活跃信号的 default action：

| 信号 | default action | 用途 |
|------|---------------|------|
| SIGINT (2) | Terminate | Ctrl+C 终止前台进程 |
| SIGILL (4) | Terminate | 非法指令 |
| SIGABRT (6) | Terminate | abort() |
| SIGFPE (8) | Terminate | 算术异常 |
| SIGKILL (9) | Terminate（不可捕获/不可屏蔽） | 强制终止 |
| SIGSEGV (11) | Terminate | 段错误/页错误 |
| SIGTERM (15) | Terminate | 优雅终止 |
| SIGCHLD (17) | Ignore | 子进程退出通知 |
| SIGSTOP (19) | Stop（暂不实现，走 Terminate） | 暂停进程 |

其余信号 default action 按 Linux 惯例：Core 或 Terminate。

### rt_sigframe（栈帧结构，跟 Linux x86-64）

```c
// 完整 sigcontext（保存所有 GP 寄存器 + 段寄存器 + cr2）
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, ss, ds, es, fs, gs;
    uint64_t fs_base, gs_base;
    uint64_t cr2;              // #PF 地址（SIGSEGV 时 si_addr 来源）
    uint64_t _pad1;            // 对齐
};

struct ucontext_t {
    uint64_t          uc_flags;    // 目前清零
    struct ucontext_t *uc_link;    // NULL（无 sigaltstack 链）
    sigset_t          uc_sigmask;  // handler 执行期间的阻塞信号集
    struct sigcontext  uc_mcontext;
};

struct siginfo_t {
    int si_signo;
    int si_errno;    // 清零
    int si_code;     // SI_USER / SI_KERNEL / SI_QUEUE
    union {
        struct { pid_t si_pid; int si_uid; } _kill;   // kill 来源
        void *si_addr;                                 // SIGSEGV 崩溃地址
    } _sifields;
    // 填充到 128 字节（与 Linux siginfo_t 大小对齐）
    char _pad[128 - 3*sizeof(int) - sizeof(union)];
};

struct rt_sigframe {
    uint64_t pretcode;           // 返回地址 = SIG_TRAMPOLINE_ADDR
    struct siginfo_t info;
    struct ucontext_t uc;
};
```

### proc_t signal_state（重构）

```c
struct signal_state {
    uint64_t      pending;           // bitmask: pending signals
    sigset_t      blocked;           // 当前阻塞信号集（内核在投递/sigreturn 时修改）
    struct sigaction action[NSIG];   // per-signal handler 注册
};
```

删除旧字段：`have_handler`, `saved_rip`, `saved_rsp`, `saved_rflags`。这些信息现在在栈帧 sigframe 里，不再需要在 PCB 中保存。

### vdso sigreturn trampoline

第一步（当前）：单页 vdso，固定映射到 `SIG_TRAMPOLINE_ADDR = 0x50000000`，每个进程创建时映射同一物理页。内容：

```asm
; mov rax, SYS_SIGRETURN (49); syscall
; 编码: 48 C7 C0 31 00 00 00  0F 05
sig_trampoline:
    mov rax, SYS_SIGRETURN
    syscall
```

第二步（未来）：扩展为完整 vdso ELF（含 clock_gettime 等符号），映射地址随机化，通过 AT_SYSINFO_EHDR 传递。当前方案不偏离目标 — 机制相同（内核映射代码页到用户空间），只是内容少、地址固定。

## syscall 接口

### sys_kill(pid_t pid, int sig) — syscall 46

```
pid > 0:  发送给指定进程
pid == 0: 发送给同进程组（预留）
pid < 0:  发送给进程组 -pid（预留）
sig == 0: 存在性检查，不发信号
```

实现：
1. 验证 pid/sig
2. `pending |= (1ULL << sig)`（atomic）
3. SIGKILL/SIGSTOP 不可阻塞（即使 blocked 中有对应位，仍设置 pending）
4. 如果目标进程 BLOCKED 且有 pending & ~blocked → 唤醒（EINTR 路径）
5. 返回 0

### sys_sigaction(int sig, const sigaction *act, sigaction *oldact) — syscall 47

实现（跟 Linux）：
1. 验证 sig（SIGKILL/SIGSTOP → -EINVAL）
2. oldact 非 NULL → 拷贝当前 action 到用户空间（CR3 切换）
3. act 非 NULL → 从用户空间拷贝新 action（CR3 切换）
4. 清除该信号 pending bit（POSIX：注册 handler 时丢弃未决信号）

### sys_sigreturn() — syscall 48

信号 handler 返回后，trampoline 执行 `syscall(SYS_SIGRETURN)`。

实现：
1. 从当前 trapframe 的 rsp 定位用户栈上的 sigframe
2. CR3 切换，从用户栈读取 sigframe
3. 恢复 sigframe.uc.uc_mcontext 中的全部 GP 寄存器到 trapframe
4. 恢复 rip/rsp/rflags/cs/ss 到 trapframe
5. 恢复 blocked mask = sigframe.uc.uc_sigmask
6. 返回 0（trapframe 已修改，SYSRET/IRET 回到被中断代码）

不再使用 `have_handler` 标记或 PCB saved_* 字段。

## 信号投递路径

### 投递时机：trap return 前

`check_pending_signals(tf)` 在 `__trapret` 和 `syscall_fast_entry` 返回用户态前调用（已实现）。循环检查：

```c
void check_pending_signals(trapframe_t *tf) {
    if (tf->cs != USER_CS) return;       // 只在返回用户态时投递
    proc_t *proc = current_proc;

    while (1) {
        uint64_t pending = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
        uint64_t deliverable = pending & ~proc->sig.blocked;
        if (!deliverable) return;

        int sig = __builtin_ctzll(deliverable);  // 最低编号优先
        __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);

        sigaction_t *sa = &proc->sig.action[sig];

        if (sa->sa_handler == SIG_DFL) {
            // default action
            ...  // SIGCHLD→ignore, SIGKILL/SIGINT/SIGTERM/SIGSEGV→terminate
        } else if (sa->sa_handler == SIG_IGN) {
            continue;  // 跳过，检查下一个
        } else {
            deliver_signal(proc, tf, sig, sa);
            return;    // tf 已修改，返回用户态执行 handler
        }
    }
}
```

### deliver_signal — 构造 sigframe 推到用户栈

```c
void deliver_signal(proc_t *proc, trapframe_t *tf, int sig, sigaction_t *sa) {
    // 1. 构造 sigframe
    struct rt_sigframe frame = {0};
    frame.pretcode = SIG_TRAMPOLINE_ADDR;

    // siginfo
    frame.info.si_signo = sig;
    frame.info.si_code = SI_KERNEL;  // 默认（force_sig 时设置具体来源）

    // sigcontext — 从 trapframe 填充
    frame.uc.uc_mcontext.r8  = tf->r8;
    frame.uc.uc_mcontext.r9  = tf->r9;
    ... // 全部 16 个 GP 寄存器 + rip/rsp/rflags/cs/ss

    // 保存当前 blocked mask 到 sigframe
    frame.uc.uc_sigmask = proc->sig.blocked;

    // 2. 更新 blocked: handler 执行期间阻塞 sa_mask + 当前信号
    proc->sig.blocked |= sa->sa_mask | (1ULL << sig);
    // SIGKILL/SIGSTOP 不可阻塞，但 sa_mask 中不应包含它们（sigaction 拒绝注册）

    // 3. 推 sigframe 到用户栈
    uint64_t user_rsp = tf->rsp - sizeof(struct rt_sigframe);
    // 对齐到 16 字节
    user_rsp &= ~0xFULL;

    // CR3 切换，拷贝 sigframe 到用户栈
    uint64_t saved_cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"(proc->cr3) : "memory");
    memcpy((void *)user_rsp, &frame, sizeof(struct rt_sigframe));
    __asm__ volatile("movq %0, %%cr3" :: "r"(saved_cr3) : "memory");

    // 4. 修改 trapframe
    tf->rip = (uint64_t)sa->sa_handler;
    tf->rsp = user_rsp;

    // handler 参数
    if (sa->sa_flags & SA_SIGINFO) {
        tf->rdi = (uint64_t)sig;
        tf->rsi = (uint64_t)(user_rsp + offsetof(rt_sigframe, info));
        tf->rdx = (uint64_t)(user_rsp + offsetof(rt_sigframe, uc));
    } else {
        tf->rdi = (uint64_t)sig;
    }
}
```

### 用户态执行流程

```
trap return → 跳到 handler(rdi=sig, rsi=&siginfo, rdx=&ucontext)
handler 执行
handler ret → pop pretcode → 跳到 SIG_TRAMPOLINE_ADDR
trampoline: mov rax, SYS_SIGRETURN; syscall
→ sys_sigreturn 读取栈帧，恢复全部寄存器 + blocked mask
→ SYSRET/IRET 回到被中断代码
```

## EINTR

### 原理

阻塞 syscall 被信号中断时返回 `-EINTR`。进程回到用户态后 `check_pending_signals` 投递信号。

### 实现位置（跟 Linux）

每个阻塞 syscall 在 `schedule()` 返回后检查 pending signals：

```c
// 在 schedule() 返回后
if (proc->sig.pending & ~proc->sig.blocked) {
    return (uint64_t)-EINTR;
}
```

需要加 EINTR 检查的阻塞 syscall：

| syscall | 阻塞场景 |
|---------|---------|
| sys_recv | WAIT_RECV（IRQ/REQ/notify 等待） |
| sys_read (pipe) | WAIT_PIPE（pipe 读等待） |
| sys_waitpid | WAIT_CHILD（子进程退出等待） |
| sys_req | WAIT_REQ_REPLY（REQ 等待回复） |
| sys_msg | WAIT_MSG_REPLY（MSG 等待回复） |
| sys_poll | WAIT_POLL（poll 等待事件） |
| sys_accept | WAIT_RECV（socket accept 等待） |

注意：**不清除 pending bit**。EINTR 只让进程退出阻塞，信号由 `check_pending_signals` 正常投递。

### sys_recv EINTR 唤醒

sys_kill 设置 pending bit 后，如果目标进程 BLOCKED，需要唤醒使其从 schedule() 返回并检查 EINTR。唤醒逻辑：

```c
// sys_kill 中，设置 pending 后:
if (target->state == BLOCKED) {
    // 唤醒目标进程（同 wake_process 但不入队 RECV_NOTIFY）
    spin_lock(&cpu_locals[target_cpu].scheduler_lock);
    if (target->pid == pid && target->state == BLOCKED) {
        target->state = READY;
        target->wait_event = WAIT_NONE;
        list_push_back(&cpu_locals[target_cpu].run_queue, &target->run_node);
        cpu_locals[target_cpu].run_count++;
    }
    spin_unlock(&cpu_locals[target_cpu].scheduler_lock);
}
```

## 内核异常翻译

### 翻译表

| 异常向量 | 信号 | siginfo.si_code |
|----------|------|-----------------|
| 0 (#DE divide error) | SIGFPE | FPE_INTDIV |
| 6 (#UD illegal opcode) | SIGILL | ILL_ILLOPC |
| 13 (#GP general protection) | SIGSEGV | SEGV_MAPERR |
| 14 (#PF page fault) | SIGSEGV | 根据错误码: present→SEGV_ACCERR, not-present→SEGV_MAPERR |

### trap_dispatch 修改

当前用户态异常 → 打印寄存器 + `sys_exit(-1)` 直接杀进程。

修改为：

```c
// 用户态异常处理
if (tf->cs == USER_CS) {
    int sig = exception_to_signal(tf->trapno);  // 翻译表
    force_sig(proc, sig, tf);                    // 强制投递
    return;                                      // 不杀进程，让 check_pending_signals 处理
}
```

### force_sig

同步信号（SIGSEGV/SIGILL/SIGFPE）必须投递，即使 SIG_IGN：

1. 设置 pending bit
2. 从 blocked 中清除该信号位（确保不被阻塞）
3. 设置 siginfo：si_signo=sig, si_code=具体值, si_addr=cr2(#PF)
4. 如果 sa_handler == SIG_IGN → 强制改为 SIG_DFL（同步信号忽略会导致故障指令无限重入）

SIGKILL 也走 force_sig（不可阻塞、不可忽略），但 si_code = SI_KERNEL。

## SIGCHLD 替代 RECV_NOTIFY

### 当前路径

```
子进程 sys_exit
  → scheduler_lock 设 ZOMBIE
  → 父进程 recv 队列入队 RECV_NOTIFY 消息
  → 唤醒 WAIT_CHILD 或 WAIT_RECV
```

### 迁移后路径

```
子进程 sys_exit
  → scheduler_lock 设 ZOMBIE
  → 父进程 sig.pending |= (1ULL << SIGCHLD)      ← 替代 RECV_NOTIFY
  → 如果父进程 WAIT_CHILD → 唤醒                  ← 保持（waitpid 仍立即返回）
  → 不再入队 RECV_NOTIFY（仅 exit 场景移除，其他 notify 保持不变）
```

waitpid 唤醒不依赖 SIGCHLD handler。即使 SIGCHLD 被 ignore，`sys_waitpid(WAIT_CHILD)` 在子进程 ZOMBIE 时仍被唤醒。

### RECV_NOTIFY 用途保留

| 用途 | 迁移 |
|------|------|
| 子进程 exit → 通知父进程 | SIGCHLD 替代 |
| 驱动间 notify（kbd→terminal） | 保持 RECV_NOTIFY |
| fs_driver block_async 完成回调 | 保持 RECV_NOTIFY |

## 新增/修改的文件

| 文件 | 变更 |
|------|------|
| `common/signal.h` | 重构 sigaction（加 union + sa_restorer），定义 rt_sigframe/sigcontext/siginfo_t/ucontext_t |
| `common/syscall.h` | SYS_KILL/SYS_SIGACTION/SYS_SIGRETURN 号 + 用户态封装（已有） |
| `kernel/proc.h` | 重构 signal_state：删除 have_handler/saved_rip/saved_rsp/saved_rflags |
| `kernel/proc.c` | proc_init 清零 sig，process_create 映射 trampoline 页（已有） |
| `kernel/trap.c` | 重构 deliver_signal（sigframe 方案），重构 sys_sigreturn（读栈帧），重构 check_pending_signals（blocked mask），修改 trap_dispatch（force_sig 替代 sys_exit），修改 sys_exit（SIGCHLD 替代 RECV_NOTIFY），所有阻塞 syscall 加 EINTR 检查 |
| `arch/x64/trapentry.S` | check_pending_signals 调用点（已有） |
| `user/include/signal.h` | SA_SIGINFO 等常量，kill/sigaction/signal 声明 |
| `user/lib/signal.cc` | kill/sigaction/sigreturn 用户态封装（已有） |

## 验证方法

1. **SIGTERM 终止进程**：`kill(pid, SIGTERM)` → 进程退出
2. **SIGINT + EINTR**：进程在 sys_recv 阻塞 → Terminal kill(SIGINT) → sys_recv 返回 -EINTR → 进程退出
3. **SIGSEGV 捕获**：进程注册 SIGSEGV handler → 触发空指针 → handler 执行（si_addr 给出崩溃地址）→ sigreturn → 进程继续或退出
4. **SA_SIGINFO**：handler(int, siginfo_t*, ucontext_t*) → si_signo/si_code/si_addr 正确
5. **SIGCHLD + waitpid**：子进程 exit → 父进程 waitpid 返回 + SIGCHLD pending
6. **嵌套信号**：handler A 执行中被信号 B 中断 → handler B 执行 → sigreturn B → 继续 handler A → sigreturn A → 回到原始代码
7. **SIG_IGN**：`sigaction(SIGTERM, SIG_IGN)` → `kill(pid, SIGTERM)` → 无效果
8. **force_sig**：进程 SIG_IGN(SIGSEGV) → 触发 #PF → 强制 SIG_DFL → 进程被杀（不会无限重入）

## 未来扩展

| 特性 | 依赖 | 说明 |
|------|------|------|
| sys_sigprocmask | blocked mask | 用户显式控制信号阻塞 |
| SA_RESTART | EINTR | libc 自动重启被中断 syscall |
| SIGPIPE | EINTR + socket | 写已关闭 socket 时 kill 进程 |
| 作业控制 | session + PTY | SIGTSTP/Ctrl+Z, fg/bg |
| sigaltstack | 信号栈 | handler 在独立栈执行（栈溢出时有用） |
| vdso ELF | trampoline 页 | 扩展为完整 vdso（clock_gettime 等） |
| real-time signal | 信号队列 | 32-64 号信号排队不丢 |
| sys_kill pgid | 进程组 | kill(-pgid, sig) 投递到整组 |
