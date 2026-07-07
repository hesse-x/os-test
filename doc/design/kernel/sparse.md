# Sparse 静态分析集成

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 桩定义位置 | `kernel/xcore/sparse.h`，通过 `kernel/kernel.h` 间接 include | 所有内核源文件自动获得注解宏 |
| 2 | 物理地址类型 | `phys_addr_t`（`__bitwise uint64_t`）+ `kern_vaddr_t`（`__bitwise uint64_t`） | sparse 在隐式转换点报警，防止物理/虚拟地址混用 |
| 3 | 用户地址 | `void __user *`（不创建 `user_vaddr_t`） | 与 Linux 内核惯例一致；syscall 参数保持 `uint64_t` 传入，使用时转换 |
| 4 | MMIO 地址 | `void __iomem *`（不创建 `mmio_vaddr_t`） | 设备地址存储时用 `kern_vaddr_t`（映射前）或 `void __iomem *`（映射后） |
| 5 | `__bitwise` 端序标注 | 暂不添加 | x86-64 little-endian only，无网络协议栈；未来支持其他架构或 AF_INET 时再加 |
| 6 | 检查范围 | 只检查内核 `.c`（不检查用户态代码） | sparse 注解是内核内部安全机制 |
| 7 | 检查方式 | `check.sh` 增量检查（`origin/master...HEAD` 改动触及的文件）；`--all` 全量 | 增量避免无改动时浪费时间；`#include` 层级检查始终全量 |

### 桩定义

`kernel/xcore/sparse.h` 定义（`__CHECKER__` 门控）：

- `__user`：`noderef, address_space(1)` — 用户空间指针
- `__iomem`：`noderef, address_space(2)` — MMIO 指针
- `__force`：跨地址空间强制转换标记
- `__bitwise`：强类型，禁止隐式转换
- `__acquires(x)`/`__releases(x)`：锁获取/释放上下文标注
- `__must_check`：不可忽略返回值

非 `__CHECKER__` 构建时全部展开为空宏，零运行时开销。

### 地址空间类型

`phys_addr_t`/`kern_vaddr_t` 已创建并应用：

| 函数 | 签名 |
|------|------|
| `phys_to_virt` | `kern_vaddr_t phys_to_virt(phys_addr_t phys)` |
| `page_to_phys` | `phys_addr_t page_to_phys(Page *p)` |
| `PHY_ADDR` | `(phys_addr_t __force)((vaddr) - VMA_BASE)` |

实现：`kernel/xcore/sparse.h` : phys_addr_t/kern_vaddr_t；`arch/x64/utils.h` : phys_to_virt/page_to_phys/PHY_ADDR

### `__iomem` 标注

| 位置 | 变量/字段 |
|------|-----------|
| `arch/x64/apic.h` | `lapic_vaddr`, `ioapic_vaddr` → `void __iomem *` |
| `arch/x64/utils.h` | `readl(addr)`, `writel(addr, val)` → `const void __iomem *`/`void __iomem *` |
| `kernel/driver/ahci.h` | AHCI MMIO 寄存器指针 |
| `kernel/driver/xhci.h` | xHCI 寄存器指针 |
| `kernel/driver/pci.h` | `pci_bar_t.vaddr` → `void __iomem *` |

### `__user` 标注

| 位置 | 变量/字段 |
|------|-----------|
| `kernel/bsd/proc.h` | `req_reply_buf`, `msg_reply_buf` → `void __user *` |
| `kernel/bsd/syscall.c` | syscall handler 用户指针参数 → `(void __user * __force)` 转换点 |
| `kernel/bsd/proc.c` | `setup_signal_frame()` 等操作用户栈帧的函数 |

### `__force` 标注

跨地址空间显式转换点：`phys_to_virt`/`PHY_ADDR`、`readl/writel` 内部实现、syscall handler `uint64_t → void __user *`、device_vma_base 计算。

### `__acquires`/`__releases` 标注

| 锁 | 函数 |
|----|------|
| `spinlock_t` | `spin_lock/spin_unlock`、`spin_lock_irqsave/spin_unlock_irqrestore` |
| `scheduler_lock` | per-CPU 调度器锁 |
| `tasks_lock` | 全局进程表锁 |
| `recv_lock` | per-process 消息队列锁 |
| `bfc_lock` | 内存分配器锁 |

### `__must_check` 标注

| 函数类别 | 示例 | 理由 |
|----------|------|------|
| 锁 | `spin_lock_irqsave` | 漏存 flags → 中断不关 |
| 分配 | `kmalloc/kzalloc` | 漏检 NULL → 空指针崩溃 |
| 页映射 | `map_user_page*` | 漏检返回值 |

### 编译器警告与检查脚本

CMake 全局 `CMAKE_C_FLAGS`：`-Wall -Wextra -Werror`

`check.sh` sparse flags：`-Werror -Waddress-space -Wdecl -Wdo-while -Wtransparent-union -Wreturn-void-ptr`

检查方式：先尝试 `cgcc`（sparse 编译器包装器），不行换直接调 `sparse`。退出码：sparse 输出 → 非零；基准无法解析 → 2；增量无改动 → 跳过 exit 0。

实现：`build_script/sparse_check.sh`（被 `check.sh` 调用）

### 与其他模块的关系

| 模块 | 关系 |
|------|------|
| 内存管理 | `phys_addr_t/kern_vaddr_t` 类型 + BFC/Slab `__must_check` |
| 驱动 | MMIO `__iomem` 标注（AHCI/xHCI/PCI/APIC） |
| 进程管理 | `__user` 标注（syscall handler / proc_t 字段） |
| 锁 | `__acquires/__releases` 标注 |
| 编码规范 | 增量检查规则详见 [code_standard.md](../code_standard.md) |

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| `__bitwise` 端序标注 | x86-64 LE only 当前无网络栈，暂不加；未来多架构或 AF_INET 时再加 | 低 |
| `copy_from_user/copy_to_user __must_check` | 用户空间拷贝返回值标注 | 中 |
| cgcc 持续集成 | 当前 check.sh 先试 cgcc 再 fallback direct sparse；cgcc 与 CMake 集成稳定性待验证 | 低 |
