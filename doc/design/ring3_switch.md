# Ring 3 特权级切换方案

> **历史文档**：这是 x86-32 阶段一的 ring 3 切换设计方案。当前代码已迁移至 x86-64。
>
> 当前实现概要：
> - GDT: 7项（null/code64/data/user_code64/user_data/TSS_low/TSS_high），TSS 跨两个 slot
> - TSS: 128字节，仅 RSP0（无 esp0/ss0），iomap_base=sizeof(tss_t)
> - IDT: 256项，16字节门描述符；vector128 走 syscall_entry
> - trapframe_t: 全64位（R15-R8, RDI-RAX, trapno, err_code, RIP, CS, RFLAGS, RSP, SS）
> - __alltraps: push 16个GP寄存器 + movw $0x10 → trap_dispatch(rdi=&trapframe)
> - __trapret: pop 16个GP寄存器 + addq $16 + iretq
> - USER_CS=0x1B, USER_DS=0x23, TSS_SEL=0x28
> - 参见 CLAUDE.md 了解当前架构

## 设计决策汇总

| # | 决策点 | 选择 | 理由 |
|---|--------|------|------|
| 1 | TSS 内存分配 | 静态全局变量 | 和 GDT 同理，内核永久需要，大小固定，放 .bss |
| 2 | TSS.esp0 填充 | 初始化时设一次（boot stack top） | 阶段一无进程/调度，无需 per-process kernel stack |
| 3 | GDT 排列 | null, kcode, kdata, ucode, udata, TSS | 不改变现有 kernel code/data index，兼容性最好 |
| 4 | trapframe_t | 扩展增加 esp/ss（struct 尾部，eflags 之后） | esp/ss 仅在特权级变化时由 CPU push，不在 __alltraps 手动 push（与 ucore 一致）；ring 0 中断时 esp/ss 不在栈上，不应访问 |
| 5 | TSS 描述符 type | 0x89（Available） | ltr 后 CPU 自动标记 Busy |
| 6 | 内核空间权限 | supervisor-only | 用户态不能访问内核内存，标准微内核安全模型 |
| 7 | PD 内核映射共享 | 拷贝 PDE | 每进程独立 PD，拷贝 ~10-20 个 PDE 代价极小 |
| 8 | 用户态测试程序 | 不分配用户页，EIP 指向 not-present 地址 | #PF 即证明切换成功，代码最少 |
| 9 | 用户态栈 | 不分配，ESP=0xBFFFFFFC | iret 只写寄存器不访问栈内存，#PF 前不需要栈 |
| 10 | PD[0] identity map | 用户态 PD not present；内核 kernel_init_finish 清除 | 引导过渡遗留，初始化完成后清理 |
| 11 | int 0x80 IDT flags | 0xEE（interrupt gate, DPL=3） | ring 3 可调用，进入内核自动禁止中断 |
| 12 | syscall 入口路径 | 独立入口（不走 __alltraps） | 职责分离，后续独立优化 |
| 13 | syscall 段寄存器切换 | push DS/ES/FS/GS 后加载 0x10 | 内核必须用内核数据段访问内存 |
| 14 | #PF ring 3 检测 | 检查 trapframe->cs & 0x3 | CS.RPL 是标准判断 CPL 的方式 |
| 15 | ring 3 测试代码位置 | kernel.cc 内联，注释标注测试用 | 后续正确处理完内核后删除 |
| 16 | kernel_init_finish | 清 identity map + 禁 bump 分配器 | 引导完成后不再需要 |
| 17 | trapframe_t esp/ss 位置 | struct 尾部，eflags 之后 | 和栈上 CPU push 顺序一致 |
| 18 | 验证后内核行为 | halt | 验证目标达成即止 |

## 实现步骤

### 步骤 1：新增基础库

**新增文件：** `arch/x86/lib.h` + `arch/x86/lib.cc`

- `memcpy(void *dst, const void *src, size_t n)` —— freestanding 无标准库，拷贝 PDE 等场景需要
- 后续可按需补充 `memset`, `memmove` 等

**CMakeLists.txt 更新：** `arch/x86/CMakeLists.txt` 中 `add_library(... OBJECT ...)` 加入 `lib.cc`。

验证：编译通过。

---

### 步骤 2：扩展 GDT + TSS

**修改文件：** `arch/x86/paging.cc`, `arch/x86/paging.h`

#### paging.h 新增

```cpp
// TSS 结构体
typedef struct {
  uint32_t link;
  uint32_t esp0;       // ring 0 栈指针
  uint32_t ss0;        // ring 0 栈段选择子（0x10）
  uint32_t esp1;
  uint32_t ss1;
  uint32_t esp2;
  uint32_t ss2;
  uint32_t cr3;
  uint32_t eip;
  uint32_t eflags;
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t es, cs, ss, ds, fs, gs;
  uint32_t ldt;
  uint16_t trap;
  uint16_t iomap_base;  // = sizeof(tss_t)，无 I/O permission bitmap
} __attribute__((packed)) tss_t;

// 用户态选择子
#define USER_CS  0x1B    // index 3, RPL=3
#define USER_DS  0x23    // index 4, RPL=3
#define TSS_SEL  0x28    // index 5
```

#### paging.cc 修改

```cpp
// gdt 从 3 项扩展到 6 项
static gdt_entry_t gdt[6];      // 原 gdt[3]
static tss_t tss;                // 静态全局 TSS

void gdt_init() {
  set_gdt_gate(0, 0, 0, 0, 0);                     // null
  set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0x0C);      // kernel code
  set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0x0C);      // kernel data
  set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0x0C);      // user code (DPL=3)
  set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0x0C);      // user data (DPL=3)
  set_gdt_gate(5, (uint32_t)&tss, sizeof(tss_t)-1, 0x89, 0x0); // TSS, Available, byte granularity
  set_gdt();                                        // lgdt + 段寄存器加载

  // 初始化 TSS
  tss.ss0 = 0x10;
  tss.esp0 = (uint32_t)stack_bottom + 8192;  // boot stack top（虚拟地址）
  tss.iomap_base = sizeof(tss_t);            // 无 I/O bitmap
  __asm__ volatile("ltr %0" :: "r"(TSS_SEL));
}
```

注意：
- `stack_bottom` 在 `boot.cc` 定义，paging.cc 需声明 `extern`
- TSS 描述符 granularity 字段为 `0x0`（byte granularity），因为 limit 是字节偏移（104 或 sizeof(tss_t)-1）
- `ltr` 加载后 CPU 自动将 TSS 描述符 type 改为 Busy（0x89→0x8B）
- `gdt_reg.limit` 更新为 `sizeof(gdt) - 1`（原为 3*8-1，新为 6*8-1）

验证：QEMU 启动后 `ltr` 不触发 #GP，内核继续正常运行。

---

### 步骤 3：扩展 IDT + vector128

**修改文件：** `arch/x86/trap.cc`, `arch/x86/trap.h`, `arch/x86/vectors.S`

#### trap.h

```cpp
#define IDT_ENTRIES 49    // 原 48，新增 vector128
```

#### vectors.S 新增

```asm
vector128:
    pushl $0            /* dummy error code（int 指令不 push error code） */
    pushl $128
    jmp syscall_entry   /* 不走 __alltraps，走独立 syscall 入口 */
```

#### trap.cc

在 `idt_install` 中新增 vector128 的 IDT gate 安装：

```cpp
set_idt_gate(128, (uint32_t)vector128);   // DPL=3，flags=0xEE
```

注意：当前 `idt_install` 用循环安装 0-47，需要在循环后单独安装 vector128。`set_idt_gate` 的 flags 参数需要支持传入 0xEE 而非固定的 0x8E。

验证：内核启动正常，vector128 安装无异常。

---

### 步骤 4：扩展 trapframe_t + 汇编入口

**修改文件：** `arch/x86/trap.h`, `arch/x86/trapentry.S`

#### trap.h — trapframe_t 增加 esp/ss

```cpp
typedef struct {
  pushregs_t regs;
  uint32_t gs;
  uint32_t fs;
  uint32_t es;
  uint32_t ds;
  uint32_t trapno;
  uint32_t err_code;
  uint32_t eip;
  uint32_t cs;
  uint32_t eflags;
  uint32_t esp;    /* 新增：ring 3 中断时 CPU push 或 __alltraps 手动 push */
  uint32_t ss;     /* 新增：同上 */
} trapframe_t;
```

栈布局（从栈顶低地址到栈底高地址）：
```
gs, fs, es, ds, pushregs, trapno, err_code, eip, cs, eflags, esp, ss
```

#### trapentry.S — __alltraps 不手动 push SS/ESP

esp/ss 仅在特权级变化时由 CPU 自动 push（位于 eflags 之后），与 ucore 一致。`__alltraps` 保持原逻辑不变，无需手动 push SS/ESP。

**原因：** 手动 push SS/ESP 在 __alltraps 顶部（最低地址），与 CPU push 的位置（eflags 之后，最高地址）不一致，无法真正统一栈布局。ucore 的做法是 esp/ss 放在结构体末尾，仅在特权级变化时由 CPU 自动 push，ring 0 中断时不访问这两个字段。

```asm
__alltraps:
    /* 原有代码不变，不手动 push SS/ESP */
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal
    ...
```

#### trapentry.S — __trapret 不 pop SS/ESP

iret 在特权级变化时自动从栈 pop ESP 和 SS，无需手动 pop。`__trapret` 保持原逻辑不变。

```asm
__trapret:
    popal
    popl %gs
    popl %fs
    popl %es
    popl %ds
    addl $8, %esp     /* skip trapno + err_code */
    iret              /* 无特权变化: pop EIP, CS, EFLAGS; 有特权变化: pop EIP, CS, EFLAGS, ESP, SS */
```

#### trapentry.S — 新增 syscall_entry / syscall_ret

段寄存器用 `pushl`（32 位，与 __alltraps 一致）。esp/ss 由 CPU 在特权级变化时自动 push，不手动 push。iret 在特权级变化时自动 pop ESP 和 SS。

```asm
syscall_entry:
    /* CPU 已 push: SS, ESP, EFLAGS, CS, EIP（特权变化时） */
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    pushal
    movl $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl %esp           /* push trapframe* 作为参数 */
    call syscall_dispatch
    addl $4, %esp

syscall_ret:
    popal
    addl $8, %esp        /* skip trapno + err_code */
    popl %gs
    popl %fs
    popl %es
    popl %ds
    iret                 /* 无特权变化: EIP,CS,EFLAGS; 特权变化: EIP,CS,EFLAGS,ESP,SS */
```

**和 __trapret 的恢复逻辑本质上相同**：都不手动 pop esp/ss，都依赖 iret 处理特权级变化时的 ESP/SS 恢复。

验证：修改后 QEMU 启动，内核正常运行（现有中断路径不受影响）。

---

### 步骤 5：kernel_init_finish + ring 3 验证测试

**修改文件：** `kernel/kernel.cc`

#### kernel_init_finish

```cpp
// 清理引导临时内容
void kernel_init_finish() {
  // 1. 清除 identity map（PD[0] 设为 not present）
  page_directory[0] = 0;
  flush_tlb();

  // 2. 禁止 bump 分配器（防止误用）
  bump_disable();
}
```

需要在 `paging.h` 中声明 `bump_disable()`，在 `paging.cc` 中实现：
```cpp
static bool bump_disabled = false;
void bump_alloc(size_t size) {
  if (bump_disabled) panic("bump_alloc called after init");
  ...
}
void bump_disable() { bump_disabled = true; }
```

#### test_ring3（验证测试）

```cpp
// [TEST] 验证 ring 0 → ring 3 切换能力，后续删除
static void test_ring3() {
  // 在内核栈上手动构造 iret 帧
  // iret 依次 pop: EIP, CS, EFLAGS, ESP, SS
  // 栈帧从低地址到高地址: EIP → SS
  __asm__ volatile(
    "movl $0x1000, -20(%esp)\n\t"      /* EIP: not-present 地址 */
    "movl $0x1B, -16(%esp)\n\t"        /* CS: user code, RPL=3 */
    "movl $0x202, -12(%esp)\n\t"       /* EFLAGS: IF=1 */
    "movl $0xBFFFFFFC, -8(%esp)\n\t"   /* ESP: 随机用户栈指针 */
    "movl $0x23, -4(%esp)\n\t"         /* SS: user data, RPL=3 */
    "subl $20, %esp\n\t"
    "iret\n\t"
  );
  // 不应到达此处——iret 进入 ring 3 后触发 #PF
}
```

#### kernel_main 流程

```cpp
void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
  // 现有初始化...
  init_mem(mbi);
  isr_init();           // 包含 GDT（含 TSS）、IDT（含 vector128）初始化
  kernel_init_finish(); // 清理引导临时内容

  // [TEST] ring 3 验证
  test_ring3();
  // 不会到达 halt 循环
}
```

验证：QEMU 串口输出 "Ring 3 switch verified! #PF at EIP=0x00001000" 后 halt。

---

### 步骤 6：trap_dispatch 处理 #PF from ring 3

**修改文件：** `kernel/trap.cc`

```cpp
void trap_dispatch(trapframe_t *tf) {
  // 现有逻辑...

  // #PF (vector 14) from ring 3 检测
  if (tf->trapno == 14 && (tf->cs & 0x3) == 3) {
    serial_puts("Ring 3 switch verified! #PF at EIP=");
    serial_put_hex(tf->eip);
    serial_puts("\n");
    while (1) __asm__ volatile("hlt");
  }

  // 现有默认处理...
}
```

验证：QEMU 串口输出正确，内核 halt。

---

## 完整启动流程（阶段一后）

```
GRUB2 → _start (物理地址) → enable_page (物理地址, identity+higher-half)
→ kernel_main (虚拟地址)
→ init_mem → isr_init (GDT+TSS+IDT+vector128)
→ kernel_init_finish (清identity map, 禁bump)
→ test_ring3 (iret→ring3→#PF→串口输出→halt)
```

## 阶段一省略 / 后续补充

| 省略内容 | 后续阶段 | 说明 |
|----------|----------|------|
| TSS.esp0 per-process 更新 | 阶段二 | 进程切换时更新 `tss.esp0 = next_proc->kernel_stack_top` |
| 用户页分配 + 用户程序加载 | 阶段二 | 分配物理页映射到用户地址，拷贝用户代码 |
| 用户栈分配 | 阶段二 | 为每个用户进程分配真实的用户栈页 |
| syscall 入口优化（减少 push） | 阶段三 | 只保存 callee-saved + eax 返回值占位 |
| syscall_dispatch 实现 | 阶段三 | 系统调用分发表 + 参数传递 |
| IRQ/ISR 入口分离 | 阶段三 | ISR（异常）走 __alltraps，IRQ 走更精简路径 |
| 内核 PD[0] identity map 清除 | 阶段一（已完成） | kernel_init_finish 处理 |
| lib.h/lib.cc 补充 | 按需 | 需要时补充 memset/memmove 等 |
| test_ring3 删除 | 阶段二 | 正确的进程创建替代测试代码 |