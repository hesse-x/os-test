# UEFI 引导

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 引导方式 | EFI Stub（内核即 EFI 应用） | 单一二进制，不需要 GRUB 等额外引导器 |
| 2 | EFI 库 | 手写 efi.h | 只需约 5 个 EFI 函数，自己定义比引入 GNU-EFI 更轻 |
| 3 | 信息传递 | 全局变量 boot_info | stub 在物理地址写入，内核设页表后从虚拟地址读取 |
| 4 | PE/COFF 构建 | ELF → objcopy 转 PE | 链接脚本开头预留 ~4KB 给 PE/COFF 头 + EFI 头 |
| 5 | Stub 寻址 | RIP 相对寻址 | 不依赖 UEFI 重定位，无需 .reloc 段 |
| 6 | 代码分工 | stub 入口+调 EFI API→汇编，建页表→C，切 CR3+跳内核→汇编 | 汇编处理硬件状态切换，C 复用 paging.cc 逻辑 |
| 7 | Framebuffer | stub 从 GOP 获取物理地址 | 内核在自己的页表中映射到 device_vma_base 区域 |
| 8 | ACPI RSDP | stub 从 UEFI 配置表提取 | 内核未来加 ACPI 时直接读取 |
| 9 | 介质构建 | 单盘两分区 disk.img | mkdisk.sh 生成 ESP(FAT16) + 根(FAT32)：ESP 放 BOOTX64.EFI + myos.elf + init.elf |
| 10 | QEMU 启动 | -bios OVMF.fd + 硬盘启动 | -drive file=build/disk.img,format=raw |

### 启动流程

```
UEFI 固件 → 加载 BOOTX64.EFI → stub 汇编入口(EFI环境, RIP相对寻址)
  →  UEFI API 读信息 → 写全局变量 → ExitBootServices
  → 建页表(C函数) → 切 CR3 + 跳内核(汇编)
  → 内核入口(纯净长模式, 虚拟地址)
```

### boot_info 结构

boot/stub.c : boot_info（全局变量，stub 写入物理地址，内核读虚拟地址）

boot_info 字段：
- magic : uint64_t — 魔数，内核入口校验用
- kernel_phys : uint64_t — 内核被加载的物理地址
- rsdp : uint64_t — RSDP 物理地址
- fb_addr : uint64_t — framebuffer 物理地址
- fb_width / fb_height : uint32_t — 分辨率
- fb_pitch : uint32_t — 行字节数
- fb_bpp : uint32_t — 每像素位数
- fb_pixel_format : uint32_t — RGB/BGR 等
- mmap_addr : uint64_t — EFI memory descriptor 数组地址
- mmap_size / mmap_desc_size / mmap_desc_ver : uint64_t — EFI mmap 元数据

SMP 扩展字段详见 [smp.md](smp.md)。

### 内存映射安全

ExitBootServices 后 mmap 缓冲区数据仍然有效：
- mmap 缓冲区在 stub AllocatePool 分配的普通内存中，ExitBootServices 不回收
- 必须在 ExitBootServices 紧之前最后一次调用 GetMemoryMap，确保 map_key 一致
- stub 流程：获取 GOP → AllocatePool 分配 mmap 缓冲区 → GetMemoryMap → ExitBootServices → mmap 数据安全

### 关键源码位置

- EFI stub 入口：boot/stub.c
- EFI 类型定义：common/efi.h
- 页表构建（stub 侧）：boot/stub.c 中 C 函数
- 内核入口：arch/x64/start.S : _start
- 磁盘映像构建（ESP + 根两分区）：build_script/mkdisk.sh

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| ACPI 表解析 | 当前 stub 只提取 RSDP，内核未来需解析 XSDT/MADT 等 ACPI 表 | 低 |
| GOP 模式选择 | 当前使用 UEFI 默认 GOP 模式，未来应支持用户指定分辨率 | 低 |
| boot_info 复制硬编码 128B | start.S 中 memcpy boot_info 用固定 128 字节，结构体扩展会截断，应改为按实际大小拷贝 | 中 |
