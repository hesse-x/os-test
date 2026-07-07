# 进程管理

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | PCB 拆分 | task_t（调度实体）+ mm_t（地址空间）+ files_t（fd 表） | fork 共享地址空间和 fd 表需独立引用计数；为 CLONE_VM/CLONE_FILES 预留 |
| 2 | 退出状态 | ZOMBIE → REAPING → UNUSED | 父进程需获取退出码，立即回收丢失信息；REAPING 防止 waitpid 与 exit 竞争 |
| 3 | 资源回收时机 | 全部延迟到 sys_waitpid | 避免 sys_exit 中解映射 PML4 后 schedule 切换 CR3 前的时序问题 |
| 4 | 孤儿进程 | reparent 到 init（mm->parent_pid = init_pid） | init 调 waitpid(-1) 兜底回收所有孤儿 |
| 5 | 进程创建 | fork + execve（替代旧 sys_spawn） | 与 Linux 语义一致；spawn 已删除（slot 8 为 NULL） |
| 6 | fork 页拷贝 | COW（共享物理页 + PTE_COW 标记） | fork 延迟 O(PTE 修改) vs O(memcpy)，内存不翻倍；详见 [page.md](page.md) COW 章节 |
| 7 | execve 失败处理 | 先验证 ELF 再替换地址空间 | ELF 无效返回 -ENOEXEC，旧地址空间不受影响 |
| 8 | idle 进程 | task->mm = NULL，使用内核 PML4 | idle 无用户地址空间；所有访问 task->mm 的代码必须先检查 NULL |

### 核心数据结构

task_t（kernel/proc.h : task_t）— 调度实体，动态分配自 `xtask_cache` 专用 slab cache（`tasks[MAX_PROC]` 为指针数组，`pid == 数组下标`）
  pid : pid_t — 进程 ID（数组下标）
  state : proc_state_t — UNUSED / READY / RUNNING / BLOCKED / ZOMBIE / REAPING
  k_rsp : uint64_t — 内核栈保存的 RSP（switch_to 用）
  k_stack_top : uint64_t — 内核栈顶物理地址
  cr3 : uint64_t — cached PML4 物理地址（权威值在 mm->cr3）
  entry : uint64_t — 用户入口 RIP
  wait_event : wait_event_t — 阻塞原因
  tgid : pid_t — 线程组 ID（单线程时 == pid）
  mm : mm_t* — 地址空间指针（NULL 为 idle）
  assigned_cpu : int — 运行 CPU
  iopm : uint8_t* — IOPM 位图（NULL = deny all）
  exit_code : int32_t — 退出码（ZOMBIE 时有效）
  run_node / wait_node : list_node_t — 嵌入 per-CPU 就绪队列 / 定时器队列
  wait_deadline : uint64_t — sched_clock() 纳秒超时
  recv_buf[16][64] : uint8_t — 统一 recv 队列
  recv_lock : spinlock_t — recv 队列保护
  req_caller_pid / msg_caller_pid : pid_t — REQ/MSG 状态
  cpu_time_ns / last_sched : uint64_t — CPU 时间记账
  sig_pending : uint64_t — per-task 私有 pending（tgkill/pthread_kill 目标）
  sig_blocked : sigset_t — per-task 阻塞掩码
  sig_force_info : siginfo_t — force_sig 临时数据
  signal : signal_struct* — 线程组共享（shared_pending/action[]/group_exit；fork 独立拷贝，CLONE_SIGHAND ref++）
  alarm_deadline : uint64_t — alarm 超时（0=无，else sched_clock() ns 绝对值，放 xtask 供 timer handler 直接访问）
  sid / pgid : pid_t — session / process group（job control 预留）
  ctty : pty* — 控制终端（NULL = none）
  uid/euid/gid/egid/umask — POSIX 身份与权限（uint32_t，默认 0/0/0/0/0022，fork 继承）

sizeof(proc)=256（STATIC_ASSERT 验证，kernel/driver/bsd_types.h 需 byte-identical 镜像；files 偏移锁定 offsetof=184）

mm_t（kernel/proc.h : mm_t）— 地址空间，kmalloc 分配，独立引用计数
  cr3 : uint64_t — 权威 PML4 物理地址
  ref_count : int — COW/CLONE_VM 预留，初始=1
  files : files_t* — fd 表指针（独立引用计数）
  mmap_brk : uint64_t — mmap 区高水位（初始 0x800000）
  mmap_phys_brk : uint64_t — MAP_PHYSICAL 区高水位（初始 MAP_PHYSICAL_BASE）
  mmap_regions : mmap_region_t* — mmap 区域链表（含用户栈）
  parent_pid : pid_t — 父进程 PID

files_t（kernel/proc.h : files_t）— fd 表，独立引用计数
  fd_table[32] : file_t — per-process 文件描述符表
  ref_count : int — fork 共享时 +1，初始=1

### 关键流程

#### sys_exit（kernel/trap.c : sys_exit）

1. 保存退出码到 task->exit_code
2. 最终 CPU 时间记账
3. 孤儿收养：遍历 tasks[]，将 mm->parent_pid == proc->pid 的子进程 reparent 到 init_pid
4. 无父进程（mm==NULL 或 mm->parent_pid < 0）：直接 task_reap
5. 有父进程：设 ZOMBIE，通过 SIGCHLD 信号唤醒父进程（atomic or parent->sig.pending + scheduler_lock 入队）
6. 唤醒等待本进程 REQ reply 的进程（req_result = ESRCH）
7. schedule()，永不返回

#### sys_waitpid（kernel/bsd/syscall.c : sys_waitpid）

签名 `sys_waitpid(pid_t pid, int32_t *exit_code, int options)`。支持 pid > 0（指定子进程）和 pid == -1（任意子进程）：
- pid == -1：遍历 tasks[] 找 ZOMBIE 子进程，无则 BLOCKED on WAIT_CHILD
- pid > 0：验证 mm->parent_pid == current->pid，不是则 -ECHILD
- 找到 ZOMBIE：设 REAPING，拷贝退出码到用户指针，task_reap 回收
- 无 ZOMBIE：BLOCKED on WAIT_CHILD，等待 sys_exit 唤醒

`options` 三层已打通：libc inline `__syscall3` 把 options 放进 `rdx`（`user/include/syscall.h`），wrapper 转发（`user/lib/sys_wait.cc`），内核第 3 参读 `options`。当前只兑现 `WNOHANG`：

- **`WNOHANG`**：两条等待路径（pid==-1 / pid>=0）在「无 ZOMBIE、有 children」原本要 `schedule()` 阻塞处，按 `nohang = options & WNOHANG` 短路返回 0。无需新唤醒机制——复用现有 `schedule()`/`wake_process_any`。EINTR 语义不变（WNOHANG 不阻塞故无 EINTR；阻塞路径仍按 `deliv` 检查返回 `-EINTR`）。

退出码编码已就绪（`do_exit`：正常退出 `(code & 0xff) << 8`，信号致死 `sig & 0x7f`），`user/include/sys/wait.h` 据此提供 `WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG` 宏。`WUNTRACED/WCONTINUED` 及 `WIFSTOPPED/WSTOPSIG/WIFCONTINUED` 宏已定义但内核未兑现停止态上报，运行时不会触发真值（见下「停止态上报」）。

##### 停止态上报（待实现：WUNTRACED / WCONTINUED + WIFSTOPPED）

job control 基础，当前 Wayland 验收路径不依赖，待真实需求（Ninja 并行构建等）触发再做。

**数据结构**：`xtask_t` 状态枚举加 `STOPPED`（当前 `enum proc_state { RUNNABLE, BLOCKED, ZOMBIE, REAPING }` 无停止态），加字段记录停止信号与防重复上报：

```c
typedef enum proc_state { RUNNABLE, BLOCKED, ZOMBIE, REAPING, STOPPED } proc_state;
int32_t stop_sig;        // state==STOPPED 时有效
uint8_t stop_reported;   // 防止 waitpid 重复上报同一停止事件
```

**SIGSTOP/SIGTSTP/SIGCONT 默认动作**（`signal.c` check_pending_signals 的 SIG_DFL 分支，当前 `case SIGSTOP/SIGTSTP/SIGCONT: break;` 即忽略）：

```c
case SIGSTOP:
case SIGTSTP:
  proc->stop_sig = sig;
  proc->state = STOPPED;
  wake_parent_on_child_event(proc);   // 投 SIGCHLD + 唤醒 WAIT_CHILD
  schedule();                          // 让出 CPU，不返回用户态
  break;
case SIGCONT:
  if (proc->state == STOPPED) {
    proc->state = RUNNABLE;
    proc->stop_sig = 0;
    enqueue_runqueue(proc);
    wake_parent_on_child_event(proc);  // WIFCONTINUED 上报
  }
  break;
```

关键时序：SIGSTOP 在 `check_pending_signals`（运行中进程的内核态返回点）处理，此时持有 trapframe，置 STOPPED 后 `schedule()` 切走，SIGCONT 唤醒后从原 trapframe 返回用户态——**不需保存/恢复额外用户上下文**，trapframe 已是完整用户态快照。

**waitpid 停止上报**（`sys_waitpid` 扫描除 ZOMBIE 外识别 STOPPED）：

```c
if (child->state == STOPPED && !child->stop_reported &&
    (options & WUNTRACED)) {
  *exit_code_ptr = (child->stop_sig << 8) | 0x7f;  // WIFSTOPPED 编码
  child->stop_reported = 1;
  return child->pid;          // 不 reap，子进程仍存活
}
```

**WCONTINUED**：SIGCONT 处理时给父进程投 SIGCHLD，`sys_waitpid` 用单独的 `continued_reported` 标志 + `status == 0xffff` 编码上报一次。

**竞态与锁**：STOPPED 是稳定态（非终态），`sys_waitpid` 读 `child->state == STOPPED` 时**不设 REAPING、不 reap**，子进程仍可被 SIGCONT 唤醒。STOPPED↔RUNNABLE 与 waitpid 读 state 的竞态用 `scheduler_lock[child->cpu]` 保护（与现有 ZOMBIE 路径同锁）。唤醒父进程用 `wake_process_any`（已有），父进程在 WAIT_CHILD 上醒来重新扫描。

**SIGCHLD**：当前 SIG_DFL 是 `break`（忽略）。job control 需在子进程停止/继续/退出时给父进程 `sig_pending |= (1<<SIGCHLD)`，`deliver_signal_to(parent, SIGCHLD)` 即可，已有机制。完整 job control 还涉及 setpgid/tcgetpgrp 等进程组语义，超出本项范围。

#### sys_fork（kernel/proc.c : sys_fork）

1. 分配新 task_t 槽位（tasks_lock 保护）：`xtask_alloc(&child_pid)` 从 `next_pid` 起 ffz 扫描空槽（NULL 或 REAPING），返回新 xtask_t* 并写出 pid。消除旧 `proc - tasks` 指针算术 UB（静态数组时代遗留）
2. 创建新 mm_t：分配新 PML4，copy_page_table COW 共享用户页表（父 RW PTE 改为只读+PTE_COW，子 PTE 同样只读+PTE_COW，物理页 p_refcount++），flush 父 TLB
3. 深拷贝 files_t：逐 fd 复制 file_t，对应资源 ref_count++（pipe/shm/inode 等）
4. 深拷贝 mmap_regions 链表
5. 分配新内核栈，拷贝父进程 trapframe（rax=0 表示子进程返回值）
6. 设置 mm->parent_pid = current->pid，child->tgid = child->pid
7. 拷贝信号状态（blocked/action），清空 pending
8. 拷贝 sid/pgid/ctty/uid/euid/gid/egid/umask
9. 入队调度（scheduler_lock 保护）
10. 父进程返回子 PID，子进程返回 0

#### sys_execve（kernel/proc.c : sys_execve）

1. 内核打开 pathname（VFS/FAT32），fstat 获取文件大小
2. kmalloc 缓冲区，read 整个 ELF 到内核
3. 验证 ELF magic，无效则 kfree + 返回 -ENOEXEC
4. 关闭 FD_CLOEXEC 标记的 fd（遍历 fd_table）
5. mm_release_pages 释放旧地址空间（用户页+PML4+栈，跳过 SHM/MAP_PHYSICAL）
6. 分配新 PML4，拷贝内核条目，elf_load 加载新 ELF
7. 分配新用户栈，映射信号 trampoline 页
8. 原地修改 trapframe：rip=entry, rsp=stack_top
9. 更新 mm_t 字段（cr3, mmap_brk 重置）
10. kfree ELF 缓冲区，返回用户态执行新程序

### 生命周期

```
创建:
  mm_create() → kmalloc mm_t, ref_count=1, allocate PML4
  files_create() → kmalloc files_t, ref_count=1, fd_table init FD_NONE

fork:
  新 mm_t, ref_count=1（COW 共享物理页，独立 PML4；详见 [page.md](page.md) COW 章节）
  新 files_t, ref_count=1（逐 fd 复制 + ref_count++）

释放:
  mm_put(mm) → atomic --ref_count
    → >0: 不做任何事
    → ==0: mm_release(mm, owner_pid)
  mm_release(mm, owner_pid):
    mm_release_pages(): 释放用户页表+物理页（叶页 refcount_dec_and_test，减到 0 才 free；共享页减到 >0 不释放）+PML4
    files_put(mm->files): 递减 fd 表引用，归零则关闭所有 fd + kfree
    释放 mmap_regions + SHM 引用
    devtmpfs_cleanup_pid / irq_owner_cleanup
    唤醒等待 REQ/MSG reply 的进程
    kfree(mm)

  task_reap(task):
    释放内核栈（2 页 bfc_free）
    释放 IOPM
    mm_put(task->mm)
    释放 recv 队列 RECV_MSG 缓冲区
    清信号状态
    设 REAPING（动态化：不立即 kmem_cache_free xtask_t 对象，保留 pid=-1 / mm=NULL /
    proc=NULL 等所有 reader 守卫语义，无锁读 112+ 处不需 refcount 仍安全；
    xtask_alloc 复用该槽时 reclaim_lazy_resources 释放 k_stack/fpu_page 后
    kmem_cache_free 旧对象再 alloc 新对象）
```

### 锁协议

| 操作 | 持锁 | 说明 |
|------|------|------|
| sys_exit 孤儿收养 | tasks_lock | 保护 mm->parent_pid 修改 |
| sys_exit 设 ZOMBIE | scheduler_lock[cpu] | 保护 state |
| sys_exit 唤醒父进程 | scheduler_lock[pcpu] | 入队父进程 |
| sys_waitpid 查找子进程 | tasks_lock | 读 mm->parent_pid |
| sys_waitpid 设 REAPING | scheduler_lock[cpu] | 保护 state |
| sys_fork 分配槽位 | tasks_lock | 保护空闲槽位 |
| sys_fork 入队 | scheduler_lock[cpu] | 保护 run_queue |

锁获取顺序（防死锁）：tasks_lock → scheduler_lock[cpu]。sys_exit 中唤醒父进程在释放本进程 scheduler_lock 之后、不与本进程锁交互。

### 与其他模块的关系

- 调度器：task_t.run_node 嵌入 per-CPU run_queue，详见 [schedule.md](schedule.md)
- IPC：recv 队列 / REQ / MSG 状态在 task_t 中，详见 [ipc.md](ipc.md)
- PTY：task_t.sid/pgid/ctty 用于 session/job control，详见 [terminal.md](terminal.md)
- 信号：proc 的 sig_pending/sig_blocked/signal 详见 ipc.md 信号章节
- VFS：files_t.fd_table 通过 fd 管理文件/pipe/socket/tty，详见 [vfs.md](vfs.md)

### 系统调用

| 编号 | syscall | 签名 | 行为 |
|------|---------|------|------|
| 6 | sys_exit | `sys_exit(int32_t exit_code)` | 进程退出，无父进程直接回收，有父进程 ZOMBIE 等待回收 |
| 7 | sys_waitpid | `sys_waitpid(pid_t pid, int32_t *exit_code, int options)` | 等待子进程退出并回收；pid==-1 等任意子进程；`options` 支持 `WNOHANG`（无就绪子立即返回 0）。停止态上报（`WUNTRACED`/`WCONTINUED`）见待完成项 |
| 57 | sys_fork | `sys_fork()` | 创建子进程（深拷贝地址空间+fd表） |
| 58 | sys_execve | `sys_execve(const char *pathname)` | 替换进程映像（内核打开 ELF → 释放旧空间 → 加载新 ELF） |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| COW fault handler | #PF 需识别 PTE_COW 并 resolve（分配新页/恢复 RW），当前写共享页仍 SIGSEGV | 高 |
| CLONE_VM / 线程 | task_t/mm_t/files_t 拆分已预留，需实现 pthread 级别的线程创建（共享地址空间） | 中 |
| ~~WNOHANG 非阻塞 waitpid~~ | ~~当前 waitpid 只有阻塞模式~~ | 已实现：`options` 三层打通（libc inline `__syscall3` + wrapper 转发 + 内核 `sys_waitpid` 读 `options`），两条等待路径（pid==-1 / pid>=0）在无 ZOMBIE 时按 `WNOHANG` 短路返回 0。`wait.h` 补齐 `WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG` 宏（内核 wait status 编码已就绪） |
| 停止态上报（`WUNTRACED`/`WCONTINUED`） | `xtask_t` 加 `STOPPED` 状态 + `stop_sig`/`stop_reported` 字段；SIGSTOP/SIGTSTP/SIGCONT 默认动作（当前 `signal.c` SIG_DFL 分支为 `break`=忽略）改为置 STOPPED + `schedule()` / SIGCONT 唤醒回 RUNNABLE；`sys_waitpid` 扫描识别 STOPPED 并按 `(stop_sig<<8)|0x7f` 编码上报（不 reap，子进程仍存活）。涉及 SMP 锁顺序（state 读写走 `scheduler_lock[child->cpu]`）。设计详见 `extend_wait.md` P2/P3。job control 基础，待真实需求触发 | 中 |
| execve argv/envp 传递 | 当前 argv=NULL, envp=NULL，需支持命令行参数和环境变量传入新进程 | 高 |
| ELF 数据内核复制 | execve 当前直接用用户态指针（同步调用安全），防御性编程应先 kfree 到内核再加载 | 低 |
| 进程优先级 | 当前所有进程同等优先级，需 nice 值或实时优先级支持 | 低 |
| 用户栈仅 4KB 无 guard page | 栈溢出触发 #PF 被 kill，应扩栈 + 加 guard page | 中 |
| 内核栈仅 8KB | 深层调用路径偏紧，应扩栈或加溢出检测 | 中 |
| ~~pid 未校验上界~~ | ~~procs[pid] 未检查 pid >= MAX_PROC~~ | 已修复（动态化）：`task_get(pid)` 编码 `pid >= 0 && pid < MAX_PROC` 守卫到运行时（debug ASSERT / release 零开销），所有 `tasks[pid]` 访问点统一走该 helper。优先级由高→已闭环 |
| cross-process files_t UAF | SCM_RIGHTS 跨进程读 fd_table 时，目标进程 exit + kfree(files_t) 导致 UAF。缓解：`files_put` 中 `synchronize_rcu()` 保证 grace period 后才 kfree | 低 |
