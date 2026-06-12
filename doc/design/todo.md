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
| 进程生命周期管理（sys_exit/sys_waitpid/sys_spawn） | [process_lifecycle.md](process_lifecycle.md) |
| hello.elf + shell run 命令 | [process_lifecycle.md](process_lifecycle.md) |
| libc.a 静态库（printf + FILE + _start） | [libc.md](libc.md) |
| KMS 用户态驱动（framebuffer 渲染移至用户态） | [kms.md](kms.md) |
| KMS Bug 修复：主循环通知丢失 + sys_notify WAIT_CHILD + proc_reap PTE NX 位 | [kms.md](kms.md) |

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动（参与调度）→ 加载 5 个 ELF（disk_driver → kbd_driver → kms_driver → shell → fs_driver）→ 多进程协作。Framebuffer 渲染已移至用户态 KMS 驱动，内核仅保留 `init_fb` 做物理页映射和元信息保存。`sys_putc` 已删除，所有输出通过 libc 的 `kms_write_flush`（写 KMS_REQ 共享页 + sys_notify KMS 驱动）。`run hello.elf` 可正常执行，子进程退出后 shell 恢复交互。

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

核心机制已具备（ELF loader、FAT32 read、sbrk），进程生命周期管理 + shell 加载能力已实现。

### 进程退出 + 资源回收（sys_exit / sys_waitpid） ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] proc_t 新增 `parent_pid`（pid_t）、`exit_code`（int32_t）字段
- [x] 新增 `PROC_ZOMBIE` 状态 + `WAIT_CHILD` 等待事件
- [x] sys_exit syscall (#8)：设 ZOMBIE + 保存退出码 + notify 父进程 + schedule；无父进程（parent_pid==-1）时直接回收
- [x] proc_reap：回收 PML4 + 用户页映射 + 页表页 + 内核栈 + PCB 槽位（共享页物理帧不回收）
- [x] sys_waitpid syscall (#9)：阻塞等指定子进程 ZOMBIE + 回收资源 + 返回退出码
- [x] 验证: 编译通过，系统启动正常

### 运行时加载新进程（sys_spawn） ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] sys_spawn syscall (#10)：用户态 ELF 缓冲区指针 → process_create_elf，IOPL 权限检查（调用者 IOPL < 请求 IOPL 返回 -EPERM），设 parent_pid，返回子 PID
- [x] 验证: 编译通过，系统启动正常

### 异常退出路径改造 ✅

> 设计文档: [process_lifecycle.md](process_lifecycle.md)

- [x] trap_dispatch CPU 异常：用户态异常 → sys_exit(-1) 替代 halt()；内核态异常仍 halt
- [x] 验证: 编译通过，系统启动正常

### hello.elf + shell run 命令 ✅

- [x] hello.cc：最小用户程序（sys_putc 输出 "Hello, World!" → sys_exit(0)）
- [x] shell cmd_run：从 FAT32 读 ELF 文件 → malloc 缓冲区 → sys_spawn → sys_waitpid → 打印退出码
- [x] hello.elf 写入 FAT32（build.sh mcopy）
- [x] 验证: 编译通过，hello.elf 可通过 shell run 命令执行

### libc.a 静态库（printf + FILE + _start） ✅

> 设计文档: [libc.md](libc.md)

- [x] user/include/stdio.h：FILE 结构体（fd/buffer/mode/flags/write_fn）、stdout/stderr、printf/fprintf/vfprintf/fputc/fputs/puts/fflush、EOF 及缓冲模式常量
- [x] user/include/string.h：strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr、memcpy/memset/memmove
- [x] user/include/stdlib.h：增加 exit() 声明 + EXIT_SUCCESS/EXIT_FAILURE
- [x] user/lib/stdio.cc：FILE 实现、stdout/stderr 实例（line-buffered/unbuffered）、sys_putc_flush、vfprintf 格式化引擎（%s/%d/%u/%c/%x/%X/%p/%ld/%lu/%lX/%% + 宽度前缀）
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

- [x] `common/pid.h`：集中定义所有驱动 PID（DISK_DRIVER_PID=2, KBD_DRIVER_PID=3, KMS_DRIVER_PID=4, SHELL_PID=5, FS_DRIVER_PID=6）
- [x] `kernel/fb.cc` 瘦身：删除所有渲染函数 + 字体数据 + Cursor/Framebuffer struct，仅保留 `init_fb` 映射逻辑 + 全局 `g_fb_info`
- [x] `kernel/fb.h` 瘦身：仅保留 `init_fb` 声明
- [x] `kernel/trap.cc`：删除 `sys_putc` 实现（保留编号返回 -ENOSYS）、删除 `fb_lock`
- [x] `common/shm.h`：新增 KMS_INFO(0x508000) + KMS_REQ(0x509000) 地址定义 + `kms_fb_info`/`kms_cmd`/`kms_req_shm` 结构体
- [x] `kernel/mem/alloc.cc` `shm_init`：分配 KMS_INFO + KMS_REQ 物理页，末尾拷贝 `g_fb_info` 到 KMS_INFO
- [x] `kernel/proc.cc` `map_shared_pages`：新增 KMS_INFO + KMS_REQ 映射
- [x] `kernel/proc.cc` `process_create_elf`：KMS 进程额外映射 framebuffer 物理页到 0x700000（4KB 页），`map_fb` 参数控制
- [x] `driver/kms_driver.cc`：KMS 用户态驱动（字体数据 + 命令解析主循环 + 渲染逻辑 + framebuffer 直写），IOPL=0
- [x] `user/lib/stdio.cc`：stdout/stderr write_fn 改为 `kms_write_flush`（写 KMS_REQ + notify KMS 驱动）
- [x] `shell/shell.cc`：删除 sys_putc 输出路径，新增 kms_flush 写 KMS_REQ + sys_notify(KMS_DRIVER_PID)
- [x] 驱动 PID 引用统一改为 `#include "common/pid.h"`（shell/fs_driver/disk_driver/kbd_driver）
- [x] `kernel/kernel.cc`：删除 `clear()` 调用，新增 KMS ELF 加载（LBA 201），调整 shell(301)/fs_driver(401) LBA
- [x] `build.sh`：新增 kms_driver.elf 编译 + dd 写入 LBA 201，调整 shell/fs_driver LBA 偏移 + MBR 分区表
- [x] Bug 修复：KMS 主循环改为先处理再等待（避免丢失通知）
- [x] Bug 修复：`sys_notify` 同时匹配 `WAIT_NOTIFY` 和 `WAIT_CHILD`（修复 run 命令卡住）
- [x] Bug 修复：`proc_reap` PTE 物理地址提取改用 `pte & 0x000FFFFFFFFFF000`（修复 NX 位导致 #GP）
- [x] 验证: 编译通过，KMS 驱动启动后屏幕输出正常，shell 交互正常，`run hello.elf` 正常执行

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

- [ ] KMS 驱动改造：双 framebuffer（front + back），compositor 写 back buffer
- [ ] sys_fb_flip()：原子切换 front/back buffer（更新 FB 基地址）
- [ ] KMS 新增 FLIP 命令：compositor 通过 KMS_REQ 发 page flip 请求
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

## 已知 Bug 与技术债务

### 严重（可导致内核崩溃/数据损坏）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 1 | BFC 分配器无锁保护 | `kernel/mem/alloc.cc` | `alloc_page`/`free_page` 操作全局 `free_list` 无锁，SMP 并发损坏空闲链表，导致 double-allocation 或丢失页面。加一把全局自旋锁即可 |
| 2 | `sys_waitpid` 写用户指针未校验 | `kernel/trap.cc:389` | `*exit_code_ptr = child->exit_code`，`exit_code_ptr` 来自用户参数未校验，恶意程序可传入内核地址导致任意内核内存写入 |
| 3 | `sys_spawn` 读用户 ELF 指针未校验 | `kernel/trap.cc:396` | `elf_data` 直接解引用用户指针，无效地址触发内核 #PF halt 全机 |
| 4 | ELF 加载越界读 | `kernel/kernel.cc:43-48` | 只读首 512 字节就遍历 program header，`e_phoff + e_phnum * e_phentsize > 512` 时访问未初始化数据 |
| 5 | `pdpt_hh` 越界写入 | `arch/x64/paging.cc:154,169` | `pdpt_hh[510 + n]`，物理内存 > 2GB 时 `n >= 2`，写入 `pdpt_hh[512]` 越界（数组仅 512 项）。`extend_mapping` 循环 `n <= max_1gb_block` 边界错误 |

### 高（SMP 竞态 / 内存安全）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 6 | `sys_notify`/`trap_dispatch` 访问 `procs[]` 无 `procs_lock` | `kernel/trap.cc:253-267, 40-53` | 遍历 `procs[]` 比较 PID 时未持锁，另一 CPU 在 `proc_reap` 清零槽位可读到陈旧数据 |
| 7 | `sys_waitpid` TOCTOU | `kernel/trap.cc:359-365` | `procs_lock` 下验证父子关系后释放锁，轮询前子进程可能已被回收或 PID 槽位被重用 |
| 8 | ZOMBIE 临时设 READY 引入竞态 | `kernel/trap.cc:372` | `child->state = READY` 临时标记，另一 CPU 调度器可能将其当作可运行进程 |
| 9 | 进程创建失败内存泄漏 | `kernel/proc.cc:261-443` | `process_create_elf` 后续 `alloc_page` 失败时，前面已分配的页面未释放 |
| 10 | disk_driver 共享页无边界检查 | `driver/disk_driver.cc:78-84` | `cnt * 512` 可能超过 `resp->data` 的 8180 字节，缓冲区溢出 |
| 11 | `scancode_normal` 越界读 | `driver/kbd_driver.cc:76` | `scancode` 值域 0-255，数组仅 128 项（break code 检查后 make code 仍无范围检查） |
| 12 | framebuffer glyph 渲染越界 | `driver/kms_driver.cc` | 未检查 `py + row < fb.height`，`fb.height` 非 `FONT_HEIGHT` 整数倍时最后几行写入越界 |

### 中（逻辑错误 / 健壮性）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 13 | `fb_lock` 保护不完整 | `kernel/fb.cc` | `fb_putc` 持 `fb_lock`，但 `clear()` 用 `IrqGuard`，`prints()` 不持锁，多 CPU 并发写 framebuffer 竞态 | **已修复**：KMS 移至用户态后此问题消除（KMS 单进程渲染，无并发） |
| 14 | `udelay` 精度损失 | `arch/x64/smp.cc:217` | `ticks_calibrated / 10000 * us` 先除后乘截断，应改为 `ticks_calibrated * us / 10000` |
| 15 | `total_sectors` 64→32 位截断 | `kernel/kernel.cc:52` | `(uint32_t)((file_end + 511) / 512)`，ELF > 4GB 静默截断 |
| 16 | 用户栈仅 4KB 无 guard page | `kernel/proc.cc:323-328` | 栈溢出触发 #PF 被 kill 而非友好报错，应至少 2 页 + 1 页 guard page |
| 17 | 内核栈仅 8KB | `kernel/proc.cc` | `trap_dispatch → sys_waitpid → schedule → proc_reap` 路径 + trapframe，8KB 偏紧 |
| 18 | `ALIGN_UP` 宏参数多次求值 | `common/macro.h:4` | 若 `aligned` 参数有副作用会被求值两次 |
| 19 | ATA `count=0` 意为 256 扇区 | `kernel/ata.cc:24` | 未检查 `count` 为 0 或超缓冲区大小 |
| 20 | `pick_cpu` 总倾向 CPU 0 | `kernel/proc.cc:248-259` | 所有 CPU run_count 相同时返回 0，负载不均 |
| 21 | `pid` 未校验上界 | `kernel/trap.cc:360` | `procs[pid]` 未检查 `pid >= MAX_PROC`，可能越界访问 |
| 22 | `malloc free` 不验证指针合法性 | `user/lib/malloc.cc:243-299` | `free()` 不检查 `ptr` 是否在堆范围内，任意指针损坏空闲链表 |
| 23 | ELF `e_phnum`/`e_phentsize` 未校验 | `common/elf.cc:63-65` | `e_phnum` 过大或 `e_phentsize` 为 0 可导致异常行为 |
| 24 | `proc_reap` 未校验 `cr3` 有效性 | `kernel/proc.cc:505-596` | 若 `cr3` 被损坏或已释放，解引用页表为 UAF |
| 25 | `proc_reap` PTE 物理地址提取未清除 NX 位 | `kernel/proc.cc` | **已修复**：原 `pte & ~0xFFF` 不清除 bit 63（NX），带 PTE_NX 的页提取出错误物理地址，导致 `frames[]` 越界 → #GP。改用 `pte & 0x000FFFFFFFFFF000` |
| 26 | `sys_notify` 不匹配 `WAIT_CHILD` | `kernel/trap.cc` | **已修复**：`sys_exit` → `sys_notify(parent_pid)` 只匹配 `WAIT_NOTIFY`，但 `sys_waitpid` 设 `WAIT_CHILD`，导致父进程永远不被唤醒。改为同时匹配两者 |

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
| sbrk 缩小堆 | sbrk.md | 中 | 内核侧 sys_sbrk 支持 increment<0，用户态 malloc 达阈值后归还堆顶空闲块 |
| malloc/free 加锁 | sbrk.md | 中 | MALLOC_LOCK/MALLOC_UNLOCK 占位宏替换为实际锁（需用户态线程/futex 支持） |
| 串口打印统一 NDEBUG 控制 | — | 低 | 全项目串口输出用宏包装，NDEBUG 下为空实现 |
| printf %f 浮点格式化 | 用户态 SSE/FPU + libc.md | 中 | 完整 printf 支持 %f/%lf，需 FPU 上下文保存 + 浮点到十进制转换算法（Ryu） |
| 动态库加载（.so 支持） | — | 远 | PIC 编译 + 动态链接器 + PLT/GOT + 运行时重定位 |

| 运行时 IPI | LAPIC | 低 | reschedule / TLB shootdown |
| 驱动工作流重构 | 无 | 中 | 统一驱动 IPC 机制（共享页 + notify → 通用通道），统一多客户端模型，PID 发现机制替代硬编码 |
| KMS PAT/write-combining | KMS | 低 | framebuffer 页映射加 PCD/PAT 标记，优化真机性能 |
| KMS huge page 映射 | KMS | 低 | 用户态 framebuffer 映射改用 2MB huge page，减少 TLB miss |
| MSI / MSI-X | APIC | 低 | PCIe 设备中断 |
| 用户态 SSE/FPU | 无 | 中 | lazy FPU restore，gcc 构建依赖 |
| RMW 组合命令 | disk_driver | 低 | 一次 IPC 内读扇区→改→写回 |
| FSINFO 穷闲簇提示 | FAT32 | 低 | 用上次穷闲簇提示加速查找 |
| LFN 支持 | FAT32 | 低 | 解析长文件名目录项 |
| 多客户端 fs_driver | 无 | 低 | 打开文件表按 PID 索引 |
| VFS 层 | 无 | 低 | 支持多种文件系统类型 |
| 页面换出 (swap) | FAT32 write | 低 | BFC 不足时换出到磁盘 |