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
| 用户态驱动 — IPC 基础设施 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 驺动进程 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 多 ELF 启动 | [user_driver.md](user_driver.md) |
| 用户态驱动 — 内核态 kbd ISR 移除 + sys_getc deprecated | [user_driver.md](user_driver.md) |

## 当前状态

内核已完整运行：UEFI 引导 → 内核初始化 → AP 启动 → 加载 3 个 ELF（disk_driver → kbd_driver → shell）→ 多进程协作。BSP 运行调度循环，AP 进入 idle 循环。键盘/磁盘驱动在用户态通过共享页 + wait/notify/irq_bind 与 shell 协作。

## 未完成

### 多核 SMP — 调度与锁

> 设计见 [smp.md](smp.md) 阶段四/五

#### 步骤 1: 自旋锁原语

- [ ] 实现 `spinlock_t`（`volatile uint32_t locked`）
- [ ] `spin_lock()`: `atomic_exchange` + `pause` 自旋
- [ ] `spin_unlock()`: `atomic_store` 释放
- [ ] 验证: 编译通过，单核启动无回归

#### 步骤 2: BKL 大内核锁

> 依赖: 步骤 1

- [ ] `__alltraps` 入口加 `spin_lock(&kernel_lock)`
- [ ] `syscall_fast_entry` 入口加 `spin_lock(&kernel_lock)`
- [ ] `__trapret` 出口加 `spin_unlock(&kernel_lock)`（用户态返回时 + sti）
- [ ] `syscall_ret` 出口加 `spin_unlock(&kernel_lock)` + sti
- [ ] 验证: `-smp 2` 启动，AP idle 但中断处理串行化，系统稳定

#### 步骤 3: AP 参与调度

> 依赖: 步骤 2

- [ ] 实现 `pick_cpu()`: 遍历 `cpu_locals[].run_count`，选最小负载的 CPU
- [ ] AP idle 循环从 `while(1) hlt` 改为调用 `schedule()`（无 READY 进程时 hlt）
- [ ] `process_create`/`process_create_elf` 调用 `pick_cpu()` 分配进程到 AP
- [ ] 验证: 串口输出可见进程在不同 CPU 上调度运行

#### 步骤 4: 细粒度锁拆分（BKL → 独立子系统锁）

> 依赖: 步骤 2，可渐进式拆分，每次拆一个验证一个

- [ ] `scheduler_lock` 保护 `schedule()` + 运行队列
- [ ] `procs_lock` 保护 `procs[]` 进程表
- [ ] 键盘/驱动路径独立锁
- [ ] 验证并发正确性

### 其他扩展

| 功能 | 依赖 | 状态 |
|------|------|------|
| 运行时 IPI | LAPIC | [ ] reschedule / TLB shootdown |
| IPC 消息传递 | 无 | [ ] 用户态服务化基础 |
| 文件系统 | ATA | [ ] 替代硬编码 LBA |
| 更多 shell 命令 | 无 | [ ] echo, clear, pid（help 和 disk read 已完成） |
| MSI / MSI-X | APIC | [ ] PCIe 设备中断 |