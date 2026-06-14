# 微内核实现进度

## 项目目标

| 目标 | 阶段 | 验收标准 |
|------|------|----------|
| **支持简单 ELF 可执行文件的执行** | 短期 | 在宿主机编译 hello world，静态链接 OS 的 print 函数，在 OS 上执行并输出字符 |
| **支持 Wayland 核心协议** | 中期 | 见下方 [Wayland 验收标准](#wayland-验收标准) |
| **支持构建 gcc** | 远期 | 成功在 OS 上构建出 gcc |

### Wayland 验收标准

在 OS 上运行 Wayland compositor 和至少 2 个客户端进程，客户端通过 wl_shm 共享内存提交像素缓冲区到 surface，compositor 合成所有 surface 并 page-flip 到 framebuffer 显示。键盘输入通过 wl_seat 路由到获得键盘焦点的 surface。

具体验收点：

1. **wl_display + wl_registry**：客户端连接 compositor，获取全局对象列表（wl_compositor、wl_shm、wl_seat、wl_output）
2. **wl_compositor + wl_surface**：客户端创建 surface，提交 damage 区域
3. **wl_shm + wl_buffer**：客户端通过 mmap 创建共享内存池，从中分配 wl_buffer，attach 到 surface 并 commit
4. **wl_seat + wl_keyboard**：compositor 将键盘事件路由到焦点 surface 的客户端
5. **compositor 合成 + page flip**：compositor 读取所有 surface 的 buffer 内容，合成到 framebuffer，双缓冲原子切换显示
6. **多客户端**：至少 2 个客户端同时运行，各自有独立 surface，compositor 正确合成它们的像素内容

验收演示：2 个客户端分别用不同颜色填充矩形区域（如红色和蓝色），compositor 将它们合成到屏幕上，可见两个色块同时显示。按键盘切换焦点，焦点客户端收到键盘事件。

**不在验收范围内**：wl_pointer/鼠标、xdg-shell/window decoration、wl_output 详细模式通告、客户端库封装（验收只需要 test client）

## 已完成（设计文档索引）

| 功能 | 设计文档 |
|------|----------|
| 用户态基础设施（GDT/TSS/IDT/trapframe） | [ring3_switch.md](ring3_switch.md) |
| 进程与调度（PCB/switch_to/schedule/idle） | [process_scheduler.md](process_scheduler.md) |
| 系统调用 + 用户分页 + 用户栈 | [sys_call.md](sys_call.md) |
| Shell + ATA PIO + ELF Loader | [shell.md](shell.md) |
| x86-64 迁移 | [x64_migration.md](x64_migration.md) |
| UEFI 引导 | [uefi.md](uefi.md) |
| 多核 SMP — Per-CPU 基础设施 | [smp.md](smp.md) |
| 多核 SMP — APIC 替换 PIC | [smp.md](smp.md) |
| 多核 SMP — AP 启动 | [smp.md](smp.md) |
| SYSCALL/SYSRET 快速系统调用 | [syscall_fastpath.md](syscall_fastpath.md) |
| TSS IST 栈 | [tss_ist.md](tss_ist.md) |
| NX 位 | [nx_bit.md](nx_bit.md) |
| 自旋锁原语 | [spinlock.md](spinlock.md) |
| BKL 大内核锁 | [bkl.md](bkl.md) |
| 用户态驱动 — IPC 基础设施 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 驱动进程 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 多 ELF 启动 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 内核态 kbd ISR 移除 | [user_driver.md](user_driver.md) |
| AP 参与调度（idle 进程模型 + pick_cpu） | [ap_schedule.md](ap_schedule.md) |
| 细粒度锁拆分（BKL 移除） | [fine_grained_lock.md](fine_grained_lock.md) |
| FAT32 写入（touch/mkdir） | [fat32.md](fat32.md) |
| 内存管理系统（slab + mmap + 用户态 malloc 重写） | [mem.md](mem.md) |
| 进程生命周期管理（sys_exit/sys_waitpid/sys_spawn） | [process_lifecycle.md](process_lifecycle.md) |
| hello.elf + shell run 命令 | [process_lifecycle.md](process_lifecycle.md) |
| libc.a 静态库（printf + FILE + _start） | [libc.md](libc.md) |
| KMS 用户态驱动（framebuffer 渲染移至用户态） | [kms.md](kms.md) |
| KMS Bug 修复：主循环通知丢失 + sys_notify WAIT_CHILD + proc_reap PTE NX 位 | [kms.md](kms.md) |
| TSC 时钟 + sched_clock + udelay 修复 | [driver_workflow.md](driver_workflow.md) |
| 定时等待队列 + sys_wait(timeout_ms) | [driver_workflow.md](driver_workflow.md) |
| sys_fb_info / sys_shm_create / sys_shm_attach | [driver_workflow.md](driver_workflow.md) |
| 共享页重构（KBD/KMS 硬编码→动态 shm） | [driver_workflow.md](driver_workflow.md) |
| 驱动工作流重构（ring buffer + sleeping flag + 轮询窗口） | [driver_workflow.md](driver_workflow.md) |
| KMS 帧调度优化（240 slot ring + 16ms 定时刷帧） | [driver_workflow.md](driver_workflow.md) |
| Phase 3: 最小 fd + VT100 + Terminal/Shell 拆分 | [terminal_split.md](terminal_split.md) |
| 设备管理器（dev_table + sys_load_dev/sys_lookup_dev） | [dev_table.md](dev_table.md) |
| 统一 IPC 机制（sys_recv/sys_rpc/sys_reply 替换 sys_wait） | [rpc.md](rpc.md) |
| 用户态驱动（3 层 kbd + bind/unbind + 各驱动工作流） | [user_driver.md](user_driver.md) |
| 键盘驱动 3 层架构 + RPC bind/unbind | [user_driver.md](user_driver.md) |

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动（参与调度）→ 加载 6 个 ELF（disk_driver → kbd_driver → kms_driver → terminal → shell → fs_driver）→ 多进程协作。Phase 3 完成：fd + pipe + terminal 进程拆分。Shell 不再直写 KMS ring，通过 fd 0/1 pipe 与 terminal 通信。Terminal 管理 VT100 状态机 + cell 缓冲区，负责键盘输入分发和屏幕渲染。KMS 驱动不变（仅做像素渲染）。新增 4 个 syscall（sys_pipe/sys_write/sys_read/sys_close），sys_spawn 自动继承 fd 0/1。设备管理器完成：`dev_table[dev_type→PID]` 映射 + `sys_load_dev`/`sys_lookup_dev` 两个新 syscall，驱动间通过设备类型动态发现对端 PID，`common/pid.h` 已删除。

**Phase 2 完成**：驱动工作流重构完成（[driver_workflow.md](driver_workflow.md)），kbd/kms 驱动已迁移到动态 shm + 环形缓冲区 + sleeping flag。disk/fs 驱动也已迁移到动态 SHM（[dynamic_shm_migration.md](dynamic_shm_migration.md)），硬编码共享页基础设施（shm_init/map_shared_pages/7 路物理地址特判）已完全删除。

**统一 IPC 完成**：sys_recv/sys_rpc/sys_reply 已替换 sys_wait（[rpc.md](rpc.md)），建立轻量级同步 RPC 机制。sys_recv 统一接收 IRQ/RPC/notify 三类消息（per-process 16 slot recv 队列 + 超时支持），sys_rpc 同步调用（56 字节载荷 + WAIT_RPC_REPLY 阻塞），sys_reply 跨地址空间拷贝回复。NR_SYSCALL 从 20 增至 22（新增 sys_rpc/sys_reply，sys_notify 移至 #21 改为消息入队模式）。键盘驱动 3 层架构完成（acquire→translate→push），RPC bind/unbind 协议实现（KBD_RPC_BIND/KBD_RPC_UNBIND），kbd_driver 创建含 kbd_ring + kms_ring 的单一 SHM 页，terminal 通过 sys_rpc 绑定键盘。

**待实施**：文件系统重构（[fs_restructure.md](fs_restructure.md)）— LFN 长文件名读写 + Linux FHS 目录结构 + Shell 路径执行改造 + 启动流程改造（init + exec）。Phase 1-3 不改启动流程，Phase 4 需在 kbd 重构完成后审视。

## 多核 SMP — 调度与锁（全部完成）

### 步骤 1: 自旋锁原语 ✅

- [x] 实现 `spinlock_t`（`volatile uint32_t locked`）
- [x] `spin_lock()`: `atomic_exchange` + `pause` 自旋
- [x] `spin_unlock()`: `atomic_store` 释放
- [x] `spin_lock_irqsave` / `spin_unlock_irqrestore`
- [x] 验证: 编译通过，单核启动无回归

### 步骤 2: BKL 大内核锁 ✅（已移除）

> 设计文档: [bkl.md](bkl.md)。BKL 已在步骤 4 中完全移除，替换为细粒度锁。

- [x] `__alltraps` 入口加 `kernel_lock_acquire()`（仅用户态，CS==0x2B）
- [x] `syscall_fast_entry` 入口加 `kernel_lock_acquire()`
- [x] `__trapret` 出口加 `kernel_lock_release()`（仅用户态，CS==0x2B）
- [x] syscall 返回路径加 `kernel_lock_release()`
- [x] `schedule()` 中 `kernel_lock_release()` → `switch_to` → `kernel_lock_acquire()`
- [x] `process_entry` 加 `kernel_lock_acquire()` 补平衡
- [x] `kernel_main` idle 循环前 `kernel_lock_acquire()` + `sti()`
- [x] AP timer 只做 EOI 不调 `schedule()`
- [x] 验证: `-smp 2` 启动，AP idle 但中断处理串行化，系统稳定

### 步骤 3: AP 参与调度 ✅

> 设计文档: [ap_schedule.md](ap_schedule.md)

- [x] `cpu_local_t` 新增 `idle_proc` 字段
- [x] 实现 `create_idle_process(cpu_id)`：分配独立内核栈 + switch_frame（ret_addr=idle_entry）+ 内核 PML4
- [x] 实现 `idle_entry()`：`sti → while(1) { schedule(); sti(); hlt; }`
- [x] BSP/AP idle：栈切换进入 idle_entry
- [x] `schedule()` 改造：per-CPU run_queue + scheduler_lock，移除 BKL 和线性扫描
- [x] `timer_handler`：`tf->cs == 0x2B` 判断抢占（移除 `cpu_id != 0` 守卫）
- [x] 实现 `pick_cpu()`：遍历 `cpu_locals[].run_count`（RELAXED）选最小值
- [x] `run_count` 维护：创建++、BLOCKED--、唤醒++
- [x] 验证: `-smp 2` 启动，进程在不同 CPU 上调度运行

### 步骤 4: 细粒度锁拆分 ✅

> 设计文档: [fine_grained_lock.md](fine_grained_lock.md)

- [x] `scheduler_lock`（per-CPU, irqsave）保护 schedule() + run_queue + state + run_count
- [x] `procs_lock`（全局）保护 `procs[]` 空闲槽位分配 + PCB 初始化
- [x] `fb_lock`（全局）保护 fb_putc cursor + VGA 缓冲区
- [x] `irq_owner[]` atomic 化（`__atomic_store_n` RELEASE / `__atomic_load_n` ACQUIRE）
- [x] `kernel/list.h`：内嵌双向链表（`list_node_t` + `LIST_ENTRY`）
- [x] `proc_t` 新增 `run_node` / `wait_node`
- [x] `cpu_local_t` 新增 `scheduler_lock` / `run_queue`
- [x] BKL 完全移除：汇编入口/出口不再有 `kernel_lock_acquire/release`
- [x] 验证: 多进程调度 + shell 交互 + kbd/disk 驱动正常

---

## 短期目标 — 支持简单 ELF 可执行文件的执行

核心机制已具备（ELF loader、FAT32 read、mmap），进程生命周期管理 + shell 加载能力已实现。

### 进程退出 + 资源回收（sys_exit / sys_waitpid） ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] proc_t 新增 `parent_pid`（pid_t）、`exit_code`（int32_t）字段
- [x] 新增 `PROC_ZOMBIE` 状态 + `WAIT_CHILD` 等待事件
- [x] sys_exit syscall (#5)：设 ZOMBIE + 保存退出码 + notify 父进程 + schedule；无父进程（parent_pid==-1）时直接回收
- [x] proc_reap：回收 PML4 + 用户页映射 + 页表页 + 内核栈 + PCB 槽位（共享页物理帧不回收）
- [x] sys_waitpid syscall (#6)：阻塞等指定子进程 ZOMBIE + 回收资源 + 返回退出码
- [x] 验证: 编译通过，系统启动正常

### 运行时加载新进程（sys_spawn） ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] sys_spawn syscall (#7)：用户态 ELF 缓冲区指针 → process_create_elf，IOPL 权限检查（调用者 IOPL < 请求 IOPL 返回 -EPERM），设 parent_pid，返回子 PID
- [x] 验证: 编译通过，系统启动正常

### 异常退出路径改造 ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] trap_dispatch CPU 异常：用户态异常 → sys_exit(-1) 替代 halt()；内核态异常仍 halt
- [x] 验证: 编译通过，系统启动正常

### hello.elf + shell run 命令 ✅

- [x] hello.cc：最小用户程序（printf 输出 "Hello, World!" → sys_exit(0)）
- [x] shell cmd_run：从 FAT32 读 ELF 文件 → malloc 缓冲区 → sys_spawn → sys_waitpid → 打印退出码
- [x] hello.elf 写入 FAT32（build.sh mcopy）
- [x] 验证: 编译通过，hello.elf 可通过 shell run 命令执行

### libc.a 静态库（printf + FILE + _start） ✅

> 设计文档: [libc.md](libc.md)

- [x] user/include/stdio.h：FILE 结构体（fd/buffer/mode/flags/write_fn）、stdout/stderr、printf/fprintf/vfprintf/fputc/fputs/puts/fflush、EOF 及缓冲模式常量
- [x] user/include/string.h：strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr、memcpy/memset/memmove
- [x] user/include/stdlib.h：增加 exit() 声明 + EXIT_SUCCESS/EXIT_FAILURE
- [x] user/lib/stdio.cc：FILE 实现、stdout/stderr 实例（line-buffered/unbuffered）、vfprintf 格式化引擎（%s/%d/%u/%c/%x/%X/%p/%ld/%lu/%lX/%% + 宽度前缀）
- [x] user/lib/string.cc：字符串和内存操作函数
- [x] user/lib/start.cc：_start 入口点（stdio_init → main → sys_exit）
- [x] user/hello.c：改为 `#include <stdio.h>` + `int main(void)` + `printf("Hello, World!\n")`
- [x] build.sh：新增 libc.a 构建步骤（ar rcs libc.a start.o stdio.o string.o malloc.o）、hello 改用 gcc + libc.a 链接
- [x] 验证: `./build.sh` 编译成功，shell `run hello.elf` 输出 "Hello, World!"

### FAT32 写入（run 命令前置依赖）

| 功能 | 说明 | 优先级 |
|------|------|--------|
| 文件写入 (write) | fs_driver 支持文件内容写入（簇分配+数据写入），需将宿主机编译的 ELF 写入 FAT32 | 高 |
| 删除 (unlink/rmdir) | 目录项标记 0xE5 + FAT 簇释放 | 中 |
| RTC 时间源 | UEFI 获取初始时间 → sys_gettime syscall → 真实时间戳 | 中 |

### 验收目标：shell 中运行 hello.elf

完整流程：宿主机编写 hello.c → gcc 编译 + 静态链接 libc.a → 生成 hello.elf → 写入 FAT32 → OS 启动 → shell `run hello.elf` → 屏幕输出 "Hello, World!" → shell 回到命令行

### KMS 用户态驱动（framebuffer 渲染移至用户态） ✅

> 设计文档: [kms.md](kms.md)

- [x] `common/dev.h`：设备类型定义（DEV_DISK/DEV_KBD/DEV_KMS/DEV_FS/DEV_TERMINAL），驱动 PID 通过 `sys_lookup_dev` 动态发现
- [x] `kernel/fb.cc` 瘦身：删除所有渲染函数 + 字体数据 + Cursor/Framebuffer struct，仅保留 `init_fb` 映射逻辑 + 全局 `g_fb_info`
- [x] `kernel/fb.h` 瘦身：仅保留 `init_fb` 声明
- [x] `kernel/trap.cc`：删除 `sys_putc` 实现、删除 `fb_lock`
- [x] `common/shm.h`：新增 KMS_INFO(0x508000) + KMS_REQ(0x509000) 地址定义 + `kms_fb_info`/`kms_cmd`/`kms_req_shm` 结构体
- [x] `kernel/mem/alloc.cc` `shm_init`：分配 KMS_INFO + KMS_REQ 物理页，末尾拷贝 `g_fb_info` 到 KMS_INFO
- [x] `kernel/proc.cc` `map_shared_pages`：新增 KMS_INFO + KMS_REQ 映射
- [x] `kernel/proc.cc` `process_create_elf`：KMS 进程额外映射 framebuffer 物理页到 0x700000（4KB 页），`map_fb` 参数控制
- [x] `driver/kms_driver.cc`：KMS 用户态驱动（字体数据 + 命令解析主循环 + 渲染逻辑 + framebuffer 直写），IOPL=0
- [x] `user/lib/stdio.cc`：stdout/stderr write_fn 改为 `kms_write_flush`（写 KMS_REQ + notify KMS 驱动）
- [x] `shell/shell.cc`：删除 sys_putc 输出路径，新增 kms_flush 写 KMS_REQ + sys_notify(sys_lookup_dev(DEV_KMS))
- [x] 驱动 PID 引用统一改为 `#include "common/dev.h"` + `sys_lookup_dev(DEV_XXX)`（shell/fs_driver/disk_driver/kbd_driver/kms_driver/terminal/libc）
- [x] `kernel/kernel.cc`：删除 `clear()` 调用，新增 KMS ELF 加载（LBA 201），调整 shell(301)/fs_driver(401) LBA
- [x] `build.sh`：新增 kms_driver.elf 编译 + dd 写入 LBA 201，调整 shell/fs_driver LBA 偏移 + MBR 分区表
- [x] Bug 修复：KMS 主循环改为先处理再等待（避免丢失通知）
- [x] Bug 修复：`sys_notify` 同时匹配 `WAIT_NOTIFY` 和 `WAIT_CHILD`（修复 run 命令卡住）
- [x] Bug 修复：`proc_reap` PTE 物理地址提取改用 `pte & 0x000FFFFFFFFFF000`（修复 NX 位导致 #GP）
- [x] 验证: 编译通过，KMS 驱动启动后屏幕输出正常，shell 交互正常，`run hello.elf` 正常执行

---

## 中期目标 — 支持 Wayland 核心协议

需建立 IPC 通道 + mmap + 双缓冲 + compositor 等基础设施，工作量较大但架构清晰。

### Phase 1: 内核基础设施

#### sys_mmap / sys_munmap ✅

- [x] sys_mmap(size)：匿名私有映射（设计见 [mem.md](mem.md) Phase 3）
- [x] sys_munmap(addr, size)：解除映射 + 归还物理页
- [x] mmap_brk 简单调高 + mmap_region 链表跟踪
- [x] 验证: 用户进程 mmap 一段内存 → 写入数据 → 可访问 → munmap 释放
- **后续**: 共享映射（MAP_SHARED + refcount），见 [mem.md](mem.md) Phase 6

#### VFS + fd 抽象

> Wayland 协议的传输层（Unix socket）和资源共享（fd passing）都依赖 fd 抽象。Phase 3 已完成最小 fd + pipe 实现。

- [x] 内核 fd 表：proc_t 新增 `struct file fd_table[MAX_FD]`（固定大小数组 32 项），每项含 type/flags/pipe 指针
- [x] sys_pipe / sys_write / sys_read / sys_close：pipe 环形缓冲区 + 阻塞/非阻塞读写 + ref_count + 对端唤醒
- [x] sys_spawn fd 继承：子进程自动继承 fd 0/1，pipe ref_count++
- [x] proc_reap fd 清理：退出时遍历 fd_table 关闭所有 pipe
- [x] 验证: shell 通过 fd 0/1 pipe 与 terminal 通信，子进程继承 fd 正常输出
- [ ] `struct file` 通用化：引用计数 + 操作向量（read/write/close/poll），统一 file/pipe/socket/shm
- [ ] sys_open：VFS 层 file_ops 虚函数分派，fs_driver IPC 作为文件后端
- [ ] 验证: 用户进程通过 sys_open("hello.txt") → sys_read(fd, buf) 读取 FAT32 文件

#### pipe / Unix domain socket + fd passing

> Wayland 线协议跑在 Unix domain socket 上，wl_buffer 通过 fd passing (SCM_RIGHTS) 共享。这是 Wayland 的致命依赖。Phase 3 已完成基本 pipe。

- [x] pipe：sys_pipe(fd[2]) 创建一对相联的 file（4KB 环形缓冲区），写入端/读取端各占一个 fd
- [ ] Unix domain socket：sys_socket(AF_UNIX) 创建 socket file，支持 bind/listen/accept/connect
- [ ] fd passing (SCM_RIGHTS)：sendmsg/recvmsg 支持辅助消息传递 fd，内核在收发进程 fd 表间转移 file 引用
- [ ] 验证: 两个进程通过 Unix socket 连接 → 传递 shm fd → 接收方可 mmap 访问共享内存

#### epoll（compositor 事件多路复用）

- [ ] sys_epoll_create()：创建 epoll 实例（内核 file，内部维护监听集）
- [ ] sys_epoll_ctl(epfd, op, fd, event)：添加/修改/删除监听的 fd
- [ ] sys_epoll_wait(epfd, events, maxevents, timeout)：阻塞等待就绪事件
- [ ] 支持 pipe/socket 可读可写事件
- [ ] 验证: compositor 同时监听 N 个 client socket 的可读事件

#### 信号机制

> 合成器至少需要 SIGCHLD 管理 client 子进程退出，未来 gcc 构建依赖完整信号。

- [ ] sigaction 系统调用：注册信号处理函数（SIG_DFL/SIG_IGN/自定义 handler）
- [ ] kill 系统调用：向指定进程发送信号
- [ ] 信号投递：进程返回用户态前检查 pending signals，修改 trapframe 跳转到 handler
- [ ] SIGCHLD：子进程 exit 时向父进程投递
- [ ] 验证: 子进程 exit → 父进程收到 SIGCHLD → handler 执行

#### shm 改进

> 当前 shm_attach 仅能附加目标进程的第一个 shm 区域，限 4 个/进程，无名。Wayland 需要客户端创建任意数量 wl_shm_pool 并通过 fd passing 传给合成器。

- [ ] shm 与 fd 绑定：sys_shm_create 返回 fd 而非裸虚拟地址，shm 通过 fd passing 共享
- [ ] 放宽数量限制：移除 MAX_SHM_PER_PROC=4 限制，按需动态分配
- [ ] sys_shm_attach 改为 fd-based：接收端通过 recvmsg 收到 fd 后，内核自动映射共享页
- [ ] 验证: 进程 A 创建 shm fd → 通过 Unix socket 传递 → 进程 B mmap 访问

#### 鼠标驱动

- [ ] PS/2 鼠标驱动（IOPL=3, irq_bind(44)，用户态进程）
- [ ] 鼠标共享页（mouse_shm：x, y, buttons, events）
- [ ] 验证: 鼠标移动事件写入 mouse_shm，compositor 可读取

#### 像素渲染基础

> 当前 KMS 驱动仅支持文本模式 8x16 字体渲染。合成器需要 pixel-level framebuffer 操作。

- [ ] 合成器进程直写 framebuffer（map_fb=true，已有机制）
- [ ] 像素级绘制原语：fill_rect / blit / memcpy 到 framebuffer 偏移
- [ ] 双缓冲 / page flip：双 framebuffer（front + back），合成器写 back buffer → 原子切换
- [ ] KMS 驱动改造：新增 FLIP 命令（compositor 通过 KMS_REQ 发 page flip 请求）
- [ ] 验证: 合成器填充红色矩形到 back buffer → flip → 屏幕显示红色

### Phase 2: Wayland 用户态服务

#### Wayland 协议库（wire format）

- [ ] Wayland wire format 编解码：header（object_id + opcode + size）+ payload
- [ ] C 结构体封装：wl_message、wl_argument、wl_object
- [ ] 验证: 编码一条 wl_display.get_registry 请求 → 解码正确

#### wl_display + wl_registry

- [ ] compositor: wl_display — 客户端连接管理，对象 ID 分配
- [ ] compositor: wl_registry — 全局对象通告（wl_compositor、wl_shm、wl_seat、wl_output）
- [ ] client: wl_display.get_registry → wl_registry.bind 各全局对象
- [ ] 验证: 客户端连接 compositor，获取 registry，绑定 wl_compositor

#### wl_compositor + wl_surface

- [ ] compositor: wl_compositor.create_surface → 创建 wl_surface 对象
- [ ] surface: damage、attach(buffer)、commit、frame(callback)
- [ ] 验证: 客户端创建 surface，attach buffer，commit

#### wl_shm + wl_buffer

- [ ] wl_shm.create_pool(handle, size) → 创建 wl_shm_pool
- [ ] wl_shm_pool.create_buffer(offset, width, height, stride, format) → 创建 wl_buffer
- [ ] wl_buffer attach 到 surface
- [ ] 验证: 客户端 mmap 创建 pool → 创建 buffer → 填充像素 → attach + commit

#### wl_seat + wl_keyboard

- [ ] compositor: wl_seat — 输入设备抽象
- [ ] wl_keyboard：keymap 通告 + key 事件 + enter/leave（焦点切换）
- [ ] compositor 焦点管理：keyboard focus = 最上层 surface
- [ ] 验证: 键盘事件路由到焦点客户端，Tab 切换焦点

#### compositor 合成循环

- [ ] compositor 主循环：poll 所有客户端通道 → 处理请求 → 合成 surface → page flip
- [ ] 软件合成：逐像素拷贝 surface buffer 到 back framebuffer
- [ ] 验证: 2 个客户端分别提交红/蓝矩形 → compositor 合成 → 屏幕显示两个色块

#### Test client

- [ ] 简单 test client：连接 compositor → 创建 surface → mmap buffer → 填充固定颜色 → commit → 等待 frame callback
- [ ] 验证: test client 运行，屏幕出现彩色矩形

---

## 远期目标 — 支持构建 gcc

需要近完整的 POSIX 环境，依赖短期和中期目标的全部基础设施。仅列出关键依赖项，具体工作在中期目标完成后再细化。

### 进程管理（POSIX 核心）

- [ ] sys_fork：复制进程地址空间 + PCB
- [ ] sys_exec：替换进程地址空间（加载新 ELF）
- [ ] sys_waitpid 完善：支持 SIGCHLD 语义（信号机制见中期 Phase 1）
- [ ] sys_kill / sys_signal：信号机制（见中期 Phase 1）
- [ ] 验证: fork + exec 运行子进程

### 文件系统（POSIX 文件 I/O）

> VFS + fd 抽象、pipe/unix socket 已在中期 Phase 1 实现，此处补充远期所需扩展。

- [ ] FAT32 完善：文件写入、删除、truncate、大文件支持
- [ ] sys_stat / sys_lseek：文件元信息和偏移
- [ ] sys_mmap 文件映射：MAP_SHARED 文件后端
- [ ] 验证: 用户程序通过 sys_open/read/write 读写文件

### Shell 增强

- [ ] 管道符号 `|`：进程间管道连接
- [ ] 重定向 `>` / `<`：文件输出/输入重定向
- [ ] 环境变量：PATH、HOME 等
- [ ] 后台执行 `&`
- [ ] 验证: shell 执行 `ls | cat` 管道命令

### libc 完善

- [ ] stdio：printf/fprintf/fopen/fclose/fread/fwrite
- [ ] string：strlen/strcmp/strcpy/strcat/strchr/memmove
- [ ] stdlib：malloc/free 重写为 size-class slab（基于 mmap，见 [mem.md](mem.md) Phase 4）、atoi/exit
- [ ] unistd：read/write/close/open/lseek/fork/exec/waitpid
- [ ] errno + 错误处理
- [ ] 验证: 标准 C 程序可编译链接运行

### 磁盘扩容

- [ ] disk.img 扩大到足够容纳 gcc 源码树（至少 100MB）
- [ ] FAT32 支持大分区（当前 1MB 限制需解除）
- [ ] 验证: gcc 源码树可完整放入 FAT32 文件系统

---

## 已知 Bug 与技术债务

### 严重（可导致内核崩溃/数据损坏）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 1 | ~~BFC 分配器无锁保护~~ | `kernel/mem/alloc.cc` | **已修复**：`bfc_lock` 自旋锁已添加（`spin_lock_irqsave`/`spin_unlock_irqrestore`） |

### 高（SMP 竞态 / 内存安全）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 9 | 进程创建失败内存泄漏 | `kernel/proc.cc:261-443` | `process_create_elf` 后续 `alloc_page` 失败时，前面已分配的页面未释放 |
| 10 | disk_driver 共享页无边界检查 | `driver/disk_driver.cc:78-84` | `cnt * 512` 可能超过 `resp->data` 的 8180 字节，缓冲区溢出 |
| 11 | `scancode_normal` 越界读 | `driver/kbd_driver.cc:76` | `scancode` 值域 0-255，数组仅 128 项（break code 检查后 make code 仍无范围检查） |
| 12 | framebuffer glyph 渲染越界 | `driver/kms_driver.cc` | 未检查 `py + row < fb.height`，`fb.height` 非 `FONT_HEIGHT` 整数倍时最后几行写入越界 |

### 中（逻辑错误 / 健壮性）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 14 | `udelay` 精度损失 | `arch/x64/smp.cc:217` | `ticks_calibrated / 10000 * us` 先除后乘截断，应改为 `ticks_calibrated * us / 10000`。**方案**: [driver_workflow.md](driver_workflow.md) §1.4 |
| 15 | `total_sectors` 64→32 位截断 | `kernel/kernel.cc:52` | `(uint32_t)((file_end + 511) / 512)`，ELF > 4GB 静默截断 |
| 16 | 用户栈仅 4KB 无 guard page | `kernel/proc.cc:323-328` | 栈溢出触发 #PF 被 kill 而非友好报错，应至少 2 页 + 1 页 guard page |
| 17 | 内核栈仅 8KB | `kernel/proc.cc` | `trap_dispatch → sys_waitpid → schedule → proc_reap` 路径 + trapframe，8KB 偏紧 |
| 18 | `ALIGN_UP` 宏参数多次求值 | `common/macro.h:4` | 若 `aligned` 参数有副作用会被求值两次 |
| 19 | ATA `count=0` 意为 256 扇区 | `kernel/ata.cc:24` | 未检查 `count` 为 0 或超缓冲区大小 |
| 20 | `pick_cpu` 总倾向 CPU 0 | `kernel/proc.cc:248-259` | 所有 CPU run_count 相同时返回 0，负载不均 |
| 21 | `pid` 未校验上界 | `kernel/trap.cc` | `procs[pid]` 未检查 `pid >= MAX_PROC`，可能越界访问 |
| 22 | `malloc free` 不验证指针合法性 | `user/lib/malloc.cc:243-299` | `free()` 不检查 `ptr` 是否在堆范围内，任意指针损坏空闲链表 |
| 23 | ELF `e_phnum`/`e_phentsize` 未校验 | `common/elf.cc:63-65` | `e_phnum` 过大或 `e_phentsize` 为 0 可导致异常行为 |
| 24 | `proc_reap` 未校验 `cr3` 有效性 | `kernel/proc.cc:505-596` | 若 `cr3` 被损坏或已释放，解引用页表为 UAF |

### 低（构建 / 链接脚本 / 代码质量）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 25 | 缺少 `-mcmodel=kernel` | `CMakeLists.txt` | 内核 VMA 在顶层 2GB，C++ 用 `-fPIE`（碰巧 RIP-relative 能工作），C 用 `-fno-pie`（绝对寻址错误），应统一 `-mcmodel=kernel` |
| 26 | 链接脚本缺 section 对齐 | `build_script/linker.ld` | `.text`/`.rodata`/`.data` 无 `ALIGN(4096)`，无法设置不同页权限（R-X / R-O / RW-） |
| 27 | `build.sh` 未检查 ELF 大小 | `build.sh:63-70` | 每个 ELF 槽位 25KB，`dd` 写入不检查文件是否超限，大 ELF 静默覆盖下一槽位 |
| 28 | `boot_info` 复制硬编码 128B | `arch/x64/start.S:60` | `rep movsb` 复制 128B，结构体扩展会截断 |
| 29 | FAT32 分区过小 | `build.sh:85` | 仅 ~230 簇，远低于规范最低 65527 簇 |
| 30 | `__memcpy` 未对齐 8 字节访问 | `arch/x64/utils.h:42-54` | 未检查对齐直接 `uint64_t*` 拷贝，UB in C/C++（x86 硬件容错但可慢） |
| 31 | `serial_init()` 为空 | `kernel/serial.cc:6` | QEMU 不需初始化，真机需设置波特率/FIFO |

---

## 其他扩展（无特定目标归属）

| 功能 | 依赖 | 优先级 | 说明 |
|------|------|--------|------|
| malloc/free 加锁 | — | 中 | MALLOC_LOCK/MALLOC_UNLOCK 占位宏替换为实际锁（需用户态线程/futex 支持） |
| 串口打印统一 NDEBUG 控制 | — | 低 | 全项目串口输出用宏包装，NDEBUG 下为空实现 |
| printf %f 浮点格式化 | 用户态 SSE/FPU + libc.md | 中 | 完整 printf 支持 %f/%lf，需 FPU 上下文保存 + 浮点到十进制转换算法（Ryu） |
| 动态库加载（.so 支持） | — | 远 | PIC 编译 + 动态链接器 + PLT/GOT + 运行时重定位 |

| 运行时 IPI | LAPIC | 低 | reschedule / TLB shootdown |
| ~~disk/fs 驱动迁移到动态 shm~~ | dynamic_shm_migration.md | ~~中~~ ✅ | 已完成 |
| DRM/KMS compositor 重构 | 无 | 高 | 当前 KMS 逐字符 PUTC 消息模型→改为共享文本 buffer + compositor 帧调度模型：shell 写 char grid（纯内存）→ compositor 定时扫描 dirty region 渲染 framebuffer → page flip。类似 Linux DRM 模型：应用写 buffer → 显示服务合成输出 |
| KMS PAT/write-combining | KMS | 低 | framebuffer 页映射加 PCD/PAT 标记，优化真机性能 |
| KMS huge page 映射 | KMS | 低 | 用户态 framebuffer 映射改用 2MB huge page，减少 TLB miss |
| MSI / MSI-X | APIC | 低 | PCIe 设备中断 |
| 用户态 SSE/FPU | 无 | 中 | lazy FPU restore，gcc 构建依赖。**前置**: 内核实现 FXSAVE/RXRSTORE 后，CMake 用户态编译可移除 `-mno-red-zone -mno-sse -mno-sse2 -mno-mmx` |
| objcopy → ld --remove-section | CMake 用户态构建 | 低 | 当前 add_user_elf 保留 compile → objcopy → ld 三步管线，可用 ld `--remove-section .note.gnu.property` 省掉中间 .stripped.o 文件。见 [cmake_user_build.md](cmake_user_build.md) |
| RMW 组合命令 | disk_driver | 低 | 一次 IPC 内读扇区→改→写回 |
| FSINFO 穷闲簇提示 | FAT32 | 低 | 用上次穷闲簇提示加速查找 |
| LFN 支持 | FAT32 | 中 | 解析长文件名目录项，设计见 [fs_restructure.md](fs_restructure.md) Phase 1/3 |
| 多客户端 fs_driver | 无 | 低 | 打开文件表按 PID 索引 |
| VFS 层 | 无 | 低 | 支持多种文件系统类型 |
| 页面换出 (swap) | FAT32 write | 低 | BFC 不足时换出到磁盘 |