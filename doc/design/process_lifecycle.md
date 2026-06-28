# 进程生命周期管理 — sys_exit / sys_waitpid / sys_spawn

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 退出状态 | ZOMBIE（非直接回收） | 父进程需获取退出码，立即回收丢失信息；避免 PCB 槽位竞态 |
| sys_waitpid 语义 | 阻塞式，指定 PID | 与现有 wait/notify 模型一致；WNOHANG 留后续；指定 PID 最简 |
| sys_waitpid 返回值 | 返回子 PID，退出码通过指针参数写出 | 与 Linux 语义一致，调用者不关心可传 NULL |
| sys_spawn ELF 传递 | 用户态缓冲区指针 | 符合微内核原则，内核不碰文件系统；shell 已有读文件能力 |
| sys_spawn 返回值 | 成功返回子 PID，失败返回负 errno | shell 需要 PID 调 waitpid |
| sys_spawn 创建方式 | 同步 | hello.elf 很小，非抢占内核态，同步不是瓶颈 |
| sys_spawn iopl 安全 | 调用者 IOPL < 请求 IOPL 时返回 -EPERM | 防止普通进程 spawn IOPL=3 子进程打破隔离 |
| 共享页映射 | 动态 SHM（sys_shm_create/attach） | 详见 [ipc.md](ipc.md) 四、SHM 共享内存 |
| 资源回收时机 | 全部延迟到 sys_waitpid | 避免 sys_exit 中解映射 PML4 后 schedule 切换 CR3 前的时序问题；每个 ZOMBIE 多占 4KB PML4 可接受 |
| 无父进程的进程退出 | parent_pid == -1 时跳过 ZOMBIE 直接回收 | 启动时 4 个进程无父进程，避免 PCB 槽位永久泄漏 |
| 异常进程退出 | CPU 异常 → ZOMBIE + notify 父 + schedule（替代 halt） | 用户程序 crash 不应拖垮全机；改动极小 |
| 退出码类型 | int32_t | 与 Linux 一致 |
| syscall 编号 | SYS_EXIT=6, SYS_WAITPID=7, SYS_SPAWN=8 | 顺序追加，零破坏性 |

## proc_t 扩展

```c
// 新增状态
typedef enum {
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE          // 新增：已退出，等待父进程回收
} proc_state_t;

// 新增等待事件
typedef enum {
    WAIT_NONE,
    WAIT_RECV,
    WAIT_REQ_REPLY,
    WAIT_MSG_REPLY,
    WAIT_CHILD,
    WAIT_PIPE,
    WAIT_POLL,
} wait_event_t;

// 新增字段
struct proc_t {
    // ... existing fields ...
    pid_t parent_pid;     // 父进程 PID，启动时进程设为 -1
    int32_t exit_code;    // 退出码，ZOMBIE 时有效
};
```

初始化：
- `proc_init()`：`parent_pid = -1`，`exit_code = 0`
- `process_create_elf()`：`parent_pid = -1`（启动时进程由内核创建，无用户态父进程）
- `sys_spawn()`：`parent_pid = current_proc->pid`

## sys_exit

```
int64_t sys_exit(int32_t exit_code)
```

语义：
- 将当前进程状态设为 ZOMBIE，保存退出码
- 若 `parent_pid == -1`：跳过 ZOMBIE，直接回收全部资源（PML4 + 用户页 + 内核栈 + PCB 槽位）
- 若 `parent_pid >= 0`：notify 父进程（sys_notify 同时匹配 WAIT_NOTIFY 和 WAIT_CHILD），资源延迟到 sys_waitpid 回收
- 调用 schedule() 切走当前进程（不返回）

实现要点：
```c
int64_t sys_exit(int32_t exit_code, uint64_t, uint64_t, uint64_t, uint64_t) {
    proc_t *proc = current_proc;
    proc->exit_code = exit_code;

    if (proc->parent_pid < 0) {
        // 无父进程：直接回收
        proc_reap(proc);
    } else {
        // 有父进程：ZOMBIE，等 waitpid 回收
        int cpu = proc->assigned_cpu;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock);
        proc->state = PROC_ZOMBIE;
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock);
        sys_notify(proc->parent_pid);
    }

    schedule();  // 永不返回
    return 0;    // 不可达
}
```

### proc_reap — 资源回收

```c
// 回收进程全部资源：PML4 + 用户页映射 + 内核栈 + PCB 槽位
// 调用者：sys_exit（无父进程时）或 sys_waitpid
void proc_reap(proc_t *proc) {
    // 1. 解映射用户页 + 释放物理页（遍历用户 PML4 条目）
    //    注意：PTE 叶页物理地址提取必须用 pte & 0x000FFFFFFFFFF000，
    //    而非 pte & ~0xFFF，因为 bit 63 是 NX 位，~0xFFF 不会清除它，
    //    导致 PHY_TO_PAGE 越界访问 frames[] 数组
    // 2. 释放 PML4 页本身
    // 3. 释放内核栈（phys = proc->k_stack_top - 2*PAGE_SIZE + VMA_BASE 偏移）
    // 4. PCB 槽位清零：pid = -1，state = ...（标记空闲）
}
```

注意：`proc_reap` 中解映射用户页需要遍历 PML4 的用户条目（0-510），对每个存在的 PDPT→PD→PT 链，释放叶页物理帧 + 页表页本身。可复用 `unmap_user_pages` 的页表遍历逻辑。

## sys_waitpid

```
int64_t sys_waitpid(pid_t pid, int32_t *exit_code)
```

语义：
- 等待指定 PID 的子进程退出
- 若子进程已是 ZOMBIE：回收资源，返回子 PID，退出码写入 *exit_code
- 若子进程尚未退出：BLOCKED on WAIT_CHILD，被唤醒后重新检查（循环）
- 若 PID 不是自己的子进程：返回 -ECHILD
- 若 PID 对应的进程不存在：返回 -ECHILD

实现要点：
```c
int64_t sys_waitpid(pid_t pid, int32_t *exit_code, uint64_t, uint64_t, uint64_t) {
    // 参数校验
    if (pid < 0) return -EINVAL;

    spin_lock_irqsave(&procs_lock);  // 或用 scheduler_lock？
    proc_t *child = &procs[pid];
    if (child->parent_pid != current_proc->pid) {
        spin_unlock_irqrestore(&procs_lock);
        return -ECHILD;
    }
    spin_unlock_irqrestore(&procs_lock);

    while (1) {
        // 检查子进程是否 ZOMBIE
        // 需要 scheduler_lock 因为 state 归它保护
        int cpu = child->assigned_cpu;
        spin_lock_irqsave(&cpu_locals[cpu].scheduler_lock);
        if (child->state == PROC_ZOMBIE) {
            child->state = PROC_READY;  // 临时标记，防止竞争
            spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock);
            break;
        }
        spin_unlock_irqrestore(&cpu_locals[cpu].scheduler_lock);

        // 子进程未退出，阻塞等待
        current_proc->wait_event = WAIT_CHILD;
        current_proc->state = PROC_BLOCKED;
        schedule();

        // 被 notify 唤醒，重新检查
    }

    // 回收子进程资源
    if (exit_code) *exit_code = child->exit_code;
    proc_reap(child);
    return pid;
}
```

**锁协议说明**：sys_waitpid 读 `child->state` 需要子进程 `assigned_cpu` 的 `scheduler_lock`。sys_exit 设 ZOMBIE 也持同一把锁。两者互斥，无死锁风险（waitpid 只持一把 scheduler_lock）。

## sys_spawn

```
int64_t sys_spawn(const void *elf_data, uint64_t elf_size, uint32_t iopl)
```

语义：
- 从用户态缓冲区读取 ELF 数据，创建新进程
- `elf_data`：用户态地址，内核在 syscall 上下文中可直接访问（CR3 未切换）
- `elf_size`：ELF 数据大小
- `iopl`：新进程的 IOPL；若调用者 IOPL < 请求的 IOPL，返回 -EPERM
- 成功返回子 PID，失败返回负 errno（-ENOMEM / -EINVAL / -EPERM）

实现要点：
```c
int64_t sys_spawn(const void *elf_data, uint64_t elf_size, uint32_t iopl,
                  uint64_t, uint64_t) {
    // IOPL 权限检查
    if (current_proc->iopl < iopl) return -EPERM;

    // 基本参数校验
    if (!elf_data || elf_size == 0 || elf_size > ELF_MAX_BUFSIZE)
        return -EINVAL;

    // 复制 ELF 数据到内核缓冲区（避免用户态在加载过程中修改数据）
    // 注：当前 process_create_elf 直接读传入指针，如果不复制，
    //     用户态缓冲区在 ELF 加载期间必须保持有效（当前同步调用，不会抢占，安全）
    //     但为防御性编程，复制一份更稳妥

    // 调用现有 process_create_elf，设置 parent_pid
    proc_t *child = process_create_elf(elf_data, elf_size, iopl);
    if (!child) return -ENOMEM;  // 或 -EINVAL（ELF 格式非法）

    child->parent_pid = current_proc->pid;
    return child->pid;
}
```

**关于 ELF 数据复制**：sys_spawn 是同步调用，process_create_elf 在同一 syscall 上下文完成，内核态非抢占，用户态缓冲区不会被修改。为减少改动，第一版可直接传用户态指针，不做内核复制。

## 异常退出路径改造

当前 `trap_dispatch` 中 CPU 异常（page fault、undefined opcode 等）调用 `halt()` 冻结全机。

改为：
```c
// trap_dispatch 中 CPU 异常分支
if (tf->cs == 0x2B) {
    // 用户态异常：进程退出，不拖垮全机
    serial_printf("Process %d crashed: vector %d, rip=0x%lx\n",
                  current_proc->pid, trapno, tf->rip);
    sys_exit(-1, 0, 0, 0, 0);  // 退出码 -1 表示异常退出
    // 不返回
} else {
    // 内核态异常：仍然 halt（内核 bug，不可恢复）
    halt();
}
```

## 用户态封装

### common/syscall.h 新增

```c
#define SYS_EXIT      6
#define SYS_WAITPID   7
#define SYS_SPAWN     8

static inline void sys_exit(int32_t exit_code) {
    __syscall1(SYS_EXIT, exit_code);
    // 不应返回
}

static inline int64_t sys_waitpid(pid_t pid, int32_t *exit_code) {
    return __syscall2(SYS_WAITPID, pid, (uint64_t)exit_code);
}

static inline int64_t sys_spawn(const void *elf_data, uint64_t elf_size, uint32_t iopl) {
    return __syscall3(SYS_SPAWN, (uint64_t)elf_data, elf_size, iopl);
}
```

## 锁协议补充

新增的锁交互：

| 操作 | 持锁 | 说明 |
|------|------|------|
| sys_exit 设 ZOMBIE | scheduler_lock[child->assigned_cpu] | 保护 state |
| sys_exit 无父进程回收 | procs_lock | 保护 PCB 槽位释放 |
| sys_waitpid 检查 state | scheduler_lock[child->assigned_cpu] | 保护 state |
| sys_waitpid 回收 (proc_reap) | procs_lock | 保护 PCB 槽位释放 |
| sys_spawn 创建 | procs_lock（分配槽位）+ scheduler_lock[target_cpu]（入队） | 复用现有 process_create_elf 路径 |

**死锁分析**：
- sys_waitpid 持 scheduler_lock → 释放 → 持 procs_lock：与现有顺序一致（procs_lock 先，scheduler_lock 后）
- sys_exit 持 scheduler_lock → 释放 → notify → 持 procs_lock：同上
- sys_spawn 复用 process_create_elf，已遵循 procs_lock → scheduler_lock 顺序

**注意**：sys_exit 中 `sys_notify(parent_pid)` 不持锁（notify 内部获取 scheduler_lock），sys_exit 在此之前已释放 scheduler_lock，安全。

## 实现步骤

```
1. proc_t 扩展：新增 parent_pid / exit_code 字段 + PROC_ZOMBIE 状态 + WAIT_CHILD 事件
   → 验证: 编译通过

2. sys_exit + proc_reap（无父进程直接回收路径）
   → 验证: 用户进程调 sys_exit，无父进程时 PCB 槽位可复用

3. sys_waitpid（阻塞等 ZOMBIE + 回收）
   → 验证: shell spawn 子进程 → 子进程 exit → shell waitpid 收到退出码

4. sys_spawn（用户态 ELF → process_create_elf + IOPL 权限检查）
   → 验证: shell 调 sys_spawn 创建子进程，子进程运行后 exit

5. 异常路径改造（halt → sys_exit for 用户态异常）
   → 验证: 用户进程 crash 不再 halt 全机

6. crt0.o + 静态 libc + hello.c + shell run 命令
   → 验证: shell 输入 `run hello.elf` → hello world 输出字符 → shell 回到命令行

7. FAT32 文件写入（fs_driver write 支持）
   → 验证: 宿主机编译的 ELF 可写入 FAT32，shell 可读取执行
```

步骤 1-4 是 syscall 骨架，步骤 5 是防御性改进，步骤 6-7 是用户态集成。

## 实现状态

全部步骤 1-6 已实现，步骤 7 用 build.sh mcopy 代替（无需 fs_driver write）。

### 实现偏差

| 设计 | 实际 | 原因 |
|------|------|------|
| sys_waitpid 持 procs_lock 验证父关系 | ✅ 一致 | |
| sys_exit 中 sys_notify(parent_pid) | ✅ 一致，复用 sys_notify 函数，且 sys_notify 同时匹配 WAIT_NOTIFY 和 WAIT_CHILD | 原设计 sys_notify 只匹配 WAIT_NOTIFY，导致 sys_waitpid(WAIT_CHILD) 永远不会被 sys_exit 唤醒 |
| proc_reap 解映射用户页遍历 PML4[0-255] | ✅ 一致，加上共享页物理帧排除 | 共享页是全局资源，不能随进程回收 |
| proc_reap PTE 物理地址提取 | `pte & 0x000FFFFFFFFFF000`（清除 NX 位 bit 63） | 原实现用 `pte & ~0xFFF` 不会清除 bit 63，带 PTE_NX 的页（用户栈、共享页）提取出错误的物理地址，导致 frames[] 越界 → #GP |
| proc_reap 释放页表页（PDPT/PD/PT） | ✅ 新增 free_table_page | 设计中未提及中间页表回收，实际实现完整回收 |
| ELF 数据复制到内核缓冲区 | 第一版直接传用户态指针 | 同步调用，内核态非抢占，安全。后续可加内核复制 |
| hello.c + crt0.o + 静态 libc | hello.cc 直接 `_start` 调用 syscall | 最简实现，后续改进为 crt0/main 模型 |
| FAT32 文件写入 | build.sh mcopy 代替 fs_driver write | 避免实现完整文件写入，最小可行 |
| process_create_elf 串口调试输出 | 已清理 | .data PTE 调试输出移除 |

### 后续工作

- [ ] crt0.o + `_start → main` 模型 + 静态 libc（putc/puts/memset 等）
- [ ] FAT32 文件内容写入（fs_driver FS_CMD_WRITE）
- [ ] sys_spawn ELF 数据内核复制（防御性）
- [ ] WNOHANG 非阻塞 waitpid（可选）
- [ ] 磁盘扩容（当前 1MB 太小，hello.elf+4个驱动已占大部分）
