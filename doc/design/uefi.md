# UEFI 启动方案设计

## 方案：EFI Stub

内核即 EFI 应用程序，UEFI 固件直接加载，彻底去掉 GRUB。

## 启动流程

```
UEFI 固件 → 加载 BOOTX64.EFI → stub 汇编入口(EFI环境, RIP相对寻址)
  → 调 UEFI API 读信息 → 写全局变量 → ExitBootServices
  → 建页表(C函数) → 切 CR3 + 跳内核(汇编)
  → 内核入口(纯净长模式, 虚拟地址)
```

## 决策记录

### 1. EFI Stub vs 独立 Bootloader
选 EFI Stub。内核单一二进制，不需要复杂引导逻辑，少一层依赖。

### 2. EFI 库
手写 `efi.h`，不依赖 GNU-EFI。只需约 5 个 EFI 函数，自己定义类型比引入外部库更轻。

### 3. 信息传递协议
全局变量。stub 在物理地址写入，内核设置页表后从虚拟地址读取。内存映射用 EFI 原始 mmap descriptor 格式，内核侧适配。

boot_info 结构：
```c
struct boot_info {
    uint64_t magic;           // 魔数，内核入口校验用
    uint64_t kernel_phys;     // 内核被加载的物理地址
    uint64_t rsdp;            // RSDP 物理地址
    uint64_t fb_addr;         // framebuffer 物理地址
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;          // 每像素位数
    uint32_t fb_pixel_format; // RGB/BGR 等
    uint64_t mmap_addr;       // EFI memory descriptor 数组地址
    uint64_t mmap_size;       // 总大小
    uint64_t mmap_desc_size;  // 单个 descriptor 大小
    uint64_t mmap_desc_ver;   // descriptor 版本
};
```

mmap 使用全局静态数组，设上限（如 256 条），在 stub 中提前解析。

### 4. PE/COFF 构建
链接 ELF 后 `objcopy -O pe-x86-64` 转 PE。链接脚本开头预留 ~4KB 给 PE/COFF 头 + EFI 头，代码入口从偏移处开始。

### 5. Stub 寻址
汇编用 RIP 相对寻址，不依赖 UEFI 重定位。无需 `.reloc` 段。

### 6. 代码分工
- stub 入口 + 调 UEFI API → 汇编
- 建页表 → C 函数（从 paging.cc 移植逻辑）
- 切 CR3 + 跳内核 → 汇编

### 7. Framebuffer
stub 从 GOP 获取 framebuffer 物理地址写入全局变量，内核在自己的页表中映射到 device_vma_base 区域。

### 8. ACPI RSDP
stub 从 UEFI 配置表提取 RSDP 物理地址写入全局变量，内核暂不使用，未来加 ACPI 时直接读取。

### 9. 介质构建
FAT32 磁盘映像替代 ISO。`mkimg.sh` 创建 FAT32 img，将 myos.efi 放到 `\EFI\BOOT\BOOTX64.EFI`。

### 10. QEMU 启动
保持 `-bios OVMF.fd`，改 `-cdrom` 为硬盘启动：`-drive file=build/boot.img,format=raw`。

## 内存映射安全

ExitBootServices 后 mmap 缓冲区数据仍然有效：
- mmap 缓冲区存在 stub 分配的普通内存中，ExitBootServices 不回收
- 必须在 ExitBootServices 紧之前最后一次调用 GetMemoryMap，确保 map_key 一致
- stub 流程：获取 GOP → AllocatePool 分配 mmap 缓冲区 → GetMemoryMap → ExitBootServices(用刚拿到的 key) → mmap 数据安全

## 需要删除/替换的文件

- `grub.cfg` — 不再需要 GRUB
- `mkiso.sh` — 替换为 `mkimg.sh`
- `arch/x64/trampoline.S` — 32→64位模式切换不再需要，UEFI 直接在长模式交权
- `arch/x64/boot.cc` — multiboot2 header 不再需要

## 需要新增的文件

- `arch/x64/efi.h` — EFI 类型定义和函数指针
- `arch/x64/stub.S` — EFI stub 汇编入口
- `arch/x64/stub.cc` — UEFI API 调用 + 页表设置 C 函数
- `mkimg.sh` — FAT32 磁盘映像构建脚本
