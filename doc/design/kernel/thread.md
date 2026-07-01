# 多线程设计（xtask_t + proc_t + signal_struct + clone + pthread）

> 架构方向：Mach 对齐（`xtask_t` 调度实体 + `proc_t` POSIX 语义分层保留），接口贴 Linux（`clone` flags / `futex` / TLS / 两级信号语义）。
> 本文档描述完整 pthread 栈：`clone(CLONE_VM|FILES|SIGHAND|THREAD)` + `futex` + TLS（ELF `.tdata`/`.tbss` + TCB + FS_BASE，variant II）+ FPU lazy switch + `exit_group` + `tgkill` + 两级信号（shared_pending + per-task pending）+ `clear_tid` + `pthread_join` + cancel/detach/TSD/attr。
> 与 `proc.md` 的关系：`proc.md` 描述单进程语义（fork/execve/exit/waitpid），本文档在其上叠加线程语义。结构体定义以本文档为准（`proc.md` 的 `task_t`/`sig` 字段描述为线程改造前的旧状态）。

## 设计决策总结

| # | 决策 | 结论 |
|---|------|------|
| 1 | kthread vs 用户态线程 | 只做用户态 pthreads（1:1 模型），不做 kthread。微内核策略全在用户态，无内核后台任务需求 |
| 2 | 结构体分层 | 保持 master 现有 `xtask_t`(Xcore,调度) + `proc_t`(BSD,POSIX) 分层。新增 `signal_struct`(线程组共享,ref counted)。`mm_t`/`files_t` 已独立 ref counted，直接复用 |
| 3 | 创建接口 | `clone()` 系统调用，贴 Linux。`fork = clone(0,...)` 语义等价，但 `sys_fork` 保留独立实现（COW 路径稳定，见 `proc.md`）。`sys_fork` 必须同步适配 `signal_struct`（P1 强制项） |
| 4 | 共享体引用计数 | 4 个共享体独立 ref count：`mm_t.m_count`（CLONE_VM）、`files_t.f_count`（CLONE_FILES）、`signal_struct.sig_count`（CLONE_SIGHAND）、`tgid`（CLONE_THREAD）。各自归零独立释放 |
| 5 | 退出语义 | 贴 Linux `do_exit`：所有线程 `do_exit` 做 `clear_tid` + `futex_wake`（唤醒 joiner）。`signal_struct.thread_count` atomic dec 判最后线程，最后线程通知父进程。`exit_group` 设 `group_exit` 标志，同组线程在 `check_pending_signals` 入口自行退出。**`do_exit` 不做 `mm_put`/`files_put`/`signal_put`**——SMP 下设 ZOMBIE 后 `schedule()`，父进程在另一 CPU 上 `waitpid`→`task_reap` 可能并发 `kfree(proc_t)`，若 `do_exit` 也 put 资源会与 `task_reap` 形成并发 UAF。所有资源释放集中到 `task_reap`/`proc_reap` 单一 owner。`exit_code` 移到 `xtask_t`（静态数组，slot 生命周期由 `tasks_lock` 保），`waitpid` 从 `xtask->exit_code` 读，不依赖 `proc_t` |
| 6 | fork + execve | 与线程解耦。`sys_fork` 保留 master 现有 COW 路径（见 `proc.md`），`sys_clone` 的 fork 分支（flags=0）复用 `copy_page_table`/`copy_fd_table`/`copy_mmap_regions`/`build_kstack_from_tf` 辅助函数（从 `sys_fork` 抽出为独立函数）但入口独立。两路径并存，helper 共享，入口编排不同 |
| 7 | TLS | 完整加载 ELF `.tdata`/`.tbss` 段 + TCB。**variant II 布局**（TCB 在 TLS 页高地址端，`FS_BASE = &TCB`，TLS 变量 `%fs:(-offset)` 访问，与 musl/glibc x86-64 一致）。`elf_loader` 加 PT_TLS 解析，`clone(CLONE_SETTLS)` 分配 TLS 页，`__trapret`/`syscall_fast_entry` 返回用户态前加载 fs_base（不动 `switch_to` 汇编）。主线程 TLS 由用户态 `_start` 调 `__libc_tls_init()` 初始化，通过 `linker.ld` 导出的 `__tls_template_*` 符号定位模板（无 auxv） |
| 8 | FPU/SSE | eager FPU 上下文切换（创建时预分配 fpu_page + schedule 时 fxsave prev/fxrstor next）。`xcore_fpu_alloc` 在 `process_create_elf`/`sys_fork` 时调用 `bfc_alloc_page(1)` 并初始化为合法 fxsave 镜像（memset 0 + MXCSR=0x1F80）。`schedule()` 的 C helper `fpu_context_switch` 直接 fxsave prev + fxrstor next，不设 CR0.TS，用户态 SSE 零 trap。`#NM` handler `fpu_lazy_switch` 保留作兜底（TS 意外泄漏时 clts + fxrstor）。fork 时 child memcpy parent 当前 FPU 快照（POSIX 语义）。内核仍 `-mno-sse`，用户态移除 |
| 9 | clone 签名 | `clone(flags, stack, parent_tid, child_tid, tls)`，与 Linux 一致 |
| 10 | task 表大小 | 维持 `MAX_PROC=64`，`pid==数组下标`。`tgid` 字段已存在于 `xtask_t`，单线程时 `tgid==pid`，多线程时 `tgid=leader.pid`。动态扩容后续 |
| 11 | fd_table 归属 | `files_t` 已从 `mm_t` 独立（master commit 9279262），`CLONE_FILES` 时 `files_t.f_count++`，与 `CLONE_VM` 解耦。第一版 `CLONE_FILES` 和 `CLONE_VM` 绑定一起用，但结构上不耦合 |
| 12 | 信号与线程 | 两级 pending，贴 Linux：`proc_t.sig_pending`（per-task 私有，`tgkill`/`pthread_kill` 产生）+ `signal_struct.shared_pending`（进程级，`kill(pid,sig)` 产生）。`sigaction action[]` 挪到 `signal_struct`（线程组共享） |
| 13 | execv 线程行为 | 设 `signal->group_exit` 标志，同组线程在 `check_pending_signals` 入口自行退出（Linux 方式） |
| 14 | proc_reap 重构 | `task_reap`（waitpid 触发）批量回收同组非 leader ZOMBIE：清调度器资源（内核栈/IOPM/FPU/recv/PCB）+ `mm_put`/`files_put`/`signal_put`/`kfree(proc)`。**资源释放单一 owner**——`do_exit` 不 put 这些资源（SMP 并发 UAF 防护，见决策 #5）。内核栈释放延迟到 `xtask_alloc` 复用 slot 时（SMP 下 `task_reap` 释放子进程栈与子进程仍在 `switch_to` 用栈的竞态防护） |
| 15 | syscall 编号 | 追加 60-67，不重排。`NR_SYSCALL=68`。`sys_getpid` 改返回 tgid，新增 `sys_gettid`(66)、`sys_sigprocmask`(67)。60-66 见下文 syscall 表 |
| 16 | futex | 全局 hash 表 64 bucket + bucket lock + `futex_key`。**第一版只 anon key**（`key = (mm->cr3, uaddr>>PAGE_SHIFT)`，同进程线程共享 mm 可匹配）。shm/phys key 延后（跨进程 SHM futex 返回 -ENOSYS）。`FUTEX_WAIT` + `FUTEX_WAKE`，PI/requeue 返回 -ENOSYS |
| 17 | 调度器 | 完全无感知线程，`xtask_t` 平等调度，per-CPU FIFO round-robin 不变 |
| 18 | waitpid | 只回收线程组 leader（`tgid==pid`），非 leader 由 `pthread_join` 回收 |
| 19 | 锁协议 | 零嵌套。`sig_lock` 和 futex bucket lock 都不与其他锁同时持有。`sys_kill`/`exit_group`/`futex_wake` 采用"先改状态后释放再唤醒"模式（局部数组收集 waiter）。debug 模式加 `ASSERT(!holding_any_other_lock())` 运行时校验（per-CPU nesting counter，违反 panic） |
| 20 | 设计约束代码化 | flag 组合约束、锁序、refcount 释放顺序等约束落到运行时校验（返回 -EINVAL + printk）或 `BUG_ON`/`ASSERT`，不只写文档 |
| 21 | xcore/posix 边界 | `fs_base`/`fpu_page` 放 `xtask_t`（xcore 快路径直接访问，不为"xcore 纯度"塞 `proc_t` 让汇编多解引用+判 NULL）。约束：字段进 xcore 后依赖单向——xcore 不为访问这些字段回解引用 `proc_t`。POSIX 纯线程语义（tgid 线程组、sig_pending、clear_tid、futex_node）仍优先 `proc_t` |
| 22 | pthread 范围 | practical complete：create/exit/join/detach/cancel + mutex(3 types)+timed/try + cond+timed + rwlock + barrier + once + TSD(pthread_key) + pthread_kill/sigmask + attr(stack/stacksize/detachstate/guardsize) + setname_np。延后：atfork/spin/sigqueue/schedparam/affinity_np/process-shared |
| 23 | cancel 模型 | 信号驱动推迟式。`pthread_cancel` → `tgkill(tgid, tid, SIGCANCEL)`，`SIGCANCEL=32`（`NSIG` 从 32 扩到 33）。目标线程在取消点检查 cancelstate，走 cleanup stack 后 `pthread_exit`。异步取消（PTHREAD_CANCEL_ASYNCHRONOUS）第一版推迟到下一取消点 |
| 24 | detached 回收 | detached 线程 `do_exit` 自回收：非 leader 或最后线程（`thread_count==0`）时在 `do_exit` 末尾调 `task_reap` 自清。`exit_group` 遍历同组只读 `xtask_t.tgid`（不读 `proc`），自回收 UAF 安全 |
| 25 | TSD 实现 | TCB 内定长数组 128 slots + 进程级 `key_used[128]`/`key_destructor[128]`。析构按 POSIX 顺序最多 4 轮。O(1) 访问 |
| 26 | mutex/cond 原语 | 只用 `FUTEX_WAIT`/`FUTEX_WAKE`，musl 风格状态机（mutex 原子状态字）+ 序号计数器（cond）。`FUTEX_REQUEUE` 返回 -ENOSYS。`PTHREAD_PROCESS_SHARED` 第一版返回 ENOSYS（anon key 不跨进程匹配） |

## 结构体设计

### xtask_t（Xcore 调度实体，新增字段）

**重要**：前 5 个字段（`pid`/`state`/`k_rsp`/`k_stack_top`/`cr3`）必须保持固定偏移，供 `switch_to` 汇编使用（`k_rsp` 在 offset 8，`cr3` 在 offset 24）。`_Static_assert` 已在 `xtask.h` 和 `sched.c` 验证。**新增字段追加在末尾，不动现有 offset**。

```c
// kernel/xcore/xtask.h — 新增字段（追加在 xtask_t 末尾）
typedef struct xtask_t {
    // ... 现有字段不变（pid/state/k_rsp/k_stack_top/cr3/entry/wait_event/tgid/mm/...）
    // ... recv 队列 / REQ 状态 / MSG 状态 / cpu_time 等不变

    // === 新增：线程支持 ===
    uint64_t fs_base;           // TLS 基址（FS_BASE MSR 镜像），__trapret 加载
    Page    *fpu_page;          // fxsave 区页（创建时预分配：xcore_fpu_alloc 在 process_create_elf/sys_fork 调用 bfc_alloc_page(1)）。存 Page* 而非数据指针，类型层面防误用；使用时 phys_to_virt(page_to_phys(fpu_page)) 取数据页虚拟地址喂 fxsave/fxrstor。NULL = idle（无 FPU 状态）
    int32_t  exit_code;         // 退出码（state==ZOMBIE 时有效）。放 xtask_t（静态数组，slot 生命周期由 tasks_lock 保）而非 proc_t（kmalloc'd），waitpid 可在 proc_t 被 task_reap 释放后安全读
} xtask_t;
```

**为什么 fs_base 和 fpu_page 在 xtask_t 而非 proc_t**：
约束是依赖方向单向：某个字段进了 xcore，外部（BSD 层）就通过 xcore 的接口访问它，xcore 不反过来为访问这些字段解引用 `proc_t`。
- `fs_base`：`__trapret`/`syscall_fast_entry` 汇编要 `wrmsr(MSR_FS_BASE, current_task->fs_base)`。放 `proc_t` 多一次解引用（`current_task->proc->fs_base`），且 `proc` 可能为 NULL（idle）要判空——汇编里判空很丑。放 `xtask_t` 汇编直接 `movq offset(%rax), %rdx`，xcore 不回解引用 `proc_t`，依赖干净。
- `fpu_page`：`fpu_context_switch` 在 `schedule()`（Xcore 层）直接拿 `current_task`。eager FPU 的核心是"创建时预分配 + 切换时直接 fxsave/fxrstor"，这条路径不碰 BSD 语义，纯调度器职责。放 `xtask_t` 不需要 `trap_dispatch` 解引用 `proc_t`。
- `exit_code`：SMP 下 `do_exit` 设 ZOMBIE 后 `schedule()`，父进程在另一 CPU 上 `waitpid`→`task_reap` 可能并发 `kfree(proc_t)`。`exit_code` 放 `proc_t` 的话，`waitpid` 读时 `proc_t` 可能已被释放。放 `xtask_t`（静态数组，slot 生命周期由 `tasks_lock` 保），`waitpid` 从 `xtask->exit_code` 读，不依赖可能被 reap 的 `proc_t`。`proc_t.exit_code` 仍保留为 legacy dead write（3b 移除）。

POSIX 纯线程语义（`tgid` 线程组、`sig_pending`、`clear_tid_addr`、`futex_node`）仍优先 `proc_t`，因为这些不会被 xcore 快路径直接碰。

### proc_t（BSD POSIX 语义，改造）

```c
// kernel/bsd/proc.h — proc_t 改造
typedef struct proc {
    struct xtask_t *xtask;      // 反向引用（1:1 绑定）

    // === POSIX 进程语义 ===
    int32_t  exit_code;         // 退出码（legacy dead write，waitpid 已改读 xtask_t.exit_code，3b 移除）
    pid_t sid;                  // session ID
    pid_t pgid;                 // process group ID
    struct pty *ctty;           // 控制终端

    // === 信号（线程级 + 进程级共享） ===
    uint64_t      sig_pending;  // per-task 私有 pending（tgkill/pthread_kill 产生）
    sigset_t      sig_blocked;  // per-task 信号阻塞掩码
    siginfo_t     sig_force_info;  // force_sig 临时数据（现有）
    struct signal_struct *signal;  // 进程级共享（fork 独立拷贝；clone(CLONE_SIGHAND) ref++）

    // === fd 表（不变） ===
    struct files_t *files;      // fork 深拷贝；clone(CLONE_FILES) ref++

    // === 新增：线程支持 ===
    pid_t    clear_tid_addr;    // CLONE_CHILD_CLEARTID 用户态地址（0=无）
    list_node_t futex_node;     // futex bucket 链表节点
    uint64_t futex_uaddr;       // 等待的用户态地址（0=未在 futex 等待）
} proc_t;
```

**sig.action[] 挪到 signal_struct**：master 现状 `proc_t->sig.action[]` 改为 `proc_t->signal->action[]`（线程组共享，贴 Linux）。`sigaction` 系统调用写 `proc->signal->action[sig]`。

### signal_struct（新增，线程组共享，ref counted）

```c
// kernel/bsd/signal.h — 新增
struct signal_struct {
    refcount_t    sig_count;        // 共享体引用计数（CLONE_SIGHAND 时 ++）
    atomic_t      thread_count;     // 线程组存活线程数（CLONE_THREAD 时 ++，do_exit 时 --）
    atomic_t      live_count;       // 还活着的线程（未 ZOMBIE），用于 waitpid 判线程组是否全死
    spinlock_t    sig_lock;         // 保护 shared_pending
    uint64_t      shared_pending;   // 进程级 pending（kill/pgsignal 产生）
    sigaction_t   action[NSIG];     // handler 表（线程组共享）
    uint8_t       group_exit;       // exit_group 标志
    int32_t       group_exit_code;  // exit_group 退出码
    pid_t         parent_pid;       // 线程组的父进程 PID（从 mm_t.parent_pid 镜像，避免 mm 释放后 UAF）
};
```

**为什么 parent_pid 镜像到 signal_struct**：`do_exit` 判最后线程通知父进程时要访问 `parent_pid`（ZOMBIE gate 之前读到局部变量 `ppid`）。`mm_put` 不在 `do_exit` 路径（由 `task_reap` 独占释放），但 `signal->parent_pid` 在 ZOMBIE 前读取，此时 `signal_struct` 存活，安全。镜像到 `signal_struct` 是为了与 `mm_t.parent_pid` 解耦——`mm` 释放后 `signal` 可能还在（CLONE_SIGHAND 共享，其他线程持有引用）。`fork`/`clone` 时两份 `parent_pid` 同步设置。

**为什么 thread_count 和 live_count 分开**：
- `thread_count`：线程组存活线程数（含 ZOMBIE 未被 reap 的）。`do_exit` 时 dec，归零判"线程组全部退出"→ 通知父进程。
- `live_count`：还活着的线程（未 ZOMBIE）。`waitpid` 用它判线程组是否全死（用于 `WNOHANG` 或避免回收还在跑的线程组）。
- 第一版可只实现 `thread_count`，`live_count` 为后续 `WNOHANG` 预留。

### mm_t / files_t（复用现有，无改动）

`mm_t`（`kernel/xcore/mm_types.h`）和 `files_t`（`kernel/bsd/types.h`）已独立 ref counted：
- `mm_t.m_count`：`CLONE_VM` 时 `refcount_inc`，`task_reap` 时 `mm_put`（`refcount_dec_and_test` 归零触发 `mm_release`；`do_exit` 不做 `mm_put`，SMP 并发 UAF 防护）
- `files_t.f_count`：`CLONE_FILES` 时 `refcount_inc`，`proc_reap` 时 `files_put`

**与 pthread 分支的关键差异**：pthread 分支把 `fd_table` 内嵌 `mm_t`，master 已把 `files_t` 独立（commit 9279262），`CLONE_FILES` 和 `CLONE_VM` 解耦。重写后第一版 `CLONE_FILES` 和 `CLONE_VM` 绑定一起用（pthread_create 同时带两者），但结构上不耦合，后续可独立组合。

## clone 实现

### clone 签名与 flag

```c
// sys_clone(flags, stack, parent_tid, child_tid, tls)
//   flags      — CLONE_* 位掩码
//   stack      — 新线程用户态栈顶（CLONE_VM 时必传）
//   parent_tid — CLONE_PARENT_SETTID 时写父进程 tid 的地址
//   child_tid  — CLONE_CHILD_CLEARTID 时记录子线程 tid 的地址
//   tls        — CLONE_SETTLS 时新线程 FS_BASE
```

**第一版支持的 flag**：

| flag | 值 | 说明 |
|------|----|------|
| CLONE_VM | 0x00000100 | 共享地址空间（`mm_t` ref++） |
| CLONE_FILES | 0x00000400 | 共享 fd_table（`files_t` ref++） |
| CLONE_SIGHAND | 0x00000800 | 共享信号 handler（`signal_struct` ref++） |
| CLONE_THREAD | 0x00010000 | 放入同一线程组（`tgid = parent->tgid`，`thread_count++`） |
| CLONE_PARENT_SETTID | 0x00100000 | 写子线程 tid 到 `parent_tid` |
| CLONE_CHILD_CLEARTID | 0x00200000 | 线程退出时清 `*child_tid` + `futex_wake` |
| CLONE_SETTLS | 0x00080000 | 设置新线程 FS_BASE |

**flag 组合约束（代码化，入口校验返回 -EINVAL + printk）**：

```c
// kernel/bsd/proc_create.c — sys_clone 入口校验
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                  uint64_t child_tid, uint64_t tls, uint64_t _) {
    // CLONE_SIGHAND 必须带 CLONE_VM（handler 共享但地址空间不同无意义）
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
        return -EINVAL;
    // CLONE_THREAD 必须带 CLONE_SIGHAND（线程组必须共享 handler）
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
        return -EINVAL;
    // CLONE_VM 时 stack 必传（新线程需要独立用户栈）
    if ((flags & CLONE_VM) && stack == 0)
        return -EINVAL;
    // ... 后续逻辑
}
```

### 拷贝路径（4 共享体独立 ref count）

```
sys_clone(flags, stack, parent_tid, child_tid, tls):
  1. xtask_alloc → child slot（tasks_lock 保护）
  2. 分配内核栈（2 页 bfc_alloc_page）
  3. mm_t:
       CLONE_VM?  child->mm = parent->mm; refcount_inc(&mm->m_count)
                  child->cr3 = parent->cr3
       else:       new_mm = mm_create()
                   copy_page_table(parent->mm, new_mm)  # master 现有 COW 路径
                   copy_mmap_regions(parent->mm->mmap_regions)
                   child->mm = new_mm; child->cr3 = new_mm->cr3
                   new_mm->parent_pid = parent->pid
  4. files_t:
       CLONE_FILES? child->proc->files = parent->proc->files
                    refcount_inc(&files->f_count)
       else:        child_bp = proc_create()  # 内部建新 files_t
                    copy_fd_table(parent->proc->files, child_bp->files)
  5. signal_struct:
       CLONE_SIGHAND? child->proc->signal = parent->proc->signal
                      refcount_inc(&signal->sig_count)
       else:          new_sig = signal_create()
                      memcpy(new_sig->action, parent->proc->signal->action, NSIG * sizeof(sigaction_t))
                      new_sig->shared_pending = 0
                      new_sig->group_exit = false
                      new_sig->parent_pid = parent->pid
                      child->proc->signal = new_sig
  6. tgid:
       CLONE_THREAD? child->tgid = parent->tgid
                     atomic_inc(&child->proc->signal->thread_count)
       else:         child->tgid = child->pid   # 新 leader
                     atomic_set(&child->proc->signal->thread_count, 1)
  7. trapframe: 拷贝 parent_tf, rax=0, rsp=(CLONE_VM? stack : parent_tf->rsp)
  8. xtask_t 新字段初始化:
       fs_base = (CLONE_SETTLS? tls : parent->fs_base)
       clear_tid_addr = (CLONE_CHILD_CLEARTID? child_tid : 0)
       fpu_page=xcore_fpu_alloc(child)（预分配，见 FPU/SSE 节）
  9. proc_t 线程级:
       sig_pending=0, sig_blocked=parent->proc->sig_blocked
       exit_code=0
       futex_node init, futex_uaddr=0
  10. CLONE_PARENT_SETTID? *parent_tid = child->pid
  11. CLONE_CHILD_CLEARTID? child->proc->clear_tid_addr = child_tid
  12. 入队调度（scheduler_lock 保护）
```

**fork 等价性**：`fork() = clone(0, 0, 0, 0, 0)` —— 全部 flag=0，走 else 分支，全部深拷贝。`sys_fork` 保留独立实现（master COW 路径稳定，见 `proc.md`），`sys_clone` 的 fork 分支复用 `copy_page_table`/`copy_fd_table`/`copy_mmap_regions` 辅助函数但入口独立。两条路径语义等价，实现上解耦避免把稳定路径和新增路径耦合在一个函数里。

## exit 语义

### do_exit（所有线程退出都走这个）

贴 Linux `do_exit` 流程：

```
do_exit(exit_code):  // 所有线程都走这个
  1. current->exit_code = exit_code（写 xtask_t，waitpid 从此读）
     current->proc->exit_code = exit_code（legacy，dead write，3b 移除）
  2. CPU 时间记账（cpu_time_ns += sched_clock() - last_sched）
  3. 孤儿收养（用 mm->parent_pid，遍历 tasks[] 把 mm->parent_pid == current->pid 的子进程 reparent 到 init）
     —— 注意：孤儿收养用 mm->parent_pid（地址空间关系），不用 signal->parent_pid
  4. clear_tid_addr:  ← 关键，所有线程都在这里做（ZOMBIE 之前，proc_t 存活）
     if (current->proc->clear_tid_addr):
         *current->proc->clear_tid_addr = 0  (CR3 切换到用户地址空间后写)
         futex_wake(current->proc->clear_tid_addr, 1)  // 立刻唤醒 joiner
  5. Thread-group bookkeeping（ZOMBIE 之前，proc_t/signal 存活）：
     sig = current->proc->signal  ← 读到局部变量
     ppid = sig->parent_pid       ← 读到局部变量
     atomic_dec(&sig->live_count)
     notify_parent = atomic_dec_and_test(&sig->thread_count)
  6. 设 ZOMBIE（scheduler_lock 保护）
     —— GATE：此后 task_reap/proc_reap 在另一 CPU 可能 kfree(proc_t) + signal_put，
        禁止再解引用 current->proc 或 sig
  7. 唤醒等待本线程 REQ/MSG reply 的进程（现有逻辑保留，ESRCH，只用 xtask_t 字段）
  8. if (notify_parent):  ← 用局部 ppid，不读 sig->parent_pid
         通知父进程（parent = tasks[ppid]）
         atomic_or SIGCHLD to parent->proc->sig_pending
         拿 parent scheduler_lock，若 parent 在 WAIT_CHILD 则 wake_from_wait
  9. schedule()  永不返回
     —— do_exit 不做 mm_put/files_put/signal_put，task_reap/proc_reap 独占释放
```

**关键设计点**：
- `clear_tid` 在 `do_exit` 第4步做（**不是** `task_reap`），立刻 `futex_wake` 唤醒 joiner。如果延迟到 `task_reap`，`task_reap` 又延迟到父进程 `waitpid` 之后——joiner 会永远阻塞。detached 线程的 `clear_tid_addr` 通常为 `&tcb->tid`，第4步照样执行（futex_wake 无 joiner 等待，no-op）。
- **`mm_put`/`files_put`/`signal_put` 不在 `do_exit` 做，由 `task_reap`/`proc_reap` 独占释放**。SMP 下 `do_exit` 设 ZOMBIE 后 `schedule()`，父进程在另一 CPU 上 `waitpid`→`task_reap` 可能并发 `kfree(proc_t)`；若 `do_exit` 自己也 put 这些资源，就和 `task_reap` 形成并发 UAF。把所有资源释放集中到 `task_reap`/`proc_reap` 单一 owner，避免竞态。`exit_code` 因此移到 `xtask_t`（静态数组，slot 生命周期由 `tasks_lock` 保），`waitpid` 从 `xtask->exit_code` 读，不依赖可能被 reap 的 `proc_t`。
- "最后一个线程"用 `signal->thread_count` 判（atomic_dec_and_test），不是 `mm` refcount 也不是 `signal->sig_count`。理由：refcount 是"共享体引用计数"，thread_count 是"线程组存活线程数"，两者语义不同——CLONE_VM 时 mm ref++（不一定是新线程），CLONE_SIGHAND 时 sig_count++（也不一定是新线程），只有 CLONE_THREAD 时 thread_count++。
- **ZOMBIE gate 模式**：ZOMBIE 前把 `sig`/`ppid` 读到局部变量，ZOMBIE 后只用局部变量 + `xtask_t` 数组字段，不解引用 `proc->proc` 或 `sig`。这是 SMP 并发 reap 的安全保证。
- **detached 自回收 UAF 安全**：`exit_group` 遍历同组只读 `xtask_t.tgid`（不读 `proc`），自回收 `kfree(proc)` 后 `exit_group` 不会 UAF。leader 且 `thread_count>0` 的 detached 情况不存在（主线程 `pthread_exit` = `exit_group`）。

### sys_exit_group（杀整个线程组）

```
sys_exit_group(status):
  1. signal = current->proc->signal
  2. spin_lock(&signal->sig_lock)
     signal->group_exit = true
     signal->group_exit_code = status
     spin_unlock(&signal->sig_lock)
  3. 遍历 tasks[]，找同 tgid 且 != current 的线程:
     for each target where target.tgid == current.tgid && target.pid != current.pid:
       int cpu = target->assigned_cpu
       spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock, &flags)
       if (target->state == BLOCKED):
         timer_queue_cancel(target)          // 取消超时
         target->state = READY
         target->wait_event = WAIT_NONE
         target->wait_timed_out = 0
         list_push_back(&run_queue, &target->run_node)
         cpu_locals[cpu].run_count++
       spin_unlock_irqrestore(...)
       —— 不动 ZOMBIE/READY/RUNNING 线程
  4. do_exit(status)  // 当前线程退出
```

**group_exit 传播**：被唤醒的 BLOCKED 线程被设为 READY 后会调度运行，最终走 `__trapret`/`syscall_fast_entry` → `check_pending_signals` 入口检查 `signal->group_exit` → `do_exit(group_exit_code)` 自行退出。

**check_pending_signals 入口新增 group_exit 检查**（最高优先级）：

```c
// kernel/bsd/signal.c — check_pending_signals 入口
void check_pending_signals(trapframe_t *tf) {
    if (tf->cs != 0x2B) return;
    xtask_t *proc = current_task;
    if (!proc || !proc->proc) return;

    // ← 新增：group_exit 检查，最高优先级
    if (proc->proc->signal->group_exit) {
        do_exit(proc->proc->signal->group_exit_code);
        return;  // unreachable
    }

    // 原有信号处理逻辑（两级 pending）...
}
```

**为什么这个 race 在 master 上不存在**：master 没内核线程（只有 idle，`mm=NULL`，不走 `check_pending_signals`），用户线程的阻塞都是 `schedule()`——唤醒后返回到 `schedule()` 的调用者（`futex_wait`/`pipe_read`/`recv` 等），syscall 返回时走 `syscall_fast_entry` 的 `check_pending_signals`。即使线程在内核态跑了一阵（比如 `copy_from_user` 的 memcpy），最终都要 trap return 回用户态，那个边界会检查 `group_exit`。

### pthread_exit（用户态封装）

```c
// user/lib/pthread.cc
void pthread_exit(void *retval) {
    (void)retval;
    sys_exit((int)(intptr_t)retval);  // 调 do_exit，只杀自己
    __builtin_unreachable();
}
```

`pthread_exit` 调 `sys_exit`（不是 `sys_exit_group`），只退自己。如果是主线程调 `pthread_exit`，行为同 `exit_group`（glibc 语义：主线程 `pthread_exit` 后进程不退出，等所有非主线程退出；第一版简化为主线程 `pthread_exit` = `exit_group`，文档记此偏差）。

### pthread_join（futex 等待 tid 归零）

```c
// user/lib/pthread.cc
int pthread_join(pthread_t thread, void **retval) {
    int32_t *tid_addr = __pthread_get_clear_tid_addr((pid_t)thread);
    if (!tid_addr) return -ESRCH;

    // 等待 tcb->tid 变 0。目标线程 do_exit 第4步清零 + futex_wake
    while (1) {
        int32_t val = __atomic_load_n(tid_addr, __ATOMIC_ACQUIRE);
        if (val == 0) break;
        sys_futex((uint32_t *)tid_addr, FUTEX_WAIT, (uint32_t)val,
                  NULL, NULL, 0);
    }
    if (retval) *retval = NULL;  // 第一版不传退出值
    return 0;
}
```

`pthread_create` 时通过线程注册表（`tid → clear_tid_addr` 映射）记录子线程的 `clear_tid_addr`，`pthread_join` 查表获取。

## 资源回收

### task_reap（waitpid 触发，批量回收同组 ZOMBIE）

`task_reap` 由父进程 `waitpid` 触发（只回收 leader，`waitpid` 验证 `tgid == pid`）。leader 是最后一个退出的线程（`thread_count` 归零保证了这点），此时所有非 leader 已经 ZOMBIE。

```
task_reap(leader_task):  // waitpid 触发
  1. 释放 leader IOPM
  2. 释放 leader FPU state（fpu_page，如果非 NULL）
  3. 释放 leader recv 队列 RECV_MSG 缓冲
  4. mm_put(leader.mm)  —— 主动释放（do_exit 不做，单一 owner）
     refcount_dec_and_test 归零触发 mm_release（用户页+PML4+mmap+SHM+devtmpfs+irq_owner）
  5. proc_reap(leader):  —— POSIX 资源释放
     - files_put(leader.proc->files)（关所有 fd + files_t 释放）
     - signal_put(leader.proc->signal)（sig_count 归零 kfree）
     - kfree(leader.proc)
     - leader->proc = NULL
  6. 遍历同 tgid 的非 leader ZOMBIE 线程（多线程阶段，单线程无此步）：
     for each non-leader task where task.tgid == leader.tgid && task.pid != leader.pid:
       ASSERT(task.state == ZOMBIE)
       - 释放其内核栈 + IOPM + FPU state + recv 缓冲
       - mm_put/task.proc 已随各线程 do_exit 后由各自 task_reap 释放（detached）或此处批量
       - kfree(task.proc)
       - PCB 槽位清零（pid=-1, state=UNUSED）
  7. leader PCB 槽位清零（k_stack_top 保留，延迟到 xtask_alloc 复用时释放）
```

**为什么 task_reap 独占资源释放（不交给 do_exit）**：SMP 下 `do_exit` 设 ZOMBIE 后 `schedule()`，父进程在另一 CPU 上 `waitpid`→`task_reap` 可能并发 `kfree(proc_t)`。若 `do_exit` 自己也 `mm_put`/`files_put`/`signal_put`，就和 `task_reap` 形成并发 UAF。把资源释放集中到 `task_reap`/`proc_reap` 单一 owner，`do_exit` 只负责设 ZOMBIE + 通知父进程 + `clear_tid` + thread_count 记账。`exit_code` 因此移到 `xtask_t`，`waitpid` 不依赖可能被 reap 的 `proc_t`。

**内核栈延迟释放**：`task_reap` **不在此时**释放 leader 内核栈（2 页）。SMP 下父进程 `task_reap` 释放子进程栈与子进程仍在 `fpu_context_switch`/`switch_to` 使用栈存在竞态（子进程设 ZOMBIE 后 `schedule()` 切走，但 `switch_to` 汇编可能还在用栈）。栈保留在 `xtask_t.k_stack_top`，延迟到 `xtask_alloc` 复用该 slot 时 `bfc_free_page`——此时子进程早已切走，安全。

**race 处理**：
- leader 的 `task_reap` 遍历同 tgid ZOMBIE 线程时，这些线程已经 `do_exit` 完毕（`thread_count` 归零保证了这点——最后退出的那个线程看到归零，意味着其他都至少进了 ZOMBIE）。
- `do_exit` 不做 `mm_put`/`files_put`/`signal_put`，所以"进了 ZOMBIE 但还没 put"的 race 不存在——这些资源根本不在 `do_exit` 路径里，全由 `task_reap` 独占释放。

### waitpid 只回收 leader

`sys_waitpid` 验证 `child->tgid == pid`，非 leader 不符合（`tgid != pid`），不被 `waitpid` 回收。非 leader 的 PCB 由 leader 的 `task_reap` 批量回收。

```
// sys_waitpid 验证逻辑
if (child->pid != pid || !child->mm || child->mm->parent_pid != current->pid)
    return -ECHILD;
// 隐含：只有 leader 能被 waitpid 回收（非 leader 的 tgid != pid）
```

### signal_struct 生命周期

```
signal_create():  kmalloc + refcount_set(&sig_count, 1) + atomic_set(&thread_count, 1)
fork（无 CLONE_SIGHAND）:  signal_create() + 深拷贝 action[]
clone(CLONE_SIGHAND):       refcount_inc(&sig_count)，共享
do_exit:                    atomic_dec(thread_count/live_count)，不做 signal_put
task_reap/proc_reap:        signal_put() → refcount_dec_and_test → 归零 kfree(signal)
```

## futex 实现

### 数据结构

```c
// kernel/bsd/futex.h — 新增
#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE 64  // (1 << FUTEX_HASH_BITS)

struct futex_bucket {
    list_node_t waiters;    // 等待线程链表（proc_t->futex_node 挂载）
    spinlock_t  lock;
};

extern struct futex_bucket futex_table[FUTEX_HASH_SIZE];
```

### futex_key（第一版只 anon）

```c
// futex_key 把用户态 uaddr 解析成内核态 key，后续比较都用 key
// 第一版只实现 anon 类型：
// - 匿名页（私有映射）: key = (mm->cr3, uaddr >> PAGE_SHIFT)
//   CLONE_VM 共享 mm 时 key 相同，同进程内线程可匹配（pthread mutex/cond 全走这条）
// shm/phys key 延后：跨进程 SHM futex 第一版返回 -ENOSYS

struct futex_key {
    uint32_t type;       // 0=anon（第一版唯一类型）
    uint64_t cr3;        // anon: mm->cr3
    uint64_t page_off;   // anon: uaddr >> PAGE_SHIFT
};

// get_futex_key(uaddr, mm, &key): 验证 uaddr 在某个 mmap_region 内，生成 anon key
// 第一版简化：不遍历 region 判类型，直接 (mm->cr3, uaddr>>PAGE_SHIFT)
// 未映射返回 -EFAULT（可选：第一版可省略验证，依赖 bucket lock 内 copy_from_user 验值）
int get_futex_key(uint64_t uaddr, mm_t *mm, struct futex_key *key);
```

**为什么第一版只 anon**：pthread mutex/cond 都在同进程线程间（共享 mm），anon key `(cr3, page_off)` 足够匹配。跨进程 SHM futex（Wayland 跨进程同步）需要 shm/phys key，但第一版延后——Wayland 跨进程同步可走 socket 或后续补 shm key。`PTHREAD_PROCESS_SHARED` mutex 第一版返回 ENOSYS（anon key 不跨进程匹配）。

**bucket lock 内首次验值仍要 copy_from_user**：`FUTEX_WAIT` 验证 `*uaddr == expected` 必须在 bucket lock 内做（否则 lost wake-up）。futex_key 只解决 WAKE 侧链表扫描时"怎么匹配 waiter"的问题，不解决首次验值。bucket lock 内 `copy_from_user` 的 page fault 风险：master 的 `copy_from_user` 不做页表映射检查（直接 `__memcpy`），fault handler 不持 bucket lock，不死锁；实践中 futex 地址都是有效页（mutex 在栈/堆上），几乎不会 fault。

### FUTEX_WAIT(uaddr, expected, timeout)

```
1. get_futex_key(uaddr, current->mm, &key)  ← 不持锁，可安全 page fault
2. copy_from_user(&val, uaddr, 4)  ← 不持锁
3. if (val != expected) return -EAGAIN
4. bucket = &futex_table[futex_hash(key)]
5. spin_lock(&bucket->lock)
6. 再次 copy_from_user(&val, uaddr, 4)  ← 持锁，验值（防 lost wake-up）
7. if (val != expected) { spin_unlock; return -EAGAIN; }
8. current->proc->futex_uaddr = uaddr
   current->proc->futex_key = key  ← 存 key 供 WAKE 匹配
   list_push_back(&bucket->waiters, &current->proc->futex_node)
9. spin_unlock(&bucket->lock)
10. current->wait_event = WAIT_FUTEX
11. if (timeout) 设置 wait_deadline + timer_queue_insert
12. current->state = BLOCKED
13. schedule()
14. （唤醒后）return 0 或 -ETIMEDOUT
```

### FUTEX_WAKE(uaddr, count)

```
1. get_futex_key(uaddr, current->mm, &key)
2. bucket = &futex_table[futex_hash(key)]
3. spin_lock(&bucket->lock)
4. 遍历 bucket->waiters，找 futex_key 匹配的 waiter
5. 收集到局部数组（最多 count 个，数组大小 MAX_PROC=64 指针）
6. spin_unlock(&bucket->lock)  ← 释放后再唤醒（零嵌套锁序）
7. 遍历局部数组，对每个 waiter:
   - 拿 waiter->xtask->assigned_cpu 的 scheduler_lock
   - if (state == BLOCKED && wait_event == WAIT_FUTEX):
       list_remove(&waiter->futex_node)  ← 从 bucket 链表移除
       waiter->futex_uaddr = 0
       wake_from_wait(xtask)  ← 设 READY + 入 run_queue
   - if (state == READY):  ← 已被其他路径唤醒但还没清理节点
       list_remove(&waiter->futex_node)  ← 顺手清理
       waiter->futex_uaddr = 0
   - 释放 scheduler_lock
8. return 实际唤醒数
```

**FUTEX_WAKE 扫到 READY waiter 顺手清理**：被 `exit_group` 唤醒的线程（设 READY 但 `futex_node` 还挂在 bucket 链表上）会在 `FUTEX_WAKE` 扫到时被清理。`do_exit` 不清理 `futex_node`，职责清晰。

### hash 函数

```c
static inline uint32_t futex_hash(struct futex_key *key) {
    // 第一版只 anon key
    uint64_t h = key->cr3 ^ key->page_off;
    return (h >> 3) & (FUTEX_HASH_SIZE - 1);
}
```

### 第一版不支持的操作

`FUTEX_REQUEUE`、`FUTEX_CMP_REQUEUE`、`FUTEX_WAKE_OP`、`FUTEX_LOCK_PI`、`FUTEX_UNLOCK_PI`、`FUTEX_TRYLOCK_PI` 等 PI futex 和 requeue 操作全部返回 `-ENOSYS`。`FUTEX_PRIVATE_FLAG` 第一版不区分（所有 futex 都按 shared 处理）。

## TLS 实现

### ELF TLS 段加载

```c
// kernel/bsd/elf_loader.c — elf_load 扩展
elf_load_result_t elf_load(const uint8_t *elf, size_t size, uint64_t *pml4) {
    // ... 现有 PT_LOAD 段加载逻辑

    // 新增：PT_TLS 解析
    for (each PT_PROGRAM_HEADER phdr) {
        if (phdr.p_type == PT_TLS) {
            // 记录 TLS 模板信息（.tdata 初始镜像 + .tbss 大小 + 对齐）
            tls_template = (void *)(elf + phdr.p_offset);
            tls_tdata_size = phdr.p_filesz;   // .tdata 初始镜像大小
            tls_tbss_size = phdr.p_memsz - phdr.p_filesz;  // .tbss 清零区大小
            tls_align = phdr.p_align;
        }
    }
}
```

### TLS 页布局（variant II，x86-64 标准）

```
clone(CLONE_SETTLS) 时分配 TLS 页：
  ┌───────────────────────────────────┐ ← TLS 页高地址 = FS_BASE 指向这里
  │ TCB                               │   %fs:0 返回此地址
  │   struct tcb {                    │
  │     void *self;  // 指向自身（%fs:0 返回此地址）│
  │     pid_t tid;   // 线程 ID       │
  │     void *clear_tid_addr;         │
  │     int cancel_state; // PTHREAD_CANCEL_ENABLE/DISABLE │
  │     int cancel_type;  // DEFERRED/ASYNCHRONOUS │
  │     void *tsd[128];  // TSD values (pthread_key) │
  │     // 可扩展: locale, errno 等   │
  │   };                              │
  ├───────────────────────────────────┤
  │ padding（对齐到 tls_align）        │
  ├───────────────────────────────────┤
  │ .tbss 清零区                      │
  ├───────────────────────────────────┤
  │ 初始镜像（.tdata 拷贝）            │
  └───────────────────────────────────┘ ← TLS 页低地址

FS_BASE = TCB 地址（TLS 页高地址端）
%fs:0 返回 TCB 地址（tcb.self 指向自身）
%fs:(-offset) 访问 TLS 变量（offset>0，编译器生成）
```

**variant II 而非 variant I**：x86-64 ELF TLS ABI 规定 TCB 在高地址端，TLS 块在 TCB 下方（负偏移）。musl/glibc x86-64 均用 variant II。编译器生成的 TLS 访问代码（`%fs:(-offset)`）依赖此布局。thread.md 早期版本画反成 variant I（TCB 在低地址），已修正。

### clone 时 TLS 分配

```
CLONE_SETTLS:
  1. mmap 分配 1 页 TLS 区域
  2. 布局按 variant II：TCB 在高地址端，.tdata/.tbss 在 TCB 下方
  3. 拷贝 .tdata 初始镜像到 TLS 页低地址端
  4. .tbss 部分清零（.tdata 之后到 padding 之间）
  5. padding 对齐到 tls_align
  6. TCB 放 TLS 页高地址端：
     struct tcb {
         void *self;    // 指向自身（%fs:0 返回此地址）
         pid_t tid;     // 线程 ID
         void *clear_tid_addr;
         int cancel_state; // PTHREAD_CANCEL_ENABLE
         int cancel_type;  // PTHREAD_CANCEL_DEFERRED
         void *tsd[128];   // TSD values (pthread_key)
     };
     tcb->self = &tcb;
  7. child->fs_base = TCB 地址（TLS 页高地址端）
```

### FS_BASE 保存/恢复

```
// 不动 switch_to 汇编（保持极简：只 k_rsp + cr3）
// FS_BASE 在 __trapret / syscall_fast_entry 返回用户态前加载：

// arch/x64/trapentry.S — __trapret 和 syscall_fast_entry 在 check_pending_signals 之后加：
    # 加载 current_task->fs_base 到 MSR_FS_BASE
    movq %gs:0, %rax          # current_task（per-CPU）
    movq fs_base_offset(%rax), %rdx
    movl $0xC0000100, %ecx    # MSR_FS_BASE
    wrmsr
```

**为什么不在 switch_to 切 fs_base**：switch_to 是内核态到内核态切换，此时 GS_BASE 指向 cpu_local，FS_BASE 不被内核使用，切它没意义。真正需要正确 FS_BASE 的是返回用户态那一刻，`__trapret` 加一条 wrmsr 几乎零成本。switch_to 保持极简（只 k_rsp + cr3），offset 8/24 的 `_Static_assert` 不动。

### 主线程 TLS 初始化

```c
// user/lib/start.cc — _start 中调用 __libc_tls_init()
void __libc_tls_init() {
    // 为主线程分配 TLS 页 + 设置 TCB + sys_arch_prctl(ARCH_SET_FS)
    // + sys_set_tid_address(&tcb->tid)
    // TLS 模板通过 linker.ld 导出的符号定位（无 auxv）：
    //   extern char __tls_template_start[], __tls_template_end[];
    //   extern size_t __tdata_size, __tbss_size, __tls_align;
    // 布局按 variant II（TCB 在高地址端）
    // 主线程 clear_tid_addr = &tcb->tid（无人 join 但无害，exit_group 清理）
}
```

**TLS 模板发现机制**：现有进程无 ELF auxv（`process_create_elf` 不构造 auxv），`__libc_tls_init` 通过 `linker.ld` 导出的 `__tls_template_start`/`__tls_template_end`/`__tdata_size`/`__tbss_size`/`__tls_align` 符号定位 PT_TLS 模板。零运行时查找开销，与 musl 静态链接模式一致。`linker.ld` 在 `.tdata`/`.tbss` 段周围定义这些符号。

## FPU/SSE eager 上下文切换

### 初始化

```
// kernel/xcore/sched.c — xcore_fpu_alloc（process_create_elf / sys_fork 调用）
// 分配 fpu_page 并初始化为合法 fxsave 镜像（memset 0 + MXCSR=0x1F80）
// 纯内存操作，无 SSE 指令，不破坏调用者 xmm
```

eager 模式下 `isr_init` 不再设置 `CR0.TS`（TS 恒为 0），用户态 SSE 指令直接执行不触发 `#NM`。

### schedule() 中的 fpu_context_switch（C helper，不修改 switch_to 汇编）

```c
// kernel/xcore/sched.c — schedule() 中，switch_to(prev, next) 之前调用
void fpu_context_switch(xtask_t *prev, xtask_t *next) {
    if (prev && prev->fpu_page) {
        void *data = phys_to_virt(page_to_phys(prev->fpu_page));  // Page* → 数据页虚拟地址
        kernel_fpu_save(data);  // clts + fxsave，保存 prev 的 FPU 状态
    }
    if (next && next->fpu_page) {
        void *data = phys_to_virt(page_to_phys(next->fpu_page));
        kernel_fpu_restore(data);  // clts + fxrstor，恢复 next 的 FPU 状态
    }
    // 不设 CR0.TS，用户态 SSE 零 trap
}
```

idle 进程 `fpu_page=NULL`，自动跳过 save/restore。

### #NM handler（兜底防御）

```c
// kernel/xcore/trap.c — trap_dispatch 的 case 7 保留
// eager 模式下 TS 恒为 0，#NM 不应触发。若 TS 被意外设置（bug），
// fpu_lazy_switch 兜底：clts + fxrstor 恢复当前线程 FPU 状态
```

`nm_nesting_depth` 防护保留（检测 fxrstor/fxsave 在 TS=1 下嵌套 #NM → #DF）。

### fork FPU 继承

```c
// kernel/bsd/proc.c — sys_fork 中，stack 分配后
xcore_fpu_alloc(child);  // child 预分配 fpu_page
ASSERT(parent->fpu_page != NULL);
kernel_fpu_save(parent_data);  // 存 parent 当前 live xmm（fxsave 不改寄存器）
__memcpy(child_data, parent_data, PAGE_SIZE);  // child 继承 parent FPU 快照
```

POSIX fork 语义：child 拿到 parent 当前 FPU 状态快照。`fxsave` 不修改 xmm 寄存器，parent syscall 返回后继续跑不受影响。

### 编译 flags 变更

- **内核**（`build_script/cmake/kernel_rules.cmake`）：保持 `-mno-sse -mno-sse2 -mno-mmx`（内核不用 SSE）
- **用户态**（`build_script/cmake/user_rules.cmake`）：**移除** `-mno-sse -mno-sse2 -mno-mmx`，允许 SSE 指令生成

## 信号与线程

### 两级 pending

| 位图 | 位置 | 产生者 | 消费者 |
|------|------|--------|--------|
| `proc_t.sig_pending` | proc_t（per-task） | `pthread_kill` / `tgkill` / force_sig | 当前线程返回用户态前检查 |
| `signal_struct.shared_pending` | signal_struct（线程组共享） | `kill(pid, sig)` / `pgsignal` | 任意未阻塞该信号的线程消费 |

### 投递逻辑（返回用户态前）

```c
// kernel/bsd/signal.c — check_pending_signals 改造为两级
int pick_signal(xtask_t *t) {
    // 1. 先查私有 pending
    uint64_t pending = t->proc->sig_pending & ~t->proc->sig_blocked;
    pending |= (t->proc->sig_pending & ((1UL << SIGKILL) | (1UL << SIGSTOP)));
    if (pending) {
        int sig = __builtin_ctzl(pending);
        __atomic_and_fetch(&t->proc->sig_pending, ~(1UL << sig), __ATOMIC_RELEASE);
        return sig;
    }
    // 2. 再查共享 pending
    spin_lock(&t->proc->signal->sig_lock);
    pending = t->proc->signal->shared_pending & ~t->proc->sig_blocked;
    pending |= (t->proc->signal->shared_pending & ((1UL << SIGKILL) | (1UL << SIGSTOP)));
    if (pending) {
        int sig = __builtin_ctzl(pending);
        t->proc->signal->shared_pending &= ~(1UL << sig);
        spin_unlock(&t->proc->signal->sig_lock);
        return sig;
    }
    spin_unlock(&t->proc->signal->sig_lock);
    return 0;  // 无信号
}
```

### kill(pid, sig) 投递

```c
// pid 指向线程组 leader
// 进程级信号投递到 signal_struct.shared_pending（线程组共享）
int sys_kill(pid_t pid, int sig) {
    if (pid > 0) {
        if (pid >= MAX_PROC) return -ESRCH;
        xtask_t *leader = &tasks[pid];
        if (leader->pid != pid) return -ESRCH;
        // 投递到进程级 shared_pending
        spin_lock(&leader->proc->signal->sig_lock);
        leader->proc->signal->shared_pending |= (1UL << sig);
        spin_unlock(&leader->proc->signal->sig_lock);
        // 唤醒所有未阻塞该信号的线程（任意线程可消费）
        for (int i = 0; i < MAX_PROC; i++) {
            if (tasks[i].pid >= 0 && tasks[i].tgid == pid
                && !(tasks[i].proc->sig_blocked & (1UL << sig))
                && tasks[i].state == BLOCKED) {
                wake_process(tasks[i].pid);  // 拿 scheduler_lock 唤醒
            }
        }
        return 0;
    }
    // pid == 0: pgsignal(my_pgid, sig)
    // pid == -1: EPERM（第一版不支持）
    // pid < -1: pgsignal(-pid, sig)
}
```

### tgkill(tgid, tid, sig) — 线程定向信号

```c
// 投递到指定 tid 的 per-task sig_pending
int sys_tgkill(pid_t tgid, pid_t tid, int sig) {
    if (tid >= MAX_PROC) return -ESRCH;
    xtask_t *target = &tasks[tid];
    if (target->pid != tid || target->tgid != tgid) return -ESRCH;
    // 投递到线程级 sig_pending（atomic，无 sig_lock）
    __atomic_or_fetch(&target->proc->sig_pending, 1UL << sig, __ATOMIC_RELEASE);
    if (target->state == BLOCKED) wake_process(tid);  // 拿 scheduler_lock 唤醒
    return 0;
}
```

## 锁协议

### 零嵌套原则

`sig_lock` 和 futex bucket lock 都不与其他锁同时持有。任何路径同时只持一把锁。`sys_kill`/`exit_group`/`futex_wake` 采用"先改状态后释放再唤醒"模式（局部数组收集 waiter，释放锁后再拿 `scheduler_lock` 唤醒）。

### 锁序图（扁平，无嵌套）

```
tasks_lock          — 进程表槽位分配
scheduler_lock[cpu] — per-CPU 调度器（state/run_queue/timer_queue）
fd_lock             — fd 表写路径
sig_lock            — signal_struct 共享状态
bucket lock         — futex hash bucket
recv_lock           — recv 队列
```

每个锁单独获取释放，不存在"A 持有 B"的路径。`do_exit` 的多段锁操作每段之间有显式释放点，不是嵌套。

### 关键路径锁序分析

| 路径 | 锁序列 | 说明 |
|------|--------|------|
| `check_pending_signals` | `sig_lock`（读+消费 shared_pending），释放 | 不持其他锁。入口先检查 `group_exit`（无锁读，atomic） |
| `sys_kill(pid>0)` | `sig_lock`（写 shared_pending），释放 → 遍历线程 `scheduler_lock[cpu]`（唤醒），释放 | 两段不嵌套 |
| `sys_tgkill` | 无 `sig_lock`（线程级 `sig_pending` 用 atomic）→ `scheduler_lock`（唤醒） | 线程级不走 `sig_lock` |
| `do_exit` | `scheduler_lock`（设 ZOMBIE），释放 → clear_tid（无锁）→ atomic dec thread_count → `scheduler_lock[parent]`（通知父进程），释放 | 各段独立 |
| `exit_group` | `sig_lock`（设 group_exit），释放 → 遍历线程 `scheduler_lock[cpu]`（唤醒），释放 → `do_exit` | 三段不嵌套 |
| `futex_wait` | `bucket lock`（检查+入队），释放 → `scheduler_lock`（设 BLOCKED+WAIT_FUTEX），释放 → `schedule()` | 各段独立 |
| `futex_wake` | `bucket lock`（收集 waiter 到局部数组），释放 → 遍历数组 `scheduler_lock[cpu]`（唤醒），释放 | 两段不嵌套 |
| `fork/clone` | `tasks_lock`（分配槽位），释放 → 各 `refcount_inc`（atomic）→ `scheduler_lock`（入队），释放 | 现有模式不变 |

### lost wake-up 防护

"释放再唤醒"窗口内状态可能变？不会——
- `sys_kill`：`sig_lock` 内写 `shared_pending` 后释放。线程被唤醒后走 `check_pending_signals` 重新拿 `sig_lock` 读 `shared_pending`。即使唤醒前 `shared_pending` 被其他 CPU 消费了，线程也只是没信号可处理，正常返回。**信号已经在 `shared_pending` 里，唤醒只是让线程有机会检查**。
- `exit_group`：`sig_lock` 内设 `group_exit=true` 后释放。线程被唤醒后走 `check_pending_signals` 看到 `group_exit` 就退出。即使唤醒前线程自己因为别的原因退出了，也不影响。**`group_exit` 标志是持久的**，唤醒只是加速传播。
- `futex_wake`：`bucket lock` 内收集 waiter 到局部数组后释放。即使释放后 waiter 自己因为超时醒了（`timer_queue` 触发），`scheduler_lock` 唤醒时检查 `state == BLOCKED`，不是就跳过。**不会重复唤醒**。

### 约束代码化

`spinlock.h` 的 debug 模式加 `ASSERT(!holding_any_other_lock())` 检查——每个 `spin_lock` 调用前断言当前 CPU 没持有其他 spinlock。这样零嵌套约束变成运行时校验，违反会立刻 panic。master 的 `spinlock.h` 已有 debug 基础设施（commit 2739c91），扩展它加这个断言。

## syscall 新增

| 编号 | syscall | 签名 | 归属层 | 说明 |
|------|---------|------|--------|------|
| 60 | sys_clone | `clone(flags, stack, parent_tid, child_tid, tls)` | BSD | 创建线程/进程，4 共享体独立 ref count |
| 61 | sys_futex | `futex(uaddr, op, val, timeout, uaddr2, val3)` | BSD | 用户态互斥，第一版 WAIT/WAKE + anon key |
| 62 | sys_arch_prctl | `arch_prctl(code, addr)` | BSD | ARCH_SET_FS / ARCH_GET_FS，写 `xtask_t->fs_base` |
| 63 | sys_tgkill | `tgkill(tgid, tid, sig)` | BSD | 线程定向信号，投递到 `proc_t->sig_pending` |
| 64 | sys_exit_group | `exit_group(status)` | BSD | 杀整个线程组，设 `group_exit` + 唤醒同组 |
| 65 | sys_set_tid_address | `set_tid_address(tidptr)` | BSD | 设置 `proc_t->clear_tid_addr` |
| 66 | sys_gettid | `gettid()` | BSD | 返回线程 ID（tid），区别于 getpid 返回 tgid |
| 67 | sys_sigprocmask | `sigprocmask(how, set, oldset)` | BSD | 信号屏蔽字操作（how=SIG_BLOCK/UNBLOCK/SETMASK），同时服务 `pthread_sigmask`/`sigprocmask`/`bsd_sigprocmask` |

NR_SYSCALL: 60 → 68。`xcore_syscall_table` 不动（0-19），新 syscall 全在 BSD 的 `syscall_dispatch`。

**sys_getpid 语义调整**：现有 `sys_getpid` 返回 `pid`。重写后 `pid == tid`（数组下标），`sys_getpid` 改为返回 `tgid`（进程 ID），`sys_gettid` 返回 `tid`。单线程时 `tgid == tid` 兼容。

**SIGCANCEL 与 NSIG**：`NSIG` 从 32 扩到 33，`SIGCANCEL=32`（与 Linux glibc 一致）。`sig_pending`（uint64_t）能装 64 位足够，`sigaction[NSIG]` 数组多一项。`pthread_cancel` 通过 `tgkill(tgid, tid, SIGCANCEL)` 投递。

**sys_fork 保留**：`sys_fork`(54) 保留独立实现（master COW 路径稳定），不重写为 `clone(0)` wrapper。`sys_clone` 的 fork 分支复用辅助函数但入口独立。文档记两条路径等价：`fork() = clone(0, 0, 0, 0, 0)`。

## 实现阶段

### 阶段 1：结构体拆分与字段新增

- 定义 `signal_struct`（`kernel/bsd/signal.h`），`signal_create`/`signal_put`
- `proc_t` 改造：`sig_pending`/`sig_blocked`/`signal`/`clear_tid_addr`/`futex_node`/`futex_uaddr`，`sig.action[]` 挪到 `signal_struct`
- `xtask_t` 新增：`fs_base`/`fpu_page`，`tgid` 真正启用
- 现有进程创建路径适配：`proc_create`/`process_create_elf` 初始化 `signal_struct`（`thread_count=1`，`parent_pid` 镜像）
- **`sys_fork` 同步适配（P1 强制项）**：现有 `sys_fork` 用 `proc_t.sig.action[]` 内嵌数组，改造后必须改成 `signal_create()` + `memcpy(action[])` + `thread_count=1` + `parent_pid` 镜像 + `tgid=child.pid`。否则 fork 出的子进程没有 `signal_struct`，所有信号路径崩
- `check_pending_signals` 适配两级 pending（先私有后共享）
- `sys_kill` 适配：pid 指向 leader 时投递到 `signal->shared_pending`
- `signal.c` 所有 `proc->proc->sig.action[sig]` 改为 `proc->proc->signal->action[sig]`
- **验证**：编译通过 + shell/hello 正常运行（单线程行为不变）

### 阶段 2：FPU/SSE eager switch（已实现）

- `xcore_fpu_alloc`：`bfc_alloc_page(1)` + memset 0 + MXCSR=0x1F80，在 `process_create_elf`/`sys_fork` 创建时预分配 `fpu_page`
- `schedule()` 中 `fpu_context_switch`：fxsave prev + fxrstor next（C helper，不修改 `switch_to` 汇编），不设 `CR0.TS`
- `isr_init` 不设 `CR0.TS`；`#NM` handler `fpu_lazy_switch` 保留作兜底（TS 意外泄漏时 clts + fxrstor），`nm_nesting_depth` 防护保留
- `sys_fork` FPU 继承：child `memcpy` parent 当前 FPU 快照（POSIX fork 语义）
- 移除用户态编译 `-mno-sse -mno-sse2 -mno-mmx`（`user_rules.cmake`）
- **验证**：编译通过 + 用户态浮点程序正常运行

### 阶段 3a：do_exit 重构 + exit_group + 信号扩展（单线程可验证）

- `do_exit` 重构：**不做 `mm_put`/`files_put`/`signal_put`**（SMP 并发 UAF 防护，资源释放集中到 `task_reap`/`proc_reap` 单一 owner）；`clear_tid_addr` 写 0 + `futex_wake`（futex 未实现前 stub）；ZOMBIE 前读 `sig`/`ppid` 到局部变量（ZOMBIE gate 模式），`thread_count` atomic dec + 最后线程通知父进程；`exit_code` 写 `xtask_t`（waitpid 从此读，不依赖 `proc_t`）
- `task_reap`：**主动 `mm_put` + `proc_reap` 做 `files_put`/`signal_put`/`kfree(proc)`**（单一 owner）；内核栈延迟到 `xtask_alloc` 复用 slot 时释放（SMP 栈竞态防护）；`fpu_page` 在 `task_reap` 释放
- `sys_exit_group` 实现：设 `group_exit` + 唤醒同组 BLOCKED 线程 + `do_exit`
- `check_pending_signals` 添加 `group_exit` 检查（入口最高优先级）
- `sys_tgkill`：线程定向信号（设置 per-task `sig_pending` + 唤醒）
- `sys_sigprocmask`(67)：信号屏蔽字操作（SIG_BLOCK/UNBLOCK/SETMASK）
- `sys_set_tid_address`：设置 `clear_tid_addr`
- `sys_gettid`(66)：返回 `xtask_t->pid`（tid）
- `sys_getpid` 改为返回 `tgid`
- `NSIG` 32 → 33，`SIGCANCEL=32` 加入 `common/signal.h`
- 零嵌套锁序 debug ASSERT 代码化（`spinlock.h` per-CPU nesting counter，第一版简化为现有递归死锁检测，完整实现延后）
- **验证**：编译通过 + shell/hello/init 正常运行（单线程 do_exit 路径，task_reap 独占释放，16 测试 PASS）

### 阶段 3b：clone + TLS + futex

- `WAIT_FUTEX` 加入 `wait_event_t` 枚举
- `NR_SYSCALL` 从 60 升到 68，syscall 表填充 slot 60-67
- `copy_page_table`/`copy_fd_table`/`copy_mmap_regions`/`build_kstack_from_tf` 从 `sys_fork` 抽出为独立 helper 函数
- `sys_clone` 实现（CLONE_VM 线程路径 + fork 路径）：copy trapframe(rax=0) + 新内核栈 + `process_entry`
- fork 路径复用上述 helper（master 现有 COW 路径），入口编排独立
- 线程路径：4 共享体独立 ref count，flag 组合约束代码化（入口校验 -EINVAL + printk）
- `elf_loader` 加 PT_TLS 解析；`clone(CLONE_SETTLS)` 分配 TLS 页（variant II：TCB 在高地址端，.tdata 拷贝 + .tbss 清零 + padding + TCB）
- `__trapret`/`syscall_fast_entry` 加载 `fs_base`（wrmsr MSR_FS_BASE）
- `sys_arch_prctl`：ARCH_SET_FS（`fs_base` + wrmsr）+ ARCH_GET_FS
- `sys_futex` 实现：全局 `futex_table[64]` + anon `futex_key`（`(cr3, page_off)`）+ FUTEX_WAIT + FUTEX_WAKE；shm/phys key 返回 -ENOSYS
- **验证**：编译通过（release + debug）

### 阶段 4：pthread 用户态支撑

- `sys_futex`/`sys_arch_prctl`/`sys_tgkill`/`sys_set_tid_address`/`sys_sigprocmask` — Phase 3a/3b 中实现
- TLS 页分配 + FS_BASE 保存/恢复 — Phase 3b 中实现
- 信号完整语义（`shared_pending` + `tgkill` + `sigprocmask`）— Phase 3a 中实现
- `linker.ld` 导出 `__tls_template_start`/`_end`/`__tdata_size`/`__tbss_size`/`__tls_align` 符号
- `_start` 中调用 `__libc_tls_init()`：通过 linker 符号定位 PT_TLS 模板 + 分配 TLS 页 + 设置 TCB（variant II）+ `sys_arch_prctl(ARCH_SET_FS)` + `sys_set_tid_address(&tcb->tid)`
- 新增 `user/include/pthread.h`：`pthread_t`/`pthread_attr_t`/`pthread_mutex_t`/`pthread_cond_t`/`pthread_rwlock_t`/`pthread_barrier_t`/`pthread_once_t`/`pthread_key_t` 及常量
- 新增 `user/lib/pthread.cc`：
  - `pthread_create`：分配线程栈(64KB) + guard page（mmap PROT_NONE，guardsize>0 时）+ TLS 页 + thread_start info，内联 asm clone + 子线程直接跳转 `__pthread_start`
  - `pthread_attr_t`：detachstate/stack/stacksize/guardsize（inheritsched/schedpolicy/schedparam 返回 ENOSYS）
  - `pthread_join`：futex 等待 `tcb->tid` 归零（`do_exit` 清零 + `futex_wake`），通过线程注册表查找 `clear_tid_addr`
  - `pthread_detach`：标记 detached，线程 `do_exit` 时自回收（最后线程或非 leader）
  - `pthread_exit`：`sys_exit`
  - `pthread_cancel`：`tgkill(tgid, tid, SIGCANCEL)`，目标线程在取消点检查 cancelstate，走 cleanup stack 后 `pthread_exit`
  - `pthread_cleanup_push/pop`：cleanup stack（per-thread，TCB 或注册表）
  - `pthread_setcancelstate`/`setcanceltype`：用户态原子，异步取消推迟到下一取消点
  - `pthread_self`：`sys_gettid`
  - `pthread_mutex_t`：musl 风格状态机（原子 + FUTEX_WAIT/WAKE），3 types（normal/errorcheck/recursive）+ timed/try
  - `pthread_cond_t`：seq counter + FUTEX_WAIT/WAKE（无 requeue）
  - `pthread_rwlock_t`/`pthread_barrier_t`/`pthread_once_t`：基于 atomic + futex
  - TSD：`pthread_key_create`/`getspecific`/`setspecific`，TCB 内 `tsd[128]` + 进程级 `key_used[128]`/`key_destructor[128]`，析构最多 4 轮
  - `pthread_kill`：复用 `sys_tgkill`
  - `pthread_sigmask`：复用 `sys_sigprocmask`
  - `pthread_setname_np`：写入 `/proc/self/comm` 或 TCB 字段
  - `PTHREAD_PROCESS_SHARED` 返回 ENOSYS（anon key 不跨进程）
  - 线程注册表（`tid → clear_tid_addr` 映射），供 `pthread_join` 查找
- `start.cc` 改为 `sys_exit_group`（替代 `sys_exit`），确保多线程进程退出时杀全部线程
- `user/include/unistd.h` 新增 `gettid()`，`user/lib/unistd.cc` 实现
- `common/syscall.h` 新增 SYS_GETTID(66)/SYS_SIGPROCMASK(67) + 内联封装
- **验证**：编译通过（release + debug）
- **验证**：`pthread_create`/`mutex`/`cond`/`join` 跑通
- **验证**：`pthread_cancel`/`detach`/`rwlock`/`barrier`/`once`/`TSD` 跑通

## cancel / detach / TSD / attr 补充设计

### pthread_cancel（信号驱动推迟式）

```
pthread_cancel(thread):
  1. tid = (pid_t)thread
  2. tgkill(tgid, tid, SIGCANCEL)  // SIGCANCEL=32
  // 信号到达目标线程，进入 check_pending_signals
  // SIGCANCEL 的默认动作（SIG_DFL）改为"触发取消流程"而非终止

check_pending_signals 收到 SIGCANCEL:
  1. 检查 cancel_state：
     - PTHREAD_CANCEL_DISABLE：清除 pending 标志，返回（推迟到下次 ENABLE）
     - PTHREAD_CANCEL_ENABLE：继续
  2. 检查是否在取消点（read/recv/futex_wait/waitpid/poll 等）：
     - 是：走 cleanup stack（pthread_cleanup_push 注册的函数，反序调用）
     - 否：推迟到下一取消点
  3. pthread_exit(PTHREAD_CANCELED)

异步取消（PTHREAD_CANCEL_ASYNCHRONOUS）：
  第一版推迟到下一取消点（不真正异步）。文档记此偏差。
  真正异步取消需在 check_pending_signals 入口（不限取消点）直接走 cleanup，
  风险是 cleanup 在任意位置执行可能破坏不变量，第一版不做。
```

**SIGCANCEL 与普通信号的区别**：SIGCANCEL 不走 `sigaction`（用户不能 `sigaction(SIGCANCEL, ...)`），内核在 `check_pending_signals` 特判 `sig == SIGCANCEL` → 走取消流程而非用户 handler。`sigaction(SIGCANCEL)` 返回 -EINVAL。

### pthread_detach 与自回收

```
pthread_detach(thread):
  1. 在线程注册表标记该线程为 detached
  2. 如果线程已经 ZOMBIE（已 exit）：立即自回收 task_reap
  3. 否则：do_exit 时检测 detached 标志，自回收

detached 线程 do_exit 路径:
  do_exit 末尾（schedule 之前）:
    if (detached && (非 leader || thread_count==0)):
        设 REAPING 状态
        task_reap(current)  // 自回收：清调度器资源 + kfree(proc)
        // 不再 schedule（已回收）
    else:
        schedule()  // 永不返回

UAF 安全性:
  - exit_group 遍历同组只读 xtask_t.tgid（不读 proc）
  - 自回收 kfree(proc) 后 exit_group 不会 UAF
  - leader 且 thread_count>0 的 detached 情况不存在
    （主线程 pthread_exit = exit_group，见下）
```

**主线程 pthread_exit**：第一版简化为主线程 `pthread_exit` = `exit_group`（glibc 语义是"主线程 pthread_exit 后进程不退出，等所有非主线程"，第一版偏差，文档记此）。

### TSD（pthread_key）

```c
// 进程级（所有线程共享）
static int key_used[PTHREAD_KEYS_MAX];       // 128
static void (*key_destructor[PTHREAD_KEYS_MAX])(void *);

// per-thread（TCB 内）
struct tcb {
    // ...
    void *tsd[PTHREAD_KEYS_MAX];  // 128 个 TSD 值
};

pthread_key_create(key, destructor):
  1. 找 key_used 中第一个 0 的 slot
  2. key_used[slot] = 1; key_destructor[slot] = destructor
  3. *key = slot

pthread_getspecific(key):  return tcb->tsd[key]
pthread_setspecific(key, val):  tcb->tsd[key] = val

pthread_exit 时 TSD 析构（在 cleanup stack 之后）:
  for (iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++):  // 4 轮
    for (key = 0; key < 128; key++):
      if (tcb->tsd[key] && key_destructor[key]):
        val = tcb->tsd[key]
        tcb->tsd[key] = NULL  // 先清，析构中可能重新 set
        key_destructor[key](val)
    if (本轮无任何 tsd 非空) break
```

### pthread_attr_t

```c
typedef struct {
    int    detachstate;    // PTHREAD_CREATE_JOINABLE (0) / DETACHED (1)
    void  *stack;          // 用户提供栈地址（NULL = 库分配）
    size_t stacksize;      // 栈大小
    size_t guardsize;      // guard page 大小（通常 1 页）
    // inheritsched/schedpolicy/schedparam 不实现，返回 ENOSYS
} pthread_attr_t;

pthread_create 时:
  if (attr->stack == NULL):
    分配 stacksize + guardsize
    if (guardsize > 0): mmap guard page 为 PROT_NONE（栈溢出触发 #PF 而非默默越界写）
  else:
    使用用户提供的 stack（不分配 guard page，用户自己负责）
```

## 与其他模块的关系

- 调度器：`xtask_t.run_node` 嵌入 per-CPU run_queue，线程与进程平等调度，详见 [schedule.md](schedule.md)
- IPC：recv 队列 / REQ / MSG 状态在 `xtask_t` 中，线程共享地址空间后 IPC 状态仍 per-task，详见 [ipc.md](ipc.md)
- PTY：`proc_t.sid`/`pgid`/`ctty` 用于 session/job control，线程组共享 session 语义，详见 [terminal.md](terminal.md)
- 信号：两级 pending（`proc_t.sig_pending` + `signal_struct.shared_pending`），详见本文档"信号与线程"章节
- VFS：`files_t.fd_table` 通过 fd 管理文件/pipe/socket/tty，CLONE_FILES 时共享，详见 [vfs.md](vfs.md)
- 进程管理：单进程语义（fork/execve/exit/waitpid）见 [proc.md](proc.md)，本文档在其上叠加线程语义
- 锁模型：零嵌套锁序，详见 [kernel_lock.md](kernel_lock.md)
