# TSS IST 栈设计

> **已实现**。每 CPU 3 个 IST 栈用于 NMI、Double Fault 和 Machine Check 异常，在 `smp_init_cpu()` 中分配。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | IST 数量 | 3 个（IST1/IST2/IST3） | 仅对最关键的异常提供独立栈：NMI(#2)、Double Fault(#8)、Machine Check(#18) |
| 2 | IST 栈大小 | 4KB（1 页） | 这些异常极少发生，处理逻辑简短，4KB 足够 |
| 3 | IST 分配时机 | `smp_init_cpu()` 中用 BFC 分配 | BFC 在 `init_mem` 后可用，per-CPU 初始化时分配 |
| 4 | IST 复用 | 不复用，每 CPU 独立栈 | NMI 可在任何 CPU 任何时刻触发，包括正在处理 Double Fault 时，栈必须独立 |
| 5 | IDT IST index | `set_idt_gate()` 增加 ist 参数 | IDT 门描述符 bit 0-2 为 IST index，0=不用 IST，1/2/3=对应 IST 栈 |

## IST 分配（arch/x64/smp.cc，smp_init_cpu()）

```c
// Allocate per-CPU IST stacks (1 page each: NMI, Double Fault, Machine Check)
for (int i = 0; i < 3; i++) {
    Page *ist_page = bfc_alloc.alloc_page(1);
    uint64_t ist_phys = (uint64_t)(ist_page - BFCAllocator::frames) * PAGE_SIZE;
    per_cpu_ist_stack[cpu_id][i] = ist_phys + VMA_BASE + PAGE_SIZE; // 栈顶（高地址）
}

tss->ist[0] = per_cpu_ist_stack[cpu_id][0]; // IST1 = NMI (#2)
tss->ist[1] = per_cpu_ist_stack[cpu_id][1]; // IST2 = Double Fault (#8)
tss->ist[2] = per_cpu_ist_stack[cpu_id][2]; // IST3 = Machine Check (#18)
```

TSS 结构中 `ist[7]` 数组有 7 个 slot（x86-64 定义最多 7 个 IST），当前只使用前 3 个。

## IDT IST 配置（arch/x64/trap.cc，idt_install()）

```c
// IST assignments for critical exceptions
set_idt_gate(2, (uint64_t)vector2, 0x8E, 1);   // NMI → IST1
set_idt_gate(8, (uint64_t)vector8, 0x8E, 2);   // Double Fault → IST2
set_idt_gate(18, (uint64_t)vector18, 0x8E, 3);  // Machine Check → IST3
```

`set_idt_gate()` 的第 4 参数 ist 写入 IDT 门描述符的 bit 0-2。其余向量（包括定时器、键盘等 IRQ）ist=0，使用 TSS RSP0 栈。

## IDT 门描述符 IST 字段

16 字节 IDT 门描述符中：

```
byte 2 (ist): bit 0-2 = IST index (0=不使用 IST，使用 RSP0/RSP1/RSP2)
              bit 3-7 = reserved (must be 0)
```

当 CPU 收到带 IST index > 0 的异常时，自动切换到 `tss.ist[index]` 指定的栈，即使当前已在内核态。这保证 NMI/DF/MCE 始终有可用栈。

## 全局数据（arch/x64/smp.h）

```c
extern uint64_t per_cpu_ist_stack[MAX_CPUS][3]; // IST1=NMI, IST2=DF, IST3=MCE
```

每 CPU 3 个 uint64_t 保存栈顶虚拟地址，用于 TSS ist[] 字段填写。