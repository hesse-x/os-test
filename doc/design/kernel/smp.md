# 多核 SMP

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 目标规模 | 2-4 核，全局运行队列 + 自旋锁 | 先全局队列跑通多核，后增量优化 |
| 2 | Per-CPU 状态 | GS base + cpu_local_t | x86-64 SMP 标准做法，swapgs/wrmsr(MSR_GS_BASE) 访问 |
| 3 | AP 启动方式 | SIPI（LAPIC ICR 发送 INIT + Startup IPI） | OVMF 不支持 EFI_mp_services，只能走硬件 SIPI |
| 4 | AP trampoline | 独立汇编文件 ap_trampoline.S | 多模式代码(16/32/64-bit)需精确布局，.org 控制偏移 |
| 5 | GDT/TSS | Per-CPU GDT + Per-CPU TSS | 每个 CPU 独立 GDT 副本，仅 TSS slot 不同 |
| 6 | 中断控制器 | LAPIC + I/O APIC（直接替换 PIC） | UEFI 机器必然有 APIC，不需 PIC fallback |
| 7 | 定时器 | PIT 校准 LAPIC Timer，校准后弃用 PIT | PIT 校准经典做法，~20 行代码 |
| 8 | 锁策略 | 细粒度锁（scheduler_lock + procs_lock 等） | 已从 BKL 演进到细粒度锁 |
| 9 | swapgs | 中断/异常：检查 trapframe.cs 判断来源；syscall：无条件 swapgs | SYSCALL 只从 ring 3 来，无条件 swapgs 安全 |
| 10 | IPI/TLB shootdown | 第一阶段不需要 | scheduler_lock 保护串行，switch_to 重载 CR3 自动刷新 TLB |
| 11 | ACPI/MADT 解析 | 在 EFI stub 中解析，结果通过 boot_info 传内核 | stub 在 UEFI 环境中解析方便 |
| 12 | boot_info SMP 扩展 | ncpus, apic_ids[4], lapic_base, ioapic_base | MAX_CPUS=4 |
| 13 | IST 数量 | 3 个（IST1=NMI #2, IST2=Double Fault #8, IST3=MCE #18） | 仅对最关键异常提供独立栈 |
| 14 | IST 栈大小 | 4KB（1 页） | 这些异常极少发生，处理逻辑简短，4KB 足够 |
| 15 | IST 分配时机 | smp_init_cpu() 中 BFC 分配 | BFC 在 init_mem 后可用，per-CPU 初始化时分配 |
| 16 | IST 复用 | 不复用，每 CPU 独立栈 | NMI 可在任何 CPU 任何时刻触发（包括处理 DF 时），栈必须独立 |

### Per-CPU 数据结构

arch/x64/smp.h : cpu_local_t

字段：
- cpu_id : int — 本 CPU 编号（0 = BSP）
- apic_id : uint32_t — LAPIC ID
- _cur_proc : proc_t* — 替代全局 current_proc（宏 current_proc 访问此字段）
- lapic_base : uint64_t — LAPIC MMIO 基地址
- kernel_stack : uint64_t — 本 CPU 内核栈顶
- tss_rsp0 : uint64_t — 本 CPU TSS 的 RSP0（schedule() 更新，syscall 入口通过 %gs:32 读取）
- run_count : int — 本 CPU 可运行进程数（pick_cpu() 负载均衡用，当前未实现）
- active_slab[9] : Page* — per-CPU slab 活跃页指针

BSP 和每个 AP 各分配一个 cpu_local_t，通过 wrmsr(MSR_GS_BASE) 设置 GS base 指向。中断入口 swapgs 获取内核 GS base。

### TSS IST 栈

每 CPU 3 个 IST 栈（4KB/页），在 smp_init_cpu() 中用 BFC 分配，栈顶虚拟地址写入 TSS ist[] 字段：

- ist[0] = IST1 → NMI（vector #2）
- ist[1] = IST2 → Double Fault（vector #8）
- ist[2] = IST3 → Machine Check（vector #18）

IDT 门描述符 bit 0-2 为 IST index（0=不用 IST，使用 RSP0）。set_idt_gate() 第 4 参数 ist 写入此字段。其余向量 ist=0，使用 TSS RSP0 栈。

全局数据：arch/x64/smp.h — `per_cpu_ist_stack[MAX_CPUS][3]`，每 CPU 3 个 uint64_t 保存栈顶虚拟地址。

IST 栈分配：arch/x64/smp.cc : smp_init_cpu() — BFC alloc_page → 物理地址 → 转虚拟地址 → 写 tss.ist[]
IDT IST 配置：kernel/trap.cc : idt_install() — vector 2→IST1, 8→IST2, 18→IST3

### AP 启动流程

#### EFI stub 侧（ExitBootServices 之前）

1. 解析 ACPI RSDP→XSDT→MADT：获取 LAPIC 基地址、I/O APIC 基地址、CPU LAPIC ID 列表
2. 填充 boot_info 中 ncpus、apic_ids、lapic_base、ioapic_base
3. ExitBootServices → 跳转内核 _start

#### BSP 启动 AP（smp_boot_aps）

BSP 在 kernel_main 完成全部初始化后：

1. 将 ap_trampoline.S 拷贝到物理地址 0x8000（通过 identity map 虚拟地址写入）
2. 为每个 AP（cpu_id 1..ncpu-1）：分配 8KB 内核栈 → smp_init_cpu 填充 per-CPU 数据 → init_ap_idle 创建 idle 进程 → 填充 trampoline 数据区（PML4 物理地址、内核栈顶、ap_entry_c 虚拟地址、cpu_id）
3. 发送 INIT IPI → 等 10ms → 发送 SIPI (vector=0x08) → 等 200μs → 发送第二次 SIPI

#### AP Trampoline（ap_trampoline.S）

AP 从物理地址 0x8000 以 16-bit 实模式开始执行：
- 0x00-0x3F：16-bit code → cli/cld，DS=0x0800，A20 enable，lgdt，CR0.PE=1，ljmpl 到 32-bit code
- 0x40-0x7F：32-bit code → 加载段寄存器，CR4.PAE=1，CR3=pml4_phys，EFER.LME=1，CR0.PG=1，ljmpl 到 64-bit code
- 0x80-0xBF：64-bit code → RSP=kernel_stack，EDI=cpu_id，JMP *ap_entry_c
- 0xC0-0xDF：数据区（BSP 写入，AP 读取：pml4_phys, kernel_stack, ap_entry_addr, cpu_id）
- 0xE0-0xFF：GDT（4 项：null, code32, data, code64）

#### AP 入口 ap_entry_c

arch/x64/smp.cc : ap_entry_c(int cpu_id)

1. smp_apply_cpu 加载本 CPU 硬件状态：lgdt + reload_cs、swapgs 设 GS base、ltr
2. idt_install 加载 IDT（与 BSP 共享同一 IDT）
3. 启用本 CPU LAPIC（MSR + SVR + 屏蔽 LINT0/LINT1）
4. 启动 LAPIC 定时器（periodic，使用 BSP 校准值）
5. sti → idle_entry（AP 参与调度）

注意事项：
- smp_boot_aps 用 uelay 做延时，udelay 将 LAPIC 定时器改为 masked one-shot。所有 AP 启动完成后必须恢复 BSP 的 LAPIC 定时器为 periodic，否则调度器停转
- AP 在 sti 前必须调用 idt_install 加载 IDT，否则定时器中断触发 triple fault

### 中断控制器迁移

BSP 在 AP 启动前完成：映射 LAPIC → 禁用 PIC（mask 全部 16 IRQ）→ 使能 LAPIC → 配置 I/O APIC（IRQ→vector 映射）→ LAPIC Timer 校准 → EOI 改为 LAPIC EOI。

LAPIC Timer 校准：设 PIT 通道 0 为 100Hz → 记录 LAPIC timer 初始计数 → 等 PIT 中断 → 读 LAPIC timer 当前计数 → 计算差值 → 配置 LAPIC divider + periodic 模式。

I/O APIC 中断路由：第一阶段所有外部中断路由到 BSP。arch/x64/apic.cc : I/O APIC 配置。

### swapgs 策略

中断/异常入口 __alltraps：检查栈上 CS，用户态 CS=0x18 → swapgs，内核态 CS=0x08 → 不 swapgs。

syscall 入口 syscall_fast_entry：无条件 swapgs（SYSCALL 只从 ring 3 触发）。

出口同理：返回用户态 swapgs，返回内核态不 swapgs。详见 [syscall.md](syscall.md)。

### 锁模型

arch/x64/smp.h : spinlock_t — volatile uint32_t locked，spin_lock 用 atomic_exchange，spin_unlock 用 atomic_store。

| 锁名称 | 类型 | 保护范围 | 获取顺序 |
|---------|------|---------|---------|
| scheduler_lock | per-CPU spinlock | schedule() 和运行队列（irqsave） | 1 |
| procs_lock | 全局 spinlock | procs[] 进程表 | 2 |
| bfc_lock | 全局 spinlock | BFC 分配器 free_list | 3 |
| fb_lock | 全局 spinlock | framebuffer cursor + 缓冲区 | — |
| slab per-cache lock | per-cache spinlock | slab partial list | — |

锁获取顺序声明：scheduler_lock → procs_lock → bfc_lock。违反此顺序可能导致死锁。详见 [kernel_lock.md](kernel_lock.md)。

### 关键源码位置

- AP trampoline：arch/x64/ap_trampoline.S
- SMP 初始化：arch/x64/smp.cc : smp_boot_aps / smp_init_cpu / ap_entry_c / smp_apply_cpu
- APIC：arch/x64/apic.cc : LAPIC/I/O APIC 配置 + Timer 校准
- Per-CPU 数据：arch/x64/smp.h : cpu_local_t
- IDT 加载：kernel/trap.cc : idt_install
- idle 进程创建：kernel/proc.cc : init_ap_idle
- IST 栈分配：arch/x64/smp.cc : smp_init_cpu() IST 部分
- IST 全局数据：arch/x64/smp.h : per_cpu_ist_stack
- IDT IST 配置：kernel/trap.cc : idt_install() IST 参数

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| IPI（reschedule_ipi） | 当前 scheduler_lock 保证串行，跨 CPU 调度提示需 IPI 通知 idle CPU | 中 |
| TLB shootdown | 当前 switch_to 重载 CR3 自动刷新 TLB，大规模 unmmap 需主动 shootdown | 中 |
| IRQ affinity | I/O APIC 中断路由到特定 CPU，而非全部路由到 BSP | 低 |
| Per-CPU 运行队列 + work stealing | 当前全局运行队列，改为 per-CPU 队列 + idle CPU steal | 低 |
| run_count 负载均衡 | cpu_local_t.run_count 字段已预留，pick_cpu() 未实现 | 低 |
