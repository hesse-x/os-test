# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

32位 x86 higher-half 内核，Multiboot2/GRUB2 引导，freestanding C++ + 纯汇编。所有代码编译为 `-fPIE`，利用 GOTOFF（PC推导基址+常量偏移）实现位置无关。`_start` 和 `boot_main` 在物理地址运行，设置分页后跳转到 `kernel_main` 在虚拟地址 0xC010xxxx 运行。

`mem.cc`（BFC 物理内存分配器）存在但 build.sh **未编译链接它**。

## 构建与运行

```bash
./build.sh          # 编译 + 链接 + 生成 ISO (myos.iso)
./build.sh clear    # 清除 .o, .bin, .iso 产物
./run.sh            # QEMU 启动（OVMF UEFI, 512MB, VGA, 串口输出到 log.txt）
```

编译流程：`start.S` → `boot.cc` → `kernel.cc` → `serial.cc`，统一 `-fPIE`。链接：`ld -m elf_x86_64 -T linker.ld`（输出 elf32-i386）。

**添加新源文件：** 在 build.sh 增加编译步骤（复制现有 g++ 行），并在 ld 行加入 `.o` 文件。

## 启动流程

```
GRUB2 (0x100000) → _start (start.S, 物理地址) → boot_main (boot.cc, 物理地址) → 分页启用 → kernel_main (kernel.cc, 虚拟地址 0xC010xxxx)
```

1. GRUB2 在 LMA=0x100000 加载内核，%eax=魔数, %ebx=multiboot_info地址
2. `_start`（纯汇编）：保存 %eax/%ebx → 设置物理栈 → 调用 `boot_main`（VMA-VMA_BASE计算物理地址）
3. `boot_main`：清零 PD/PT → 填充 identity map + higher-half → 内联asm启用分页(CR3/CR0) → 切换ESP到虚拟地址 → jmp kernel_main
4. `kernel_main`：串口初始化 → 验证魔数 → 遍历 multiboot2 标签 → framebuffer 填白

## 地址映射

- 物理 0-4MB → 虚拟 0-4MB（identity map，过渡期保留）
- 物理 0-4MB → 虚拟 0xC0000000-0xC0400000（higher-half）
- PD[0] 和 PD[768] 共享同一个 PT（4KB小页）
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
| `start.S` | 纯汇编入口 `_start`，保存multiboot参数，设置物理栈，调用 `boot_main` |
| `boot.cc` | Multiboot2 头构造，PD/PT 数组，`boot_main`（物理地址运行，启用分页+跳转） |
| `kernel.cc` | `kernel_main`（虚拟地址运行），串口输出，multiboot2标签遍历，framebuffer填白 |
| `serial.cc/h` | COM1 串口驱动（init/putc/puts），QEMU `-serial file:log.txt` 捕获输出 |
| `mem.h` | Page/BFCAllocator 定义，VMA/LMA 常量，PHY_ADDR 宏 |
| `mem_layout.h` | VMA_BASE/KERNEL_VMA_BASE/KERNEL_LMA_BASE/PHY_ADDR（与 mem.h 有重叠） |
| `mem.cc` | BFC 分配器实现 + mmap 解析 — **当前未链接** |
| `common.h` | 链接脚本导出符号声明（`kernel_end`） |
| `macro.h` | ALIGN_UP 宏 |
| `multiboot2.h` | Multiboot2 规范定义 |
| `doc/design/page_plan.md` | Higher-half + 分页实现方案设计文档 |

## 开发备注

- 没有分页以外的保护机制（无 IDT、中断、键盘驱动）
- QEMU 调试：run.sh 注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` = LLVM 风格
- `mem_layout.h` 与 `mem.h` 地址常量有重叠，合并时应统一
- 未来扩展：relocatable header tag + 任意基址支持时需 EIP 推导方式替代 VMA-VMA_BASE

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