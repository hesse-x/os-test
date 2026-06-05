---
name: page_plan1
description: 分页重构方案：分阶段映射，enable_page只做最小映射，init_mem在虚拟地址解析mmap+初始化BFC+扩展映射
type: project
---

# 分页重构方案：分阶段映射

> **历史文档**：这是 x86-32 / Multiboot2 阶段的分页重构方案。当前代码已迁移至 x86-64 / UEFI。
>
> 当前实现概要：
> - 4级页表 (PML4→PDPT→PD)，enable_paging 在物理地址构建初始映射，init_mem 在虚拟地址扩展
> - 2MB huge pages 初始覆盖 1GB，extend_mapping 按 1GB 块动态扩展
> - VMA_BASE = `0xFFFFFFFF80000000`
> - 栈切换：_entry64 中 `lea stack_bottom(%rip), %rsp` 直接切到虚拟地址
> - 参见 CLAUDE.md 了解当前架构

## 设计目标

将分页设置从 boot_main 中拆出，分为两个阶段：
1. 物理地址阶段：只做最小分页映射（enable_page），确保内核能跳转到虚拟地址
2. 虚拟地址阶段：解析 mmap、扩展映射、初始化 BFC（init_mem）

参考 Linux x86-32 做法：boot 时最小映射，虚拟地址下做全量映射。

## 启动流程

```
_start (start.S, 物理地址)
  → 保存 multiboot 参数 (%eax → %ecx, %ebx → %edx)
  → 设置物理栈 (ESP = stack_bottom + 8192 - VMA_BASE)
  → push addr, magic_num
  → call kernel_main (物理地址 = kernel_main - VMA_BASE)

kernel_main (物理地址, -fPIE):
  1. enable_page()     → 设置 PD/PT，启用分页，返回
  2. saved_mbi_addr = addr  → 保存 multiboot info 地址到全局变量
  3. 内联asm: addl $VMA_BASE, %esp → 栈平移到虚拟地址（内容不损坏）
     jmp *kernel_main_higher     → 跳转到虚拟地址
  // 以下不可达

kernel_main_higher (虚拟地址 0xC010xxxx, -fPIE):
  1. init_mem(saved_mbi_addr)  → 解析 mmap, bump 分配 PT+frames, 扩展映射, 初始化 BFC
  2. serial_init()
  3. framebuffer 初始化
  ...
```

## enable_page — 最小分页设置

在 boot.cc 中，物理地址运行，只做最低必要映射：

```c
void enable_page() {
    // 清零 PD 和初始 PT
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        page_table[i] = 0;
    }

    // 填充初始 PT：物理 0-4MB → 4KB 页，present + writable
    for (int i = 0; i < 1024; i++) {
        page_table[i] = (i * 4096) | 0x03;
    }

    // PD[0] = 初始 PT | flags (identity map: virt 0-4MB → phys 0-4MB)
    page_directory[0] = ((uintptr_t)page_table) | 0x03;

    // PD[768] = 初始 PT | flags (higher-half: virt 0xC0000000-0xC0400000 → phys 0-4MB)
    page_directory[768] = ((uintptr_t)page_table) | 0x03;

    // 启用分页（不跳转，返回调用者）
    __asm__ volatile(
        "movl %0, %%cr3\n"
        "movl %%cr0, %%eax\n"
        "orl $0x80000000, %%eax\n"
        "movl %%eax, %%cr0\n"
        :
        : "r"((uintptr_t)page_directory)  // GOTOFF → 物理地址
        : "eax", "memory"
    );
}
```

映射范围：
- **identity map**：virt 0-4MB → phys 0-4MB（固定，保证分页切换过渡期 EIP/ESP 有效）
- **higher-half**：virt 0xC0000000-0xC0400000 → phys 0-4MB（初始 4MB，覆盖内核映像）

PD[0] 和 PD[768] 共享同一个初始 PT。

## 栈平移 + 地址切换

kernel_main 中内联 asm，分页已启用后执行：

```asm
addl $0xC0000000, %esp    // ESP += VMA_BASE，栈平移到虚拟地址（内容不损坏）
jmp *kernel_main_higher    // EIP 切到虚拟地址
```

关键点：
- **栈平移不是栈切换**：ESP 从物理地址加 VMA_BASE 变为虚拟地址，指向同一块物理内存（identity map 和 higher-half 映射同一物理页）。栈帧内容完好，magic_num 和 addr 仍在栈上。
- 但由于 jmp 不是 call，kernel_main_higher 没有标准 C 返回地址，所以用全局变量 `saved_mbi_addr` 传递 multiboot info 地址。
- kernel_main_higher 不应返回（没有有效返回地址）。

## init_mem — mmap 解析 + BFC 初始化 + 映射扩展

在虚拟地址运行，mem.cc 中实现。

### 步骤

1. **解析 multiboot2 mmap 标签**：遍历 multiboot info 标签，找到 mmap tag，读取可用物理内存区域
2. **计算总页帧数**：从 mmap 最大物理地址算出 total_page_frames
3. **Bump 分配器初始化**：从 kernel_end 之后的第一个可用物理区域取页
4. **Bump 分配 frames 数组**：`bump_alloc(total_page_frames * sizeof(Page))`
5. **Bump 分配扩展 PT**：为每个需要映射的 4MB 物理块分配一个 PT 页
6. **扩展 higher-half 映射**：填充新 PT 条目，设置 PD[769], PD[770], ...
7. **初始化 BFC 分配器**：填充 frames 数组（根据 mmap 标记 FREE/USED/RESERVED），建立 free list
8. **标记 bump 分配的页为 USED**：frames 数组、扩展 PT、内核自身占用等标记为 USED

### Bump 分配器

极简物理内存分配器，仅在 BFC 初始化前使用：

```c
static uintptr_t bump_next_phys;  // 下一个空闲物理页地址（物理地址）

void bump_init(uintptr_t start) {
    bump_next_phys = ALIGN_UP(start, PAGE_SIZE);
}

// 返回虚拟地址（phys + VMA_BASE），直接可当指针用
void* bump_alloc(size_t size) {
    uintptr_t phys = bump_next_phys;
    bump_next_phys += ALIGN_UP(size, PAGE_SIZE);
    return (void*)(phys + VMA_BASE);
}
```

bump 分配器返回**虚拟地址**（phys + VMA_BASE），kernel_main_higher 可直接读写。
需要物理地址时（填 PD/PT entry）用 `PHY_ADDR(ptr)` 转回。

bump_next_phys 起始值：`ALIGN_UP(kernel_end_phys, 4096)`，其中 `kernel_end_phys = (uintptr_t)kernel_end - VMA_BASE`。

### 约束：初始 4MB 映射必须覆盖所有 bump 分配

所有 bump 分配的页（frames 数组 + 扩展 PT）物理地址必须在 0-4MB 范围内，
因为初始 higher-half 只映射了 4MB。超出 4MB 的页虚拟地址不可访问。

实际约束：kernel_end 在物理 ~0x10xxxx，之后到 0x3FFFFF 约 3MB 空闲。
- 512MB RAM → 127 个扩展 PT = 508KB
- frames 数组 ≈ 2MB（131K pages × sizeof(Page)）
- 总计约 2.5MB → 3MB 空间够用

超过此范围（RAM > 1GB）需扩大初始映射。

### 扩展映射流程

```c
for 每个需要映射的 4MB 物理块（从 4MB 开始，逐块扫描 mmap）:
    1. uint32_t *pt = (uint32_t*)bump_alloc(4096);    // 取一页当 PT（虚拟地址）
    2. uintptr_t pt_phys = PHY_ADDR(pt);              // 转物理地址
    3. 填充 pt 条目：phys_base + i*4096 | 0x03       // 映射该 4MB 物理块
    4. PD[768 + n] = pt_phys | 0x03                   // 加入 PD
```

PD entry 编号：PD[768] 对应 0xC0000000（已映射），PD[769] 对应 0xC0400000，PD[770] 对应 0xC0800000，...

### BFC 初始化

参考现有 mem.cc 中 init_frames 逻辑，但改用虚拟地址：

```c
void init_mem(uintptr_t mbi_addr) {
    // 1. 从 mbi_addr 解析 mmap
    // 2. 计算 total_page_frames
    // 3. bump_init(kernel_end_phys)
    // 4. frames = bump_alloc(total_page_frames * sizeof(Page))
    // 5. 扫描 mmap，为每个超出 4MB 的物理块分配 PT 并扩展映射
    // 6. 初始化 frames 数组（RESERVED/USED/FREE）
    // 7. 建立 free list
    // 8. BFCAllocator::frames = frames
}
```

multiboot info 地址 mbi_addr 为物理地址（GRUB 传入），通过 identity map 直接可访问，
或通过 mbi_addr + VMA_BASE 在 higher-half 访问。

## PD/PT 存放

| 项目 | 位置 | 大小 | 说明 |
|---|---|---|---|
| PD | boot.cc BSS | 4KB (1024 entries) | 固定，覆盖 4GB 虚拟空间，extern 供 mem.cc 修改 |
| 初始 PT | boot.cc BSS | 4KB (1024 entries) | 映射 identity 4MB + higher-half 4MB，extern |
| 扩展 PT | bump 分配 | 每个 4KB | 每映射 4MB 物理内存需一个 PT |

PD 和初始 PT 留 BSS 的原因：
- 分页启用前必须存在
- 分页启用后需在虚拟地址可访问（在内核映像内，初始 PT 已映射）
- 扩展 PT 是 bump 分配的物理页，虚拟地址在已映射范围内可访问

## 文件变更清单

| 文件 | 变更 |
|---|---|
| `start.S` | `boot_main` → `kernel_main`（物理地址计算不变） |
| `boot.cc` | 删除 `boot_main`；新增 `enable_page()`；PD/PT 改为 extern（去 static）；保留 Multiboot2 头 + stack |
| `kernel.cc` | `kernel_main`：调 enable_page() + 保存 addr + 内联asm栈平移+jmp；新增 `kernel_main_higher`：init_mem + serial + framebuffer |
| `kernel.h` | 新增 `kernel_main_higher` 声明 |
| `common.h` | 新增 `page_directory` / `page_table` extern 声明 |
| `mem.h` | 新增 `init_mem` 声明；bump 分配器相关声明 |
| `mem.cc` | 新增 `init_mem` 实现：mmap 解析 + bump 分配 + 映射扩展 + BFC 初始化；删除空的 `enable_page()` |
| `build.sh` | 新增 mem.cc 编译步骤；链接命令加入 mem.o |

## multiboot info 地址传递

kernel_main 通过全局变量传递 multiboot info 地址给 kernel_main_higher：

```c
// kernel.cc
static uintptr_t saved_mbi_addr;

void kernel_main(int32_t magic_num, uintptr_t addr) {
    enable_page();
    saved_mbi_addr = addr;
    __asm__ volatile(...);  // 栈平移 + jmp
}

void kernel_main_higher() {
    init_mem(saved_mbi_addr);
    ...
}
```

栈虽然平移后内容完好，但 jmp 不是 call，kernel_main_higher 没有标准 C 函数调用帧，
无法通过栈参数接收 addr。全局变量是最简单可靠的方式。

## 与旧方案对比

| | 旧方案 (boot_main) | 新方案 |
|---|---|---|
| 分页设置 | boot_main 物理地址，一步完成 | enable_page 物理地址，最小映射 |
| 地址切换 | boot_main 内联asm jmp | kernel_main 内联asm addl+jmp |
| mmap 解析 | kernel_main 虚拟地址 | init_mem 虚拟地址（BFC 内） |
| 映射范围 | 固定 4MB | 按需扩展（基于 mmap） |
| BFC 初始化 | 未集成 | init_mem 中完成 |
| PD/PT | BSS 预留 | PD+初始PT BSS，扩展PT bump分配 |

## 开放问题

- identity map 何时撤除？当前保留，撤除后所有物理地址访问需转虚拟地址
- RAM > 1GB 时需扩大初始映射范围（或分批扩展：先映射 4MB，bump 从中取 PT 映射下一个 4MB，逐步扩大）
- mem_layout.h 与 mem.h 地址常量重叠，后续需合并