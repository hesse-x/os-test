# 多线程设计

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 线程模型 | 用户态 pthreads（1:1），不做 kthread | 微内核策略全在用户态，无内核后台任务需求 |
| 2 | 结构体分层 | `xtask`(Xcore,调度) + `proc`(BSD,POSIX) + `signal_struct`(线程组共享,ref counted) | 调度器只操作 xtask，POSIX 语义在 proc/signal_struct |
| 3 | 创建接口 | `clone()` 系统调用，贴 Linux；`sys_fork` 保留独立实现 | COW 路径稳定，clone/fork 入口独立但 helper 共享 |
| 4 | 共享体引用计数 | 4 个独立 ref count：mm.m_count / files.f_count / signal_struct.sig_count / tgid(thread_count) | 各归零独立释放；CLONE_VM/FILES/SIGHAND/THREAD 各自递增 |
| 5 | 退出语义 | `do_exit` 不做 mm_put/files_put/signal_put，资源释放集中在 sched_task_reap/proc_reap | SMP 下 do_exit 设 ZOMBIE 后 schedule，task_reap 在另一 CPU 可能并发 kfree(proc)；do_exit 也 put 会形成并发 UAF |
| 6 | exit_code 位置 | `xtask.exit_code`（动态分配自 xtask_cache，slot 生命周期由 tasks_lock 保） | waitpid 从 xtask 读，不依赖可能被 reap 的 proc；proc.exit_code 为 legacy dead write |
| 7 | exit_code 编码 | D13：Linux wait status 编码。sys_exit(code) → `(code&0xff)<<8`；death-by-signal → `sig&0x7f`。waitpid 结果可直接喂 WIFEXITED/WEXITSTATUS | 统一编码，避免信号号被误放入 exit status bits |
| 8 | TLS 布局 | variant II：TCB 在 TLS 页高地址端，`FS_BASE = &TCB`，TLS 变量 `%fs:(-offset)` | 与 musl/glibc x86-64 一致；编译器生成 `%fs:(-offset)` 依赖此布局 |
| 9 | FPU/SSE | eager 上下文切换（创建预分配 fpu_page + schedule 时 fxsave/fxrstor） | 不设 CR0.TS，用户态 SSE 零 trap；#NM handler 保留作兜底 |
| 10 | clone 签名 | `clone(flags, stack, parent_tid, child_tid, tls, clone_info_ptr)`，与 Linux 一致 + 第 6 参数传 thread_clone_info | clone_info 传递 detached/tls_page/stack 等线程清理信息给内核 |
| 11 | fs_base/fpu_page 位置 | 放 xtask（xcore 快路径直接访问） | __trapret 汇编 `wrmsr(MSR_FS_BASE, current_task->fs_base)`；proc 可为 NULL(idle)；xcore 不回解引用 proc |
| 12 | 信号与线程 | 两级 pending：`proc.sig_pending`(per-task) + `signal_struct.shared_pending`(进程级) | tgkill/pthread_kill → per-task；kill(pid,sig) → shared；action[] 线程组共享 |
| 13 | cancel 模型 | 信号驱动推迟式：`pthread_cancel → tgkill(SIGCANCEL=32)`；内核 SIGCANCEL 路径检查 `proc.cancel_handler` | 目标线程在取消点检查 cancelstate，走 cleanup stack 后 pthread_exit；cancel_handler=0 时 do_exit_with_code(sig&0x7f)（D13） |
| 14 | 锁协议 | 零嵌套：sig_lock 和 futex bucket lock 不与其他锁同时持有 | sys_kill/exit_group/futex_wake 采用"先改状态后释放再唤醒"模式 |
| 15 | 调度器 | 完全无感知线程，xtask 平等调度，per-CPU FIFO 不变 | — |
| 16 | waitpid | 只回收线程组 leader（tgid==pid），非 leader 由 pthread_join 回收 | — |
| 17 | futex 第一版 | anon key `(cr3, page_off)`；FUTEX_WAIT/WAKE；shm/phys 返回 ENOSYS | pthread mutex/cond 在同进程线程间，anon key 足够 |
| 18 | detached 回收 | detached 线程 do_exit 末尾 task_reap 自清；clone_info 传递 tls_page/stack 给内核 | exit_group 遍历同组只读 xtask.tgid，自回收 UAF 安全 |
| 19 | TSD | TCB 内定长数组 128 slots + 进程级 key_used/key_destructor | O(1) 访问，析构按 POSIX 最多 4 轮 |
| 20 | 信号投递 | rt_sigframe 推用户栈 + sigreturn 恢复完整寄存器状态 | SA_SIGINFO 三参数；sigreturn 从栈帧恢复 sig_blocked |

### 核心数据结构

xtask（kernel/xcore/xtask.h : xtask）— 调度实体，动态分配自 xtask_cache 专用 slab
  pid : pid_t — 进程/线程 ID（数组下标）
  state : proc_state — UNUSED/READY/RUNNING/BLOCKED/ZOMBIE/REAPING
  k_rsp : uint64_t — 内核栈保存 RSP（switch_to 用，offset 8）
  k_stack_top : uint64_t — 内核栈顶（offset 16）
  cr3 : uint64_t — cached PML4 物理地址（权威值在 mm->cr3，offset 24）
  entry : uint64_t — 用户入口 RIP
  wait_event : wait_event — block reason（WAIT_RECV/REQ_REPLY/CHILD/PIPE/MSG_REPLY/POLL/FUTEX/PAUSE）
  tgid : pid_t — 线程组 ID（单线程 == pid，多线程 == leader.pid）
  mm : mm* — 地址空间（NULL 为 idle）
  assigned_cpu : int — 运行 CPU 编号
  iopm : uint8_t* — IOPM bitmap（NULL = deny all）

  run_node : list_node — per-CPU run_queue
  wait_node : list_node — per-CPU timer_queue
  wait_deadline : uint64_t — sched_clock() nanosecond deadline
  alarm_deadline : uint64_t — alarm 超时（0=无，else sched_clock() ns 绝对值）
  wait_timed_out : uint8_t — 1 = timer expired wakeup
  recv_intr : uint8_t — set by wake_process when WAIT_RECV

  // === unified recv queue ===
  recv_buf[16][64] : uint8_t — ring buffer
  recv_head/recv_tail : uint32_t
  recv_lock : spinlock

  // === REQ state ===
  req_caller_pid : pid_t / req_reply_buf/len : void*/size / req_result : int32_t / req_target_pid : pid_t

  // === MSG state ===
  msg_reply_buf/len : void*/size / msg_caller_pid : pid_t / msg_result : int32_t / msg_target_pid : pid_t

  // === CPU time ===
  cpu_time_ns : uint64_t / last_sched : uint64_t

  // === threading support (appended at end) ===
  fs_base : uint64_t — TLS 基址（FS_BASE MSR 镻像），__trapret/syscall_fast_entry 加载
  fpu_page : struct page* — fxsave 区页（xcore_fpu_alloc 预分配；NULL=idle）
  exit_code : int32_t — 退出码（ZOMBIE 时有效，D13 编码）

  // === thread cleanup ownership ===
  detached : int — 1 = sched_task_reap owns TLS/stack unmap
  tls_page : uint64_t — user vaddr of TLS+TCB page（0 if N/A）
  tls_total : size_t — TLS+TCB mapping size
  user_stack_base : uint64_t — user vaddr of stack base（incl guard）
  user_stack_size : size_t — stack+guard total size

  need_resched : uint8_t — 1 = current task must yield
  proc : struct proc* — BSD extension data（NULL = idle/task without POSIX semantics）

前 5 个字段（pid/state/k_rsp/k_stack_top/cr3）固定偏移供 switch_to 汇编使用，_Static_assert 在 xtask.h 和 sched.c 验证。新增字段追加在末尾。

proc（kernel/bsd/proc.h : proc）— POSIX 进程语义
  xtask : xtask* — 反向引用（1:1 绑定）
  exit_code : int32_t — exit code（legacy，waitpid 读 xtask.exit_code）
  sid/pgid : pid_t — session / process group
  ctty : struct pty* — 控制终端

  // === signals ===
  sig_pending : uint64_t — per-task 私有 pending（tgkill/pthread_kill/force_sig 产生）
  sig_blocked : sigset_t — per-task 信号阻塞掩码
  sig_force_info : siginfo_t — force_sig 临时数据
  signal : struct signal_struct* — 线程组共享（fork 独立拷贝；CLONE_SIGHAND ref++）

  // === fd table ===
  files : struct files* — fd 表（fork 深拷贝；CLONE_FILES ref++）

  // === threading ===
  clear_tid_addr : pid_t — CLONE_CHILD_CLEARTID 用户态地址（0=无）
  futex_node : list_node — futex bucket 链表节点
  futex_uaddr : uint64_t — 等待的用户态地址（0=未在 futex 等待）
  cancel_handler : uint64_t — __pthread_cancel_check 函数地址（0=未注册）

  // === POSIX identity & permissions ===
  uid/euid/gid/egid : uint32_t — POSIX 身份（默认 0）
  umask : uint32_t — 文件创建掩码（默认 0022）

sizeof(proc) == 256，ABI drift guard 验证 files offset == 184 / signal offset == 176（确保 signal 是指针而非内嵌）。

signal_struct（kernel/bsd/signal.h : signal_struct）— 线程组共享，ref counted
  sig_count : refcount_t — 共享体引用计数（CLONE_SIGHAND 时 ++）
  thread_count : atomic_t — 存活线程数（CLONE_THREAD ++，do_exit --）
  live_count : atomic_t — 仍活跃线程（未 ZOMBIE），waitpid 线程组检查用
  sig_lock : spinlock_t — 保护 shared_pending
  shared_pending : uint64_t — 进程级 pending（kill/pgsignal 产生）
  action[NSIG] : sigaction_t — handler 表（线程组共享）
  group_exit : uint8_t — exit_group 标志
  group_exit_code : int32_t — exit_group 退出码
  parent_pid : pid_t — 线程组父 PID（镜像自 mm.parent_pid，防 mm 释放后 UAF）

mm（kernel/xcore/mm_types.h : mm）— 地址空间，独立 ref counted
  cr3 : uint64_t — PML4 物理地址
  m_count : refcount_t — CLONE_VM 时 ++
  mmap_brk/mmap_phys_brk : uint64_t — 私有/物理映射 brk
  mmap_regions : mmap_region* — 映射链表
  parent_pid : pid_t — 父进程 PID（孤儿收养用）
  mmap_lock : spinlock — 保护映射操作

files（kernel/bsd/types.h : files）— fd 表，独立 ref counted
  fd_lock : spinlock — 保护 fd_table 操作
  fd_table[MAX_FD=32] : struct file* — RCU-protected fd 数组
  f_count : refcount_t — CLONE_FILES 时 ++

### clone 实现

sys_clone(flags, stack, parent_tid, child_tid, tls, clone_info_ptr) — kernel/bsd/proc.c : sys_clone

支持的 flag：CLONE_VM(0x100) / CLONE_FILES(0x400) / CLONE_SIGHAND(0x800) / CLONE_THREAD(0x10000) / CLONE_PARENT_SETTID(0x100000) / CLONE_CHILD_CLEARTID(0x200000) / CLONE_CHILD_SETTID(0x1000000) / CLONE_SETTLS(0x80000)

flag 组合约束（入口校验 -EINVAL）：CLONE_SIGHAND 必须带 CLONE_VM；CLONE_THREAD 必须带 CLONE_SIGHAND；CLONE_VM 时 stack 必传。

拷贝路径（4 共享体独立 ref count）：CLONE_VM → mm ref++ 共享，else → mm_create + COW；CLONE_FILES → files ref++ 共享（先释放 proc_create 默认创建的 files），else → copy_fd_table；CLONE_SIGHAND → signal ref++ 共享（先释放默认的 signal），else → memcpy action[]；CLONE_THREAD → tgid=parent.tgid + thread_count/live_count++，else → tgid=child.pid + thread_count=1。FPU：child xcore_fpu_alloc + memcpy parent 快照。TLS：CLONE_SETTLS → child->fs_base = tls。CLONE_THREAD 时从第 6 参数 clone_info_ptr 读 detached/tls_page/tls_total/user_stack_base/user_stack_size。

CLONE_PARENT_SETTID：`*(pid_t*)parent_tid = child_pid`（父线程可见）。CLONE_CHILD_SETTID：`*(pid_t*)child_tid = child_pid`（内核在子线程调度前写入，防 lost wake-up 竞态）。

fork 等价性：fork() = clone(0, 0, 0, 0, 0, 0)，全 else 分支，全部深拷贝。sys_fork 保留独立实现（COW 路径稳定），sys_clone fork 分支复用 helper 但入口独立。

### clone wrapper 汇编 trampoline

pthread_create 不能用纯 C wrapper 调 sys_clone（__syscall5 epilogue 切回父栈，child 并发时父栈被覆盖）。__libc_clone_thread 用内联汇编发 syscall，child 返回 0 后在汇编重设 rsp → 清 rbp → 从 TCB(%fs:0) 读 start_routine/arg → call start_routine → call pthread_exit。arg 必须放 rdi（第一参数），不是 rsi（否则 tcb 当 arg，写 *p=42 覆盖 tcb.self → page fault）。

实现：user/lib/pthread.cc : pthread_create 内联 asm trampoline

### TLS 实现

**variant II 布局**：TCB 在 TLS 页高地址端（`FS_BASE = &TCB`），.tdata 拷贝 + .tbss 清零 + padding + TCB。`%fs:0` 返回 TCB 地址（tcb.self 指向自身），`%fs:(-offset)` 访问 TLS 变量。

tcb 结构（user/include/sys/tls.h : struct tcb）
  self : void* — 指向自身（%fs:0 返回此地址）
  tid : pid_t — 线程 ID
  clear_tid_addr : void* — clear_tid 目标地址
  cancel_state / cancel_type : int — PTHREAD_CANCEL_ENABLE/DEFERRED
  tsd[PTHREAD_KEYS_MAX=128] : void* — TSD values (pthread_key)
  cleanup_head : pthread_cleanup_handler* — cleanup stack
  detached : int — detached 标记
  entry : struct thread_entry* — 线程注册表 slot（NULL for main thread）
  start_routine : void*()() — 线程入口函数
  arg : void* — 线程入口参数
  tls_page : void* — TLS 页 vaddr（munmap on exit，NULL for main thread）
  tls_total : size_t — TLS mapping size
  errno_val : int — per-thread errno（__errno_location returns &this field）

**FS_BASE 保存/恢复**：不在 switch_to 汇编切 fs_base（内核态不用），在 __trapret/syscall_fast_entry 返回用户态前 `wrmsr(MSR_FS_BASE, current_task->fs_base)`。

实现：arch/x64/trapentry.S : __trapret / syscall_fast_entry（xtask_fs_base_offset linker 符号定位 offset）

**主线程 TLS 初始化**：`__libc_tls_init()`（两阶段：__libc_tls_init_first 分配 TLS 页 + 设置 TCB + sys_arch_prctl(ARCH_SET_FS) + sys_set_tid_address；__libc_tls_init_rest 设置取消 handler）。TLS 模板通过 elf_loader PT_TLS 解析 + linker.ld 导出符号定位（无 auxv）。

实现：user/lib/tls.cc : __libc_tls_init / alloc_tls_block；kernel/bsd/elf_loader.c : elf_load PT_TLS

**CLONE_CHILD_SETTID 时序**：pthread_create 不能在 clone 返回后由父线程写 tcb->tid（子线程可能先退出写 0 + futex_wake，父线程后写 tid 覆盖 0 → lost wake-up）。内核 sys_clone 在子线程调度前写 `*(pid_t*)child_tid = child->pid`。

### FPU/SSE eager 上下文切换

创建时 xcore_fpu_alloc 调 bfc_alloc_page(1) 并初始化为合法 fxsave 镻像（memset 0 + MXCSR=0x1F80）。schedule() 的 C helper fpu_context_switch 在 switch_to 前调用：fxsave prev + fxrstor next，不设 CR0.TS。idle fpu_page=NULL 自动跳过。#NM handler fpu_lazy_switch 保留作兜底（TS 意外泄漏时 clts + fxrstor）。fork/clone 时 child memcpy parent 当前 FPU 快照（POSIX 语义）。内核保持 `-mno-sse`，用户态已移除 `-mno-sse`。

实现：kernel/xcore/sched.c : fpu_context_switch / xcore_fpu_alloc；arch/x64/trapentry.S : switch_to 不改

### do_exit 语义

贴 Linux do_exit 流程（kernel/bsd/syscall.c : do_exit_with_code）：

1. exit_code 写 xtask（D13 编码）+ proc（legacy dead write）
2. CPU 时间记账
3. 孤儿收养（用 mm->parent_pid 遍历 tasks[] reparent 到 init）
4. clear_tid_addr：写 0 + futex_wake 唤醒 joiner（ZOMBIE 之前，proc 存活）
5. thread_count/live_count atomic dec → thread_count 归零则 notify_parent（ppid 读到局部变量）
6. 设 ZOMBIE（scheduler_lock 保护）— 此后禁止再解引用 proc 或 sig（ZOMBIE gate）
7. notify_parent：SIGCHLD 投递 + wake_from_wait（用局部变量 ppid，不读 sig->parent_pid）
8. 唤醒等待 REQ/MSG reply 的进程（xtask 字段 only，不碰 proc）
9. schedule() 永不返回

**ZOMBIE gate 模式**：ZOMBIE 前把 sig/ppid 读到局部变量，ZOMBIE 后只用局部变量 + xtask 字段，不解引用 proc 或 signal_struct。

**mm_put/files_put/signal_put 不在 do_exit 做**：SMP 下设 ZOMBIE 后 schedule，task_reap 在另一 CPU 可能并发 kfree(proc)；资源释放集中到 sched_task_reap/proc_reap 单一 owner。

### sys_exit_group

设 signal->group_exit=true + group_exit_code → 遍历同 tgid 非 current 线程唤醒 BLOCKED → do_exit(status)。被唤醒线程走 check_pending_signals → group_exit 检查 → sys_exit_group(group_exit_code) 自行退出。

实现：kernel/bsd/syscall.c : sys_exit_group

### task_reap（waitpid 触发）

只回收 leader（waitpid 验证 tgid==pid）。leader 是最后退出线程（thread_count 归零保证）：

1. 释放 IOPM + FPU state(fpu_page) + recv 缓冲
2. mm_put（主动，do_exit 不做）→ 归零触发 mm_release
3. proc_reap：files_put + signal_put + kfree(proc) + proc=NULL
4. 遍历同 tgid 非 leader ZOMBIE：释放内核栈/IOPM/FPU + kfree(proc) + PCB 槽位清零
5. leader PCB 槽位清零（内核栈延迟到 xtask_alloc 复用 slot 时释放，SMP 栈竞态防护）

实现：kernel/xcore/sched.c : sched_task_reap；kernel/bsd/proc.c : proc_reap

### 信号与线程

两级 pending：

| 位图 | 位置 | 产生者 | 消费者 |
|------|------|--------|--------|
| proc.sig_pending | proc(per-task) | tgkill/pthread_kill/force_sig | 当前线程返回用户态前 |
| signal_struct.shared_pending | signal_struct(线程组共享) | kill(pid,sig)/pgsignal | 任意未阻塞该信号的线程 |

check_pending_signals（kernel/bsd/signal.c : check_pending_signals）：
1. tf->cs != 0x2B → return
2. proc->signal->group_exit → sys_exit_group(group_exit_code)（最高优先级）
3. 先查私有 sig_pending（含 SIGKILL/SIGSTOP 强制位，atomic 操作）
4. 再查共享 shared_pending（持 sig_lock，含 SIGKILL/SIGSTOP）
5. SIGCANCEL 特殊路径：检查 proc.cancel_handler，0 时 do_exit_with_code(sig&0x7f)（D13）；非 0 时投递到 cancel_handler 地址
6. 信号循环结束后 while(need_resched) schedule()（确保 resched 不遗漏）

sys_kill(pid>0) 投递到 signal->shared_pending + 唤醒同组未阻塞线程。sys_tgkill 投递到 per-task sig_pending + wake_process_any 唤醒目标线程（可打断任何 BLOCKED 状态，含 WAIT_FUTEX）。

信号投递：deliver_signal 在用户栈构建 rt_sigframe（siginfo + sigcontext），trapframe 修改为跳转 handler，sigreturn 系统调用恢复完整寄存器状态 + sig_blocked。

### futex 实现

全局 futex_table[64] hash bucket + bucket lock + futex_key。第一版只 anon key：`key = (mm->cr3, uaddr>>PAGE_SHIFT)`，同进程线程共享 mm 可匹配。shm/phys key 返回 -ENOSYS。

FUTEX_WAIT：get_futex_key → copy_from_user 验值 → bucket lock 内二次验值（防 lost wake-up）+ 入队 → 释放锁 → 设 WAIT_FUTEX + schedule → 唤醒后 return 0/-ETIMEDOUT。

FUTEX_WAKE：get_futex_key → bucket lock 内收集 waiter 到局部数组 → 释放锁 → 遍历数组 wake_from_wait。

FUTEX_REQUEUE / PI / requeue 返回 -ENOSYS。

实现：kernel/bsd/futex.c : sys_futex；kernel/bsd/futex.h

### pthread 用户态

pthread.cc（866 行）+ tls.cc（195 行）提供 practical complete pthread：

| 类 | 函数 | 状态 |
|---|------|------|
| 线程管理 | create/exit/join/detach/self/equal | ✅ |
| attr | init/destroy/getdetachstate/setdetachstate/getstack/setstack/getstacksize/setstacksize/getguardsize/setguardsize | ✅ |
| mutex | init/destroy/lock/trylock/unlock/timedlock（normal/errorcheck/recursive 3 types） | ✅ |
| mutexattr | init/destroy/gettype/settype | ✅ |
| cond | init/destroy/wait/signal/broadcast | ✅ |
| rwlock | init/destroy/rdlock/wrlock/unlock | ✅ |
| barrier | init/destroy/wait | ✅ |
| once | pthread_once | ✅ |
| cancel | pthread_cancel / cleanup_push/pop / setcancelstate/setcanceltype / testcancel | ✅ |
| TSD | key_create/getspecific/setspecific | ✅ |
| 信号 | pthread_kill / pthread_sigmask | ✅ |
| 其他 | setname_np | ✅ |

线程注册表（tid → thread_entry 映射），供 pthread_join 查找 clear_tid_addr。

PTHREAD_PROCESS_SHARED mutex/cond 返回 ENOSYS（anon key 不跨进程匹配）。

实现：user/lib/pthread.cc / user/lib/tls.cc / user/include/pthread.h / user/include/sys/tls.h

### syscall 编号

| 编号 | 名称 | 归属 | 说明 |
|------|------|------|------|
| 60 | SYS_CLONE | BSD | 线程/进程创建 |
| 61 | SYS_FUTEX | BSD | futex 等待/唤醒 |
| 62 | SYS_ARCH_PRCTL | BSD | ARCH_SET_FS/GET_FS |
| 63 | SYS_TGKILL | BSD | 线程级信号投递 |
| 64 | SYS_EXIT_GROUP | BSD | 线程组退出 |
| 65 | SYS_SET_TID_ADDRESS | BSD | clear_tid 地址注册 |
| 66 | SYS_GETTID | BSD | 获取线程 ID |
| 67 | SYS_SIGPROCMASK | BSD | 信号掩码操作 |
| 68 | SYS_PTHREAD_SET_CANCEL_HANDLER | BSD | cancel handler 注册 |
| 85 | SYS_SIGPENDING | BSD | 查询 pending 信号集 |

sys_getpid 返回 tgid，sys_gettid 返回 tid（单线程 tgid==tid 兼容）。NSIG=33，SIGCANCEL=32。

sys_fork(#54) 保留独立实现，不重写为 clone(0) wrapper。

sys_sigaction(#41) / sys_sigreturn(#42) / sys_kill(#40) / sys_sigprocmask(#67) / sys_sigpending(#85) 均在 signal.c 实现。

### 锁协议

零嵌套原则：sig_lock 和 futex bucket lock 不与其他锁同时持有。sys_kill/exit_group/futex_wake 采用"先改状态后释放再唤醒"（局部数组收集 waiter）。

| 路径 | 锁序列 |
|------|--------|
| check_pending_signals | sig_lock（读+消费 shared_pending），释放 |
| sys_kill(pid>0) | sig_lock（写 shared_pending），释放 → scheduler_lock（唤醒），释放 |
| sys_tgkill | 无 sig_lock（atomic）→ scheduler_lock（wake_process_any），释放 |
| do_exit | scheduler_lock(ZOMBIE)，释放 → clear_tid(无锁) → atomic dec → scheduler_lock(notify)，释放 |
| exit_group | sig_lock(group_exit)，释放 → scheduler_lock(唤醒同组)，释放 → do_exit |
| futex_wait | bucket lock(检查+入队)，释放 → scheduler_lock(BLOCKED)，释放 → schedule() |
| futex_wake | bucket lock(收集 waiter)，释放 → scheduler_lock(唤醒)，释放 |
| sigprocmask | 无锁（proc 字段，单线程访问） |
| sigpending | sig_lock（读 shared_pending），释放 |

完整锁层级见 [kernel_lock.md](kernel_lock.md)。

### 与其他模块的关系

| 模块 | 关系 | 文档 |
|------|------|------|
| 调度器 | xtask.run_node 嵌入 per-CPU run_queue | [schedule.md](schedule.md) |
| IPC | recv/REQ/MSG 状态 per-task，线程共享地址空间后仍独立 | [ipc.md](ipc.md) |
| PTY | sid/pgid/ctty 线程组共享 session 语义 | [terminal.md](terminal.md) |
| VFS | files.fd_table CLONE_FILES 时共享 | [vfs.md](vfs.md) |
| 进程管理 | fork/execve/exit/waitpid 单进程语义 | [proc.md](proc.md) |
| 锁模型 | 零嵌套锁序 | [kernel_lock.md](kernel_lock.md) |
| 内存管理 | mm ref counted，CLONE_VM 共享 | [mem.md](mem.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| sigsuspend | 需新 syscall + "原子换掩码并阻塞到信号"的等待点，避免 sigprocmask+pause 竞态窗口。是 sigwait 家族的基础 | 中 |
| sigwait/sigwaitinfo/sigtimedwait | 复用 sigsuspend 等待点 + "消费但不投递 handler"分支 + siginfo 返回 | 中 |
| sigaltstack | proc 加 sigaltstack/on_alt_stack 字段（触发 sizeof(proc) ABI guard + bsd_types.h 同步）+ SA_ONSTACK 投递路径 + sigreturn 清栈标志。建议最后做 | 低 |
| pthread_cond_timedwait | 超时条件等待 | 低 |
| pthread_atfork | fork handler 注册（prepare/parent/child） | 低 |
| pthread_spinlock | 自旋锁，纯用户态 atomic 实现 | 低 |
| PTHREAD_PROCESS_SHARED | 当前 anon key 不跨进程匹配返回 ENOSYS；需 shm/phys futex key 支持 | 低 |
| errno 全表对齐 Linux（策略 B） | 重排所有 errno 编号 + 全量审计内核 return -E…，单独立项 | 低 |
