# 阶段四：Shell + ATA PIO + ELF Loader

> **历史文档**：这是 x86-32 / ELF32 / Multiboot2 阶段的 Shell 设计方案。当前代码已迁移至 x86-64 / ELF64 / UEFI。
>
> 当前实现概要：
> - Shell 源码: `shell/shell.cc`（C++），通过 KBD_SHM 共享页接收键盘输入，SYS_WAIT 阻塞等待通知
> - Shell 构建: `g++ -m64 -ffreestanding -nostdlib ... -c shell/shell.cc` + `ld -m elf_x86_64 -Ttext 0x400000`
> - Shell 不再使用 sys_getc，改用共享页环形缓冲区 + sys_wait/sys_notify 与键盘驱动进程通信
> - Shell 功能：readline（支持退格）、disk_read（hex dump）、help 命令
> - 内核启动加载 3 个 ELF：disk_driver(LBA 1) → kbd_driver(LBA 33) → shell(LBA 65)
> - ATA PIO: 驱动字节 0xF0（从盘，QEMU 第二个 IDE 设备），非 0xE0
> - ELF64 Loader: Elf64_Ehdr/Elf64_Phdr，4级页表映射（PML4→PDPT→PD→PT）
> - process_create_elf 支持 iopl 参数：驱动 IOPL=3，shell IOPL=0
> - 共享页通信设计见 [user_driver.md](user_driver.md)
> - 参见 CLAUDE.md 了解当前架构

## 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| shell 代码加载方式 | ELF32 static binary + Multiboot2 module tag | 标准格式，ELF loader 写一次以后不用改；不用手写机器码 |
| 磁盘驱动 | ATA PIO LBA28 | 最简单，~50 行，QEMU 默认支持 |
| 文件系统 | 暂不做，硬编码 LBA | 最快打通全链路，之后替换为自定义文件表 |
| ELF loader 支持范围 | 多 PT_LOAD 段，不支持动态链接/重定位 | 比单段只多 20 行，一劳永逸；static binary 不需要 PT_DYNAMIC |
| shell 功能 | 极简：打字回显 + 回车换行 + 滚动 | 只验证全链路正确性，不堆功能 |
| 新 syscall | 零 | 现有 putc/getc/getpid/yield 够用 |

## 全链路流程

```
1. QEMU 启动，GRUB 加载内核 + shell.elf（module tag）
2. 内核初始化（现有流程不变）
3. 内核遍历 multiboot2 info，找到 module tag，拿到 shell.elf 地址+大小
4. ATA PIO 驱动：读 LBA 1 起始扇区 → 拿到 shell.elf 原始字节（后续替换为文件系统）
5. ELF loader：解析 header，按 PT_LOAD 段映射到用户地址空间
6. process_create(elf_entry)：用加载好的页表/入口创建进程
7. schedule() → 切到 shell 进程
8. shell 循环：getc → putc 回显，回车换行
9. fb_putc 加了滚动，打到底自动上移
```

> **注意：** 步骤 3 和步骤 4 短期内有重叠。先用 module tag 方式拿到 shell.elf 验证 ELF loader + shell 全链路；ATA PIO 并行开发，验证磁盘读取正确性后，替换 module tag 为从磁盘读取。两步独立，互不阻塞。

## 1. ATA PIO 磁盘驱动

### 接口

```c
// driver/ata.h
// 读取 LBA28 起始的连续扇区到 buf
// buf 大小必须 >= count * 512
void ata_read_lba(uint32_t lba, uint32_t count, void *buf);
```

### 实现（driver/ata.cc）

```c
// ATA PIO LBA28 读扇区
// I/O 端口：
//   0x1F0  Data
//   0x1F1  Features (未用)
//   0x1F2  Sector Count
//   0x1F3  LBA low
//   0x1F4  LBA mid
//   0x1F5  LBA high
//   0x1F6  Drive/LBA[27:24]
//   0x1F7  Status

#define ATA_DATA     0x1F0
#define ATA_SECTOR   0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7

#define ATA_CMD_READ 0x20
#define ATA_BSY      0x80
#define ATA_DRDY     0x40
#define ATA_DRQ      0x08
#define ATA_ERR      0x01

void ata_read_lba(uint32_t lba, uint32_t count, void *buf) {
    // 1. 等 BSY 清零
    while (inb(ATA_STATUS) & ATA_BSY);

    // 2. 写参数
    outb(ATA_SECTOR, count);          // 扇区数
    outb(ATA_LBA_LO,  lba & 0xFF);   // LBA[7:0]
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);   // LBA[15:8]
    outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);  // LBA[23:16]
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode + LBA[27:24]

    // 3. 发读命令
    outb(ATA_STATUS, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        // 4. 等 DRQ 或 ERR
        uint8_t st;
        while (((st = inb(ATA_STATUS)) & ATA_DRQ) == 0 &&
               (st & ATA_ERR) == 0);

        // 5. 读 256 words = 512 bytes
        for (int i = 0; i < 256; i++) {
            *dst++ = inw(ATA_DATA);
        }
    }
}
```

需要在 `arch/x86/utils.h` 中补充 `inw`：
```c
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
```

### QEMU 启动参数

`run.sh` 中追加磁盘映像：
```
-drive file=disk.img,format=raw,if=ide
```

### 磁盘映像生成

`mkiso.sh` 或构建脚本中：
```bash
# 创建 1MB 空白磁盘映像
dd if=/dev/zero of=disk.img bs=512 count=2048
# LBA 1 起始写入 shell.elf
dd if=shell.elf of=disk.img bs=512 seek=1 conv=notrunc
```

## 2. ELF32 Loader

### 接口

```c
// kernel/elf.h
struct elf_load_result {
    uint32_t entry;      // 入口地址 (e_entry)
    bool     success;
};

// 从内存中的 ELF 数据加载到用户地址空间
// 需要一个页分配回调来映射 PT_LOAD 段
elf_load_result elf_load(const uint8_t *data, uint32_t size,
                         uint32_t *new_pd);
```

### ELF32 关键结构

```c
// ELF32 固定头部
#define EI_NIDENT 16
struct Elf32_Ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;     // program header offset
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize; // program header entry size
    uint16_t e_phnum;     // program header count
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

// Program header
struct Elf32_Phdr {
    uint32_t p_type;   // PT_LOAD = 1
    uint32_t p_offset; // 文件内偏移
    uint32_t p_vaddr;  // 虚拟地址
    uint32_t p_paddr;
    uint32_t p_filesz; // 文件中字节数
    uint32_t p_memsz;  // 内存中字节数（含 BSS）
    uint32_t p_flags;
    uint32_t p_align;
};
```

### 实现（kernel/elf.cc）

```c
elf_load_result elf_load(const uint8_t *data, uint32_t size,
                         uint32_t *new_pd) {
    elf_load_result result = {0, false};

    // 1. 校验 ELF magic
    if (data[0] != 0x7F || data[1] != 'E' ||
        data[2] != 'L'  || data[3] != 'F')
        return result;

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)data;
    result.entry = ehdr->e_entry;

    // 2. 遍历 program headers
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(data + ehdr->e_phoff +
                         i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        // 3. 按 p_vaddr 映射页
        // p_vaddr 决定放哪个 PD/PT 项
        // 需要为每个 4KB 页分配物理页 + PT（如未分配）
        // 拷贝 p_filesz 字节，memsz > filesz 的部分清零（BSS）
        // PDE/PTE flags: 根据段对齐需求至少 0x07 (User+Present+Writable)
        // ... 具体映射逻辑与 process_create 中类似 ...
    }

    result.success = true;
    return result;
}
```

### 映射细节

ELF 段的 `p_vaddr` 可能跨多个页，且不一定页对齐。加载逻辑：

1. 计算覆盖的页范围：`first_page = p_vaddr & ~0xFFF`，`last_page = (p_vaddr + p_memsz - 1) & ~0xFFF`
2. 对每个页：检查 PD 中是否已有对应 PT，没有则分配 + 清零
3. 分配用户物理页，拷贝文件数据（处理页内偏移），BSS 部分清零
4. 设置 PTE flags：`0x07`（Present + Writable + User），PDE 同理

## 3. Shell 程序

### 源码（user/shell.asm）

```nasm
; 极简 shell：打字回显 + 回车换行
; 注意：此示例使用已废弃的 int 0x80 路径和旧 syscall 编号（putc=0, getc=3）
; 当前实现使用 SYSCALL 指令，编号已紧凑重排为 0-17

%define SYS_PUTC  0    ; 已删除
%define SYS_GETC  3    ; 已删除

section .text
global _start

_start:
.prompt:
    ; 打印提示符 "> "
    mov eax, SYS_PUTC
    mov ebx, '>'
    int 0x80
    mov eax, SYS_PUTC
    mov ebx, ' '
    int 0x80

.loop:
    ; 读一个键
    mov eax, SYS_GETC
    int 0x80

    ; 回显
    mov ebx, eax
    mov eax, SYS_PUTC
    int 0x80

    ; 回车换行后回到提示符
    cmp bl, 0x0A
    je .prompt

    jmp .loop
```

### 编译与嵌入

构建流程：
```bash
nasm -f elf32 user/shell.asm -o build/shell.o
i686-elf-ld -Ttext 0x400000 -o build/shell.elf build/shell.o
# shell.elf 写入磁盘映像 LBA 1
dd if=build/shell.elf of=disk.img bs=512 seek=1 conv=notrunc
```

## 4. framebuffer 滚动

### 修改 fb_putc（driver/fb.cc）

在 `\n` 处理逻辑中，当光标已在最后一行时上移：

```c
void fb_putc(char c, uint32_t fg) {
    // ... 现有的 \n, \r, \t 处理 ...

    if (c == '\n') {
        cursor.x = 0;
        if (cursor.y < rows() - 1) {
            cursor.y++;
        } else {
            // 滚动：framebuffer 内容上移一行
            uint32_t line_bytes = fb.pitch * FONT_HEIGHT;
            uint8_t *fb_buf = (uint8_t *)fb.vaddr;
            // 上移：从第 1 行开始覆盖到第 0 行
            for (uint32_t i = 0; i < fb.size - line_bytes; i++) {
                fb_buf[i] = fb_buf[i + line_bytes];
            }
            // 最后一行清零
            for (uint32_t i = fb.size - line_bytes; i < fb.size; i++) {
                fb_buf[i] = 0;
            }
        }
        return;
    }

    // ... 正常字符渲染 ...
}
```

## 5. process_create 适配（已实现方案 A）

`process_create(entry)` + `init_code[]` 已删除。当前仅保留 `process_create_elf`，所有进程从 ELF 加载。

增加参数，传入时走 ELF 路径，否则走 init_code 路径。

推荐 **A**，职责清晰，不改已有接口。

```c
// kernel/proc.h
proc_t *process_create_elf(const uint8_t *elf_data, uint32_t elf_size);
```

实现：
```c
proc_t *process_create_elf(const uint8_t *elf_data, uint32_t elf_size) {
    // 1. 查找空闲 PCB 槽位（同 process_create）
    // 2. 分配内核栈（同 process_create）
    // 3. 分配独立 PD + 拷贝内核 PDE（同 process_create）
    // 4. 调用 elf_load(elf_data, elf_size, new_pd) 映射用户段
    // 5. 分配用户栈页 + PT（同 process_create）
    // 6. 构建 trapframe，entry = elf_load_result.entry
    // 7. 填充 PCB
}
```

## 6. Multiboot2 Module Tag（短期方案）

在内核初始化时遍历 multiboot2 info，找 module tag：

```c
// arch/x86/multiboot2.h 中已有 tag 类型定义
// 需要确认 MULTIBOOT_TAG_TYPE_MODULE = 3

multiboot_tag_module *find_module(uintptr_t mbi_addr) {
    multiboot_tag *tag = (multiboot_tag *)(mbi_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            return (multiboot_tag_module *)tag;
        }
        tag = (multiboot_tag *)((uintptr_t)tag +
                 ALIGN_UP(tag->size, MULTIBOOT_TAG_ALIGN));
    }
    return nullptr;
}
```

kernel_main 中使用：
```c
// 短期：从 module tag 获取 shell.elf
// 长期：从 ATA PIO 读取磁盘 LBA
multiboot_tag_module *mod = find_module(addr);
if (mod) {
    process_create_elf((const uint8_t *)mod->mod_start,
                       mod->mod_end - mod->mod_start);
}
```

grub.cfg 中添加 module 行：
```
multiboot2 /boot/myos.bin
module /boot/shell.elf
```

## 7. 构建集成

### 新增构建步骤

```
1. nasm -f elf32 user/shell.asm → build/shell.o
2. i686-elf-ld -Ttext 0x400000 build/shell.o → build/shell.elf
3. dd 生成 disk.img（ATA PIO 用）
4. shell.elf 拷贝到 iso/boot/（module tag 用）
5. mkiso.sh 打包 ISO（含 shell.elf）
```

### CMakeLists.txt 修改

- `driver/CMakeLists.txt`：添加 `ata.cc`
- `kernel/CMakeLists.txt`：添加 `elf.cc`
- 顶层 CMakeLists.txt 或 build.sh：添加 nasm 编译 shell.asm + ld 链接步骤

## 8. 新增文件汇总

| 文件 | 说明 | 行数 |
|------|------|------|
| `driver/ata.cc` + `driver/ata.h` | ATA PIO LBA28 读扇区 | ~60 |
| `kernel/elf.cc` + `kernel/elf.h` | ELF32 static loader（多 PT_LOAD） | ~80 |
| `user/shell.asm` | 极简 shell（回显+换行） | ~30 |
| 修改 `driver/fb.cc` | fb_putc 加滚动 | ~15 |
| 修改 `kernel/proc.cc/h` | 新增 process_create_elf | ~40 |
| 修改 `arch/x86/utils.h` | 新增 inw | ~4 |
| 修改 `arch/x86/multiboot2.h` | 确认 module tag 定义 | ~0 |
| 修改 `CMakeLists.txt` / `build.sh` / `mkiso.sh` | 构建集成 | ~20 |
| **总计** | | **~250** |

## 实现顺序

```
1. fb_putc 滚动
   → 验证: 串口/putc 循环输出多行，到底时自动上移

2. ATA PIO 驱动 + inw
   → 验证: 读 LBA 0（MBR），串口打印前几个字节确认非全零

3. shell.asm 编译 + disk.img 生成
   → 验证: nasm + ld 生成 shell.elf，file 命令确认 ELF32 static

4. ELF loader
   → 验证: 内核中手工调 elf_load 测试解析 shell.elf 头部

5. process_create_elf + module tag
   → 验证: 内核启动后创建 shell 进程，schedule 切过去

6. Shell 运行
   → 验证: QEMU 中打字回显，回车换行，滚动正常
```

## 后续扩展

- **文件系统**：硬编码 LBA → 自定义文件表（文件名+起始LBA+长度），~50 行
- **FAT32**：UEFI 引导阶段，UEFI boot service 提供文件读取
- **更多 shell 命令**：echo, help, clear, pid 等
- **sys_clear syscall**：清屏，1 行实现
