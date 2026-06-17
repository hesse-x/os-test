# xHCI 驱动 + MSI-X

## 概述

内核实现最小 xHCI 驱动（`kernel/xhci.cc`），使用 MSI-X 中断（`kernel/pci.cc` 通用 MSI-X 函数）。当前阶段验证 xHCI 裸控制器命令通路 + MSI-X 中断投递，不涉及 USB 设备枚举或键盘。

## 依赖

```
pci_init (ECAM 枚举 + BAR 映射 + capability 链 + MSI-X 配置)
  └→ xhci_init (内核 xHCI 驱动，MSI-X 中断)
```

**前置依赖**：MSI-X 必须在 xHCI polling 命令之前配置。xHCI 控制器在 MSI-X 未配置时不产生 event（包括 command completion），这是 Enable Slot 超时的根因。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 中断模式 | **只做 MSI-X** | 实机 2013+ xHCI 控制器用 MSI-X；不支持 MSI-X 的设备直接跳过，不做 MSI/INTx fallback |
| 2 | IDT 向量范围 | **64-127** | 扩展 vectors.S 到 0-127，MAX_IRQ_HANDLERS=128；MSI-X 向量从 64 起分配 |
| 3 | MSI-X Table/PBA 映射 | **复用 BAR MMIO** | xHCI MSI-X Table/PBA 在 BAR 0 MMIO 空间内，BAR 已被 pci_enable_device 映射，直接 offset 访问 |
| 4 | PCI capability 存储 | **pci_device 加 msix_cap_offset** | pci_scan_bus 时 walk capability 链，找到 MSI-X (ID=0x11) 记 offset，没找到留 0 |
| 5 | MSI-X 向量数 | **按 MaxIntrs 分配** | 读 HCSPARAMS1.MaxIntrs，分配对应数量向量；Entry 0 unmask，其余 mask |
| 6 | 验证设备 | **xHCI 裸控制器** | 不碰键盘、不迁移 AHCI；先验证 MSI-X 中断投递机制 |
| 7 | MSI-X 启用时机 | **polling 命令之前** | MSI-X 是控制器响应命令的前置条件；先配 MSI-X（Entry mask），再做 reset/command |
| 8 | 初始化阶段中断 | **Table Entry mask** | 配 MSI-X 时 Mask bit=1，控制器放 event 到 ring 但不发中断；polling 独占 event ring；init 完成后 unmask |
| 9 | 代码组织 | **pci.cc 通用函数** | MSI-X 是 PCI 通用机制：`pci_enable_msix()`/`pci_msix_mask_entry()`/`pci_msix_unmask_entry()` 在 pci.cc；xHCI 调用一行 |
| 10 | 串口输出 | **逐步打印 + 失败 dump 寄存器** | 每个关键步骤串口打印一行；任何步骤超时/失败 dump 相关 MMIO 寄存器 |
| 11 | 验收终点 | **Enable Slot + ISR 中断** | Enable Slot 成功证明命令通路；unmask Entry 0 后 ISR 收到 MSI-X 中断证明中断投递 |
| 12 | PS/2 键盘 | **保持不动** | 先不碰键盘驱动，xHCI 键盘集成后续再做 |
| 13 | xHCI 驱动位置 | **内核态** | 需 MMIO BAR 访问、物理连续 DMA 内存、中断处理；和 AHCI 同模式 |
| 14 | TRB 环大小 | **固定 1 页（256 个 TRB）** | 单设备绑绑有余 |
| 15 | 内存分配 | **alloc_page_low** | 初始化时一次性分配，永不释放 |

## MSI-X 基础设施

### PCI Capability 链解析

在 `pci_scan_bus()` 中，对每个 Type 0 header 设备 walk capability 链：

1. 读 Config offset 0x34 (Capabilities Pointer)
2. 逐项读 Capability ID (offset+0) + Next Pointer (offset+1)
3. 找到 MSI-X (ID=0x11) 时存入 `pci_device.msix_cap_offset`
4. 链结束（Next Pointer=0）时停止

### pci_device 扩展

```c
struct pci_device {
    // ... existing fields ...
    uint8_t msix_cap_offset;  // MSI-X capability offset in config space, 0 = not found
};
```

### MSI-X Capability 结构（PCI Config Space）

```
Offset 0:  Capability ID (0x11)
Offset 1:  Next Pointer
Offset 2:  Message Control (16-bit):
            bit 0:   MSI-X Enable
            bit 1:   Function Mask (global mask)
            bits 2-10: Table Size (N-1, 即 Entry 数量)
Offset 4:  Table Offset / BIR (32-bit):
            bits 0-2:  Table BIR (哪个 BAR)
            bits 3-31: Table Offset (BAR 内偏移)
Offset 8:  PBA Offset / BIR (32-bit):
            bits 0-2:  PBA BIR (哪个 BAR)
            bits 3-31: PBA Offset (BAR 内偏移)
```

### MSI-X Table Entry 格式（MMIO 空间内）

```
Offset 0:  Message Address (32-bit, 低 32 位)
Offset 4:  Message Address (32-bit, 高 32 位)
Offset 8:  Message Data (32-bit):
            bits 0-7:   Vector Number
            bits 8-15:  Delivery Mode (0=Fixed)
            bits 16-18: Reserved
            bit 19:     Trigger Mode (0=Edge)
Offset 12: Vector Control (32-bit):
            bit 0:      Mask (1=masked, 0=unmasked)
            bits 1-31:  Reserved
```

### MSI-X Message Address/Data

```
Message Address = 0xFEE00000 | (target_apic_id << 12)
Message Data    = allocated_vector | (delivery_mode << 8)
                 delivery_mode = 0 (Fixed), trigger_mode = Edge
```

### 通用 API（kernel/pci.cc）

```c
// 解析 MSI-X capability，分配向量，写 Table Entry（mask），设 Enable bit
// 返回分配的向量数，失败返回负 errno
int pci_enable_msix(pci_device *dev, int num_vectors);

// Mask/Unmask 单个 Table Entry
void pci_msix_mask_entry(pci_device *dev, int entry);
void pci_msix_unmask_entry(pci_device *dev, int entry);

// 获取 MSI-X Table/PBA 的虚拟地址（BAR vaddr + offset）
void *pci_msix_table_addr(pci_device *dev);
void *pci_msix_pba_addr(pci_device *dev);

// 获取分配的向量基址
int pci_msix_vector_base(pci_device *dev);
```

### 向量分配

- 分配范围：64-127
- 机制：bitmap 或简单 next_vector 计数器
- 每个 MSI-X 设备按需求量连续分配
- `pci_enable_msix(dev, num_vectors)` 调用 `alloc_irq_vectors(num_vectors)`

### IDT 扩展

- `vectors.S`：扩展 stub 从 vector0 到 vector127（128 个）
- `idt_install()`：循环扩展到 128 项
- `MAX_IRQ_HANDLERS`：从 64 改为 128
- `irq_handlers[]` / `irq_owner[]`：数组扩展到 128

## 内核 xHCI 驱动

### 文件

- `kernel/xhci.cc` — 实现
- `kernel/xhci.h` — 公开接口

### 内存分配（6 页 / 24KB）

初始化时 `alloc_page_low` 一次性分配，永不释放：

| 结构 | 大小 | 用途 |
|------|------|------|
| DCBAA | 1 页 | Device Context Base Address Array（256×8B）|
| Input Context | 1 页 | Address Device 命令的输入上下文 |
| Command Ring | 1 页 | 256 个 TRB（index 255 为 Link TRB）|
| ERST | 1 页 | Event Ring Segment Table（1 entry×16B）|
| Event Ring Segment | 1 页 | 256 个 TRB |
| Scratchpad Buffer | 1 页 | xHC 要求的 scratchpad buffer |

### 初始化流程

```
xhci_init()
  1.  查找 xHCI PCI 设备 (class=0x0C03, prog_if=0x30)
      → 串口: "[xHCI] Found PCI device: bus=X dev=X func=X"
  2.  pci_enable_device: 映射 BAR0 MMIO + Bus Master + Memory Space
      → 失败: dump PCI command/status 寄存器
  3.  检查 msix_cap_offset != 0
      → 串口: "[xHCI] MSI-X: cap_offset=0xXX, table_bar=0, table_offset=0xXXXX"
      → msix_cap_offset == 0: 串口 "[xHCI] No MSI-X capability, skip" → return
  4.  分配 DMA 内存 (6 页)
  5.  读 HCSPARAMS1: MaxSlots, MaxIntrs, MaxPorts
      → 串口: "[xHCI] MaxSlots=X MaxIntrs=X MaxPorts=X"
  6.  配置 MSI-X:
      a. pci_enable_msix(dev, max_intrs) — 分配向量, 写 Table Entry (mask), 设 Enable
      b. 分配 ISR 向量 → register_irq(vector, xhci_isr)
      → 串口: "[xHCI] MSI-X enabled, vectors 64-XX"
      → 失败: dump MSI-X Message Control / Table Entry
  7.  xHC reset: USBCMD.HCRST → poll HCRST=0 (超时 1s)
      → 串口: "[xHCI] Controller reset done"
      → 失败: dump USBCMD/STATUS 寄存器
  8.  编程 DCBAA 基地址
  9.  Command Ring: index 255 写 Link TRB (TC=1)，写 CRCR (PCS=1)
  10. Event Ring: ERST[0] = {phys, 256}，写 ERSTSZ/ERSTBA/ERDP
  11. Scratchpad Buffer Array: DCBAA[0] = scratchpad phys
  12. CONFIG = MaxSlots (max slots enabled)
  13. USBCMD.RS=1 → poll HCH=0 (超时 1s)
      → 串口: "[xHCI] Controller running (HCH=0)"
      → 失败: dump USBCMD/STATUS 寄存器
  14. Enable Slot 命令 (polling mode, MSI-X Entry mask)
      → 串口: "[xHCI] Enable Slot: completion_code=X, slot_id=X" ← 关键验收点
      → 超时: dump CRCR/doorbell 寄存器
  15. pci_msix_unmask_entry(dev, 0) — unmask Entry 0，中断开始投递
  16. USBCMD.INTE=1, IMAN.IE=1
      → 等待 ISR 收到 MSI-X 中断
      → ISR 串口: "[xHCI] MSI-X interrupt received on vector XX" ← 第二验收点
```

### 命令提交（polling 模式）

初始化阶段用 `cmd_ring_push` + `poll_cmd_complete` 轮询：

1. 写 TRB 到 Command Ring `cmd_ring_enqueue` 位置，设置 cycle bit = `cmd_ring_ccs`
2. Ring doorbell (slot 0, target 0)
3. 轮询 Event Ring 等待 Command Completion Event (type=33)
4. 返回 completion code

### ISR 流程（验证阶段最小实现）

```
xhci_isr(trapframe_t *tf)
  1. 串口打印 "[xHCI] MSI-X interrupt received on vector XX"
  2. 读 IMAN 确认 IP
  3. 清 IMAN.IP + 确保 IE
  4. 遍历 Event Ring (cycle state 匹配):
     a. Command Completion Event (type=33): 串口打印 completion code
     b. 其他事件类型: 跳过
     c. 更新 ERDP (dequeue pointer + EHB)
  5. lapic_eoi()
```

## QEMU 验证配置

当前 `run.sh` 使用 q35 但无 USB 设备。验证时需加 xHCI 控制器 + USB 设备：

```bash
qemu-system-x86_64 \
    -machine q35 \
    -device qemu-xhci \
    -device usb-kbd \
    # ... 其他现有参数不变 ...
```

说明：
- `-device qemu-xhci`：添加 xHCI 控制器（PCI 设备，有 MSI-X capability）
- `-device usb-kbd`：USB 键盘，使端口 CCS=1，Enable Slot 才有设备可配
- 验收后此配置应保留在 run.sh 中，后续 USB 设备枚举/键盘集成继续使用

## 已知后续工作

| 项目 | 说明 | 优先级 |
|------|------|--------|
| USB 设备枚举 | Get Descriptor / Set Configuration / Configure Endpoint | 高（MSI-X 验证通过后） |
| xHCI 键盘集成 | HID Boot Protocol → hid_ring → kbd_driver | 高（USB 枚举完成后） |
| AHCI MSI-X 迁移 | AHCI 同样可受益于 MSI-X，只需一行 `pci_enable_msix` 调用 | 中 |
| USB mouse | 第二设备 slot / 第二 endpoint | 按需 |
| 多 interrupter | unmask 更多 MSI-X Entry，per-interrupter event ring | 按需 |
| PS/2 移除 | xHCI 键盘稳定后一次性删除 PS/2 路径 | 低 |
