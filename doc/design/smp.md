# 多核 SMP 设计

## 目标

2-4 核 + 全局运行队列 + 自旋锁。先跑通多核，后优化并发。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 目标规模 | 2-4 核，全局运行队列 + 自旋锁 | 内核无锁基础设施，先全局队列可增量演进 |
| 2 | Per-CPU 状态 | GS base + `cpu_local_t` 结构体 | x86-64 SMP 标准做法，通过 `swapgs`/`wrmsr(MSR_GS_BASE)` 访问 |
| 3 | AP 启动方式 | SIPI（LAPIC ICR 发送 INIT + Startup IPI） | OVMF 不支持 EFI_MP_SERVICES_PROTOCOL，只能走硬件 SIPI 路径；AP 硬件规定从 16-bit 实模式启动 |
| 4 | AP trampoline | 独立汇编文件 `ap_trampoline.S` | 多模式代码(16/32/64-bit)需精确布局，`.org` 控制偏移，不适合内联或 C 字节数组 |
| 5 | GDT/TSS | Per-CPU GDT + Per-CPU TSS | 每个 CPU 独立 GDT 副本，仅 TSS slot 不同，互不干扰 |
| 6 | 中断控制器 | 直接替换 PIC → LAPIC + I/O APIC | UEFI 机器必然有 APIC，不需要保留 PIC fallback |
| 7 | 定时器 | PIT 校准 LAPIC Timer，校准后弃用 PIT | PIT 校准是经典做法，~20 行代码，校准后全用 LAPIC timer |
| 8 | 锁策略 | 先 BKL，后拆细粒度锁 | BKL 保证正确性，改动最小；稳定后逐步拆分 |
| 9 | swapgs 策略 | 用户态来 swapgs，内核态来不 swapgs | 检查 trapframe.cs 判断来源，避免 GS base 翻转 |
| 10 | IPI/TLB shootdown | 第一阶段不需要 | BKL 保证串行，switch_to 重载 CR3 自动刷新 TLB |
| 11 | ACPI/MADT 解析 | 在 EFI stub 中解析，结果通过 boot_info 传给内核 | stub 在 UEFI 环境中解析方便，内核不依赖 ACPI 解析 |
| 12 | boot_info 扩展 | ncpus, apic_ids[4], lapic_base, ioapic_base | MAX_CPUS=4，与 2-4 核目标一致 |

## Per-CPU 数据结构

```c
struct cpu_local_t {
    int cpu_id;           // 本 CPU 编号 (0 = BSP)
    uint32_t apic_id;     // LAPIC ID
    proc_t *_cur_proc;    // 替代全局 current_proc（宏 current_proc 访问此字段）
    uint64_t lapic_base;  // LAPIC MMIO 基地址
    uint64_t kernel_stack;// 本 CPU 内核栈顶
    uint64_t tss_rsp0;   // 本 CPU TSS 的 RSP0
};
```

- BSP 和每个 AP 各分配一个 `cpu_local_t`
- 通过 `wrmsr(MSR_GS_BASE)` 设置 GS base 指向本 CPU 的 `cpu_local_t`
- 中断入口 `swapgs` 获取内核 GS base
- `current_proc` 改为通过 GS base 访问（内联函数）

## boot_info 扩展

新增字段：

```c
#define MAX_CPUS 4

// SMP
uint32_t ncpus;                   // CPU 数量
uint32_t apic_ids[MAX_CPUS];      // 每个 CPU 的 LAPIC ID
uint64_t lapic_base;              // LAPIC MMIO 基地址

// I/O APIC
uint64_t ioapic_base;             // I/O APIC MMIO 基地址
```

> `ioapic_entries[]`、`ap_go_phys`、`ap_entry_phys` 字段已移除：IOAPIC 重定向由内核自行配置，AP 启动改用 SIPI（不再需要 parking 机制）。

## AP 启动流程

### EFI stub 侧（ExitBootServices 之前）

1. 解析 ACPI RSDP→XSDT→MADT：获取 LAPIC 基地址、I/O APIC 基地址、CPU LAPIC ID 列表
2. 填充 `boot_info` 中的 ncpus、apic_ids、lapic_base、ioapic_base
3. `ExitBootServices`
4. 跳转内核 `_start`

### 内核侧 — BSP 启动 AP（`smp_boot_aps`）

BSP 在 `kernel_main` 中完成全部初始化后：

1. 将 `ap_trampoline.S` 的代码拷贝到物理地址 0x8000（通过 identity map 的虚拟地址写入）
2. 为每个 AP（cpu_id 1..ncpu-1）：
   - 分配 8KB 内核栈
   - 调用 `smp_init_cpu()` 填充 per-CPU 数据（cpu_local、GDT、TSS），但不加载硬件状态
   - 调用 `init_ap_idle()` 创建 idle 进程
   - 填充 trampoline 数据区：PML4 物理地址、内核栈顶、`ap_entry_c` 虚拟地址、cpu_id
   - 发送 INIT IPI → 等待 10ms
   - 发送 SIPI (vector=0x08) → 等待 200μs
   - 发送第二次 SIPI → 等待 200μs（Intel 手册推荐）

### AP Trampoline（`ap_trampoline.S`）

AP 收到 SIPI 后从物理地址 0x8000 以 16-bit 实模式开始执行：

```
0x00-0x3F: 16-bit code
    cli/cld
    DS=0x0800（使 DS:offset = 0x8000+offset）
    A20 enable (port 0x92)
    lgdt (GDTR at 0x100)
    CR0.PE = 1
    ljmpl $0x08, $0x8040  ← 跳到 32-bit code（选择子 0x08 = code32）

0x40-0x7F: 32-bit code
    加载段寄存器 (DS/ES/FS/GS/SS = 0x10)
    CR4.PAE = 1
    CR3 = pml4_phys (从 0x80C0 读取)
    EFER.LME = 1
    CR0.PG = 1
    ljmpl $0x18, $0x8080  ← 跳到 64-bit code（选择子 0x18 = code64）

0x80-0xBF: 64-bit code
    RSP = kernel_stack (从 0x80C8 读取)
    EDI = cpu_id (从 0x80D8 读取)
    JMP *ap_entry_c (从 0x80D0 读取)

0xC0-0xDF: 数据区 (BSP 写入，AP 读取)
    0xC0: pml4_phys (uint64_t)
    0xC8: kernel_stack (uint64_t)
    0xD0: ap_entry_addr (uint64_t)
    0xD8: cpu_id (uint32_t)

0xE0-0xFF: GDT (4项)
    0xE0: null
    0xE8: code32 (D=1, Code/Read, 4GB limit)
    0xF0: data (Data/Write, 4GB limit)
    0xF8: code64 (L=1, Code/Read)

0x100-0x107: GDTR
    limit=31, base=0x80E0
```

### AP 入口 `ap_entry_c`（C 代码）

AP 从 trampoline 跳转到 `ap_entry_c(int cpu_id)`（虚拟地址，分页已启用）：

1. 调用 `smp_apply_cpu()` 加载本 CPU 的硬件状态：lgdt + reload_cs、swapgs 设置 GS base、ltr
2. 调用 `idt_install()` 加载 IDT（与 BSP 共享同一 IDT，内核地址空间统一）
3. 启用本 CPU 的 LAPIC（MSR + SVR + 屏蔽 LINT0/LINT1）
4. 启动 LAPIC 定时器（periodic，使用 BSP 校准值）
5. `sti`，进入调度循环：`schedule()` + `hlt`

### 注意事项

- **恢复 BSP 定时器**: `smp_boot_aps()` 内部使用 `udelay()` 做微秒延时，`udelay()` 将 LAPIC 定时器改为 masked one-shot 模式。所有 AP 启动完成后，必须恢复 BSP 的 LAPIC 定时器为 periodic 模式，否则 BSP 不再收到定时器中断，调度器停转
- **AP 必须加载 IDT**: AP 在 `sti()` 前必须调用 `idt_install()` 加载 IDT，否则定时器中断触发时 CPU 读到无效 IDT 条目导致 triple fault

## 中断控制器迁移

### 初始化顺序（BSP，在 AP 启动前）

1. 从 `boot_info` 读取 LAPIC 基地址，映射到虚拟地址
2. 禁用 PIC：mask 全部 16 个 IRQ
3. 使能 LAPIC：写 spurious interrupt vector (0xFF) + software enable bit
4. 配置 I/O APIC：从 `boot_info` 读取重定向表，设置 IRQ→vector 映射（timer→vector 32, keyboard→vector 33）
5. 配置 LAPIC timer：用 PIT 校准后设为 periodic 模式
6. 修改 `trap_dispatch` 中 EOI：`outb(0x20, 0x20)` → `write(lapic_base + LAPIC_EOI, 0)`

### LAPIC Timer 校准

1. 设置 PIT 通道 0 为已知频率（100Hz，divisor=11932）
2. 记录 LAPIC timer 初始计数
3. 等待一次 PIT 中断
4. 读 LAPIC timer 当前计数，计算差值
5. 差值即为一个 PIT 周期内 LAPIC timer 的 tick 数
6. 据此计算 LAPIC timer 的 divider 值，配置 periodic 模式

### I/O APIC 中断路由

- I/O APIC 有 24 个重定向 entry
- 每个 entry 指定：vector、delivery mode、destination mode、destination APIC ID
- 第一阶段所有外部中断路由到 BSP（destination = BSP APIC ID）
- 后续可做 IRQ affinity 路由到不同 CPU

## swapgs 策略

中断入口 `__alltraps` / `syscall_entry`：

1. 检查栈上 CS（CPU 自动保存的）：用户态 CS=0x18 → `swapgs`；内核态 CS=0x08 → 不 swapgs
2. 用户态来的：`swapgs` → 读 GS base 获取 `cpu_local_t` → 切换到 per-CPU 内核栈
3. 内核态来的：继续用当前栈

中断出口 `__trapret` / `syscall_ret`：

1. 用户态返回：`swapgs`（换回用户 GS base）
2. 内核态返回：不 swapgs

## 锁策略

### 第一阶段：大内核锁（BKL）

- `__alltraps` 入口：`cli` + `spin_lock(&kernel_lock)`
- `syscall_entry` 入口：`cli` + `spin_lock(&kernel_lock)`
- `__trapret` 出口：`spin_unlock(&kernel_lock)` + `sti`（用户态返回时）
- `syscall_ret` 出口：`spin_unlock(&kernel_lock)` + `sti`

效果：同一时刻只有一个 CPU 在内核态执行，所有共享数据天然串行访问。

### 第二阶段：细粒度锁（BKL 拆分）

- `scheduler_lock`：保护 `schedule()` 和运行队列
- `procs_lock`：保护 `procs[]` 进程表
- `kbd_lock`：保护键盘缓冲区和 WAIT_KBD 唤醒
- 每次只拆一个子系统，逐个验证

### 自旋锁实现

```c
struct spinlock_t {
    volatile uint32_t locked;
};

void spin_lock(spinlock_t *lk) {
    while (atomic_exchange(&lk->locked, 1) == 1)
        asm volatile("pause");
}

void spin_unlock(spinlock_t *lk) {
    atomic_store(&lk->locked, 0);
}
```

## 实现阶段

> 对应 todo.md 中的 checklist

### 阶段一：Per-CPU 基础设施
- 定义 `cpu_local_t` 结构体
- 实现 GS base 机制
- 修改 `current_proc` 为 per-CPU
- Per-CPU GDT + TSS

### 阶段二：APIC 替换 PIC
- 从 `boot_info` 读取 LAPIC/I/O APIC 信息
- 禁用 PIC
- 使能 LAPIC + 配置 I/O APIC
- 修改 EOI 为 LAPIC EOI
- LAPIC Timer + PIT 校准

### 阶段三：AP 启动（SIPI）
- ~~stub.c：MP Protocol 获取 CPU 数量和 APIC ID~~（OVMF 不支持，改用 SIPI）
- stub.c：MADT 解析（已完成）
- `arch/x64/ap_trampoline.S`：16-bit 实模式 trampoline（real→protected→long mode）
- `arch/x64/smp.cc`：`smp_boot_aps()` + `ap_entry_c()` + IPI 辅助函数
- `kernel/proc.cc`：`init_ap_idle()` 为 AP 创建 idle 进程
- `run.sh`：`-smp 2`

### 阶段四：SMP 调度 + BKL
- 实现自旋锁
- BKL：中断入口/出口加锁
- 全局调度器 + 进程表加锁
- AP 进入调度循环

### 阶段五（优化）：BKL → 细粒度锁
- 拆分 BKL 为独立子系统锁
- 添加 IPI（reschedule_ipi, tlb_shootdown_ipi）
- 验证并发正确性
