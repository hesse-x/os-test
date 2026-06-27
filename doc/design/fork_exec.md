# fork+exec 设计：proc_t 拆分 + fork + execve + spawn 用户态化

## 1 项目概述

### 1.1 目标
将现有 `sys_spawn`（用户态读 ELF→内核创建进程）替换为 Linux 标准的 fork+execve 模型，同时将 `proc_t` 拆分为 `task_t`（调度实体）+ `mm_t`（地址空间+资源），为后续线程（CLONE_VM）打基础。

### 1.2 核心变更
- `proc_t` → `task_t` + `mm_t` 结构拆分
- 新增 `sys_fork` 系统调用（全量页拷贝，预留 COW 接口）
- 新增 `sys_execve` 系统调用（内核打开 pathname + 替换进程映像）
- 删除 `sys_spawn` 系统调用，libc 中 `spawn()` = `fork()` + `execve()`
- `proc_reap` 拆分为 `task_reap` + `mm_release`

### 1.3 不在范围
- CLONE_VM / 线程 / pthread（后续，结构拆分已为此预留）
- COW 页拷贝优化（后续，mm_t ref_count 已预留）
- futex / exit_group / tgkill（随线程一起做）

## 2 总体架构

### 2.1 结构拆分

```
task_t（调度实体，数组内嵌）
├── tid, state, k_rsp, k_stack_top, cr3(cached)
├── tgid (= tid，单线程时)     ← 新增，为线程预留
├── assigned_cpu, run_node, wait_node
├── mm: *mm_t                  ← 指针，非内嵌
├── iopm, exit_code, parent_pid
├── per-task IPC (recv/req/msg)
├── per-task 信号 (sig_pending, sig_blocked)
├── sig_force_info, sig action  ← 留 task（单线程时 per-process）
├── sid, pgid, ctty
└── cpu_time_ns, last_sched

mm_t（kmalloc 分配，引用计数）
├── cr3                         ← 权威 PML4 物理地址
├── ref_count = 1              ← COW/CLONE_VM 预留
├── fd_table[MAX_FD]
├── mmap_brk, mmap_phys_brk, mmap_regions
├── u_stack_phys, u_stack_pages
└── parent_pid                 ← 从 task_t 移入
```

### 2.2 全局变量重命名
- `procs[]` → `tasks[]`，`procs_lock` → `tasks_lock`
- `current_proc` → `current_task`（宏在 smp.h）
- `MAX_PROC` 不变（64），tid == 数组下标

### 2.3 fork 语义

```
sys_fork():
  1. 分配新 task_t 槽位
  2. 分配新 mm_t (kmalloc)
  3. 分配新 PML4，拷贝内核条目
  4. 深拷贝父进程用户页表（全量页拷贝）:
     - 遍历父 PML4[0-255]，对每个 present 的 PTE:
       - 分配新物理页，memcpy 内容
       - 在子 PML4 中建立相同虚拟地址映射（相同 flags）
     - SHM/MAP_PHYSICAL 页：共享物理页，shm_get()/不 free
  5. 拷贝 fd_table: 逐 fd 复制 file_t，bump ref count
  6. 拷贝 mmap_regions 链表
  7. 分配新内核栈，拷贝父进程 trapframe（rax=0 表示子进程返回值）
  8. 设置 mm->parent_pid = current->tid
  9. 子 task->tgid = 子 task->tid（新线程组 leader）
  10. 入队调度
  11. 父进程返回子 tid，子进程返回 0
```

### 2.4 execve 语义

```
sys_execve(pathname, argv, envp):
  1. 内核打开 pathname（复用 sys_open 的 VFS/FAT32 路径）
  2. fstat 获取文件大小
  3. kmalloc 缓冲区，read 整个 ELF 文件到内核
  4. 验证 ELF magic，无效则 kfree + 返回 -ENOEXEC
  5. 关闭 FD_CLOEXEC 标记的 fd
  6. 释放旧地址空间（mm_release_pages）:
     - 遍历 PML4 释放用户物理页（跳过 SHM/MAP_PHYSICAL）
     - 释放旧 PML4 页
     - 释放旧用户栈
  7. 分配新 PML4，拷贝内核条目
  8. elf_load 新 ELF 到新 PML4
  9. 分配新用户栈，映射信号 trampoline 页
  10. 原地修改当前 task 的 trapframe: rip=entry, rsp=stack_top
  11. 更新 mm_t 字段（cr3, u_stack_*, mmap_brk 重置）
  12. 设置 argv 到用户栈（可选，第一版不传 envp）
  13. kfree ELF 缓冲区
  14. 返回到用户态执行新程序
```

### 2.5 spawn 用户态封装

```c
// libc: user/lib/posix.cc
pid_t spawn(const char *path) {
    pid_t pid = sys_fork();
    if (pid == 0) {
        // 子进程
        sys_execve(path, NULL, NULL);
        // execve 失败则退出
        sys_exit(-ENOENT);
        __builtin_unreachable();
    }
    return pid;  // 父进程返回子 PID
}
```

### 2.6 mm_t 生命周期

```
创建:
  mm_create() → kmalloc mm_t, ref_count=1, 分配 PML4

共享 (fork/COW 未来):
  fork → 新 mm_t, ref_count=1（全量拷贝，不共享）
  COW 后续 → 共享同一 mm_t, ref_count++

释放:
  mm_put(mm) → atomic --ref_count
    → 若 >0: 什么都不做
    → 若 ==0: mm_release(mm)

mm_release(mm):
  1. mm_release_pages(): 释放用户页表+物理页+PML4+用户栈
  2. 关闭所有 fd (per-type close 逻辑)
  3. 释放 mmap_regions + SHM 引用
  4. devtmpfs_cleanup_pid / irq_owner_cleanup
  5. 唤醒等待 REQ/MSG reply 的进程
  6. kfree(mm)

task_reap(task):
  1. 释放内核栈
  2. 释放 IOPM
  3. mm_put(task->mm)
  4. 释放 recv 队列 RECV_MSG 缓冲区
  5. 清 PCB 槽位
```

## 3 新增系统调用

| 编号 | syscall | 签名 | 说明 |
|------|---------|------|------|
| 61 | sys_fork | `fork()` | 创建子进程（拷贝地址空间） |
| 62 | sys_execve | `execve(pathname, argv, envp)` | 替换进程映像 |

删除 `SYS_SPAWN = 8`（syscall 表置 nullptr，编号不重排）。

`NR_SYSCALL` 从 61 升至 63。

## 4 关键实现细节

### 4.1 switch_to 兼容
task_t 前 5 个字段（tid/state/k_rsp/k_stack_top/cr3）偏移必须与现有 proc_t 一致，供 `switch_to` 汇编使用。

验证：当前 proc_t 前部布局为 pid(4)+pad(4)+state(4)+pad(4)+k_rsp(8)+k_stack_top(8)+cr3(8)。task_t 需保持 `k_rsp` 在 offset 16, `cr3` 在 offset 32。

### 4.2 idle 进程
idle 的 `task->mm = NULL`，使用内核 PML4。所有访问 `task->mm->` 的代码必须先检查 `task->mm != NULL`。

### 4.3 fork 页表拷贝
新增 `copy_page_table(src_pml4, dst_pml4)`（user_mapping.c）:
- 遍历 src PML4[0-255]（用户态条目）
- 对每个 present 的 PDPT→PD→PT→PTE 链：
  - 分配新页表页（PDPT/PD/PT），拷贝结构
  - 对 leaf PTE：分配新物理页，memcpy 内容，设置相同 flags
  - 对 SHM/MAP_PHYSICAL 页：不分配新物理页，直接复用 PTE（shm_get bump ref）
- 跳过信号 trampoline 页（共享物理页）

### 4.4 execve 失败处理
execve 在替换地址空间前先验证 ELF。如果 ELF 无效，返回 -ENOEXEC，旧地址空间不受影响。验证通过后再释放旧空间。

### 4.5 FD_CLOEXEC 消费
execve 中遍历 fd_table，对 flags & FD_CLOEXEC 的 fd 执行关闭（与 sys_close 相同的 per-type 逻辑）。

### 4.6 sys_waitpid 适配
- `parent_pid` 从 task_t 移到 mm_t，waitpid 检查 `child->mm->parent_pid == current->tid`
- 由于单线程阶段 tgid==tid，waitpid 逻辑不变

### 4.7 sys_exit 适配
- 设置 task->exit_code，task->state = ZOMBIE
- 孤儿收养：遍历 tasks[]，将 mm->parent_pid == current->tid 的子进程 reparent 到 init_pid
- mm 不在 exit 时释放，延迟到 waitpid 中 task_reap → mm_put

## 5 风险与稳定性

### 5.1 结构拆分回归风险
拆分影响所有引用 proc_t 的代码（trap.c, proc.c, vfs.c, socket.c, ahci.c 等）。需逐一适配。

缓解：分步推进，每步编译验证。

### 5.2 fork 页表拷贝性能
全量拷贝对大进程（8MB 栈 + ELF + mmap）开销显著。当前进程普遍小，可接受。

### 5.3 execve 原子性
Linux 中 execve 失败不影响原进程。本设计中先验证 ELF 再替换，失败返回 -ENOEXEC。

### 5.4 idle mm=NULL 空指针
所有 `task->mm->` 访问点必须加 NULL 检查。遗漏会导致 #PF。

## 6 实现阶段

### 阶段 1：结构拆分（核心重构）

1. 定义 task_t + mm_t 结构体
2. 实现 mm_create() / mm_put() / mm_release() / mm_release_pages()
3. `procs[]` → `tasks[]`，`procs_lock` → `tasks_lock`，`current_proc` → `current_task`
4. 适配所有引用点（schedule/switch_to/trap_dispatch/syscall_dispatch/proc_reap/ahci/socket 等）
5. idle 进程 mm=NULL
6. proc_reap 重构为 task_reap（释放内核栈）→ mm_put（归零触发 mm_release）→ 清 PCB

验证：编译通过（release + debug）+ shell/hello 正常运行

### 阶段 2：fork

1. 新增 `copy_page_table()`（user_mapping.c）
2. 新增 `copy_fd_table()`（proc.c）
3. 新增 `copy_mmap_regions()`（proc.c）
4. 实现 `sys_fork`（SYS_FORK = 61）
5. libc 添加 `fork()` 封装
6. fork 子进程返回 0，父进程返回子 tid

验证：fork 后子进程 getpid() ≠ 父进程，子进程修改全局变量不影响父进程

### 阶段 3：execve

1. 实现 `sys_execve`（SYS_EXECVE = 62）：内核 open+read ELF → FD_CLOEXEC 关闭 → mm_release_pages → elf_load → 新栈 → 修改 trapframe
2. libc 添加 `execve()` / `execl()` 封装
3. 删除 SYS_SPAWN syscall 表项，libc 中 spawn() 改为 fork+execve

验证：spawn("/usr/bin/hello") 正常运行（等价于 fork+execve）

### 阶段 4：清理与回归验证

1. 删除 sys_spawn 实现代码（trap.c 中）
2. init.c / shell.cc 中 spawn_service() 改用 fork+execve 路径（或通过 libc spawn 封装透明切换）
3. 全量回归：shell/hello/init/kbd/terminal 全部正常运行
4. 更新设计文档（process_lifecycle.md, syscall.md, todo.md）

## 7 修改文件清单（预估）

| 文件 | 改动 |
|------|------|
| kernel/proc.h | task_t + mm_t 定义，删除 proc_t，新增 mm_api 声明 |
| kernel/proc.c | task_t/mm_t 初始化，mm_create/put/release，task_reap 重构，sys_fork |
| kernel/trap.c | sys_execve，删除 sys_spawn，所有 current_proc→current_task |
| kernel/mem/user_mapping.c | copy_page_table() |
| kernel/vfs.c | current_proc→current_task |
| kernel/socket.c | current_proc→current_task |
| kernel/ahci.c | current_proc→current_task |
| kernel/fat32.c | current_proc→current_task（如有） |
| kernel/inode.c | 引用适配 |
| kernel/devtmpfs.c | 引用适配 |
| kernel/serial.c | 引用适配 |
| kernel/fb.c | 引用适配 |
| kernel/pty.c | 引用适配 |
| kernel/page_cache.c | 引用适配 |
| kernel/blk_dev.c | 引用适配 |
| arch/x64/trapentry.S | switch_to 参数类型注释（无功能变化） |
| arch/x64/smp.h | current_proc→current_task 宏 |
| common/syscall.h | 新增 SYS_FORK/EXECVE，删除 sys_spawn，新增 fork/execve 封装 |
| common/errno.h | 新增 ENOEXEC |
| user/lib/posix.cc | spawn() = fork()+execve() |
| user/lib/start.cc | 适配 |
| init/init.c | spawn_service 改用 fork+execve |
| shell/shell.cc | exec_path 改用 fork+execve |
| doc/design/process_lifecycle.md | 更新 |
| doc/design/syscall.md | 更新 |
