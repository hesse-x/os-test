# 微内核实现进度

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

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动（参与调度）→ 加载 3 个 ELF（disk_driver → kbd_driver → shell）→ 多进程协作。BKL 已完全移除，替换为 per-CPU `scheduler_lock` + 全局 `procs_lock` + `fb_lock` + `irq_owner[]` atomic。每 CPU 有独立 idle 进程和 run_queue。`timer_handler` 用 `tf->cs == 0x2B` 判断抢占，AP 参与调度。

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

## 未完成

### pwd/touch/mkdir + FAT32 写入（设计文档: [fat32.md](fat32.md)）

- [ ] disk_driver: ATA PIO LBA28 WRITE cmd (cmd=1)
- [ ] shm.h: disk_req_shm 扩到 2 页，地址常量重排（8 页布局）
- [ ] shm.h: 新增 FS_CMD_CREATE=5, FS_CMD_MKDIR=6
- [ ] kernel/proc.cc: shm_init 多分配 1 物理页，map_shared_pages 8 页映射
- [ ] fs_driver: handle_create (touch) — 解析路径→查找/创建目录项→更新时间戳
- [ ] fs_driver: handle_mkdir — 创建目录项+分配簇+初始化 . 和 ..
- [ ] fs_driver: find_free_cluster — 线性扫描 FAT 表
- [ ] fs_driver: allocate_cluster — 更新 FAT1+FAT2
- [ ] fs_driver: dir_chain_extend — 目录簇链扩展
- [ ] fs_driver: disk_write — 封装 disk_req WRITE IPC
- [ ] fs_driver: 缓存更新（写后更新缓存）
- [ ] shell: 重构为表驱动命令解析
- [ ] shell: cmd_pwd (puts(cwd))
- [ ] shell: cmd_touch (fs_req CREATE)
- [ ] shell: cmd_mkdir (fs_req MKDIR)
- [ ] 验证: touch 创建空文件、touch 更新已存在文件时间戳、mkdir 创建目录

### FAT32 写入后续优化

| 功能 | 说明 | 优先级 |
|------|------|--------|
| RMW 组合命令 | disk_driver 新增 READ-MODIFY-WRITE cmd，一次 IPC 内读扇区→改→写回 | 中 |
| FSINFO 空闲簇提示 | 解析 BPB FSINFO sector，用上次空闲簇提示加速查找 | 低 |
| RTC 时间源 | UEFI 获取初始时间 → sys_gettime syscall → 真实时间戳 | 中 |
| 文件写入 (write) | fs_driver 支持文件内容写入（簇分配+数据写入） | 高 |
| 删除 (unlink/rmdir) | 目录项标记 0xE5 + FAT 簇释放 | 中 |
| LFN 支持 | 解析长文件名目录项 | 低 |
| 多客户端 | fs_driver 打开文件表按 PID 索引 | 低 |
| VFS 层 | 支持多种文件系统类型 | 低 |

### 内存管理增强

| 功能 | 说明 | 优先级 |
|------|------|--------|
| 用户态堆 (sbrk/mmap) | sys_sbrk syscall → BFC 分配物理页 → 映射到用户地址空间 | 高 |
| 共享内存扩展 | sys_shm_create/shm_attach → 进程间共享内存区域创建与映射 | 中 |
| 进程退出 + 内存回收 | proc_exit → 释放 PCB + 用户 PML4 + 所有映射页 | 高 |
| 页面换出 (swap) | BFC 不足时换出到磁盘 FAT32 分区 | 低 |
| 内存统计 | sys_meminfo → 返回已用/空闲页数 | 低 |

### 其他扩展

| 功能 | 依赖 | 状态 |
|------|------|------|
| 运行时 IPI | LAPIC | [ ] reschedule / TLB shootdown |
| IPC 消息传递 | 无 | [ ] 用户态服务化基础 |
| 更多 shell 命令 | 无 | [ ] echo, clear, pid |
| MSI / MSI-X | APIC | [ ] PCIe 设备中断 |
| 用户态 SSE/FPU | 无 | [ ] lazy FPU restore |
