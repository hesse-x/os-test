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

task_t（kernel/proc.h : task_t）— 调度实体，数组内嵌 `tasks[MAX_PROC]`
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
  sig : signal_state — 信号子系统（pending/blocked/action[]）
  sig_force_info : siginfo_t — force_sig 临时数据
  sid / pgid : pid_t — session / process group（job control 预留）
  ctty : pty* — 控制终端（NULL = none）

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

#### sys_waitpid（kernel/trap.c : sys_waitpid）

支持 pid > 0（指定子进程）和 pid == -1（任意子进程）：
- pid == -1：遍历 tasks[] 找 ZOMBIE 子进程，无则 BLOCKED on WAIT_CHILD
- pid > 0：验证 mm->parent_pid == current->pid，不是则 -ECHILD
- 找到 ZOMBIE：设 REAPING，拷贝退出码到用户指针，task_reap 回收
- 无 ZOMBIE：BLOCKED on WAIT_CHILD，等待 sys_exit 唤醒

#### sys_fork（kernel/proc.c : sys_fork）

1. 分配新 task_t 槽位（tasks_lock 保护）
2. 创建新 mm_t：分配新 PML4，copy_page_table COW 共享用户页表（父 RW PTE 改为只读+PTE_COW，子 PTE 同样只读+PTE_COW，物理页 p_refcount++），flush 父 TLB
3. 深拷贝 files_t：逐 fd 复制 file_t，对应资源 ref_count++（pipe/shm/inode 等）
4. 深拷贝 mmap_regions 链表
5. 分配新内核栈，拷贝父进程 trapframe（rax=0 表示子进程返回值）
6. 设置 mm->parent_pid = current->pid，child->tgid = child->pid
7. 拷贝信号状态（blocked/action），清空 pending
8. 拷贝 sid/pgid/ctty
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
    清 PCB 槽位（pid=-1, state=UNUSED）
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
- 信号：task_t.sig 嵌入 signal_state，详见 ipc.md 信号章节
- VFS：files_t.fd_table 通过 fd 管理文件/pipe/socket/tty，详见 [vfs.md](vfs.md)

### 系统调用

| 编号 | syscall | 签名 | 行为 |
|------|---------|------|------|
| 6 | sys_exit | `sys_exit(int32_t exit_code)` | 进程退出，无父进程直接回收，有父进程 ZOMBIE 等待回收 |
| 7 | sys_waitpid | `sys_waitpid(pid_t pid, int32_t *exit_code)` | 等待子进程退出并回收；pid==-1 等任意子进程 |
| 57 | sys_fork | `sys_fork()` | 创建子进程（深拷贝地址空间+fd表） |
| 58 | sys_execve | `sys_execve(const char *pathname)` | 替换进程映像（内核打开 ELF → 释放旧空间 → 加载新 ELF） |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| COW fault handler | #PF 需识别 PTE_COW 并 resolve（分配新页/恢复 RW），当前写共享页仍 SIGSEGV | 高 |
| CLONE_VM / 线程 | task_t/mm_t/files_t 拆分已预留，需实现 pthread 级别的线程创建（共享地址空间） | 中 |
| WNOHANG 非阻塞 waitpid | 当前 waitpid 只有阻塞模式，添加 WNOHANG 选项避免 shell 等子进程时卡死 | 高 |
| execve argv/envp 传递 | 当前 argv=NULL, envp=NULL，需支持命令行参数和环境变量传入新进程 | 高 |
| ELF 数据内核复制 | execve 当前直接用用户态指针（同步调用安全），防御性编程应先 kfree 到内核再加载 | 低 |
| 进程优先级 | 当前所有进程同等优先级，需 nice 值或实时优先级支持 | 低 |
| 用户栈仅 4KB 无 guard page | 栈溢出触发 #PF 被 kill，应扩栈 + 加 guard page | 中 |
| 内核栈仅 8KB | 深层调用路径偏紧，应扩栈或加溢出检测 | 中 |
| pid 未校验上界 | procs[pid] 未检查 pid >= MAX_PROC，可越界访问 | 高 |
| cross-process files_t UAF | SCM_RIGHTS/shm_attach 跨进程读 fd_table 时，目标进程 exit + kfree(files_t) 导致 UAF。缓解：`files_put` 中 `synchronize_rcu()` 保证 grace period 后才 kfree | 低 |
| FPU/SSE lazy context switch | 当前不保存 xmm 寄存器，需 fxsave/fxrstor + CR0.TS + #NM handler | 高 |
