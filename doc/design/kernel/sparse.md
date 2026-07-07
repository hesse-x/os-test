# Sparse 静态分析集成设计

## 目标

集成 [sparse](https://git.kernel.org/pub/scm/linux/kernel/git/jcm/sparse.git) 静态分析工具，对内核代码进行地址空间安全检查，防止内核意外解引用用户空间指针或 MMIO 指针。

## 桩定义

新建 `kernel/sparse.h`，所有内核源文件通过 `kernel/kernel.h` 间接 include。

```c
#ifdef __CHECKER__
#define __user      __attribute__((noderef, address_space(1)))
#define __iomem     __attribute__((noderef, address_space(2)))
#define __force     __attribute__((force))
#define __bitwise   __attribute__((bitwise))
#define __acquires(x)  __attribute__((context(x,0,1)))
#define __releases(x)  __attribute__((context(x,1,0)))
#define __must_check   __attribute__((warn_unused_result))
#else
#define __user
#define __iomem
#define __force
#define __bitwise
#define __acquires(x)
#define __releases(x)
#define __must_check
#endif
```

## 地址空间类型

### 物理地址与内核虚拟地址

使用 `__bitwise` 创建强类型，让 sparse 在每个隐式转换点报警：

```c
typedef uint64_t __bitwise phys_addr_t;   // 物理地址
typedef uint64_t __bitwise kern_vaddr_t;  // 内核虚拟地址
```

转换函数签名更新：

| 函数 | 旧签名 | 新签名 |
|------|--------|--------|
| `phys_to_virt` | `uint64_t phys_to_virt(uint64_t phys)` | `kern_vaddr_t phys_to_virt(phys_addr_t phys)` |
| `page_to_phys` | `uint64_t page_to_phys(Page *p)` | `phys_addr_t page_to_phys(Page *p)` |
| `PHY_ADDR` | `(vaddr) - VMA_BASE` | `(phys_addr_t __force)((vaddr) - VMA_BASE)` |

### 用户空间地址

不创建 `user_vaddr_t`，直接使用 `void __user *`，与 Linux 内核惯例一致。syscall 参数保持 `uint64_t` 传入，使用时转成 `void __user *`。

### 设备 MMIO 地址

不创建 `mmio_vaddr_t`，直接使用 `void __iomem *`。设备地址存储时用 `kern_vaddr_t`（映射前）或 `void __iomem *`（映射后）。

## `__iomem` 标注范围

| 位置 | 变量/字段 | 改动 |
|------|-----------|------|
| `arch/x64/apic.h` | `lapic_vaddr`, `ioapic_vaddr` | `uint64_t` → `void __iomem *` |
| `arch/x64/utils.h` | `readl(addr)`, `writel(addr, val)` | 参数 `const void *`/`void *` → `const void __iomem *`/`void __iomem *` |
| `kernel/fb.c` | framebuffer 映射地址 | 加 `__iomem` |
| `kernel/pci.h` | `pci_bar_t.vaddr` | `kern_vaddr_t` → `void __iomem *` |
| `kernel/ahci.h` | AHCI MMIO 寄存器指针 | 加 `__iomem` |
| `kernel/xhci.h` | xHCI 寄存器指针 | 加 `__iomem` |

## `__user` 标注范围

| 位置 | 变量/字段 | 改动 |
|------|-----------|------|
| `kernel/proc.h` | `proc_t.req_reply_buf`, `proc_t.msg_reply_buf` | `void *` → `void __user *` |
| `kernel/trap.c` | 各 syscall handler 中的用户空间指针参数 | `uint64_t` → `void __user *` 转换点加 `(void __user * __force)` |
| `kernel/proc.c` | `setup_signal_frame()` 等操作用户空间栈帧的函数 | 加 `__user` |

## `__force` 标注范围

每个跨地址空间的显式转换都需要 `__force`：

| 位置 | 说明 |
|------|------|
| `phys_to_virt()` | 物理→内核虚拟 |
| `PHY_ADDR()` | 内核虚拟→物理 |
| `device_vma_base` 计算 | 设备 MMIO 映射 |
| `readl()`/`writel()` | 内部对 `__iomem` 指针解引用需要 `__force`（sparse 允许这些函数解引用 `__iomem`，但实现里要强制转换） |
| syscall handler | `uint64_t` → `void __user *` 的强制转换 |
| `copy_from_user`/`copy_to_user`（未来） | 内核↔用户空间 |

## `__acquires` / `__releases` 标注范围

| 锁 | 函数 | 标注 |
|----|------|------|
| `spinlock_t` | `spin_lock`/`spin_unlock` | `__acquires`/`__releases` |
| `spinlock_t` | `spin_lock_irqsave`/`spin_unlock_irqrestore` | `__acquires`/`__releases` |
| `scheduler_lock` | per-CPU 调度器锁 | `__acquires`/`__releases` |
| `procs_lock` | 全局进程表锁 | `__acquires`/`__releases` |
| `recv_lock` | per-process 消息队列锁 | `__acquires`/`__releases` |
| `bfc_lock` | 内存分配器锁 | `__acquires`/`__releases` |

## `__must_check` 标注范围

| 函数类别 | 示例 | 理由 |
|----------|------|------|
| 锁 | `spin_lock_irqsave` | 漏存 flags → 中断不关 |
| 分配 | `kmalloc`/`kzalloc` | 漏检 NULL → 空指针崩溃 |
| 用户空间拷贝 | `copy_from_user`/`copy_to_user`（未来） | 漏检返回值 → 安全漏洞 |
| 页映射 | `map_user_page*` | 漏检返回值 |

## `__bitwise` 标注

暂不添加。x86-64 是 little-endian only 的架构，当前无网络协议栈。等未来支持其他架构或添加 AF_INET 时再加。

**TODO**: 记录至 `doc/design/todo.md`。

## 编译器警告升级

### GCC

CMake `CMAKE_C_FLAGS` 添加：

```
-Wall -Wextra -Werror
```

### Sparse

check.sh 中 sparse flags：

```
-Werror
-Waddress-space
-Wdecl
-Wdo-while
-Wtransparent-union
-Wreturn-void-ptr
```

## check.sh 设计

- **默认行为**：增量检查 —— 只跑 `origin/master...HEAD` 改动触及的内核 `.c`（基准可由参数切换：`origin/<branch>` / 本地 `<branch>` / `--all` 全量；解析见 `doc/design/code_standard.md`「增量运行」）。`#include` 层级检查是目录级不变量，**始终全量**。
- **不检查**：用户态代码（libc、shell、drivers 等）
- **退出码**：任何 sparse 输出 → 返回非零；基准无法解析 → 2；增量无改动 → 跳过该步并 exit 0。
- **集成方式**：先尝试 `cgcc`（sparse 的编译器包装器）配合 CMake，不行换直接调 `sparse`

### 直接调 sparse 方案（备选）

```bash
#!/bin/bash
sparse -std=gnu17 -fPIE -m64 \
    -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pie \
    -D__KERNEL__ -D__CHECKER__ \
    -Werror \
    -Waddress-space \
    -Wdecl \
    -Wdo-while \
    -Wtransparent-union \
    -Wreturn-void-ptr \
    -I. \
    kernel/*.c arch/x64/*.c kernel/mem/*.c
```

### cgcc 方案（首选尝试）

```bash
#!/bin/bash
mkdir -p build-check && cd build-check
cmake .. -DCMAKE_C_COMPILER=cgcc
make -j$(nproc) 2>&1 | tee sparse.log
# 检查是否有 sparse 输出
if [ -s sparse.log ]; then
    exit 1
fi
```

## 实现顺序

1. 新建 `kernel/sparse.h` 桩定义
2. 在 `kernel/kernel.h` 中 include `kernel/sparse.h`
3. 创建 `phys_addr_t` / `kern_vaddr_t` 类型，更新 `phys_to_virt`/`page_to_phys`/`PHY_ADDR`
4. 标注 `__iomem`：APIC、readl/writel、framebuffer、PCI BAR、AHCI、xHCI
5. 标注 `__user`：proc_t 字段、syscall handler 参数
6. 标注 `__force`：跨地址空间转换点
7. 标注 `__acquires`/`__releases`：所有锁函数
8. 标注 `__must_check`：锁、分配、拷贝、映射函数
9. CMake 加 `-Wall -Wextra -Werror`
10. 修干净所有 gcc warning
11. 新建 `check.sh`，先试 cgcc，不行换直接调 sparse
12. 验证：`./build.sh && ./check.sh` 都能跑过
