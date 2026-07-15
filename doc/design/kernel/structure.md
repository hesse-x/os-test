# 内核架构

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 内核分层模式 | 内核态内分层（XNU/Mach 模式） | syscall 不穿越用户态边界，零开销；编译期通过头文件+ CMake target 实现层间隔离 |
| 2 | 层间调用方向 | BSD → Xcore 单向，Driver → Xcore + BSD 单向 | Xcore 不回调上层，避免循环依赖；Xcore→BSD 交互通过 hook 函数指针注册 |
| 3 | task_t 拆分 | xtask_t（调度）+ bsd_proc_t（POSIX），1:1 双向绑定 | 调度器只操作 xtask_t，不碰 POSIX 数据；idle 进程 bsd_proc = NULL |
| 4 | files_t 归属 | 从 mm_t 移到 bsd_proc_t | fd 表是 POSIX 语义，mm_t 只管地址空间 |
| 5 | syscall 分发 | 入口按语义归属分发，不按编号区间 | write/read/close/pipe 编号靠前但依赖 fd 表，归 BSD 层 |
| 6 | Xcore→BSD 通信 | hook 函数指针（BSD 初始化时注册） | 保持 Xcore 不 #include bsd/ 头文件，编译期隔离 |

### 三层架构

```
┌─────────────────────────────────────────────────────────┐
│                    用户态进程                              │
│  init / shell / kbd_driver / terminal / hello / test     │
├─────────────────────────────────────────────────────────┤
│              syscall / IRQ / 异常入口                     │
│  arch/x64: __alltraps → trap_dispatch → syscall_dispatch │
├──────────────────────┬──────────────────────────────────┤
│     Xcore 核心层     │        BSD 兼容层                  │
│──────────────────────│──────────────────────────────────│
│ xtask_t（调度实体）   │ bsd_proc_t（POSIX进程）           │
│ mm_t（地址空间）      │ files_t / file_t / fd管理         │
│ schedule/switch_to   │ VFS + FAT32 + inode + page_cache  │
│ IPC: recv/req/resp   │ AF_UNIX Socket + SCM_RIGHTS       │
│   msg/msg_resp       │ AF_NETLINK uevent 多播            │
│ notify_and_wake      │ PTY/TTY + termios                 │
│                      │ 信号: sigaction/force_sig/sigreturn│
│ BFC/Slab/页表/KASAN  │ fork/execve/waitpid/exit          │
│ IDT/IRQ/APIC         │ Pipe + dup2/fcntl                 │
│ spinlock/atomic/rcu  │ setsid/setpgid/getpgid            │
│ list/printk/panic    │ uid/gid/umask/hostname            │
│                      │ memfd_create/ftruncate            │
│──────────────────────│──────────────────────────────────│
│                 驱动框架层                                │
│──────────────────────────────────────────────────────────│
│ dev_driver_t 注册表    dev_ops 统一接口                    │
│ AHCI  xHCI  PCI  Display  Serial                        │
│ 用户态驱动: SHM + IRQ bind + dev_create（不变）            │
├──────────────────────────────────────────────────────────┤
│                   arch/x64                               │
│ start.S  vectors.S  trapentry.S  paging  smp  apic       │
└──────────────────────────────────────────────────────────┘

调用方向: BSD → Xcore（单向）, Driver → Xcore + BSD（单向）, Xcore 不回调上层
```

### Xcore 核心层

调度、IPC、内存管理、中断分发。不 #include bsd/ 或 driver/ 头文件。

#### 目录结构

```
kernel/xcore/
  init.c               — xcore_init（serial, mem, ACPI, ISR, proc, SMP）
  sched.c / sched.h    — xtask_alloc/free, schedule, switch_to, idle_entry, timer_queue
  xtask.h              — xtask_t 定义（PCB）
  trap.c / trap.h      — trap_dispatch + syscall_dispatch + IRQ注册表 + hook 调用点
  ipc.c                — IPC 原语实现（sys_recv/req/resp/msg/msg_to）
  kpi.h                — Xcore KPI 导出声明
  log.c / log.h        — printk/panic/dump_stack_trace/BUG_ON/WARN_ON/ASSERT
  rcu.c / rcu.h        — RCU 机制
  acpi.c / acpi.h      — ACPI 表解析
  atomic.h             — 原子操作
  list.h               — 内嵌双向链表
  spinlock.h           — spinlock_t
  wait_queue.c / wait_queue.h — 回调式等待队列原语（多等待者唤醒）
  rbtree.c / rbtree.h  — 红黑树（eventpoll interest list 用）
  sparse.h             — Sparse 注解（__user, __iomem, phys_addr_t, kern_vaddr_t）
  serial_hook.h        — 串口 hook（Xcore 层声明）
  mm_types.h           — mm_t, mmap_region_t, shm_t 类型定义
  mem/
    alloc.c / alloc.h  — Bump/BFC 分配器
    slab.c / slab.h    — Slab 分配器
    user_mapping.c     — 用户页映射辅助
    copy_user.c        — copy_to_user/copy_from_user
    kasan.c / kasan.h  — KASAN（SANITIZE=1 条件编译）
```

#### 核心数据结构

**xtask_t**（kernel/xcore/xtask.h : xtask_t）
- pid : pid_t — 进程 ID（offset 0，switch_to 兼容）
- state : proc_state_t — UNUSED / READY / RUNNING / BLOCKED / ZOMBIE / REAPING
- k_rsp : uint64_t — 内核栈保存的 RSP（offset 8，switch_to 用）
- k_stack_top : uint64_t — 内核栈顶（offset 16）
- cr3 : uint64_t — PML4 物理地址缓存（offset 24）
- entry : uint64_t — 用户态入口 RIP
- wait_event : wait_event_t — 阻塞原因
- tgid : pid_t — 线程组 ID
- mm : mm_t* — 地址空间（idle 为 NULL）
- assigned_cpu : int — 绑定 CPU
- iopm : uint8_t* — IOPM 位图（NULL = 全部拒绝）
- run_node : list_node_t — per-CPU run_queue
- wait_node : list_node_t — per-CPU timer_queue
- wait_deadline : uint64_t — sched_clock() 纳秒超时
- wait_timed_out : uint8_t — 定时器超时唤醒标志
- recv_intr : uint8_t — wake_process 设置，sys_recv 检查 EINTR
- recv_buf : uint8_t[16][64] — 统一接收队列
- recv_head/tail : uint32_t — 队列指针
- recv_lock : spinlock_t — 队列锁
- req_caller_pid / req_reply_buf / req_reply_len / req_result / req_target_pid — REQ IPC 状态
- msg_reply_buf / msg_reply_len / msg_caller_pid / msg_result / msg_target_pid — MSG IPC 状态
- cpu_time_ns / last_sched : uint64_t — CPU 时间统计
- bsd_proc : bsd_proc* — BSD 扩展数据（Xcore 不解读，NULL = idle）

STATIC_ASSERT 保证 offset 0-24 与旧 task_t 完全一致，switch_to 汇编无需改动。

**mm_t**（kernel/xcore/mm_types.h : mm_t）
- cr3 : uint64_t — PML4 物理地址
- m_count : refcount_t — COW/CLONE_VM 引用计数
- mmap_brk : uint64_t — mmap 高水位
- mmap_phys_brk : uint64_t — MAP_PHYSICAL 高水位
- mmap_regions : mmap_region_t* — mmap 区域链表
- parent_pid : pid_t — 父进程 PID

**mmap_region_t**（kernel/xcore/mm_types.h : mmap_region_t）
- vaddr / size / phys : uint64_t — 虚拟地址/大小/物理地址
- shm_obj : shm* — 关联的 SHM 对象
- prot : uint32_t — 保护位
- next : mmap_region_t* — 链表下一项

**shm_t**（kernel/xcore/mm_types.h : shm_t）
- phys : uint64_t — 起始物理地址
- npages : size_t — 页数
- file_size : size_t — 文件大小
- s_count : refcount_t — 引用计数
- flags / seals : int/uint32_t — 标志和 sealing
- name : char[32] — 名称
- page_list / num_pages — 多段 SHM 页列表

#### Xcore KPI

BSD/Driver 层可调用的 Xcore 导出函数，声明在 kernel/xcore/kpi.h：

- **调度**：xtask_alloc / xtask_free / schedule / xtask_set_state / xtask_wake_from_wait
- **地址空间**：mm_create / mm_put / mm_release / mm_release_pages / copy_page_table / add_mmap_region
- **IPC**：notify_and_wake / wake_process / kernel_msg_send
- **内存**：kmalloc / kfree / kcalloc / krealloc / bfc_alloc_page / bfc_free_page / page_to_phys / phys_to_virt / map_user_page_direct / unmap_user_pages / copy_from_user / copy_to_user
- **中断**：register_irq / unregister_irq / irq_owner_check / irq_owner_cleanup
- **SHM**：shm_alloc_pages / shm_create_internal / shm_get / shm_put

#### Hook 注册点

Xcore 通过函数指针在特定点回调 BSD 层，BSD 初始化时注册：

| Hook | 类型 | 调用时机 | 声明位置 |
|------|------|----------|----------|
| bsd_signal_check_hook | bsd_signal_check_fn | trap_dispatch 末尾检查 pending signals | trap.h |
| bsd_fault_handler | bsd_fault_handler_fn | page fault 时委托 BSD 处理文件映射 fault | trap.h |
| bsd_reap_hook | bsd_reap_fn | idle loop 周期调用，扫描孤儿僵尸 | trap.h |
| bsd_proc_reap_hook | bsd_proc_reap_fn | task_reap 中调用 BSD 进程清理 | trap.h |
| bsd_devtmpfs_cleanup_hook | bsd_devtmpfs_cleanup_fn | task_reap/mm_release 中清理 devtmpfs | trap.h |
| bsd_syscall_dispatch_hook | bsd_syscall_dispatch_fn | syscall_dispatch 中分发非 Xcore syscall | trap.h |
| bsd_signal_pending_hook | bsd_signal_pending_fn | IPC 中检测可中断等待 | trap.h |
| bsd_force_sig_hook | bsd_force_sig_fn | 异常处理中强制投递信号 | trap.h |
| timer_poll_hook | timer_poll_fn | 定时器 IRQ 中周期轮询 | trap.h |

#### Xcore syscall

Xcore 层直接处理的 syscall：

| Syscall | 说明 |
|---------|------|
| sys_getpid | 返回当前进程 PID |
| sys_yield | 主动让出 CPU |
| sys_recv / sys_req / sys_resp | 同步 IPC 请求/响应 |
| sys_msg / sys_msg_resp / sys_msg_to | 异步 IPC 消息 |
| sys_irq_bind | 绑定 IRQ 到用户态驱动 |
| sys_notify | 内联消息入队 + 唤醒 |
| sys_gettime / sys_clock | 时间获取 |
| sys_ioperm | I/O 端口权限设置 |

#### 关键流程

**启动初始化**：
1. xcore_init(boot_info) — serial → mem → ACPI → ISR → KASAN/slab → RCU → sig → proc → SMP
2. driver_init() — PCI 枚举 → driver_pci_match → 各驱动 init → serial_device_init
3. bsd_init() — VFS/FAT32/inode/page_cache/devtmpfs/sysfs/tmpfs → bsd_proc 初始化
4. create_idle_process(BSP) → 从 AHCI 加载 init.elf → 切入用户态

**syscall 路径**：
1. 用户态 SYSCALL 指令 → syscall_fast_entry（arch/x64/trapentry.S）
2. syscall_dispatch(trapframe_t *tf)（kernel/xcore/trap.c）
3. Xcore syscall → 直接处理；非 Xcore syscall → bsd_syscall_dispatch_hook(tf)

**exit/reap 流程**：
1. sys_exit：BSD 收集 exit_code、关闭 fd、发送 SIGCHLD → Xcore xtask_set_state(ZOMBIE)
2. idle loop：bsd_reap_hook() 扫描孤儿僵尸 → bsd_proc_reap → xtask_free

### BSD 兼容层

POSIX 语义、VFS、fd 管理、信号、Socket、PTY。通过 Xcore KPI 调用底层服务。

#### 目录结构

```
kernel/bsd/
  init.c               — bsd_init（vfs_init, hook 注册）
  proc.c / proc.h      — bsd_proc_t, sys_fork, sys_execve, bsd_proc_reap
  proc_create.c        — process_create_elf（ELF 加载+调度入队）
  syscall.c / syscall.h — BSD syscall 分发（~45 个 syscall）
  types.h              — fd/pipe/shm/mm/file/files_t 类型定义
  signal.c             — 信号处理（force_sig, check_pending_signals）
  vfs.c / vfs.h        — VFS 层（sys_open/sys_stat/sys_mkdir/sys_unlink/sys_rmdir/sys_dev_create）
  fat32.c / fat32.h    — FAT32 内核文件系统
  inode.c / inode.h    — inode cache（hash 表+引用计数+inode_put）
  page_cache.c / page_cache.h — 4KB page cache（LRU淘汰+写回）
  devtmpfs.c / devtmpfs.h — /dev/ 内存伪文件系统（设备节点注册+open）
  tmpfs.c / tmpfs.h     — /run tmpfs 内存文件系统（目录树+文件内容+i_op/fops，承载 udevd db/socket）
  sysfs.c / sysfs.h     — /sys 伪文件系统（属性树+ringbuf 事件流）
  elf_loader.c / elf_loader.h — ELF 加载器
  socket.c / socket.h  — AF_UNIX SOCK_STREAM + SCM_RIGHTS
  netlink.c / netlink.h — AF_NETLINK 多播事件通知（uevent 广播 + group 注册表）
  pty.c / pty.h        — PTY/TTY 子系统
  eventpoll.c / eventpoll.h — epoll 核心（eventpoll/epitem/ctl/wait）
  eventfd.c / eventfd.h — eventfd 信号计数器 fd
  timerfd.c / timerfd.h — timerfd 定时器 fd
  signalfd.c / signalfd.h — signalfd 信号 fd
  file_poll.c / file_poll.h — per-type 就绪检测 helper（file_poll）
  file_wq.c            — file->wq 惰性分配（file_wq_get）
```

#### 核心数据结构

**bsd_proc_t**（kernel/bsd/proc.h : bsd_proc_t）
- xtask : xtask_t* — 反向引用调度实体（1:1 绑定）
- exit_code : int32_t — exit 状态码（ZOMBIE 时有效）
- sid : pid_t — session ID
- pgid : pid_t — process group ID
- ctty : pty* — 控制终端
- sig_pending : uint64_t — per-task 私有 pending（tgkill/pthread_kill）
- sig_blocked : sigset_t — per-task 阻塞信号集
- sig_force_info : siginfo_t — force_sig 临时 siginfo
- signal : signal_struct* — 线程组共享（shared_pending + action[] + group_exit）
- files : files_t* — fd 表（引用计数，CLONE_FILES 可共享）
- uid/euid/gid/egid/umask — POSIX 身份与权限（uint32_t，默认 0/0/0/0/0022，fork 继承）

**file_t**（kernel/bsd/types.h : file_t）
- f_count : refcount_t — 引用计数
- type : int — FD_NONE/PIPE/REGULAR/DEV/DIR/SOCKET/NETLINK/SHM/FILE/TTY/EPOLL/EVENTFD/TIMERFD/SIGNALFD
- flags : int — FD_CLOEXEC 等
- inode : inode* — 关联 inode
- offset : uint64_t — 文件偏移
- wq : wait_queue_head* — 惰性分配等待队列（epoll/poll 等待者挂此，NULL=无）
- union：pipe* / shm* / target_pid / file_data{...} / unix_sock* / netlink_sock* / pty* / epoll* / eventfd* / timerfd* / signalfd*

**files_t**（kernel/bsd/types.h : files_t）
- fd_lock : spinlock_t — fd 表锁
- fd_table : file*[MAX_FD] — fd 指针数组（RCU 保护）
- f_count : refcount_t — 引用计数

**pipe_t**（kernel/bsd/types.h : pipe_t）
- buf : uint8_t* — 环形缓冲区
- head/tail : uint32_t — 读写指针
- read_pid/write_pid : pid_t — 阻塞等待的进程
- p_count : refcount_t — 引用计数

#### 便利宏

```
current_xtask     — get_cpu_local()->_cur_proc   （Xcore 视角）
current_bsd_proc  — current_xtask->bsd_proc       （BSD 视角）
```

#### BSD syscall

BSD 层处理的 syscall（通过 bsd_syscall_dispatch_hook 分发）：

| 子系统 | Syscall |
|--------|---------|
| fd 操作 | write, read, close, pipe, dup2, fcntl |
| VFS | open, stat, mkdir, unlink, rmdir, dev_create, getdents, ioctl, fstat |
| Socket | socket, bind, listen, accept, connect, socketpair, sendmsg, recvmsg, shutdown, poll |
| 进程 | fork, execve, exit, waitpid, setsid, setpgid, getpgid, getsid |
| 进程身份 | getuid, geteuid, getgid, getegid, setuid, setgid, getppid, getpgrp, umask, gethostname, sethostname |
| 信号 | kill, sigaction, sigreturn, tgkill, sigprocmask, sigpending, alarm, pause |
| 内存 | mmap, munmap（BSD 入口，调用 Xcore KPI） |
| 其他 | dma_alloc/free, pci_dev_info, block_async, install_fd, lseek, memfd_create, ftruncate, truncate, fsync, sync, debug_memstat, fdev_pid |

#### 与其他模块的关系

- **BSD → Xcore**：通过 Xcore KPI 函数调用（kmalloc, xtask_alloc, schedule 等）
- **BSD → Driver**：通过 devtmpfs 的 dev_ops 函数指针间接调用驱动
- **BSD hook → Xcore**：BSD 初始化时在 Xcore 注册 hook 函数指针

### 驱动框架层

内核态驱动注册表 + PCI 自动匹配 + dev_ops 统一接口。用户态驱动（kbd_driver, terminal）不受影响。

#### 目录结构

```
kernel/driver/
  init.c               — driver_init（pci_init, driver_register, driver_pci_match）
  registry.c / driver.h — dev_driver_t 注册表 + PCI 自动匹配
  ahci.c / ahci.h      — AHCI DMA 驱动
  xhci.c / xhci.h      — xHCI USB 主控驱动
  display.c / display.h — KMS 内核态 display（bochs-display, req flip）
  serial.c / serial.h   — COM1 串口
  pci.c / pci.h         — PCI/PCIe ECAM 枚举与 BAR 分配
  blk_dev.c / blk_dev.h — 块设备抽象层（AHCI 同步封装+spinlock）
  bsd_types.h           — BSD 层类型前向声明（driver 需要）
  user_check.h          — 用户态缓冲区/指针验证
```

#### 核心数据结构

**dev_driver_t**（kernel/driver/driver.h : dev_driver_t）
- name : const char* — 驱动名称（如 "ahci", "xhci"）
- pci_class : uint32_t — PCI class code（0 = 手动注册）
- pci_vendor : uint32_t — PCI vendor ID（0 = 不匹配 vendor）
- pci_device : uint32_t — PCI device ID（0 = 不匹配 device）
- init : void(*)(void) — 驱动初始化函数
- ops : dev_ops* — 设备操作接口（注册到 devtmpfs）

**dev_ops**（kernel/bsd/devtmpfs.h : dev_ops）
- driver_pid : pid_t — 0 = 内核设备, >0 = 用户态驱动
- device_type : uint32_t — 设备类型
- open/close/ioctl/mmap/read/write/poll — 函数指针

#### 驱动注册表

| 驱动 | PCI class | 注册方式 | dev_ops |
|------|-----------|----------|---------|
| AHCI | 0x010601 (SATA) | PCI 自动匹配 | blk_dev_ops |
| xHCI | 0x0C0330 (USB) | PCI 自动匹配 | usb_hid_ops |
| Display (bochs) | 0x0300 (VGA) | PCI 自动匹配 | display_ops |
| Serial | 无 PCI | 手动注册 | serial_ops |
| PCI | 无 PCI | 手动注册 | 无（枚举驱动） |

#### 初始化流程

1. pci_init() — PCI 枚举，建立 pci_dev_list
2. driver_pci_match() — PCI class 自动匹配，调用匹配驱动的 init
3. serial_device_init() — 手动注册 /dev/serial

#### 与其他模块的关系

- **Driver → Xcore**：调用 Xcore KPI（kmalloc, register_irq 等）
- **Driver → BSD**：通过 devtmpfs 注册设备（dev_create），dev_ops 参数为 xtask_t*（不碰 BSD 数据）
- **用户态驱动**：通过 SHM + IRQ bind + dev_create 与内核交互，路径不变（详见 doc/design/user_driver.md）

### 锁模型

详见 [kernel_lock.md](kernel_lock.md)。

关键约束：FAT32 锁获取顺序 `i_lock → fat_lock → ahci_lock`（固定，防死锁）。

### 构建系统

三层的 CMake target 拆分：

| Target | 源文件 | 依赖 |
|--------|--------|------|
| xcore_obj | xcore init/sched/trap/ipc/log/rcu/acpi + mem/ | arch_x64, kernel_mem |
| bsd_obj | bsd init/syscall/proc/signal/vfs/fat32/inode/page_cache/devtmpfs/sysfs/tmpfs/socket/pty/elf_loader | xcore_obj |
| driver_obj | driver init/registry/ahci/xhci/pci/display/serial/blk_dev | xcore_obj, bsd_obj (devtmpfs) |

链接顺序：arch_x64 → kernel_mem → xcore_obj → bsd_obj → driver_obj

编译期隔离：xcore/ .c 文件不 #include "bsd/" 或 "driver/" 头文件。

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 文件 mmap fault | bsd_fault_handler hook 已预留但未实现文件映射 page fault 分发，当前仅支持匿名/COW fault | 高 |
| BSD syscall 细拆 | bsd_syscall.c 单文件分发所有 POSIX syscall，可按子系统拆为 bsd_vfs.c / bsd_socket.c / bsd_pty.c 等 | 中 |
| 层间隔离 CI 检查 | 添加脚本检查 xcore/ .c 文件不 #include bsd/ 或 driver/ 头文件 | 中 |
| dev_ops 参数 xtask_t 化 | dev_ops 函数指针参数从 task_t* 改为 xtask_t*，驱动不碰 BSD 数据 | 低 |
| work stealing | assigned_cpu 静态绑定改为动态负载均衡，idle CPU 从繁忙 CPU steal 进程 | 低 |
| dentry cache | 每次开放读磁盘解析路径，添加 (path, inode) hash + LRU 缓存 | 中 |
