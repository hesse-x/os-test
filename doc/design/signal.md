# 信号机制设计

## 动机

当前系统无任何信号基础设施。子进程退出通知走 `RECV_NOTIFY` + `proc_t::wait_event=WAIT_CHILD` 的自定义路径，Ctrl+C 按键作为 0x03 字节写入 pipe（不产生中断）。为支持 Wayland 验收（Ctrl+C 终止前台进程）和未来 POSIX 兼容（`sigaction`/`kill`/SIGCHLD），引入信号机制。

## 设计范围（Level 2）

| 特性 | 包含 | 不包含 |
|------|------|--------|
| `sys_kill(pid, sig)` | ✅ pending bits 设置 | — |
| `sys_sigaction(sig, act, oldact)` | ✅ 注册/查询用户 handler | — |
| `sys_sigreturn()` | ✅ handler 返回后恢复 | — |
| 信号投递 | ✅ iret 前检查 pending signals | — |
| Default action | ✅ SIGINT/SIGTERM→exit, SIGCHLD→ignore | — |
| SIGCHLD | ✅ 替代 RECV_NOTIFY 子进程退出通知 | — |
| sigprocmask | ⏸ 预留接口，返回 -ENOSYS | ❌ 本阶段不做 |
| EINTR | ⏸ 预留 | ❌ 本阶段不做 |
| SIGPIPE | ⏸ 保持现有 -EPIPE 返回值 | ❌ 本阶段不做 |
| 作业控制 (SIGTSTP/Ctrl+Z) | ❌ 后续 | ❌ 本阶段不做 |

## 数据结构

### proc_t 扩展

```c
// kernel/proc.h — proc_t 新增字段
#define NSIG         32     // 信号数量（与 Linux 对齐）
#define SIG_BLOCK    0      // sigprocmask 预留
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

struct sigaction {
    void   (*sa_handler)(int);  // SIG_DFL=0, SIG_IGN=1, or user fn
    uint64_t sa_mask;           // sigprocmask 预留
    int      sa_flags;          // SA_RESTART 等预留
};

// per-process 信号状态
struct signal_state {
    uint64_t pending;           // bitmask: pending signals (bit N = signal N)
    struct sigaction action[NSIG];  // 每信号一个 handler
    // 以下字段为 sigprocmask/EINTR 预留（本阶段不启用）
    uint64_t blocked;           // blocked mask (预留)
    int      have_handler;      // 是否有用户态 handler 待调起
    // sigreturn 恢复上下文
    uint64_t saved_rip;
    uint64_t saved_rsp;
    uint64_t saved_rflags;
};
```

### 信号编号

```c
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
// ... 其余编号与 Linux 对齐
```

本阶段活跃的信号：

| 信号 | default action | 用途 |
|------|---------------|------|
| SIGINT (2) | Terminate | Ctrl+C 终止前台进程 |
| SIGTERM (15) | Terminate | compositor 优雅终止 client |
| SIGCHLD (17) | Ignore | 子进程退出通知（waitpid 仍正常工作） |

其余信号 default action 统一为 Terminate（`SIG_DFL`）或 Ignore（按 Linux 惯例）。`SIGKILL`（不可捕获/不可屏蔽）的 default action 为 Terminate。

### 常量

```c
#define SIG_DFL ((void (*)(int))0)   // default action
#define SIG_IGN ((void (*)(int))1)   // ignore signal
```

## syscall 接口

### `sys_kill(pid_t pid, int sig)`

```c
int sys_kill(pid_t pid, int sig);
// pid > 0: 发送给指定进程
// pid == 0: 发送给同进程组（预留，当前同组只有自己）
// pid == -1: 发送给所有进程（预留）
```

实现：
1. 检查 `pid` 合法性（`procs_lock` + `scheduler_lock` 读 state，跳过已退出/僵尸）
2. 检查 `sig` 范围（1 ≤ sig < NSIG）
3. `pending |= (1ULL << sig)`（`__atomic_or_fetch`）
4. 如果目标进程 `state == BLOCKED`（且非 WAIT_CHILD 的特殊处理），检查是否需要唤醒
   - 本阶段：不提前唤醒（Wake-on-signal 属于 EINTR 范畴，暂不做）
   - WAIT_CHILD 场景：SIGCHLD 的 pending bit 由 sys_exit 在子进程退出时设置，父进程在下次 iret 前检查
5. 返回 0

### `sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)`

```c
int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
```

实现：
1. 检查 sig 范围
2. 如果 `sig == SIGKILL` → 返回 `-EINVAL`（不可捕获）
3. `oldact` 非 NULL → 拷贝当前 action 到用户空间
4. `act` 非 NULL → 写入当前 action（`memcpy` from user）
5. 清除该信号的 pending bit（POSIX 语义：注册 handler 时丢弃未决信号）
6. 返回 0

### `sys_sigreturn(void)`

```c
int sys_sigreturn(void);
```

信号 handler 执行完毕后调用，从内核恢复被信号中断的上下文。

```asm
; 用户态 trampoline（在信号 handler 返回后自动执行）
sig_trampoline:
    mov rax, SYS_SIGRETURN
    syscall
    ; 不返回
```

实现：
1. 从当前进程 `signal_state` 恢复 `saved_rip/rsp/rflags`
2. 清理 `have_handler` 标记
3. iretq 回到被中断的执行点

## 信号投递路径

### 投递时机：iret 前

信号投递在进程从内核态返回用户态时进行（这是唯一正确的时机——内核态执行不能被信号中断）：

```c
// arch/x64/trapentry.S — __trapret 前插入
// 或 trap_dispatch 末尾，iret 前

void check_pending_signals(proc_t *proc, trapframe_t *tf) {
    // 如果正在内核态执行（tf->cs != 0x2B），不投递信号
    if (tf->cs != USER_CS) return;

    uint64_t pending = __atomic_load_n(&proc->sig.pending, __ATOMIC_ACQUIRE);
    if (!pending) return;

    // 找最低 pending 信号（Linux 惯例）
    int sig = __builtin_ctzll(pending);
    if (sig == 0 || sig >= NSIG) return;

    // 清除 pending bit
    __atomic_and_fetch(&proc->sig.pending, ~(1ULL << sig), __ATOMIC_RELEASE);

    void (*handler)(int) = proc->sig.action[sig].sa_handler;

    if (handler == SIG_DFL) {
        // default action
        switch (sig) {
        case SIGCHLD:
            // ignore — waitpid 仍然可以回收僵尸进程
            break;
        case SIGINT:
        case SIGTERM:
        default:
            // Terminate — 直接 exit
            sys_exit(-1);  // 不返回
            break;
        }
    } else if (handler == SIG_IGN) {
        // ignore — 什么都不做
    } else {
        // 用户注册了 handler → 调起 trampoline
        deliver_signal_to_userspace(proc, tf, sig, handler);
    }
}
```

### 用户态 handler 调起（trampoline）

```c
void deliver_signal_to_userspace(proc_t *proc, trapframe_t *tf, int sig, void (*handler)(int)) {
    // 1. 保存当前执行上下文到 signal_state
    proc->sig.saved_rip = tf->rip;
    proc->sig.saved_rsp = tf->rsp;
    proc->sig.saved_rflags = tf->rflags;

    // 2. 在用户栈上构造一个假栈帧，使得:
    //    - handler 返回后执行 sig_trampoline
    //    - sig_trampoline 调用 sys_sigreturn 恢复上下文
    //
    // 栈布局（从高到低）:
    //   [sig_trampoline addr]   ← 当 handler ret 时跳到这里
    //   [arg: sig]              ← 传给 handler 的参数
    //
    // 实际做法：tf->rip = handler, tf->rdi = sig
    // 在用户栈 push 一个返回地址 = sig_trampoline
    uint64_t user_rsp = tf->rsp;

    // 确保用户栈可写（如果栈顶页未映射，需要 map）
    user_rsp -= 8;
    copy_to_user(proc, user_rsp, &sig_trampoline_addr, 8);  // 返回地址

    // 3. 修改 trapframe，使 iret 后执行 handler
    tf->rip = (uint64_t)handler;
    tf->rdi = sig;
    tf->rsp = user_rsp;

    // sig_trampoline 位于用户可访问的固定地址
    // （在内核 ELF 中预先放置，每个进程启动时映射一页）
}
```

### sig_trampoline 位置

trampoline 代码位于内核固定地址映射到每个用户进程的一页只读代码页中（类似 Linux 的 `[vdso]` 风格，或直接在固定地址 `0xFFFFFFFFFF600000` 放置）。

```asm
; 固定地址 0xFFFFFFFFFF600000
sig_trampoline:
    mov rax, SYS_SIGRETURN
    syscall
    ; 不会执行到这里
```

所有进程共享同一物理页（只读，不可执行标记特例放行，或放在内核代码段已有的用户可读区域）。

简化方案：trampoline 直接放在 `user/lib/start.cc` 中作为 `_start` 附近的汇编函数，每个进程地址空间自带。这样不需要额外映射。

```c
// user/lib/signal_trampoline.S
.section .text
.globl __sig_trampoline
__sig_trampoline:
    mov $SYS_SIGRETURN, %rax
    syscall
```

进程只需知道 `__sig_trampoline` 在 ELF 中的地址，handler 返回后自动跳到这里。

## SIGCHLD 替代 RECV_NOTIFY

### 当前路径

```
子进程 sys_exit
  → scheduler_lock[child_cpu] 设 ZOMBIE
  → 父进程 recv 队列入队 RECV_NOTIFY 消息
  → 唤醒 WAIT_CHILD 或 WAIT_RECV
```

### 迁移后路径

```
子进程 sys_exit
  → scheduler_lock[child_cpu] 设 ZOMBIE
  → 父进程 sig.pending |= SIGCHLD      ← 新增，替代 RECV_NOTIFY
  → 如果父进程 WAIT_CHILD → 唤醒      ← 保持（waitpid 仍立即返回）
  → 父进程下次 iret → 检查 pending
       → SIGCHLD default=ignore → 无动作
       → (后续 EINTR 支持时) → 如果父进程在 sys_recv 中 → -EINTR
```

**关键点：** waitpid 的唤醒不依赖 SIGCHLD handler。即使 SIGCHLD 被 ignore，`sys_waitpid(BLOCKED(WAIT_CHILD))` 仍然在子进程 ZOMBIE 时被 `wake_process` 唤醒并回收。SIGCHLD 只是额外的通知机制。

### 现有 RECV_NOTIFY 消息类型

检查现有代码中 RECV_NOTIFY 的其他用途：

| 用途 | 迁移方案 |
|------|---------|
| 子进程 exit → 通知父进程 | SIGCHLD 替代 ✅ |
| 驱动间 notify（kbd→terminal） | 保持 RECV_NOTIFY 不变（不是进程退出通知） |
| fs_driver block_async 完成回调 | 保持 RECV_NOTIFY 不变（不是进程退出通知） |

→ `sys_exit` 中条件移除 RECV_NOTIFY 入队（仅在 `parent_pid > 0` 且 exit 场景），改为 `sig.pending |= SIGCHLD`。

## 调用路径集成

### 新增/修改的文件

| 文件 | 变更 |
|------|------|
| `common/syscall.h` | 新增 `SYS_KILL`、`SYS_SIGACTION`、`SYS_SIGRETURN` 号 + 用户态封装 |
| `kernel/proc.h` | `proc_t` 新增 `struct signal_state sig` |
| `kernel/proc.cc` | `proc_init` 中清零 sig 字段 |
| `kernel/trap.cc` | 新增 `sys_kill`/`sys_sigaction`/`sys_sigreturn` 实现；修改 `sys_exit`（SIGCHLD 替代 RECV_NOTIFY）；`trap_dispatch` 末尾调用 `check_pending_signals` |
| `arch/x64/trapentry.S` | `__trapret` 前插入 `check_pending_signals` 调用点 |
| `user/include/signal.h` | **新增** — `sigaction`/`kill`/`signal` 声明 + 信号编号定义 |
| `user/lib/signal_trampoline.S` | **新增** — `__sig_trampoline` 汇编入口 |
| `user/lib/CMakeLists.txt` | 添加 `signal_trampoline.S` 到 libc 编译 |

### syscall 编号

```c
#define SYS_KILL       47    // 新增
#define SYS_SIGACTION  48    // 新增
#define SYS_SIGRETURN  49    // 新增
// NR_SYSCALL = 50
```

## 验证方法

1. **SIGTERM 终止进程**：进程 A `kill(pid, SIGTERM)` → 进程 B 退出（exit code = -1）
2. **SIGINT (Ctrl+C)**：terminal 读到 Ctrl+C keycode → `kill(shell_pid, SIGINT)` → shell 退出 → init 回收 → terminal 重启 shell
3. **SIGCHLD + waitpid**：子进程 exit → 父进程 `waitpid` 立即返回（不受 SIGCHLD ignore 影响）
4. **sigaction 注册 handler**：进程注册 SIGTERM handler → 收到信号 → handler 执行 → `sigreturn` → 恢复执行
5. **SIG_IGN**：进程 `sigaction(SIGTERM, SIG_IGN)` → `kill(pid, SIGTERM)` → 无效果

## 未来扩展（本阶段不做）

| 特性 | 依赖 | 说明 |
|------|------|------|
| `sigprocmask` | 信号阻塞掩码 | 保护临界区不被信号中断，libwayland 可能用到 |
| `EINTR` | 所有阻塞 syscall | 被信号中断时返回 -EINTR，影响 recv/pipe/poll/accept 等全部阻塞路径 |
| `SIGPIPE` | EINTR + socket | 写已关闭 socket 时自动 kill 进程（目前返回 -EPIPE） |
| 作业控制 (`SIGTSTP`/`SIGCONT`) | 进程组 + 会话 | Ctrl+Z 暂停、fg/bg 恢复 |
| `SA_RESTART` | EINTR | 自动重启被信号中断的 syscall |
| `sigaltstack` | 信号栈 | 在独立栈上执行信号 handler |
| 信号队列 (`siginfo_t`) | 信息量 | 传递信号来源等附加信息 |

## 与其他文档的关系

| 文档 | 关系 |
|------|------|
| `todo.md` | 信号机制 checklist 入口 |
| `wayland_worklist.md` | 信号作为 Wayland Phase 2 的前置依赖 |
| `terminal_split.md` | Ctrl+C 在 terminal line discipline 中的处理 |
| `process_lifecycle.md` | SIGCHLD 取代 RECV_NOTIFY 对进程退出的影响 |
