# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

32位 x86 higher-half 内核，Multiboot2/GRUB2 引导，freestanding C++ + 纯汇编。所有代码编译为 `-fPIE`，利用 GOTOFF（PC推导基址+常量偏移）实现位置无关。`_start` 和 `enable_page` 在物理地址运行，设置分页后跳转到 `kernel_main` 在虚拟地址 0xC010xxxx 运行。

## 构建与运行

```bash
./build.sh          # 编译 + 链接 + 生成 ISO (myos.iso)
./build.sh clear    # 清除 .o, .bin, .iso 产物
./run.sh            # QEMU 启动（OVMF UEFI, 512MB, VGA, 串口输出到 log.txt）
```

编译流程：`start.S` → `vectors.S` → `trapentry.S` → `boot.cc` → `kernel.cc` → `serial.cc` → `mem.cc` → `fb.cc` → `isr.cc` → `kbd.cc`，C++ 统一 `-fPIE`，纯汇编不用 PIE。链接：`ld -m elf_x86_64 -T linker.ld`（输出 elf32-i386）。

**添加新源文件：** 在 build.sh 增加编译步骤（复制现有 g++ 行），并在 ld 行加入 `.o` 文件。

## 启动流程

```
GRUB2 (0x100000) → _start (start.S, 物理地址) → enable_page (mem.cc, 物理地址) → 分页启用 → kernel_main (kernel.cc, 虚拟地址 0xC010xxxx)
```

1. GRUB2 在 LMA=0x100000 加载内核，%eax=魔数, %ebx=multiboot_info地址
2. `_start`（纯汇编）：保存 %eax/%ebx → 设置物理栈 → 调用 `enable_page`（VMA-VMA_BASE计算物理地址）
3. `enable_page`：清零 PD/PT → 填充 identity map + higher-half → 内联asm启用分页(CR3/CR0) → 返回
4. `_start`：切换ESP到虚拟地址 → 间接调用 `kernel_main`
5. `kernel_main`：`init_mem`（mmap解析、Bump/BFC分配器、扩展higher-half映射、设备映射区、framebuffer初始化）→ `isr_init`（GDT/IDT/PIC/PIT/键盘）→ 串口输出 → framebuffer文字渲染 → 键盘回显 → halt循环

## 地址映射

- 物理 0-4MB → 虚拟 0-4MB（identity map，过渡期保留）
- 物理 0-4MB → 虚拟 0xC0000000-0xC0400000（higher-half，PD[0] 和 PD[768] 共享同一个 PT）
- `init_mem` 动态扩展：物理 RAM 每 4MB 块对应 PD[768+n]，覆盖全部可用内存
- 设备映射区：`device_vma_base = ALIGN_UP(VMA_BASE + max_phys_addr, 4MB)`，framebuffer 等设备 MMIO 映射到此区域
- 地址转换：`vaddr = paddr + VMA_BASE`，`PHY_ADDR(vaddr) = vaddr - VMA_BASE`

## 链接脚本（linker.ld）

VMA=0xC0100000，LMA 用 AT() 指定从 0x100000 起连续排列：

`.multiboot` → `.stack` → `.text` → `.rodata` → `.data` → `.got` → `.bss`(4KB对齐)

导出符号：`kernel_end`（内核映像结束地址）

## GOTOFF 机制

`-fPIE` 在 x86-32 使用 GOTOFF 寻址（不是 GOT 间接）：
- `mov eax, [ebx + symbol@GOTOFF]` — GOT基址+常量偏移=实际地址
- 物理地址运行时 GOTOFF 给出物理地址；虚拟地址运行时自动给出虚拟地址
- **不需要 GOT fixup**：偏移是链接时常量，PC推导基址自动适配
- 外部符号（如 `kernel_main`）不走 GOTOFF，用 R_386_32 重定位（直接VMA）

## 关键源文件

| 文件 | 作用 |
|---|---|
| `start.S` | 纯汇编入口 `_start`，保存multiboot参数，设置物理栈，调用 `enable_page`，栈平移，调用 `kernel_main` |
| `boot.cc` | Multiboot2 头构造（magic/framebuffer tag/end tag），引导栈定义 |
| `mem.cc` | `enable_page`（物理地址运行，初始分页），`init_mem`（mmap解析、Bump/BFC分配器、扩展higher-half映射、设备映射区），BFC分配器实现，GDT设置 |
| `kernel.cc` | `kernel_main`（虚拟地址运行），调用 `init_mem` → `isr_init` → 串口输出 → framebuffer文字渲染 → 键盘回显 |
| `isr.cc` | IDT/PIC/PIT初始化，中断分发（`trap`），定时器/键盘IRQ处理 |
| `isr.h` | IDT/trapframe结构定义，I/O内联函数（`outb`/`inb`），`KERNEL_CS`/`L16`/`H16` |
| `vectors.S` | 48个中断向量桩（vector0-47），处理error code有无的差异，跳转 `__alltraps` |
| `trapentry.S` | `__alltraps`（保存寄存器→调用trap）和 `__trapret`（恢复寄存器→iret） |
| `kbd.cc/h` | 键盘驱动：Set 1 scancode→ASCII表，`kbd_handle`（IRQ1回调），`kbd_register_handler` |
| `fb.cc/h` | Framebuffer 驱动：8x16 PC BIOS字体渲染，`init_fb`（设备映射区页表映射），`clear/fb_putc/prints`，光标管理 |
| `serial.cc/h` | COM1 串口驱动（init/putc/puts），QEMU `-serial file:log.txt` 捕获输出 |
| `mem.h` | Page/BFCAllocator 定义，VMA/LMA 常量，PHY_ADDR 宏，全局变量声明（page_directory/page_table/device_vma_base） |
| `kernel.h` | `kernel_main` 声明 |
| `mem_layout.h` | VMA_BASE/KERNEL_VMA_BASE/KERNEL_LMA_BASE/PHY_ADDR（与 mem.h 有重叠） |
| `common.h` | 链接脚本导出符号声明（`kernel_end`） |
| `macro.h` | ALIGN_UP 宏 |
| `multiboot2.h` | Multiboot2 规范定义 |

## 内存管理架构

两阶段分配器，均在 `init_mem` 中初始化：

1. **Bump 分配器**：极简线性分配，`kernel_end` 起始，仅向前增长。用于 `init_mem` 阶段分配 frames 数组和页表。返回虚拟地址。
2. **BFC 分配器**：Best-Fit Contiguous，基于 frames 数组 + 有序 free_list，支持分配/释放/合并。`init_mem` 完成后可用于通用分配。

初始化顺序：mmap解析 → Bump初始化 → 分配frames数组 → 标记FREE/USED/RESERVED → 扩展higher-half映射 → 设置device_vma_base → 初始化framebuffer → 建立free_list。

## 中断架构

- **GDT**：3项（null/code/data），在 `mem.cc` 的 `gdt_init` 中设置，通过 `lgdt` + 远跳转加载
- **IDT**：48项（向量0-47），`isr_init` 调用 `idt_init` 安装所有门描述符（selector=0x08, flags=0x8E）
- **向量桩**：`vectors.S` 定义 vector0-47，CPU异常自动push error code的向量（8,10-14,17）不push dummy 0，其余push 0+向量号
- **trapentry.S**：`__alltraps` 保存段寄存器+pushal→调用 `trap()`；`__trapret` 恢复→iret
- **PIC**：master(0x20)/slave(0xA0)重映射，master IRQ 32-39，slave IRQ 40-47；仅unmask timer(IRQ0)和keyboard(IRQ1)
- **PIT**：通道0 ~100Hz（divisor 11932）
- **trap分发**：向量32=定时器（tick++，EOI），向量33=键盘（`kbd_handle`，EOI），其他IRQ仅EOI，CPU异常=串口诊断+halt

## 开发备注

- QEMU 调试：run.sh 注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` = LLVM 风格
- `outb()` 内联定义重复于 `serial.cc` 和 `isr.h`；`KERNEL_CS`/`L16`/`H16` 重复于 `mem.h` 和 `isr.h`；合并时应统一
- 未来扩展：relocatable header tag + 任意基址支持时需 EIP 推导方式替代 VMA-VMA_BASE
- `enable_page` 在 mem.cc 中定义（物理地址运行），`boot_main` 已不存在
- `init_fb` 在 `init_mem` 末尾调用，依赖 bump_alloc 和 device_vma_base 就绪

# 编程指导原则

以下准则源自 Andrej Karpathy 关于 LLM 编程陷阱的观察，旨在减少常见错误。对琐碎任务自行判断。

## 1. 编码前先思考

不要假设、不要掩盖困惑、要呈现权衡。不确定时主动询问，存在多种解释时呈现所有可能而非默默选择。

## 2. 洁优先

用最少代码解决问题。不添加超出需求的特性，不为一次性代码创建抽象，不添加未要求的"灵活性"。**关键问题：资深工程师会觉得这过于复杂吗？**

## 3. 精准修改

只碰必须碰的。不"改进"相邻代码，不重构未损坏的内容，匹配现有风格。移除因自己更改产生的孤儿代码，但不主动删除既有死代码。**检验标准：每行更改都应直接追溯到用户请求。**

## 4. 目标驱动执行

定义成功标准，循环验证直到完成。对多步骤任务说明简短计划：
```
1. [步骤] → 验证: [检查]
2. [步骤] → 验证: [检查]
```