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
| 用户态驱动 — 内核态 kbd ISR 移除 + sys_getc deprecated | [user_driver.md](user_driver.md) |
| AP 参与调度（idle 进程模型 + pick_cpu） | [ap_schedule.md](ap_schedule.md) |
| 细粒度锁拆分（BKL 移除） | [fine_grained_lock.md](fine_grained_lock.md) |
| FAT32 写入（touch/mkdir） | [fat32.md](fat32.md) |
| 用户态堆（sys_sbrk） | [sbrk.md](sbrk.md) |

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动（参与调度）→ 加载 4 个 ELF（disk_driver → kbd_driver → shell → fs_driver）→ 多进程协作。BKL 已完全移除，替换为 per-CPU `scheduler_lock` + 全局 `procs_lock` + `fb_lock` + `irq_owner[]` atomic。每 CPU 有独立 idle 进程和 run_queue。`timer_handler` 用 `tf->cs == 0x2B` 判断抢占，AP 参与调度。FAT32 读写完整（readdir/open/read/close/raw_read/touch/mkdir），sbrk 可用。

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

核心机制已具备（ELF loader、FAT32 read、sbrk），需补进程生命周期管理 + shell 加载能力。

### 进程退出 + 资源回收

- [ ] sys_exit syscall：进程主动退出（状态→DEAD，内核回收 PML4 + 映射页 + 堆页 + 内核栈）
- [ ] sys_waitpid syscall：shell 等待子进程退出，获取退出码
- [ ] proc_t 新增 parent_pid 字段，退出时 notify parent
- [ ] 验证: 用户进程退出后 shell 收到通知，PCB 槽位可复用

### 用户态运行时

- [ ] crt0.o：用户态 C 运行时入口 `_start → main(argc, argv) → sys_exit(ret)`
- [ ] 用户态链接脚本：ENTRY(_start)，代码 0x400000，栈 0x7FFFFFFFD000
- [ ] 静态 libc：putc/puts/memcpy/memset/sbrk 封装（编译为 .a 供用户程序链接）
- [ ] 验证: 宿主机编译 hello.c → 静态链接 libc → 生成 ELF → 放入 FAT32

### shell: run 命令

- [ ] shell cmd_run：从 FAT32 读 ELF 文件 → 通过 fs_driver IPC 获取文件内容 → 通知内核加载（需新 syscall 或 shell 直接传 ELF 数据给内核）
- [ ] 内核需支持运行时加载 ELF（当前只在 kernel_main 启动时加载，需扩展为 shell 可请求内核加载新进程）
- [ ] sys_spawn(elf_data_ptr, elf_size, iopl)：或 shell 通过 IPC 将 ELF 数据传给内核
- [ ] shell 等待子进程退出（sys_waitpid）
- [ ] 验证: shell 输入 `run hello.elf` → hello world 输出字符 → shell 回到命令行

### FAT32 写入后续（短期目标支撑）

| 功能 | 说明 | 优先级 |
|------|------|--------|
| 文件写入 (write) | fs_driver 支持文件内容写入（簇分配+数据写入），run 命令需写入 ELF 到磁盘 | 高 |
| 删除 (unlink/rmdir) | 目录项标记 0xE5 + FAT 簇释放 | 中 |
| RTC 时间源 | UEFI 获取初始时间 → sys_gettime syscall → 真实时间戳 | 中 |

---

## 中期目标 — 支持 Wayland 核心协议

需建立 IPC 通道 + mmap + 双缓冲 + compositor 等基础设施，工作量较大但架构清晰。

### Phase 1: 内核/IPC 基础设施

#### sys_mmap / sys_munmap

- [ ] sys_mmap(addr, length, flags)：共享内存区域创建/映射（wl_shm buffer pool 依赖）
- [ ] sys_munmap(addr, length)：解除映射 + 归还物理页
- [ ] 支持匿名映射（MAP_ANONYMOUS）和共享映射（MAP_SHARED）
- [ ] 验证: 用户进程 mmap 一段内存 → 写入数据 → 可访问

#### IPC 通道（替代 Unix socket）

- [ ] 设计 IPC 通道机制：共享页环形缓冲区 + sys_notify，支持动态多客户端连接
- [ ] sys_channel_create()：创建 IPC 通道，返回 channel handle
- [ ] sys_channel_connect(target_pid)：连接到目标进程的通道
- [ ] sys_channel_send(channel, data, len)：发送消息
- [ ] sys_channel_recv(channel, buf, len)：接收消息（阻塞等待）
- [ ] 验证: 两个进程通过 IPC 通道双向通信

#### Handle 传递机制（替代 FD passing）

- [ ] 内核托管资源句柄（handle = 内核资源 ID）
- [ ] sys_handle_send(channel, handle)：通过 IPC 通道传递 handle
- [ ] sys_handle_recv(channel)：接收 handle
- [ ] handle 类型：shared memory region、IPC channel
- [ ] 验证: 进程 A 创建共享内存 → 通过 handle_send 传给进程 B → 进程 B 可访问

#### Framebuffer 双缓冲 / page flip

- [ ] fb.cc 改造：双 framebuffer（front + back），compositor 写 back buffer
- [ ] sys_fb_flip()：原子切换 front/back buffer（更新 FB 基地址）
- [ ] sys_fb_info()：返回 framebuffer 地址/分辨率/格式
- [ ] 验证: 写入 back buffer → flip → 屏幕更新，无撕裂

#### sys_poll / sys_epoll（compositor 事件多路复用）

- [ ] sys_poll(events[], timeout)：监听多个 IPC 通道的可读事件
- [ ] 验证: compositor 同时监听 N 个客户端通道输入

#### 鼠标驱动

- [ ] PS/2 鼠标驱动（IOPL=3, irq_bind(44)，用户态进程）
- [ ] 鼠标共享页（mouse_shm：x, y, buttons, events）
- [ ] 验证: 鼠标移动事件写入 mouse_shm，compositor 可读取

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
- [ ] sys_waitpid 完善：支持 SIGCHLD 语义
- [ ] sys_kill / sys_signal：信号机制
- [ ] 验证: fork + exec 运行子进程

### 文件系统（POSIX 文件 I/O）

- [ ] sys_open / sys_close / sys_read / sys_write：统一文件 I/O 接口
- [ ] sys_stat / sys_lseek：文件元信息和偏移
- [ ] 管道（pipe）：进程间数据流
- [ ] 文件描述符表：per-process fd table，替代固定共享页 IPC
- [ ] FAT32 完善：文件写入、删除、truncate、大文件支持
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
- [ ] stdlib：malloc/free（基于 sbrk）、atoi/exit
- [ ] unistd：read/write/close/open/lseek/fork/exec/waitpid
- [ ] errno + 错误处理
- [ ] 验证: 标准 C 程序可编译链接运行

### 磁盘扩容

- [ ] disk.img 扩大到足够容纳 gcc 源码树（至少 100MB）
- [ ] FAT32 支持大分区（当前 1MB 限制需解除）
- [ ] 验证: gcc 源码树可完整放入 FAT32 文件系统

---

## 其他扩展（无特定目标归属）

| 功能 | 依赖 | 优先级 | 说明 |
|------|------|--------|------|
| sbrk 缩小堆 | sbrk.md | 中 | 内核侧 sys_sbrk 支持 increment<0，用户态 malloc 达阈值后归还堆顶空闲块 |
| malloc/free 加锁 | sbrk.md | 中 | MALLOC_LOCK/MALLOC_UNLOCK 占位宏替换为实际锁（需用户态线程/futex 支持） |
| 串口打印统一 NDEBUG 控制 | — | 低 | 全项目串口输出用宏包装，NDEBUG 下为空实现 |
| 动态库加载（.so 支持） | — | 远 | PIC 编译 + 动态链接器 + PLT/GOT + 运行时重定位 |

| 运行时 IPI | LAPIC | 低 | reschedule / TLB shootdown |
| MSI / MSI-X | APIC | 低 | PCIe 设备中断 |
| 用户态 SSE/FPU | 无 | 中 | lazy FPU restore，gcc 构建依赖 |
| RMW 组合命令 | disk_driver | 低 | 一次 IPC 内读扇区→改→写回 |
| FSINFO 穷闲簇提示 | FAT32 | 低 | 用上次穷闲簇提示加速查找 |
| LFN 支持 | FAT32 | 低 | 解析长文件名目录项 |
| 多客户端 fs_driver | 无 | 低 | 打开文件表按 PID 索引 |
| VFS 层 | 无 | 低 | 支持多种文件系统类型 |
| 页面换出 (swap) | FAT32 write | 低 | BFC 不足时换出到磁盘 |