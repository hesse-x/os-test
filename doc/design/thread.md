# 多线程设计（task_t + mm_t + clone + pthread）

## 设计决策总结

| # | 决策 | 结论 |
|---|------|------|
| 1 | kthread vs 用户态线程 | 只做用户态 pthreads（1:1 模型），不做 kthread。微内核策略全在用户态，无内核后台任务需求 |
| 2 | 结构拆分 | 拆成 task_t（调度实体）+ mm_t（地址空间+资源），Linux task/mm 路线 |
| 3 | 创建接口 | clone() 系统调用，贴合 Linux。fork = clone(0)，spawn 内部复用 clone |
| 4 | mm_t 引用计数 | mm->ref_count，clone(CLONE_VM)++，exit--，归零释放 |
| 5 | 退出语义 | sys_exit = exit_group（杀全部线程），pthread_exit 只退自己。waitpid 回收进程，pthread_join 回收线程 |
| 6 | fork + execv | 和线程一起做，fork 第一版全页拷贝，COW 后续优化 |
| 7 | TLS | 中等深度：FS_BASE + ELF TLS 段（.tdata/.tbss），context switch 保存/恢复 FS_BASE |
| 8 | FPU/SSE | lazy FPU 上下文切换（CR0.TS + #NM），内核仍 -mno-sse，用户态可用 SSE |
| 9 | clone 签名 | `clone(flags, stack, parent_tid, child_tid, tls)`，与 Linux 一致 |
| 10 | task 表大小 | 维持 MAX_PROC=64，tid==数组下标，加 tgid 字段。动态扩容后续 |
| 11 | fd_table 归属 | 内嵌 mm_t，CLONE_FILES 和 CLONE_VM 绑定，不独立 files_struct |
| 12 | 信号与线程 | 完整 Linux 语义：shared_pending（mm_t）+ per-task pending，tgkill 线程定向信号 |
| 13 | execv 线程行为 | 设 mm->group_exit 标志，同组线程在 trapret 路径自行退出（Linux 方式） |
| 14 | proc_reap 重构 | 两层回收：task_reap（调度实体）+ mm_release（地址空间+资源） |
| 15 | syscall 编号 | 追加 46-52，不重排。NR_SYSCALL=53 |
| 16 | futex | 全局 hash 表 + task_t 内嵌等待节点，第一版 FUTEX_WAIT + FUTEX_WAKE |
| 17 | 调度器 | 完全无感知线程，task_t 平等调度，FIFO round-robin 不变 |
| 18 | waitpid | 只回收线程组 leader（tgid==tid），非 leader 由 pthread_join 回收 |

## 结构体设计

### task_t（调度实体）

**重要**：前 5 个字段（tid/state/k_rsp/k_stack_top/cr3）必须保持固定偏移，供 `switch_to` 汇编使用（k_rsp 在 offset 8，cr3 在 offset 24）。cr3 在 task_t 中缓存一份供汇编快速切换，权威来源在 mm->cr3。

```c
struct task_t {
    pid_t tid;              // 全局唯一 ID（== 数组下标）
    proc_state_t state;     // READY/RUNNING/BLOCKED/ZOMBIE/REAPING
    uint64_t k_rsp;         // switch_to 保存的内核 RSP  (offset 8)
    uint64_t k_stack_top;   // 内核栈顶（8KB 高端）
    uint64_t cr3;           // PML4 物理地址 (offset 24, cached from mm->cr3)
    pid_t tgid;             // 线程组 ID（= 主线程 tid，单线程时 tgid==tid）
    uint64_t entry;         // 用户入口 RIP
    wait_event_t wait_event;// 阻塞原因
    int assigned_cpu;       // 绑定的 CPU
    list_node_t run_node;   // per-CPU run_queue 链表节点
    list_node_t wait_node;  // per-CPU timer_queue 链表节点
    uint64_t wait_deadline; // sched_clock() 超时纳秒
    uint8_t  wait_timed_out;// 超时标志
    struct mm_t *mm;        // 指向地址空间（线程组共享，idle 进程为 NULL）
    uint8_t *iopm;          // per-task IOPL 位图

    // === per-task IPC 状态 ===
    uint8_t  recv_buf[RECV_QUEUE_SIZE][RECV_MSG_SIZE];
    uint32_t recv_head;
    uint32_t recv_tail;
    spinlock_t recv_lock;
    pid_t    req_caller_pid;
    void    *req_reply_buf;
    int32_t  req_result;
    pid_t    req_target_pid;
    void    *msg_reply_buf;
    size_t   msg_reply_len;
    pid_t    msg_caller_pid;
    int32_t  msg_result;
    pid_t    msg_target_pid;

    // === per-task 信号 ===
    uint64_t sig_pending;       // 私有 pending（pthread_kill 产生）
    uint64_t sig_blocked;       // 信号阻塞掩码

    // === per-task 退出 ===
    int32_t  exit_code;         // 退出码
    pid_t    clear_tid_addr;    // CLONE_CHILD_CLEARTID 用户态地址（0=无）

    // === CPU 时间 ===
    uint64_t cpu_time_ns;
    uint64_t last_sched;

    // === FPU 状态 ===
    uint8_t  used_fpu;          // 该 task 是否使用过 FPU
    void    *fpu_state;         // fxsave 区域（lazy 分配）

    // === 信号 handler 状态（per-task，暂保留在 task 中） ===
    int      sig_have_handler;  // 是否有用户态 handler 待调起
    uint64_t sig_saved_rip;
    uint64_t sig_saved_rsp;
    uint64_t sig_saved_rflags;

    // === futex 等待 ===
    list_node_t futex_node;     // 挂在 futex hash bucket 链表上
    uint64_t futex_uaddr;       // 等待的用户态地址

    // === FS_BASE (TLS) ===
    uint64_t fs_base;           // 保存的 FS_BASE 值
};
```

### mm_t（地址空间 + 资源）

```c
struct mm_t {
    uint64_t cr3;              // PML4 物理地址
    int ref_count;             // 引用计数（clone(CLONE_VM)++）

    // === fd 表 ===
    struct file fd_table[MAX_FD];

    // === mmap ===
    uint64_t mmap_brk;         // mmap 高水位（初始 0x800000）
    uint64_t mmap_phys_brk;    // MAP_PHYSICAL 高水位
    mmap_region *mmap_regions; // mmap 区域链表

    // === 信号（线程组共享） ===
    struct {
        uint64_t shared_pending;   // 进程级 pending（kill 产生）
        spinlock_t sig_lock;       // 保护 shared_pending
        struct sigaction action[NSIG];
    } sig;

    // === 进程关系 ===
    pid_t parent_pid;          // 父进程 PID（getppid 返回此值）

    // === 线程组退出 ===
    uint8_t group_exit;        // exit_group 标志
    int32_t group_exit_code;   // exit_group 退出码
};
```

## 关键实现细节（Phase 1 已验证）

- **cr3 双存储**：task_t.cr3 是 mm->cr3 的缓存，供 switch_to 汇编直接读写。CR3 切换时使用 `task->mm->cr3`（权威来源），process_create_elf 中设置 `task->cr3 = mm->cr3` 同步。
- **idle 进程 mm=NULL**：create_idle_process 设置 `task->mm = nullptr`，使用内核 PML4 直接运行。所有访问 `tasks[i].mm->` 的代码必须先检查 `tasks[i].mm &&`，否则会触发 PAGE FAULT。
- **mm_t 生命周期**：mm_create() 分配并初始化 ref_count=1；mm_put() 原子减引用计数，归零时调用 mm_release() 释放全部资源；proc_reap 调用 mm_put() 而非直接 mm_release()。
- **全局变量重命名**：`procs[]` → `tasks[]`，`procs_lock` → `tasks_lock`，`current_proc` → `current_task`（宏定义在 smp.h）。

## syscall 新增

| 编号 | syscall | 签名 | 说明 |
|------|---------|------|------|
| 46 | sys_clone | `clone(flags, stack, parent_tid, child_tid, tls)` | 创建线程/进程 |
| 47 | sys_execv | `execv(pathname, argv, envp)` | 替换进程映像 |
| 48 | sys_futex | `futex(uaddr, op, val, timeout, uaddr2, val3)` | 用户态互斥 |
| 49 | sys_arch_prctl | `arch_prctl(code, addr)` | ARCH_SET_FS / ARCH_GET_FS |
| 50 | sys_tgkill | `tgkill(tgid, tid, sig)` | 线程定向信号 |
| 51 | sys_exit_group | `exit_group(status)` | 杀整个线程组 |
| 52 | sys_set_tid_address | `set_tid_address(tidptr)` | 设置 clear_tid_addr |
| 57 | sys_gettid | `gettid()` | 返回线程 ID |

NR_SYSCALL = 58

### clone flags（第一版支持）

| flag | 值 | 说明 |
|------|----|------|
| CLONE_VM | 0x00000100 | 共享地址空间 |
| CLONE_FILES | 0x00000400 | 共享 fd_table |
| CLONE_SIGHAND | 0x00000800 | 共享信号 handler |
| CLONE_THREAD | 0x00010000 | 放入同一线程组 |
| CLONE_PARENT_SETTID | 0x00100000 | 写子线程 tid 到 parent_tid |
| CLONE_CHILD_CLEARTID | 0x00200000 | 线程退出时清 *child_tid + futex_wake |
| CLONE_SETTLS | 0x00080000 | 设置新线程 FS_BASE |

第一版 CLONE_VM/FILES/SIGHAND/THREAD 绑定一起使用，不支持独立组合。

### clone 行为

```
flags 含 CLONE_VM:
  1. 分配新 task_t（新 tid）
  2. 新 task_t->mm = current->mm, mm->ref_count++
  3. 新 task_t->tgid = current->tgid
  4. 分配新内核栈
  5. 构建 trapframe: rip=entry(from stack param), rsp=stack param, rdi=tls
  6. CLONE_SETTLS: wrmsr(MSR_FS_BASE, tls) 在新线程首次运行时生效
  7. CLONE_PARENT_SETTID: *parent_tid = 新 tid
  8. CLONE_CHILD_CLEARTID: task->clear_tid_addr = child_tid
  9. 入队调度

flags 不含 CLONE_VM (fork):
  1. 分配新 task_t + 新 mm_t
  2. 拷贝用户页表（第一版全页拷贝，无 COW）
  3. 拷贝 fd_table
  4. 新 task->tgid = 新 task->tid（新线程组 leader）
  5. mm->parent_pid = current->tgid
  6. 构建 trapframe: rip=当前返回地址, rsp=当前栈（fork 返回点）
  7. 入队调度
```

## 退出语义

### sys_exit_group(status) / sys_exit(status)

```
1. mm->group_exit = true, mm->group_exit_code = status
2. 遍历 tasks[]，找同 tgid 的其他线程:
   - 若 BLOCKED: 唤醒（走退出路径）
   - trapret 检查 mm->group_exit → 自行退出
3. 当前 task 设 ZOMBIE, exit_code = status
4. 通知父进程（RECV_NOTIFY / SIGCHLD）
```

### pthread_exit(value_ptr)

```
1. 当前 task 设 ZOMBIE, exit_code = (int32_t)value_ptr
2. mm->ref_count--
3. clear_tid_addr: *clear_tid_addr = 0 + futex_wake(clear_tid_addr, 1)
4. 若 mm->ref_count == 0（最后一个线程）→ 通知父进程
5. schedule() 让出 CPU
```

### trapret 路径拦截

```
__trapret 返回用户态前:
  if (current->mm->group_exit)
    → task_exit(current->mm->group_exit_code)  // 自行退出
  if (signal pending)  // 信号检查
    → deliver_signal()
```

## 资源回收

### task_reap(task_t)

```
1. 释放内核栈（2 页）
2. 释放 IOPM
3. clear_tid_addr: *addr = 0 + futex_wake(addr, 1)（唤醒 joiner）
4. 释放 FPU 状态（fpu_state）
5. mm_put(task->mm) → 若 ref_count == 0 → mm_release(mm)
6. PCB 槽位清零（tid = -1）
```

### mm_release(mm_t)

```
1. 释放用户页表（PML4 walk，跳过 SHM 和 MAP_PHYSICAL 页）
2. 释放 PML4 页
3. 释放 mmap_region 链表 + SHM 引用（shm_put）
4. 关闭所有 fd:
   - FD_PIPE: ref_count--, wake 对端, 归零 kfree
   - FD_FILE: kernel_msg_send CLOSE
   - FD_SOCKET: sock_close
   - FD_SHM: shm_put
5. dev_table_cleanup(pid)
6. irq_owner_cleanup(pid)
7. 唤醒等待 REQ/MSG reply 的进程
8. 释放 recv 队列 RECV_MSG 内核缓冲区
9. kfree(mm) 释放 mm_t 本身
```

## futex 实现

### 数据结构

```
#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE 64

struct futex_bucket {
    list_node_t waiters;    // 等待线程链表
    spinlock_t lock;
};

futex_bucket futex_table[FUTEX_HASH_SIZE];
```

### hash 函数

```c
static inline uint32_t futex_hash(uint64_t uaddr) {
    return (uaddr >> 3) & (FUTEX_HASH_SIZE - 1);
}
```

### FUTEX_WAIT(uaddr, expected, timeout)

```
1. atomic_load(uaddr)，若 != expected → 返回 -EAGAIN
2. bucket = &futex_table[futex_hash(uaddr)]
3. spin_lock(&bucket->lock)
4. 再次检查 *uaddr（防止 lost wake-up）
5. current->futex_uaddr = uaddr
6. list_push_back(&bucket->waiters, &current->futex_node)
7. current->state = BLOCKED, wait_event = WAIT_FUTEX
8. 若 timeout != NULL: 设 wait_deadline
9. spin_unlock(&bucket->lock)
10. schedule()
11. （唤醒后返回 0 或 -ETIMEDOUT）
```

### FUTEX_WAKE(uaddr, count)

```
1. bucket = &futex_table[futex_hash(uaddr)]
2. spin_lock(&bucket->lock)
3. 遍历 bucket->waiters，找 futex_uaddr == uaddr 的线程
4. 唤醒最多 count 个（设 READY + 入 run_queue）
5. spin_unlock(&bucket->lock)
6. 返回实际唤醒数
```

## FPU/SSE lazy 上下文切换

### 初始化

```
isr_init 中:
  cr0 |= CR0_TS  // 设置 Task Switched 位
  注册 #NM handler (vector 7)
```

### #NM handler (Device Not Available)

```
1. 若 current->used_fpu:
     若 current->fpu_state == NULL:
       current->fpu_state = kcalloc(1, 512)  // fxsave 区域
     fxsave 到 current->fpu_state
2. 若 current->fpu_state != NULL:
     fxrstor 从 current->fpu_state
3. current->used_fpu = 1
4. cr0 &= ~CR0_TS  // 清 TS，允许 SSE 指令
```

### switch_to 中

```
if (prev->used_fpu) {
    fxsave 到 prev->fpu_state
}
cr0 |= CR0_TS  // 切换后设 TS，新进程首次用 SSE 触发 #NM
```

### 编译 flags 变更

- 内核：保持 `-mno-sse -mno-sse2 -mno-mmx`
- 用户态：**移除** `-mno-sse -mno-sse2 -mno-mmx`，允许 SSE 指令生成

## TLS 实现

### clone 时 TLS 分配

```
CLONE_SETTLS:
  1. 为新线程 mmap 分配 1 页 TLS 区域
  2. 拷贝 .tdata 初始镜像到 TLS 页头部
  3. .tbss 部分清零
  4. TLS 页开头存放 TCB：
     struct tcb {
         void *self;    // 指向自身（%fs:0 返回此地址）
         pid_t tid;     // 线程 ID
         // 以后可扩展: locale, errno 等
     };
  5. 新线程 trapframe 中设 FS_BASE = TLS 页地址
```

### context switch FS_BASE 保存/恢复

```
switch_to(prev, next):
  // 保存
  prev_fs = rdmsr(MSR_FS_BASE)
  prev->fs_base = prev_fs
  // 恢复
  wrmsr(MSR_FS_BASE, next->fs_base)
```

### task_t 新增字段

```
uint64_t fs_base;    // 保存的 FS_BASE 值
```

## 信号与线程

### 两级 pending

| 位图 | 位置 | 产生者 | 消费者 |
|------|------|--------|--------|
| task->sig_pending | task_t | pthread_kill / tgkill | 当前线程返回用户态前检查 |
| mm->sig.shared_pending | mm_t | kill(pid, sig) | 任意未阻塞该信号的线程消费 |

### 投递逻辑（返回用户态前）

```c
int pick_signal(task_t *t) {
    // 1. 先查私有 pending
    uint64_t pending = t->sig_pending & ~t->sig_blocked;
    if (pending) {
        int sig = __builtin_ctzl(pending);
        t->sig_pending &= ~(1UL << sig);
        return sig;
    }
    // 2. 再查共享 pending
    spin_lock(&t->mm->sig.sig_lock);
    pending = t->mm->sig.shared_pending & ~t->sig_blocked;
    if (pending) {
        int sig = __builtin_ctzl(pending);
        t->mm->sig.shared_pending &= ~(1UL << sig);
        spin_unlock(&t->mm->sig.sig_lock);
        return sig;
    }
    spin_unlock(&t->mm->sig.sig_lock);
    return 0; // 无信号
}
```

### kill(pid, sig) 投递

```c
// pid 指向线程组 leader
task_t *leader = &tasks[pid];
spin_lock(&leader->mm->sig.sig_lock);
leader->mm->sig.shared_pending |= (1UL << sig);
spin_unlock(&leader->mm->sig.sig_lock);
// 唤醒所有未阻塞该信号的线程
for (int i = 0; i < MAX_PROC; i++) {
    if (tasks[i].tid >= 0 && tasks[i].mm == leader->mm
        && !(tasks[i].sig_blocked & (1UL << sig))
        && tasks[i].state == BLOCKED) {
        wake_task(&tasks[i]);
    }
}
```

## 实现阶段

### 阶段 1：结构拆分（✅ 已完成）

- 定义 task_t + mm_t 结构体
- `procs[x]` → `tasks[x]`，`procs_lock` → `tasks_lock`
- `current_proc` → `current_task`
- 所有引用点适配（schedule/switch_to/trap_dispatch/syscall_dispatch/proc_reap/ahci/xhci/socket/kernel）
- task_t 初始化时通过 mm_create() 创建 mm_t（现有进程流程不变）
- mm_t 生命周期：mm_create() → mm_put() → mm_release()
- proc_reap 重构为：释放内核栈 → mm_put()（归零触发 mm_release） → 释放 IOPM/FPU → 清 PCB
- idle 进程 mm=NULL，使用内核 PML4
- NR_SYSCALL 从 50 升到 53（slot 50-52 预留，入口 nullptr）
- **验证**：编译通过（release + debug）+ shell/hello 正常运行

### 阶段 2：FPU/SSE（✅ 已完成）

- 添加 read_cr0/write_cr0 内联函数（arch/x64/utils.h）
- isr_init 中 idt_install 之后设置 CR0.TS
- trap_dispatch 添加 #NM (trapno==7) 处理：lazy 分配 fpu_state + fxrstor + 清 CR0.TS
- schedule() 中 fpu_context_switch()：fxsave prev + 设 CR0.TS（C helper，不修改 switch_to 汇编）
- switch_to CLONE_VM 优化：比较 CR3 值，相同则跳过 CR3 切换（arch/x64/trapentry.S）
- 移除用户态编译 `-mno-sse -mno-sse2 -mno-mmx`（build_script/cmake/user_rules.cmake）
- **验证**：编译通过 + shell/hello 正常运行

### 阶段 3：clone + fork + execv + exit_group（✅ 已完成）

- WAIT_FUTEX 加入 wait_event_t 枚举
- cpu_local_t 新增 current_tf (void*) 字段，trap_dispatch/syscall_dispatch 保存
- NR_SYSCALL 从 53 升到 57，syscall 表填充 slot 50-56
- sys_clone 实现（CLONE_VM 线程路径 + fork 路径）：copy trapframe(rax=0) + 新内核栈 + process_entry
- fork 路径：copy_page_table（user_mapping.cc，深拷贝用户页表，SHM/MAP_PHYSICAL 共享）+ copy_fd_table + copy_mmap_regions
- sys_execv 实现：第一版 execv(pathname, elf_data, elf_size)，用户态提供 ELF 数据；FD_CLOEXEC 关闭 + mm_release_pages 释放旧地址空间 + 新 PML4 + elf_load + 新栈 + 原地修改 trapframe
- sys_exit_group 实现：设 mm->group_exit + 唤醒同组 BLOCKED 线程 + sys_exit
- check_pending_signals 添加 group_exit 检查（线程自行退出）
- sys_exit 扩展：非 leader 线程退出时 mm->ref_count--，仅最后一个线程通知父进程
- mm_release 拆分为 mm_release_pages + mm_release（供 execv 复用）
- proc_reap 重排：clear_tid_addr 写 0 + futex_wake 在 mm_put 之前
- futex 实现：全局 futex_table[64] + FUTEX_WAIT（阻塞等待）+ FUTEX_WAKE（唤醒等待者）
- sys_arch_prctl：ARCH_SET_FS（fs_base + wrmsr）+ ARCH_GET_FS
- sys_tgkill：线程定向信号（设置 per-task sig_pending + 唤醒）
- sys_set_tid_address：设置 clear_tid_addr
- FS_BASE 保存/恢复加入 schedule()（rdmsr/wrmsr MSR_FS_BASE）
- common/syscall.h 新增 CLONE_* flags、FUTEX_* ops、ARCH_* codes、内联封装
- ENOEXEC 加入 common/errno.h
- **验证**：编译通过（release + debug）

### 阶段 4：pthread 支撑（✅ 已完成）

- sys_futex（FUTEX_WAIT + FUTEX_WAKE）— Phase 3 中实现
- sys_arch_prctl（ARCH_SET_FS / ARCH_GET_FS）— Phase 3 中实现
- sys_tgkill — Phase 3 中实现
- sys_set_tid_address — Phase 3 中实现
- TLS 页分配 + FS_BASE 保存/恢复 — Phase 3 中实现
- 信号完整语义（shared_pending + tgkill）— Phase 3 中实现
- sys_getpid 改为返回 tgid（进程 ID），新增 SYS_GETTID(57) 返回 tid
- sys_kill 重构：pid 指向线程组 leader 时投递到 mm->sig.shared_pending（进程级信号），非 leader 线程投递到 task->sig_pending（线程定向信号）
- _start 中调用 __libc_tls_init()：为主线程分配 TLS 页 + 设置 TCB + sys_arch_prctl(ARCH_SET_FS) + sys_set_tid_address(&tcb->tid)
- 新增 user/include/pthread.h：pthread_t、pthread_mutex_t、pthread_cond_t 及相关常量
- 新增 user/lib/pthread.cc：
  - pthread_create：分配线程栈(64KB) + TLS 页 + thread_start info，内联 asm clone + 子线程直接跳转 __pthread_start
  - pthread_join：futex 等待 tcb->tid 归零（proc_reap 清零 + futex_wake），通过线程注册表查找 clear_tid_addr
  - pthread_exit：sys_exit
  - pthread_self：sys_gettid
  - pthread_mutex_t：基于 __atomic_exchange + futex WAIT/WAKE
  - pthread_cond_t：基于 seq counter + futex WAIT/WAKE
  - 线程注册表（tid → clear_tid_addr 映射），供 pthread_join 查找
- start.cc 改为 sys_exit_group（替代 sys_exit），确保多线程进程退出时杀全部线程
- user/include/unistd.h 新增 gettid()，user/lib/unistd.cc 实现
- common/syscall.h 新增 SYS_GETTID(57) + sys_gettid 内联封装
- NR_SYSCALL 从 57 升到 58
- **验证**：编译通过（release + debug）
- **验证**：pthread_create/mutex/join 跑通

### 待办：fd_table RCU 读路径完善（CLONE_FILES 前置）

当前 fd_table RCU 读路径（per-fd 短临界区 + 局部拷贝）依赖单线程前提：同一进程的 fd_table 不会被并发修改。CLONE_FILES 引入共享 fd_table 后，需补齐以下内容以完全对齐 Linux：

- **per-fd refcount 保护对象生命周期**：RCU 拷贝 `struct file` 后，`inode`/`sock`/`pipe`/`pty` 指针可能被 close 释放。Linux 通过 `get_file()`（bump `f_count`）保护，我们在 RCU unlock 后到操作完成期间持有引用计数，操作完毕 `fput()` 释放。需给 `unix_sock` 和 `pty` 补充 refcount（当前仅 `inode` 和 `pipe` 有）。
- **offset 写回原子性**：当前 offset 写回持 `fd_lock`（与 Linux `f_pos_lock` 粒度对齐），CLONE_FILES 后多线程共享 fd_table 时可能竞争。如竞争严重，可细化到 per-file `f_pos_lock`。
- **close + `synchronize_rcu`**：close 设置 `fd_table[fd].type = FD_NONE` 后需 `synchronize_rcu()` 确保所有 RCU 读者完成，再释放底层对象（inode/sock/pipe）。当前单线程下天然安全，CLONE_FILES 后必须显式等待。
