---
name: page_plan
description: Higher-half kernel PIE + 分页方案设计文档
type: project
---

# Higher-Half Kernel PIE + 分页实现方案

> **历史文档**：这是 x86-32 / Multiboot2 / GOTOFF 阶段的设计方案。当前代码已迁移至 x86-64 / UEFI / `-mcmodel=kernel` + RIP-relative。本文件仅作历史参考。
>
> 当前实现概要：
> - VMA_BASE = `0xFFFFFFFF80000000`，KERNEL_VMA_BASE = `0xFFFFFFFF80100000`
> - 4级页表 (PML4→PDPT→PD)，2MB huge pages，identity + higher-half 双映射
> - 寻址方式：`-mcmodel=kernel` + RIP-relative（非 GOTOFF）
> - 启动协议：UEFI stub (boot/stub.c)，非 GRUB/Multiboot2
> - 参见 CLAUDE.md 了解当前架构

## 设计目标

将内核从低地址扁平布局升级为 PIE + higher-half 设计：所有代码编译为 `-fPIE`，利用 GOTOFF（PC 推导基址 + 常量偏移）机制实现位置无关。分页前在物理地址运行，分页后无缝切换到虚拟高地址运行。物理加载地址固定 0x100000，PIE 为未来任意基址扩展预留能力。

## 地址常量

| 常量 | 值 | 说明 |
|---|---|---|
| `VMA_BASE` | `0xC0000000` | 4MB 映射区域基址（物理 0 对应的虚拟地址） |
| `KERNEL_VMA_BASE` | `0xC0100000` | 内核实际 VMA（物理 0x100000 的映射） |
| `KERNEL_LMA_BASE` | `0x100000` | 内核物理加载地址 |

地址转换公式：`vaddr = paddr + VMA_BASE`，`PHY_ADDR(vaddr) = vaddr - VMA_BASE`

## PIE 机制：GOTOFF

x86-32 上 `-fPIE` 使用 GOTOFF 寻址（不是 GOT 间接寻址）：

```
call __x86.get_pc_thunk.bx     ; ebx = 当前 EIP
add ebx, GOT_base - $          ; ebx = GOT 基址（从 PC 推导）
mov eax, [ebx + symbol@GOTOFF] ; 直接计算：GOT基址 + 常量偏移 = 实际地址
```

`symbol@GOTOFF` 是 `(symbol_VMA - GOT_VMA)`，链接时常量。最终地址 = PC推导的基址 + 常量偏移。

**不需要 GOT fixup**：GOTOFF 给出的是直接偏移（不是间接指针），代码在不同基址下自动适配：
- 分页前（物理地址运行）：PC = 0x10xxxx → GOTOFF 给出物理地址
- 分页后（虚拟地址运行）：PC = 0xC010xxxx → GOTOFF 给出虚拟地址

## 分页映射

- 物理 0-4MB → 虚拟 0-4MB（identity map，保留供过渡）
- 物理 0-4MB → 虚拟 0xC0000000-0xC0400000（higher-half）
- 4KB 小页，PD entry 0 和 768 共享同一个页表
- 页目录 4KB + 页表 4KB = 8KB 总开销

## 启动流程

```
_start (naked asm):
  1. 保存 multiboot 参数 (%eax → magic_num, %ebx → addr)
  2. 设置 ESP = stack_bottom + 8192 - VMA_BASE（栈物理地址）
  3. Push addr, magic_num 到栈
  4. 计算 boot_main 物理地址 = boot_main_VMA - VMA_BASE
  5. call boot_main

boot_main (C 函数, -fPIE):
  6. PIC thunk 自动生效，GOTOFF 给出物理地址（因 boot_main 在物理地址运行）
  7. 解析 multiboot2 标签（addr 为物理地址，identity map 范围内可访问）
  8. 清零 PD + PT（物理地址操作，GOTOFF 自动给出物理地址）
  9. 填充 PT 条目：物理 0x0-0x3FFFFF → identity + higher-half
  10. 设置 PD[0] = PT_phys | 0x03（present + writable）
  11. 设置 PD[768] = PT_phys | 0x03（共享同一个 PT）
  12. 内联 asm 启用分页并切换到高端地址：
      - mov pd_phys, %cr3
      - mov %cr0, %eax; or $0x80000000, %eax; mov %eax, %cr0（启用分页）
      - mov $stack_top_VMA, %esp（栈切到虚拟地址）
      - jmp kernel_main_VMA（EIP 切到虚拟地址）

kernel_main (C 函数, -fPIE):
  13. 在虚拟地址 0xC010xxxx 运行，GOTOFF 自动给出虚拟地址
  14. 签名不变：void kernel_main(int32_t magic_num, uintptr_t addr)
  15. addr 为物理地址，在 identity map 范围内可访问
  16. 行为与现有基本一致
```

## 链接脚本

所有段 VMA=0xC0100000，LMA=0x100000，AT() 指定物理加载地址：

```ld
ENTRY(_start)

. = 0xC0100000;

SECTIONS {
    .multiboot : AT(0x100000) ALIGN(8)         { *(.multiboot) }
    .stack     : AT(ADDR(.multiboot) + SIZEOF(.multiboot)) ALIGN(16) { *(.stack) }

    .text      : AT(ADDR(.stack) + SIZEOF(.stack))          { *(.text) }
    .rodata    : AT(ADDR(.text) + SIZEOF(.text))            { *(.rodata) *(.rodata.*) }
    .data      : AT(ADDR(.rodata) + SIZEOF(.rodata))        { *(.data) }
    .got       : AT(ADDR(.data) + SIZEOF(.data))            { *(.got) *(.got.plt) }

    . = ALIGN(4096);
    .bss       : AT(ADDR(.got) + SIZEOF(.got)) (NOLOAD)    { *(.bss) *(COMMON) }

    kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.note.gnu.property) *(.note.gnu.build-id) *(.comment) }
}
```

关键点：
- 所有段 VMA 从 0xC0100000 连续排列
- 所有段 LMA 从 0x100000 连续排列（AT() 指定）
- .bss 需 4KB 对齐（PD/PT 数组需页对齐）
- .got / .got.plt 需显式放置（-fPIE 会生成 GOT 段）
- GRUB 搜索 Multiboot2 头在加载映像的前 32KB（LMA=0x100000 起始），不受 VMA 影响

## PD/PT 定义

在 boot.cc 中定义，放在 .bss 段，4KB 对齐：

```c
__attribute__((aligned(4096))) static uint32_t page_directory[1024];
__attribute__((aligned(4096))) static uint32_t page_table[1024];
```

GOTOFF 保证访问正确：
- 分页前：GOTOFF 给出物理地址（用于清零、填充 PT、写 CR3）
- 分页后：GOTOFF 给出虚拟地址（用于后续页表更新）

## _start 地址计算策略

`_start` 是 naked 函数，无 PIC thunk。调用 `boot_main` 使用 `VMA - VMA_BASE`：

```asm
movl $boot_main - VMA_BASE, %eax    ; 物理地址 = VMA - 0xC0000000
call *%eax
```

假设物理基址=0x100000。未来扩展任意基址时改为 EIP 推导方式。

栈地址同理：
- 分页前：`ESP = $stack_bottom + 8192 - VMA_BASE`（物理地址）
- 分页后：`ESP = $stack_bottom + 8192`（VMA 虚拟地址）

## 编译策略

所有文件统一 `-fPIE`：

```bash
# 所有源文件统一编译为 -fPIE
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fPIE -c boot.cc -o boot.o
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fPIE -c kernel.cc -o kernel.o
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fPIE -c serial.cc -o serial.o
```

链接命令不变：`ld -m elf_x86_64 -T linker.ld boot.o kernel.o serial.o -o myos.bin`

## Multibout2 头变更

本次不添加 relocatable header tag 和 information_request tag。物理加载地址固定为 0x100000（由 ELF 程序头 LMA 指定），GRUB 按此加载。

未来实现任意基址支持时需添加：
- `multiboot_header_tag_relocatable`
- `multiboot_header_tag_information_request` 请求 `MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR`
- _start 改为 EIP 推导方式

现有 framebuffer request tag 和 end tag 保持不变。

## identity map 策略

identity map（物理 0-4MB → 虚拟 0-4MB）保留，暂不撤除。用途：
- 分页切换过渡期保证 EIP/ESP 有效
- multiboot info 物理地址可直接访问
- 未来可撤除，届时所有物理地址访问需转换为虚拟地址

## PHY_ADDR 宏修正

mem.h 中现有 `PHY_ADDR(addr) ((uintptr_t)addr & 0xffffff)` 是位运算 hack，仅对 0xC0xxxxxx 地址巧合有效。

修正为减法：`PHY_ADDR(addr) ((uintptr_t)(addr) - VMA_BASE)`，这是正确的地址转换公式。

## 文件变更清单

| 文件 | 变更 |
|---|---|
| `linker.ld` | 重写：所有段 VMA=0xC0100000，LMA 用 AT() 指定，.bss 4KB 对齐，新增 .got 段 |
| `boot.cc` | 修改 _start（物理地址栈、VMA-VMA_BASE 调 boot_main）；新增 boot_main（解析 multiboot、设置 PD/PT、内联 asm 启用分页+跳转）；新增 PD/PT 数组 |
| `build.sh` | 编译 flags 从 `-fPIC -fno-pie` 改为 `-fPIE` |
| `mem.h` | `PHY_ADDR` 宏从 `& 0xffffff` 改为 `- VMA_BASE` |
| `kernel.cc` | 微调（逻辑基本不变，-fPIE 后 GOTOFF 自动适配） |

不在本次范围：mem.cc 集成、relocatable header tag、任意基址支持。