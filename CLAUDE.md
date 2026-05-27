# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

32位 x86 内核，通过 Multiboot2/GRUB2 引导，使用 freestanding C++ 和内联汇编编写。`multiboot2` 分支是对旧 `master` 分支（使用自定义 boot sector 和 CMake）的重写。当前为低地址扁平布局，高端内核（higher-half）设计尚未实现（`mem.h` 中已定义 VMA/LMA 常量但链接脚本未启用）。

## 构建与运行

```bash
./build.sh          # 编译 + 链接 + 生成可启动 ISO (myos.iso)
./build.sh clear    # 清除所有 .o, .bin, .iso 产物
./run.sh            # 在 QEMU 中启动（OVMF UEFI 固件，512MB 内存，标准 VGA）
```

编译使用 `g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -c` 加 `-fPIC -fno-pie`。链接使用 `ld -m elf_x86_64 -T linker.ld`（链接脚本强制输出 `elf32-i386` 格式）。

**注意：** `mem.cc` 存在但当前 `build.sh` **未编译它**。添加它需要增加编译步骤并在链接命令中加入 `mem.o`。

## 架构

### 启动流程

```
GRUB2 → _start (boot.cc) → kernel_main (kernel.cc)
```

1. GRUB2 在 0x100000 加载内核，进入 32 位保护模式，通过 %eax 传递魔数 (0x36d76289)，%ebx 传递 multiboot 信息地址
2. `_start` 读取寄存器，设置 8KB 栈，直接调用 `kernel_main`
3. `kernel_main` 遍历 Multiboot2 标签，处理 framebuffer 标签后将屏幕填充为白色

### 内存布局

链接脚本 (`linker.ld`) 当前为简单的低地址扁平布局（`. = 0x100000`），所有段的 VMA = LMA = 从 0x100000 起连续排列：

- `.multiboot` — Multiboot2 头（GRUB 要求在前 32KB 内）
- `.stack` — 8KB 引导栈
- `.text` — 代码段
- `.rodata` / `.rodata.*` — 只读数据
- `.data` — 已初始化数据
- `.bss` — 未初始化数据

链接脚本中有注释掉的 `0xC0100000` 高地址，高端内核设计待实现。

### 链接脚本导出符号（通过 `common.h` 使用）

- `kernel_end` — 内核映像结束地址（内存管理器使用）

### 关键源文件

| 文件 | 作用 |
|---|---|
| `boot.cc` | 入口点 `_start`，Multiboot2 头构造，栈设置，调用 `kernel_main` |
| `kernel.cc` | `kernel_main` — 遍历 Multiboot2 标签，初始化 framebuffer 并填充白色 |
| `kernel.h` | `kernel_main` 的 C 声明（供 `boot.cc` 的 `extern "C"` 调用） |
| `mem.cc` | BFC 物理内存分配器 + Multiboot2 mmap 解析 — **当前构建未链接** |
| `mem.h` | 页帧描述符 `Page`、`BFCAllocator`、VMA/LMA 常量、地址宏 |
| `common.h` | 链接脚本导出符号声明（`kernel_end`） |
| `macro.h` | 通用宏（`ALIGN_UP`） |
| `multiboot2.h` | 完整 Multiboot2 规范定义（魔数、标签类型、结构体） |

## 开发备注

- 当前分支没有分页、IDT、中断和键盘驱动 — 仅支持帧缓冲区输出
- 高端内核设计尚未实现：`mem.h` 定义了 `KERNEL_VMA_BASE 0xC0100000` 和 `KERNEL_LMA_BASE 0x100000`，但链接脚本和启动代码尚未启用分页和高端映射
- QEMU 调试：`run.sh` 中有注释掉的 `-s -S` 参数用于 GDB 远程调试
- `.clang-format` 设置为 LLVM 风格
- `log` 文件包含 `myos.bin` 的 objdump 反汇编结果，供参考

# 编程指导原则

以下准则源自 Andrej Karpathy 关于 LLM 编程陷阱的观察，旨在减少常见错误。这些准则倾向于谨慎而非速度。对于琐碎的任务（简单的拼写错误修复、显而易见的一行修改），请自行判断。

## 1. 编码前先思考

**核心要求：** 不要假设、不要掩盖困惑、要呈现权衡

- 明确陈述假设，不确定时主动询问
- 存在多种解释时，呈现所有可能而非默默选择
- 存在更简单方案时应提出，必要时主动反驳
- 遇到不清晰之处应停止，明确困惑点并提问

## 2. 简洁优先

**核心要求：** 用最少的代码解决问题，不要过度设计

- 不添加超出需求的特性
- 不为一次性代码创建抽象
- 不添加未要求的"灵活性"或"可配置性"
- 不处理不可能发生的错误场景

**关键问题：** 资深工程师会觉得这过于复杂吗？如果是，简化

## 3. 精准修改

**核心要求：** 只碰必须碰的，只清理自己造成的混乱

编辑现有代码时：

- 不"改进"相邻代码、注释或格式
- 不重构未损坏的内容
- 匹配现有风格，即使你会用不同方式实现
- 发现无关死代码时应提及而非删除

当你的改动产生孤儿代码时：

- 移除因你的更改而变得未使用的导入/变量/函数
- 除非被要求，否则不能移除既有的死代码

**检验标准：** 每行更改都应直接追溯到用户请求

## 4. 目标驱动执行

**核心要求：** 定义成功标准，循环验证直到完成

任务转换示例：

- "添加验证" → "为无效输入编写测试，然后使其通过"
- "修复 bug" → "编写能够复现的最小测试，然后使其通过"
- "重构 X" → "确保重构前后测试都通过"

对于多步骤任务，说明一个简短的计划：

```
1. [步骤] → 验证: [检查]
2. [步骤] → 验证: [检查]
3. [步骤] → 验证: [检查]
```

**核心思想：** 强有力的成功标准让你能独立循环，弱标准则需要持续澄清
