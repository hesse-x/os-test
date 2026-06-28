# PCIe 枚举 + ACPI

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 配置空间访问 | ECAM only | UEFI 保证 MCFG 可用，QEMU OVMF 也有。不写端口 I/O 回退 |
| 2 | PCIe 枚举位置 | 内核态 | BAR 分配和中断路由需要全局协调，属于"机制"范畴 |
| 3 | ACPI 解析范围 | RSDP + XSDT/RSDT + MADT + MCFG | 所有目标设备支持 MSI/MSI-X，不需要传统 INTx 路由，不碰 DSDT/_PRT/AML 解释器 |
| 4 | 中断策略 | 全走 MSI/MSI-X | 不碰传统 INTx 路由 |
| 5 | PCI 桥处理 | 从第一天支持 Type 1 header 递归扫描子总线 | 实机上桥后设备（如 AMD GPU）不可见，否则扫描不到 |
| 6 | 枚举时机 | 启动时主动枚举+初始化，不分层 | smp_boot_aps 之后、加载 ELF 之前完成 |
| 7 | ECAM 映射策略 | 按 MCFG bus 范围一次性映射，用 2MB huge page | 不映射全部 64GB，只映射 start_bus 到 end_bus 范围 |
| 8 | BAR MMIO 缓存 | 默认 UC（uncacheable）；`pci_enable_device_wc` 可指定 BAR 用 WC | VBE MMIO 寄存器 UC 必须；framebuffer WC 性能好 |
| 9 | 物理连续内存 | xHCI TRB Ring 用 BFC 分配器预分配（pci_init 阶段） | virt_to_phys = 简单减 VMA_BASE，不需要新分配器 |
| 10 | GPU 用户态驱动 BAR 映射 | 复用 `mmap(MAP_PHYSICAL)` | 新增 `sys_pci_dev_info(bus, dev, func)` 返回 BAR 元信息，用户态用 mmap 映射 |
| 11 | UEFI stub 职责 | 只加载 ELF + 传 RSDP/mmap | 所有硬件发现（MADT、MCFG）由内核自己从 ACPI/PCIe 获取 |

### ACPI 表解析

文件：kernel/acpi.c / kernel/acpi.h

**解析流程**（kernel/acpi.c : acpi_init）：

1. 验证 RSDP（签名 + 校验和）
2. Rev >= 2 走 XSDT（64-bit entry），否则走 RSDT（32-bit entry）
3. 遍历 SDT entry 调用 `acpi_find_table(signature)` 通用查找
4. `parse_madt()` — LAPIC base、IOAPIC base/GSI、APIC IDs（最多 4 CPU）、ISO overrides（最多 16）
5. `parse_mcfg()` — ECAM 基物理地址 + segment + bus 范围

**全局结果**：

- `g_madt`（acpi_madt_result_t）：lapic_base, ioapic_base, ioapic_gsi_base, ncpus, apic_ids[4], num_iso, iso[16]
- `g_mcfg`（acpi_mcfg_result_t）：ecam_base, segment, start_bus, end_bus

**API**：

- `acpi_find_table(const char signature[4])` — 按 4-byte 签名查找表，返回虚拟地址或 NULL
- `acpi_find_iso(uint8_t isa_irq)` — 查找 ISA IRQ 的 ISO override

### PCIe 枚举核心

文件：kernel/pci.c / kernel/pci.h

**pci_device_t 结构**（kernel/pci.h : pci_device_t）：
  bus/dev/func : uint8_t
  vendor_id/device_id/class_code : uint16_t
  header_type : uint8_t
  msi_cap_offset/msix_cap_offset : uint8_t — 0 = not found
  msix_table_bar/msix_pba_bar : uint8_t
  msix_table_offset/msix_pba_offset : uint32_t
  msix_vector_base : int（-1 = not allocated）, msix_num_vectors : int
  enabled : bool
  bar[6] : pci_bar_t — phys/size/vaddr/type

MAX_PCI_DEV = 64

**ECAM 配置空间访问**：

`config_addr = ecam_base + (bus << 20) + (dev << 15) + (func << 12) + offset`

- `pci_read_config(bus, dev, func, offset)` / `pci_write_config(...)` — ECAM-based

**枚举策略**（kernel/pci.c : pci_init / pci_scan_bus）：

1. `map_ecam_mmio()` — 将 MCFG base_address 通过 2MB huge page 映射到内核 higher-half 设备区
2. 深度优先遍历：从 bus 0 开始，读 Vendor ID，0xFFFF = 无设备
3. PCI 桥（Type 1 header）：读 Primary/Secondary/Subordinate Bus 号，递归扫描子总线
4. Capability 链 walk：记录 MSI (ID=0x05) 和 MSI-X (ID=0x11) offset

**BAR 解析**（kernel/pci.c : pci_size_bar / pci_map_bar_mmio）：

- 读 BAR，判断 I/O 空间（bit 0=1）vs MMIO（bit 0=0）
- MMIO BAR：写全 1 → 读回大小 → 映射到内核 higher-half 设备区
- I/O BAR：记录端口基址

**API**：

- `pci_find_device(uint16_t class_code)` — 按 class code 查找
- `pci_find_device_by_id(uint16_t vendor, uint16_t device)` — 按 vendor:device ID 查找
- `pci_enable_device(pci_device_t *)` — 映射 BAR MMIO（全部 UC），启用 Bus Master + Memory Space
- `pci_enable_device_wc(pci_device_t *, int wc_bar_idx)` — 指定 BAR 用 WC 缓存
- `pci_enable_msi(pci_device_t *)` — 分配 1 MSI 向量，编程 address/data，启用 MSI
- `pci_enable_msix(pci_device_t *, int num_vectors)` — 分配 MSI-X 向量，编程 Table Entry，启用 MSI-X
- `pci_msix_mask_entry` / `pci_msix_unmask_entry` — Mask/Unmask MSI-X Table Entry
- `pci_msix_table_addr` / `pci_msix_pba_addr` / `pci_msix_vector_base` — MSI-X 地址/向量查询
- `sys_pci_dev_info(bus, dev, func, out)` — Syscall 27：返回 BAR 物理地址/大小/类型给用户态驱动

**pci_dev_info_t**（common/syscall_nums.h : pci_dev_info_t）：
  vendor_id/device_id/class_code : uint16_t
  num_bars : uint8_t
  bars[6] : pci_dev_info_bar_t — phys/size/type(0=MMIO32, 1=IO, 2=MMIO64)

### 启动流程变更

kernel/kernel.c : kernel_main 中调用顺序：

```
init_mem(bi)       — 物理内存管理
acpi_init(bi->rsdp) — RSDP → XSDT → 遍历表 → 解析 MADT + MCFG
isr_init()          — APIC 初始化（从 acpi 结果读取 LAPIC/IOAPIC 地址）
kernel_init_finish()
slab_init()
sig_init()
proc_init()
smp_boot_aps()
pci_init()          — ECAM 映射 → 枚举设备 → capability 链
display_init()      — PCI 发现 bochs-display + VBE modeset
ahci_init()         — PCI 发现 AHCI 控制器
vfs_init()          — devtmpfs 注册（含 /dev/kms）
xhci_init()         — PCI 发现 xHCI + MSI-X + USB 键盘枚举
```

`acpi_init` 在 `init_mem` 之后、`isr_init` 之前——因为 isr_init 需要 MADT 的 LAPIC/IOAPIC 地址。

### 与其他模块的关系

- **xHCI**：依赖 pci_find_device + pci_enable_device + pci_enable_msix。详见 [xhci.md](xhci.md)
- **KMS**：依赖 pci_find_device_by_id + pci_enable_device_wc。详见 [kms.md](kms.md)
- **AHCI**：依赖 PCI 发现 + BAR 映射
- **boot**：acpi_init 从 boot_info.rsdp 获取 RSDP 物理地址。详见 [boot.md](boot.md)

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| boot_info 瘦身 | 移除 fb_*/lapic_base/ioapic_base/ncpus/apic_ids，仅保留 magic/kernel_phys/rsdp/mmap_* | 中 |
| MADT 解析从 stub.c 迁移到内核 | 当前 stub.c 已解析 MADT 通过 boot_info 传递，内核 acpi_init 重新解析；统一由内核解析，boot_info 只传 RSDP | 中 |
| 用户态 GPU 驱动 BAR 映射 | `sys_pci_dev_info` + `mmap(MAP_PHYSICAL)` 支持用户态驱动（如 AMD GPU）映射 BAR | 低 |
| AMD Oland 原生 GPU 驱动 | PCI 1002:6611，CRTC 编程 → 替换 UEFI GOP | 低 |
| 多 segment group 支持 | 当前只解析 MCFG 第一个 entry，多 segment 需遍历所有 entry | 低 |
