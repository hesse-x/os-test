# PCIe 枚举 + 原生驱动迁移工作流

## 目标

将键盘驱动从 PS/2 迁移到 USB（xHCI），将显示驱动从 UEFI GOP 继承迁移到原生 PCIe GPU 编程。
**PCIe 枚举是所有后续工作的前置基础设施。**

## 依赖关系

```
ACPI (MCFG 解析)
  └→ PCIe 枚举 (配置空间读 + BAR 映射)
       ├→ xHCI 驱动 → USB HID → USB 键盘 → 替换 kbd_driver
       ├→ GPU 驱动 → CRTC 编程 → 替换 kms_driver (UEFI GOP)
       └→ MSI/MSI-X → PCIe 设备中断路由
```

## 阶段总览

| 阶段 | 内容 | 依赖 | 状态 |
|------|------|------|------|
| P0 | 内核 ACPI 表解析（MCFG + 通用遍历） | 无 | 未开始 |
| P1 | PCIe 枚举核心（ECAM + 配置空间 + BAR 映射） | P0 | 未开始 |
| P2 | xHCI 最小驱动 → USB 键盘 | P1 | 未开始 |
| P3 | GPU 最小驱动 → 原生 mode setting | P1 | 未开始 |
| P4 | kbd_driver 迁移（PS/2 → USB） | P2 | 未开始 |
| P5 | kms_driver 迁移（GOP → 原生 KMS） | P3 | 未开始 |
| P6 | MSI/MSI-X 中断 | P1 | 未开始 |

---

## P0: 内核 ACPI 表解析

### 目标

内核侧能遍历 ACPI 表，定位 MCFG 表（PCIe 配置空间基址）。
当前 stub.c 在 UEFI 环境中解析了 MADT，内核需要同样的能力。

### 现有基础

- `boot/stub.c` 已有 RSDP → XSDT 遍历 + MADT 解析（UEFI 环境）
- `boot_info.rsdp` 已传递 RSDP 物理地址给内核
- 内核侧尚无 ACPI 解析代码

### 工作项

- [ ] 实现 RSDP 校验 + XSDT 遍历（从 stub.c 移植 + 通用化）
- [ ] 实现 `acpi_find_table(signature)` 通用查找函数
- [ ] 解析 MCFG 表：提取 `base_address`（ECAM 基物理地址）+ segment/bus 范围
- [ ] 将 MADT 解析从 stub.c 迁移到内核（替换 boot_info 传递，boot_info 瘦身）
- [ ] boot_info 瘦身：移除 fb_*/lapic_base/ioapic_base/ncpus/apic_ids，仅保留 magic/kernel_phys/rsdp/mmap_*

### 关键数据结构

```c
// MCFG 表 (PCI Firmware Spec 3.0)
struct acpi_mcfg {
    struct acpi_sdt_header header;
    uint64_t reserved;       // 8 bytes reserved
    // entries[] follow, each 16 bytes:
};

struct acpi_mcfg_entry {
    uint64_t base_address;   // ECAM 基物理地址
    uint16_t pci_segment;    // PCI segment group
    uint8_t  start_bus;      // 起始 bus 号
    uint8_t  end_bus;        // 结束 bus 号
    uint32_t reserved;
};
```

### 验证

- [ ] 内核启动时串口打印 MCFG base_address + bus 范围
- [ ] QEMU + OVMF 环境下 MCFG 可正确解析

### 启动流程变更

```
kernel_main:
  init_mem          → 物理内存管理
  acpi_init         ← 新增：RSDP → XSDT → 遍历表 → 解析 MADT + MCFG
  isr_init          ← 修改：apic_init 从 acpi 结果读取而非 boot_info
  kernel_init_finish
  slab_init
  proc_init
  smp_boot_aps
  pci_init          ← 新增：ECAM 映射 → 枚举设备 → xHCI 初始化
  --- 加载 ELF → pipe → idle ---
```

`acpi_init` 在 `init_mem` 之后、`isr_init` 之前——因为 `isr_init` 需要 MADT 的 LAPIC/IOAPIC 地址，而 MADT 现在由 `acpi_init` 提供。

---

## P1: PCIe 枚举核心

### 目标

通过 ECAM 机制枚举 PCIe 总线上的设备，读取配置空间，映射 BAR，
为上层驱动（xHCI、GPU）提供 `pci_find_device(class_code)` / `pci_enable_device(dev)` 等接口。

### 设计要点

#### 1. 配置空间访问

ECAM only（见已决问题 #1）。通过 MCFG base_address + bus/dev/func 偏移计算 MMIO 地址：
```
config_addr = ecam_base + (bus << 20) + (dev << 15) + (func << 12) + offset
```

#### 2. 枚举策略

- **深度优先遍历**：从 bus 0 开始，读 Vendor ID，0xFFFF = 无设备
- **PCI 桥处理**（Type 1 header）：读 Primary/Secondary/Subordinate Bus 号，递归扫描子总线。从第一天就支持，否则实机上桥后设备（如 AMD GPU）不可见
- 启动时主动枚举+初始化，不分层（见已决问题）

#### 3. BAR 解析

- 读 BAR，判断 I/O 空间（bit 0=1）vs MMIO（bit 0=0）
- MMIO BAR：写全 1 → 读回大小 → 映射到内核 higher-half 设备区（2MB huge page）
- I/O BAR：记录端口基址，驱动通过 ioperm + inb/outb 访问
- 用户态驱动通过 `sys_pci_dev_info()` 获取 BAR 物理地址，用 `mmap(MAP_PHYSICAL)` 映射

### 工作项

- [ ] ECAM 映射：将 MCFG base_address 通过 2MB huge page 映射到内核 higher-half 设备区
- [ ] `pci_read_config(bus, dev, func, offset)` / `pci_write_config()`
- [ ] PCIe 设备扫描：遍历 bus，读 Vendor/Device ID，识别桥接设备递归扫描
- [ ] `struct pci_device`：bus/dev/func、Vendor/Device ID、Class Code、BAR[6]（phys/size/type/vaddr）、IRQ Pin/Line、header_type、enabled。MAX_PCI_DEV=64
- [ ] `pci_find_device(class_code)` — 内核驱动按类查找设备
- [ ] `pci_find_device_by_id(vendor, device)` — 内核驱动按 ID 查找设备
- [ ] `pci_enable_device(dev)` — 使能设备：映射 BAR + 分配中断
- [ ] `sys_pci_dev_info(bus, dev, func, out)` — 用户态驱动查询设备元信息（BAR 物理地址/大小/类型），复用 mmap(MAP_PHYSICAL) 映射

### 验证

- [ ] 枚举 QEMU 中的所有 PCIe 设备并打印（VGA、xHCI 控制器、virtio 等）
- [ ] 正确读取 xHCI 控制器的 BAR 地址
- [ ] BAR MMIO 映射后可读写 xHCI CAP 寄存器

---

## P2: xHCI 最小驱动 → USB 键盘

### 目标

最小化 xHCI 内核驱动，目标仅为从 USB 键盘读取键码，替代 PS/2 kbd_driver。

### 设计要点

- **内核态驱动**（见已决问题 #7/#8）：需要 MMIO 访问、物理连续内存（TRB 环）、MSI 配置
- 只支持一个 xHCI 控制器、一个设备槽位、一个 Interrupt endpoint
- 轮询模式（不用 MSI），简化中断路由
- 只支持 USB HID Boot Protocol（无需解析 HID Report Descriptor）
- TRB 环（Command Ring + Event Ring + Transfer Ring）最小实现
- **shm 接口**（见已决问题）：内核 xHCI 中断 → 写 HID code 到 kbd_shm → notify kbd_driver 用户态进程

### 工作项

- [ ] xHCI 控制器初始化（从 pci_device 获取 BAR，映射 MMIO）
- [ ] xHCI 最小操作：Reset → Run → 枚举端口 → Address Device → Configure Endpoint
- [ ] Interrupt Transfer Ring：提交 TRB → 轮询 Event Ring → 读取键码
- [ ] kbd_shm 接口：内核中断处理写 HID code，notify kbd_driver 用户态进程
- [ ] HID Boot Protocol 解码（kbd_driver 用户态：HID code → key_event）

### 验证

- [ ] QEMU 中 xHCI 控制器初始化成功
- [ ] USB 键盘按键事件可通过 xHCI 读取
- [ ] key_event 与当前 kbd_driver 输出格式一致，terminal 可直接消费

---

## P3: GPU 最小驱动 → 原生 Mode Setting

### 目标

不再依赖 UEFI GOP，由 OS 自己编程 GPU CRTC 设置显示模式。

### 设计要点

- 依赖 PCIe 枚举找到 GPU 设备
- 需要特定 GPU 的寄存器编程（bochs-display / virtio-gpu 在 QEMU 中；真实 GPU 需厂商驱动）
- **QEMU 策略**：bochs-display (PCI Vendor 0x1234) — 寄存器简单，验证 PCIe 管线
- **实机策略**：AMD Oland (R7 240, 1002:6611) 独显，QEMU 直通或实机启动均可

### 工作项

- [ ] QEMU bochs-display 驱动：PCI 枚举 → BAR 映射 → 写 CRTC 寄存器设置模式
- [ ] 替换 sys_fb_info(UEFI GOP) → 从 GPU 驱动获取 framebuffer 信息
- [ ] kms_driver 改造：从 GPU 驱动获取 fb 地址而非 sys_fb_info
- [ ] (远期) AMD Oland 原生驱动

### 验证

- [ ] QEMU 中不依赖 GOP，OS 自己设置显示模式并输出画面
- [ ] 显示与当前 GOP 方案效果一致

---

## P4: kbd_driver 迁移（PS/2 → USB）

### 前置

P2 完成后执行。

### 工作项

- [ ] 移除 PS/2 端口 0x60/0x64 访问代码
- [ ] 移除 IRQ1 绑定
- [ ] kbd_driver 主循环改为从 kbd_shm 读取 HID code
- [ ] kbd_translate 改为 HID Boot Protocol → key_event 映射
- [ ] KBD_REQ_BIND/UNBIND 协议保持不变（上层 terminal 无感知）
- [ ] 验证：键盘输入 → xHCI → kbd_driver → kbd_ring → terminal → shell 全链路

---

## P5: kms_driver 迁移（GOP → 原生 KMS）

### 前置

P3 完成后执行。

### 工作项

- [ ] 移除 sys_fb_info 依赖
- [ ] 从 GPU 驱动获取 framebuffer 物理地址和模式信息
- [ ] display 协议保持不变（terminal 无感知）
- [ ] 可选：硬件 page flip（写 CRTC framebuffer offset 寄存器替代 memcpy）

---

## P6: MSI/MSI-X 中断

### 前置

P1 完成后可独立进行。

### 工作项

- [ ] MSI capability 解析 + APIC 路由配置
- [ ] MSI-X table/pending bit 映射
- [ ] xHCI 改用 MSI 中断（替代轮询）

---

## 已决设计问题（全部已决，无待决）

1. **ECAM vs 端口 I/O** → **ECAM only**。UEFI 保证 MCFG 可用，QEMU OVMF 也有。不写端口 I/O 回退。
2. **PCIe 枚举位置** → **内核态**。BAR 分配和中断路由需要全局协调，属于"机制"范畴。用户态驱动通过 `pci_find_device` / `pci_enable_device` 消费结果。
3. **ACPI 解析范围** → **只做 MCFG**。所有目标设备（xHCI、AMD GPU、virtio-blk、HDA）均支持 MSI/MSI-X，不需要传统 INTx 路由，不碰 DSDT/_PRT/AML 解释器。
4. **中断策略** → **全走 MSI/MSI-X**，不碰传统 INTx 路由。
5. **GPU 驱动远期目标** → **AMD Oland (R7 240, 1002:6611)** 独显直通 QEMU。开发阶段可用 bochs-display 验证 PCIe 管线。
6. **键盘驱动** → **USB only**，移除 PS/2。xHCI 在内核态，kbd_driver 用户态进程通过 kbd_shm 从内核 xHCI 层获取 HID code。
7. **驱动分界原则** → 需要内核独有能力（APIC 编程、页表映射、物理连续内存分配、DMA）的驱动放内核态；只需 MMIO 读写的可留用户态。
8. **内核态驱动**：PCIe 枚举、xHCI (USB 主控)、ATA/AHCI (磁盘)
9. **用户态驱动**：GPU (MMIO 映射即可)、terminal、fs_driver、shell
10. **disk_driver 进程** → 方向 A：移除用户态 disk_driver 进程，fs_driver 直接通过 `sys_block_read/write` 访问内核磁盘驱动。时机等 fs_driver 重构完成后执行。
11. **UEFI stub 职责** → **只加载 ELF + 传 RSDP/mmap**。所有硬件发现（MADT、MCFG、GOP）由内核自己从 ACPI/PCIe 获取。boot_info 长期瘦身，移除 fb_*/lapic_base/ioapic_base/ncpus/apic_ids 等字段。MADT 解析从 stub.c 迁移到内核。
12. **物理连续内存** → xHCI TRB Ring 用现有 BFC 分配器预分配（pci_init 阶段），不在中断上下文动态分配。`virt_to_phys` = 简单减 VMA_BASE。不需要新分配器。
13. **PCI 桥处理** → 从第一天就支持 Type 1 header 递归扫描子总线，否则实机上桥后设备不可见。
14. **xHCI 内核接口** → **shm 模型**：内核 xHCI 中断 → 写 HID code 到 kbd_shm → notify kbd_driver。用户态 kbd_driver 做 HID→key_event 转换，kbd_ring + KBD_REQ_BIND 协议不变。
15. **PCIe 枚举时机** → **启动时主动枚举+初始化**，不分层。在 smp_boot_aps 之后、加载 ELF 之前完成。将来需要时再拆分枚举/初始化两步。
16. **ECAM 映射策略** → **按 MCFG bus 范围一次性映射**，用 2MB huge page。不映射全部 64GB，只映射 start_bus 到 end_bus 范围（通常 256MB × bus 数量）。与现有 `init_fb` 映射模式一致。
17. **GPU 用户态驱动 BAR 映射** → **复用 `mmap(MAP_PHYSICAL)`**。新增 `sys_pci_dev_info(bus, dev, func)` 返回 BAR 物理地址/大小等元信息，用户态驱动拿到后用 mmap 映射。不新增映射 syscall。
