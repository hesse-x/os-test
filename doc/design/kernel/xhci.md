# xHCI 驱动 + MSI-X

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 中断模式 | 只做 MSI-X | 实机 2013+ xHCI 控制器用 MSI-X；不支持 MSI-X 的设备直接跳过，不做 MSI/INTx fallback |
| 2 | IDT 向量范围 | 64-127 | MAX_IRQ_HANDLERS=128；MSI-X 向量从 64 起分配 |
| 3 | MSI-X Table/PBA 映射 | 复用 BAR MMIO | xHCI MSI-X Table/PBA 在 BAR0 MMIO 空间内，BAR 已被 pci_enable_device 映射，直接 offset 访问 |
| 4 | PCI capability 存储 | pci_device 加 msix_cap_offset | pci_scan_bus 时 walk capability 链，找到 MSI-X (ID=0x11) 记 offset，没找到留 0 |
| 5 | xHCI 驱动位置 | 内核态 | 需 MMIO BAR 访问、物理连续 DMA 内存、中断处理；和 AHCI 同模式 |
| 6 | 内存分配 | alloc_page_low | 初始化时一次性分配，永不释放 |
| 7 | TRB 环大小 | 固定 1 页（256 个 TRB） | 单设备绑绑有余 |
| 8 | USB 键盘枚举 | xhci_init 中一步完成 | ISR 中做枚举需异步状态机或内核工作队列（不存在），热插拔暂不支持 |
| 9 | ISR→驱动通知 | `isr_lookup_driver(DEV_KBD)` + wake_process | 无需新 syscall，kbd_driver 用 device_register 注册 PID，ISR 查 dev_table |
| 10 | EP 方向 | EP 1-IN (Interrupt IN)，DCI=3 | USB 键盘通过 Interrupt IN 端点向主机发送数据 |

### MSI-X 基础设施

**PCI Capability 链解析**（kernel/pci.c : pci_scan_bus）：

1. 读 Config offset 0x34 (Capabilities Pointer)
2. 逐项读 Capability ID (offset+0) + Next Pointer (offset+1)
3. 找到 MSI-X (ID=0x11) 时存入 `pci_device.msix_cap_offset`
4. 链结束（Next Pointer=0）时停止

**MSI-X 向量分配**：

- 分配范围：64-127
- `pci_enable_msix(dev, num_vectors)` 调用 `alloc_irq_vectors(num_vectors)`，连续分配
- 每个 MSI-X 设备按需求量分配，Entry 0 初始 unmask，其余 mask

**通用 API**（kernel/pci.c）：

- `int pci_enable_msix(pci_device_t *dev, int num_vectors)` — 解析 capability，分配向量，写 Table Entry（mask），设 Enable bit，返回向量数
- `void pci_msix_mask_entry(pci_device_t *dev, int entry)` / `pci_msix_unmask_entry(...)` — Mask/Unmask 单个 Table Entry
- `void *pci_msix_table_addr(pci_device_t *dev)` / `pci_msix_pba_addr(...)` — 获取 MSI-X Table/PBA 虚拟地址
- `int pci_msix_vector_base(pci_device_t *dev)` — 获取分配的向量基址

**MSI-X Message Address/Data**：

- Message Address = `0xFEE00000 | (target_apic_id << 12)`
- Message Data = `allocated_vector | (delivery_mode << 8)`，delivery_mode=0 (Fixed)，trigger_mode=Edge

### 内核 xHCI 驱动

文件：kernel/xhci.c / kernel/xhci.h

**内存分配**（6 页 / 24KB，alloc_page_low 一次性分配）：

| 结构 | 大小 | 用途 |
|------|------|------|
| DCBAA | 1 页 | Device Context Base Address Array（256×8B）|
| Input Context | 1 页 | Address Device 命令的输入上下文 |
| Command Ring | 1 页 | 256 个 TRB（index 255 为 Link TRB）|
| ERST | 1 页 | Event Ring Segment Table（1 entry×16B）|
| Event Ring Segment | 1 页 | 256 个 TRB |
| Scratchpad Buffer | 1 页 | xHC 要求的 scratchpad buffer |

**初始化流程**（kernel/xhci.c : xhci_init）：

1. `pci_find_device(class=0x0C03, prog_if=0x30)` 查找 xHCI PCI 设备
2. `pci_enable_device(dev)` — 映射 BAR0 MMIO + Bus Master + Memory Space
3. 检查 `msix_cap_offset != 0`，否则跳过
4. 分配 DMA 内存（6 页）
5. 读 HCSPARAMS1: MaxSlots, MaxIntrs, MaxPorts
6. 配置 MSI-X: `pci_enable_msix(dev, max_intrs)` + `register_irq(vector, xhci_isr)`
7. xHC reset: USBCMD.HCRST → poll HCRST=0（超时 1s）
8. 编程 DCBAA 基地址
9. Command Ring: index 255 写 Link TRB (TC=1)，写 CRCR (PCS=1)
10. Event Ring: ERST[0] = {phys, 256}，写 ERSTSZ/ERSTBA/ERDP
11. Scratchpad Buffer Array: DCBAA[0] = scratchpad phys
12. CONFIG = MaxSlots
13. USBCMD.RS=1 → poll HCH=0（超时 1s）
14. `xhci_init_keyboard()` — 完整 USB 键盘枚举（见 [kbd.md](kbd.md)）
15. `pci_msix_unmask_entry(dev, 0)` — unmask Entry 0，中断开始投递
16. USBCMD.INTE=1, IMAN.IE=1

**命令提交**（polling 模式）：

1. `cmd_ring_push` — 写 TRB 到 Command Ring，设 cycle bit
2. Ring doorbell (slot 0, target 0)
3. `poll_event` — 轮询 Event Ring 等待 Command Completion Event (type=33)
4. 返回 completion code

**ISR 流程**（kernel/xhci.c : xhci_isr）：

1. 读 IMAN 确认 IP，清 IMAN.IP + 确保 IE
2. 遍历 Event Ring (cycle state 匹配):
   - Command Completion Event (type=33): 记录 completion code
   - Transfer Event (TRB_TRANSFER): 若为键盘 EP1-IN，读 HID DMA buffer → 写 USB HID SHM keyboard sub-ring → `isr_lookup_driver(DEV_KBD)` 找 kbd_driver PID → `wake_process()` 通知；replenish TRB + ring Doorbell
   - 其他事件: 跳过
3. 更新 ERDP (dequeue pointer + EHB)
4. `lapic_eoi()`

**xhci_poll** — 定时器回调，ring EP1-IN doorbell（解决 QEMU NAK retry 问题）

### 与其他模块的关系

- **PCIe**：xhci_init 依赖 `pci_find_device`、`pci_enable_device`、`pci_enable_msix`。详见 [pcie.md](pcie.md)
- **键盘驱动**：xHCI ISR 写 USB HID SHM → wake kbd_driver。详见 [kbd.md](kbd.md)
- **共享内存**：USB HID SHM 通过 `shm_create_internal` + `devtmpfs_create("usb_hid", ..., shm)` 注册，kbd_driver 通过 `open("/dev/usb_hid")` + `mmap` 访问。详见 [ipc.md](ipc.md)
- **中断分发**：MSI-X 向量分配 + ISR 注册。详见 [boot.md](boot.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| USB 设备热插拔 | Port Status Change 事件处理 + 异步状态机（需内核工作队列）；当前枚举在 xhci_init 一步完成 | 中 |
| 多 interrupter | unmask 更多 MSI-X Entry，per-interrupter event ring（当前只用 interrupter 0） | 低 |
| USB mouse | 第二设备 slot / 第二 endpoint | 低 |
| AHCI MSI-X 迁移 | AHCI 可用 `pci_enable_msi` 一行启用 MSI，减少中断延迟 | 低 |
| PS/2 完全清理 | xHCI 键盘稳定后确认 GSI 1 mask 可靠移除 | 低 |
