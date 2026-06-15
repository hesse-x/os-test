# x86-64 迁移方案

> **已实现**。当前代码已完成 x86-64 迁移，但启动协议已从设计文档中的 Multiboot2+GRUB+trampoline 改为 UEFI stub (boot/stub.c)。
> trampoline 阶段不再需要（UEFI 直接在 64 位长模式启动），其余架构设计（VMA_BASE、4级页表、64位 GDT/TSS/IDT/trapframe 等）与当前实现一致。
> UEFI 启动设计见 [uefi.md](uefi.md)。

> 将32位 higher-half 内核从 arch/x86 全面迁移到 arch/x64

## 设计决策总览

| 决策项 | 32位（现状） | 64位（迁移后） | 理由 |
|--------|-------------|---------------|------|
| 内核位数 | 32位 | **64位** | 32位 x86 已过时 |
| 启动协议 | multiboot2 / GRUB | **multiboot2 + 32→64 trampoline** | 和 UEFI 迁移解耦，改动最小 |
| higher-half 基址 | 0xC0000000 | **0xFFFFFFFF80000000** | Linux 标准，配合 -mcmodel=kernel |
| 编译模型 | -fPIE + GOTOFF | **-mcmodel=kernel + RIP-relative** | 64位内核标配，GOTOFF 不再需要 |
| 初始页表 | 4KB pages, PD[0]+PD[768] | **2MB huge pages, PML4 identity+higher-half** | trampoline 只需3个页表，和双映射策略一致 |
| GDT | trampoline 无, kernel_main 设完整 | **trampoline 最小GDT, kernel_main 设完整** | 和现有分步思路一致 |
| TSS | 104字节, ss0+esp0 | **128字节, 只设 RSP0** | 64位 TSS 格式，IST 后续按需加 |
| trapframe | pushal (8个32位寄存器) | **pushall (16个64位寄存器) + iretq** | 和现有思路一致，寄存器更多 |
| 系统调用 | int 0x80 / iret | **syscall/sysret + MSR** | 64位标配，快速系统调用 |
| 中断控制器 | 8259A PIC | **保留 PIC** | 和 APIC/SMP 解耦 |
| ELF loader | ELF32 | **ELF64** | 全64位 |
| SSE | 无 | **-mno-sse 禁用** | 内核惯例，简化 trapframe |
| 地址空间 | 0xC0000000 以下用户, 以上内核 | **0x0-0x00007FFFFFFFFFFF 用户, 0xFFFFFFFF80000000+ 内核** | Linux 标准 canonical 划分 |
| 多核 | 单核 | **单核** | SMP 是独立问题，以后再处理 |
| 调试 | 串口 | **串口 + GDB + QEMU monitor** | trampoline 阶段必须 GDB |

---

## 启动流程

```
GRUB (multiboot2, 32位保护模式)
  → trampoline.S (32位代码, 物理地址)
    → 设置 PML4/PDPT/PD (2MB huge pages, identity + higher-half)
    → CR4.PAE=1, CR3=PML4, EFER.LME=1, CR0.PG=1
    → lgdt (最小GDT: null/code64/data64)
    → ljmp 到64位代码段
  → _start (64位, arch/x64/start.S)
    → 设置64位栈
    → 传递 multiboot2 参数
    → 调用 kernel_main (虚拟地址)
  → kernel_main (0xFFFFFFFF80100000+)
    → gdt_init (完整GDT + TSS)
    → init_mem (mmap → Bump → frames → extend_mapping → BFC)
    → isr_init (IDT + PIC + PIT)
    → 串口/帧缓冲/键盘
```

### 关键：trampoline 地址切换细节

trampoline 在物理地址运行（和现在 start.S 一样），启用长模式后 ljmp 的目标地址必须同时满足：
- CS=64位代码段选择子（0x08）
- RIP=64位代码的**物理地址**（此时还在 identity map 有效期间）

跳入64位代码后，第一件事就是切换到 higher-half 虚拟地址（栈指针+RIP），然后跳到虚拟地址的 `_start`。

---

## 目录结构（arch/x64/）

```
arch/x64/
  CMakeLists.txt
  trampoline.S      — 32→64 模式切换（32位代码，物理地址运行）
  start.S           — 64位入口（虚拟地址运行）
  vectors.S         — 48个中断向量桩（64位）
  trapentry.S       — __alltraps / __trapret（64位 pushall + iretq）
  syscall_entry.S   — syscall/sysret 入口（swapgs + 保存寄存器）
  linker.ld         — 64位链接脚本
  boot.cc           — multiboot2 header（不变）
  paging.cc/h       — 4级页表 + GDT + TSS + bump allocator
  trap.cc/h         — IDT + PIC + PIT
  utils.h           — outb/inb, KERNEL_CS, L16/H16
  multiboot2.h      — multiboot2 定义（不变）
  lib.cc/h          — memcpy 等
```

---

## 链接脚本

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

. = 0xFFFFFFFF80100000;

SECTIONS {
    .multiboot : AT(ADDR(.multiboot) - 0xFFFFFFFF80000000) {
        *(.multiboot)
    }

    .stack : AT(ADDR(.stack) - 0xFFFFFFFF80000000) {
        *(.stack)
    }

    .text : AT(ADDR(.text) - 0xFFFFFFFF80000000) {
        *(.text)
        *(.text.*)
    }

    .rodata : AT(ADDR(.rodata) - 0xFFFFFFFF80000000) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : AT(ADDR(.data) - 0xFFFFFFFF80000000) {
        *(.data)
        *(.data.*)
        *(.data.rel.ro.local)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
    }

    .got : AT(ADDR(.got) - 0xFFFFFFFF80000000) {
        *(.got)
        *(.got.plt)
    }

    . = ALIGN(4096);
    .bss (NOLOAD) : AT(ADDR(.bss) - 0xFFFFFFFF80000000) {
        *(.bss)
        *(COMMON)
    }

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.comment)
    }
}
```

**关键变化：**
- VMA 从 0xC0100000 → 0xFFFFFFFF80100000
- LMA AT() 减数为 0xFFFFFFFF80000000（VMA_BASE）
- OUTPUT_FORMAT elf64-x86-64
- 不再需要 .got 段（-mcmodel=kernel 用 RIP-relative），但保留以防万一

---

## Trampoline 伪代码

```asm
# trampoline.S — 32位保护模式入口，物理地址运行

.section .text
.code32
.global _trampoline

_trampoline:
    # 保存 multiboot2 参数
    movl %eax, %esi          # magic
    movl %ebx, %edi          # addr

    # 设置32位物理栈
    movl $stack_bottom + 8192 - VMA_BASE, %esp

    # === 构建页表 (2MB huge pages) ===
    # PML4[0] → PDPT (identity)
    # PML4[511] → PDPT (higher-half, 0xFFFFFFFF80000000 = PML4 index 511)
    # PDPT[0] → PD (identity, 1GB)
    # PDPT[0] → PD (higher-half, 1GB)
    # PD: 每个 entry = 2MB page, 512 entries = 1GB

    # 1. 清零 PML4/PDPT/PD
    # 2. PML4[0] = PDPT_phys | 0x03
    # 3. PML4[511] = PDPT_phys | 0x03
    # 4. PDPT[0] = PD_phys | 0x03  (identity 0-1GB)
    # 5. PDPT[0]... (higher-half PDPT entry)
    #    注意：0xFFFFFFFF80000000 的 PML4 index = 511
    #    PDPT index = (0x80000000 >> 30) & 0x1FF = 512... 需要精确计算
    # 6. PD[i] = (i * 2MB) | 0x83  (0x83 = Present + RW + PS(huge page))

    # === 启用长模式 ===
    # CR4.PAE = 1
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    # CR3 = PML4 物理地址
    movl $pml4_phys, %eax
    movl %eax, %cr3

    # EFER.LME = 1 (MSR 0xC0000080)
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    # CR0.PG = 1
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    # === 加载最小 GDT + 跳转64位 ===
    lgdt (gdt64_ptr - VMA_BASE)    # 物理地址

    ljmp $0x08, $(entry64 - VMA_BASE)  # 物理地址的64位入口

.code64
entry64:
    # 已进入64位长模式，仍在物理地址
    # 加载 data 段选择子
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 切换到虚拟地址栈
    movq $(stack_bottom + 8192), %rsp

    # 传递 multiboot2 参数
    movl %esi, %edi          # magic (第1个参数 rdi)
    movl %edi, %esi          # addr  (第2个参数 rsi)
    # 注意：此处需要重新审视寄存器分配，esi/edi 在32位保存的值
    #     需要确保 32→64 转换时值不被覆盖

    # 跳转到虚拟地址的 _start
    movq $_start, %rax
    jmp *%rax
```

### 地址分解：0xFFFFFFFF80000000

```
0xFFFFFFFF80000000 的页表索引：
  PML4 index = (0xFFFFFFFF80000000 >> 39) & 0x1FF = 511
  PDPT index = (0xFFFFFFFF80000000 >> 30) & 0x1FF = 512... 需要修正

精确计算：
  0xFFFFFFFF80000000 = 0b 1111...111 10 000000...000
  bits 47-39 (PML4): 1 1111 1111 = 511
  bits 38-30 (PDPT): 0 0000 0010 = 2
  bits 29-21 (PD):   0 0000 0000 = 0
  bits 20-12 (PT):   N/A (huge page)

所以：
  PML4[511] → higher-half PDPT
  PDPT[2]   → PD (映射 0xFFFFFFFF80000000 - 0xFFFFFFFFC0000000 = 1GB)
```

---

## 64位 GDT

```c
// 完整 GDT（在 kernel_main 的 gdt_init 中设置）
// 索引 0: null
// 索引 1: 64-bit code segment (0x08) — Execute/Read, ring0, L bit=1
// 索引 2: data segment (0x10) — Read/Write, ring0
// 索引 3: user code64 (0x18) — Execute/Read, ring3, L bit=1
// 索引 4: user data (0x20) — Read/Write, ring3
// 索引 5-6: TSS (128位描述符, 占两个 slot, 0x28)

static gdt_entry_t gdt[7];  // 7 entries (TSS 占2个)
```

### 64位代码段描述符关键位

| 字段 | 值 | 说明 |
|------|-----|------|
| L (Long mode) | 1 | 64位代码段 |
| D (Default operation size) | 0 | L=1时必须为0 |
| G (Granularity) | 0 | byte granularity |
| Limit | 0 | 64位忽略limit |
| Type | 0xA (Execute/Read) | |
| DPL | 0 (ring0) / 3 (ring3) | |

---

## 64位 IDT

```c
// 64位 IDT 门描述符：16字节
struct idt_entry_t {
    uint16_t offset_low;     // offset[15:0]
    uint16_t selector;       // code segment selector (0x08)
    uint8_t  ist;            // IST offset (0 = 不用 IST)
    uint8_t  type_attr;      // 0x8E = interrupt gate, 0xEE = user interrupt
    uint16_t offset_mid;     // offset[31:16]
    uint32_t offset_high;    // offset[63:32]
    uint32_t reserved;       // 必须为0
};
```

---

## 64位 trapframe

```c
struct trapframe_t {
    // __alltraps 手动 push（从高地址到低地址）
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t trapno;
    uint64_t err_code;

    // CPU 自动 push（iretq 自动 pop）
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
```

### __alltraps 伪代码

```asm
__alltraps:
    # CPU 已自动 push: SS, RSP, RFLAGS, CS, RIP, [error code]

    # 保存段寄存器
    pushq %ds
    pushq %es

    # 保存所有通用寄存器
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # 设置内核数据段
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # 调用 trap_dispatch(rdi = &trapframe)
    movq %rsp, %rdi
    call trap_dispatch

    # __trapret: 恢复
__trapret:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    popq %es
    popq %ds

    # 如果有 error code 则 addq $8, %rsp 跳过
    # 如果没有则跳过此步（向量桩已 push dummy 0）

    iretq
```

---

## syscall/sysret 入口

```asm
# syscall_entry.S

syscall_entry:
    # CPU 自动做: RCX = RIP, R11 = RFLAGS
    # 需要手动: swapgs, 保存用户 RSP, 加载内核栈

    swapgs                          # 切换 GS 到内核 GS base
    movq %gs:0, %rsp                # 从 per-CPU 区加载内核栈（单核可简化为全局变量）
    swapgs                          # 切回用户 GS base

    # 保存用户态状态
    pushq $0x1B                     # SS (user data, ring3)
    pushq %rcx                      # 用户 RSP (syscall 不自动保存, 需从 RCX 恢复? 不对)
    # 注意：syscall 不自动 push SS/RSP，需要手动从 MSR 保存
    # 实际做法：syscall 前 RSP 已在用户栈，需要用其他方式获取

    # 简化方案：syscall 前让用户程序把 RSP 存到约定位置
    # 或者：在内核入口用 swapgs 获取 GS base，从中读 saved_rsp

    pushq %r11                      # RFLAGS (被 SFMASK 屏蔽后的)
    pushq $0x23                     # CS (user code64, ring3)
    pushq %rcx                      # 用户 RIP

    # 保存通用寄存器（和 __alltraps 一样的 pushall）
    pushq %rax ... %r15

    # 设置内核段
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    # 调用 syscall_dispatch
    movq %rsp, %rdi
    call syscall_dispatch

syscall_ret:
    # 恢复通用寄存器
    popq %r15 ... %rax

    popq %rcx                       # 用户 RIP → RCX (sysret 需要)
    addq $8, %rsp                   # 跳过 CS (sysret 不用)
    popq %r11                       # RFLAGS → R11
    popq %rsp                       # 恢复用户 RSP
    addq $8, %rsp                   # 跳过 SS

    sysretq
```

### MSR 设置

```c
// 在 isr_init 中设置
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_CSTAR   0xC0000083
#define MSR_SFMASK  0xC0000084
#define MSR_EFER    0xC0000080

void setup_syscall() {
    // STAR[63:32] = sysret CS (0x23), STAR[31:0] = syscall CS (0x08)
    // sysret 时 CS = STAR[63:48], syscall 时 CS = STAR[47:32]
    uint64_t star = ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    // LSTAR = syscall 入口地址
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // SFMASK: 屏蔽的中断标志位（IF）
    wrmsr(MSR_SFMASK, (1 << 9));

    // EFER.SCE = 1 (启用 syscall/sysret)
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1 << 0);
    wrmsr(MSR_EFER, efer);
}
```

---

## 用户地址空间布局

```
0x0000000000000000 ──────────── 消极区（不映射，捕获 NULL 解引用）
0x0000000000400000   代码区
0x0000000000600000   堆区（brk 按需扩展）
0x00007FFFFFFFD000   用户栈顶（向下增长）
0x00007FFFFFFFFFFF ── 用户空间上限（canonical 低半区）
0x0000800000000000 ── 非 canonical 区（不使用）
0xFFFFFFFF80000000 ── 内核基址（higher-half）
0xFFFFFFFF80100000   内核代码段
                  ...  内核数据段
0xFFFFFFFFFFE00000 ── 内核虚拟地址空间上限（-2GB 边界）
```

---

## 构建体系变化

### 编译器标志

```cmake
# 32位（现状）
-m32 -fPIE -ffreestanding -nostdlib

# 64位（迁移后）
-m64 -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -ffreestanding -nostdlib
```

**各标志说明：**
- `-mcmodel=kernel`：内核代码在 -2GB 地址空间，用 RIP-relative 寻址
- `-mno-red-zone`：内核代码不能依赖 red zone（中断会覆盖它）
- `-mno-sse -mno-sse2 -mno-mmx`：禁用 SSE/MMX，不保存 XMM 寄存器

### 链接

```cmake
# 32位
ld -m elf_x86_64 -T arch/x86/linker.ld ... -o myos.bin

# 64位
ld -m elf_x86_64 -T arch/x64/linker.ld ... -o myos.bin
# 注意：-m elf_x86_64 其实就是64位 emulation，现在恰巧用对了
```

### toolchain

```cmake
# 32位
CMAKE_C_FLAGS "-m32"
CMAKE_CXX_FLAGS "-m32"

# 64位
CMAKE_C_FLAGS "-m64"
CMAKE_CXX_FLAGS "-m64"
```

---

## 实现顺序

```
步骤1: 构建体系 (CMake + linker script + toolchain)
  → 验证: 64位编译通过，链接出 myos.bin

步骤2: trampoline.S + start.S + boot.cc + 最小 paging
  → 验证: QEMU 启动，GDB 确认进入64位长模式

步骤3: 串口驱动 (serial.cc 适配64位)
  → 验证: 串口输出 "Hello from 64-bit kernel"

步骤4: 完整 paging (4级页表 + gdt_init + TSS + bump_alloc + extend_mapping)
  → 验证: 页表映射正确，GDT/TSS 加载成功

步骤5: IDT + 中断 (trap.cc + vectors.S + trapentry.S + PIC)
  → 验证: 定时器中断工作，中断处理函数被调用

步骤6: syscall/sysret (syscall_entry.S + MSR 设置)
  → 验证: 从用户态执行 syscall 并返回

步骤7: 内存管理 (Bump/BFC 分配器 + frames 数组)
  → 验证: 分配/释放页正常

步骤8: 进程/调度 (trapframe_t 64位化 + switch_to 适配)
  → 验证: ring3 进程轮转

步骤9: 驱动 (kbd + fb 适配64位)
  → 验证: 键盘回显 + 帧缓冲文字输出

步骤10: ELF64 loader + shell
  → 验证: 从磁盘加载64位用户程序并执行
```

每一步都有明确的验证点，确保前一步稳固再进入下一步。

---

## 后续迁移（x64 稳定后）

- APIC（Local APIC + I/O APIC）替代 PIC → **已完成**，见 [smp.md](smp.md)
- TSS IST 配置（NMI / double fault 独立栈）→ **已完成**，见 [tss_ist.md](tss_ist.md)
- UEFI 原生启动（EFI stub）→ **已完成**，见 [uefi.md](uefi.md)
- 多核 SMP 支持 → **部分完成**（AP 启动但仅 idle），见 [smp.md](smp.md)
- NX 位（PTE bit63）→ **已完成**，见 [nx_bit.md](nx_bit.md)
- 用户态驱动 IPC → **已完成**，见 [user_driver.md](user_driver.md)
- SYSCALL/SYSRET 快速系统调用 → **已完成**，见 [syscall.md](syscall.md)
- 用户态 SSE/FPU：lazy FPU restore → **未完成**
