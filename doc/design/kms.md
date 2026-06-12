# KMS 用户态驱动设计

## 概述

将内核态 framebuffer 渲染（`kernel/fb.cc`）移至用户态 KMS 驱动进程。内核仅保留 `init_fb` 做 framebuffer 物理页映射和元信息保存，所有文本渲染、光标管理、滚动逻辑移到用户态。

**定位**：KMS + 简易终端服务（过渡方案）。当前提供文本渲染能力（putc/clear/scroll/cursor_move），将来做 Wayland compositor 时切到纯 KMS（只管 framebuffer 硬件信息 + scanout），compositor 自行渲染。

## 架构

```
Shell (PID 5)
  │
  │ KMS_REQ 共享页 + sys_notify
  ▼
KMS Driver (PID 4, IOPL=0)
  │
  │ 直接写 framebuffer 映射页 (0x700000)
  ▼
Framebuffer 硬件
```

KMS 是 IOPL=0 的普通用户态进程，不绑 IRQ。Framebuffer 物理页零拷贝映射到 KMS 进程地址空间 `0x700000`，KMS 驱动直接写像素。

## IPC

### KMS_INFO 页（0x508000）

内核写入 framebuffer 元信息，KMS 驱动启动时读取。布局：

```c
struct kms_fb_info {
    uint32_t width;      // 像素宽
    uint32_t height;     // 像素高
    uint32_t pitch;      // 字节/行
    uint32_t bpp;        // 位/像素
    uint64_t fb_vaddr;   // framebuffer 在 KMS 进程内的虚拟地址 (0x700000)
    uint64_t fb_size;    // framebuffer 字节大小
    uint64_t fb_phys;    // framebuffer 物理地址（KMS 驱动参考用）
};
```

### KMS_REQ 页（0x509000）

Shell 等客户端写入渲染请求，单向 notify，无响应页。布局：

```c
#define KMS_CMD_PUTC        0   // arg1=字符, arg2=前景色(fg)
#define KMS_CMD_CLEAR       1   // 无参数
#define KMS_CMD_SCROLL      2   // 无参数，向上滚一行
#define KMS_CMD_CURSOR_MOVE 3   // arg1=x, arg2=y

struct kms_cmd {
    uint32_t cmd;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

// KMS_REQ 页头部
struct kms_req_header {
    uint32_t count;              // 命令数量
    uint32_t reserved;           // 对齐
    struct kms_cmd cmds[0];      // 命令数组，最多 (4096-8)/16 = 255 条
};
```

请求方写 N 条命令到 `cmds[]`，设 `count = N`，然后 `sys_notify(KMS_DRIVER_PID)`。KMS 驱动读 `count`，处理所有命令，清零 `count`。

## 启动顺序与 PID

```
PID 0: BSP idle
PID 1: AP idle(s)
PID 2: disk_driver       (LBA 1-50)
PID 3: kbd_driver        (LBA 51-100)
PID 4: kms_driver        (LBA 101-150)  ← 新
PID 5: shell             (LBA 151-200)  ← 原 101
PID 6: fs_driver         (LBA 201-250)  ← 原 151
LBA 251+: FAT32                         ← 原 201
```

PID 定义集中到 `common/pid.h`，所有驱动 include 该头文件。

## 磁盘布局

```
LBA 0:       MBR 分区表
LBA 1-50:    disk_driver.elf（50 扇区/25KB）
LBA 51-100:  kbd_driver.elf（50 扇区/25KB）
LBA 101-150: kms_driver.elf（50 扇区/25KB）  ← 新
LBA 151-200: shell.elf（50 扇区/25KB）       ← 原 101
LBA 201-250: fs_driver.elf（50 扇区/25KB）   ← 原 151
LBA 251+:    FAT32 分区                      ← 原 201
```

MBR 分区1 (type=0xDA) 覆盖 LBA 1-210（裸 ELF 存储），分区2 (type=0x0C) 覆盖 LBA 211+（FAT32 文件系统）。

## 共享页地址（已迁移到动态 SHM）

所有驱动间 IPC 已统一使用动态共享内存（sys_shm_create/sys_shm_attach），硬编码地址常量和 shm_init/map_shared_pages 已删除。详见 [dynamic_shm_migration.md](dynamic_shm_migration.md)。

## 内核改动

### fb.cc 瘦身

- **保留** `init_fb`：映射 framebuffer 物理页到 higher-half 设备区 + 保存元信息到全局 `g_fb_info`
- **保留** `fb.h` 中 `init_fb` 声明
- **删除** 所有渲染函数（`fb_putc`/`clear`/`scroll_up`/`advance_line`/`prints`/`cursor_move`/`cursor_get_*`）、字体数据（`font8x16`）、`Framebuffer`/`Cursor` struct
- **新增** 全局 `g_fb_info`（`kms_fb_info` 类型），`init_fb` 末尾填入

### fb_lock 删除

`sys_putc` 删除后 `fb_lock` 无使用方，从 `kernel/trap.cc` 删除。

### sys_putc 删除

- `sys_putc` 已完全删除，编号已紧凑重排（当前 NR_SYSCALL=18，编号 0-17）
- `fb_lock` 同步删除

### shm_init 改动

- `shm_init` 分配 KMS_INFO + KMS_REQ 物理页
- `shm_init` 末尾把 `g_fb_info` 拷贝到 KMS_INFO 共享页

### process_create_elf 改动

- 创建 KMS 进程时，额外将 framebuffer 物理页映射到进程地址空间 `0x700000`（4KB 页，不加 PCD/PAT）
- 需要一种方式识别 KMS 进程（通过 ELF 加载顺序/PID 判断，或传入标志位）

### map_shared_pages（已删除）

map_shared_pages 已在动态 SHM 迁移中删除，KMS 通过 sys_shm_attach 获取共享内存。

### kernel_main 改动

- `clear()` 调用删除（KMS 驱动启动后自行清屏）
- 新增 KMS ELF 加载：`load_elf_from_disk(elf_buf, ELF_MAX_BUFSIZE, 101)` → `process_create_elf(elf_buf, ELF_MAX_BUFSIZE, 0)`
- shell LBA 改为 151，fs_driver LBA 改为 201

## KMS 驱动实现

文件：`driver/kms_driver.cc`（单文件），内容包括：

1. **8x16 PC BIOS 字体数据** — 从 `kernel/fb.cc` 移出
2. **命令解析主循环** — 先 `process_commands()` 处理待处理请求 → 仅在 `req->count == 0` 时 `sys_wait()` 阻塞 → 被唤醒后重新处理
3. **渲染逻辑** — `putc`/`clear`/`scroll_up`/`cursor_move`，从 `kernel/fb.cc` 移出
4. **framebuffer 直写** — 通过 KMS_INFO 获取 `fb_vaddr`，直接写像素

**主循环设计要点**：必须先处理再等待，不能先 `sys_wait()` 再处理。原因是 `sys_notify` 只能唤醒已处于 BLOCKED 状态的进程，如果客户端在 KMS 进入 `sys_wait()` 之前就写入了请求并通知，通知会丢失。先检查 `req->count` 可以在进入等待前处理掉这些"早到"的请求。

## Shell 改动

- 删除 `sys_putc` 输出路径
- 新增 `kms_flush` 函数：向 KMS_REQ 写命令 → `sys_notify(KMS_DRIVER_PID)`
- libc 的 `kms_write_flush`：stdout/stderr 的 `write_fn` 改为写 KMS_REQ + 通知 KMS 驱动

## libc stdout 路径

- libc 的 `kms_write_flush` 替代原 `sys_putc_flush`：stdout/stderr 的 `write_fn` 改为向 KMS_REQ 共享页写入 PUTC 命令 + `sys_notify(KMS_DRIVER_PID)`
- stdout 和 stderr 均为 `_IONBF`（无缓冲），每次 `putchar`/`printf` 都直接写 KMS_REQ 并通知
- 缓冲满 255 条命令时中途 notify + yield，防止溢出

## Framebuffer 映射细节

- 映射地址：`0x700000`（用户态固定地址）
- 映射方式：4KB 页（`map_user_pages`），不加 PCD/PAT
- 物理页来源：`init_fb` 保存的 `fb.phys_addr` + `fb.size`
- 映射时机：`process_create_elf` 创建 KMS 进程时

## 内核启动阶段输出

- `init_fb` 前后均走串口输出（`serial_putc`/`serial_puts`）
- KMS 驱动启动前屏幕为黑，串口有输出
- 内核 panic 走串口

## 将来扩展

- **Compositor 接管**：compositor 取代 shell 成为 KMS 唯一客户端，自行渲染像素到共享 buffer，KMS 只做 page flip
- **VSync / VBLANK**：KMS 绑定 IRQ（显卡中断），compositor 同步 page flip
- **多客户端**：KMS 支持多个客户端连接（当前一对一）
- **驱动工作流重构**：统一驱动 IPC 机制，KMS 作为重构的一部分
- **PAT / write-combining**：framebuffer 页映射加 PCD/PAT 标记，优化真机性能
- **Huge page 映射**：用户态 framebuffer 映射改用 2MB huge page，减少 TLB miss

## 实现状态

全部功能已实现，KMS 驱动正常运行。

### 实现偏差

| 设计 | 实际 | 原因 |
|------|------|------|
| KMS_INFO 地址 0x507000 | 0x508000 | DISK_REQ/RESP 各扩展为 2 页后地址顺延 |
| KMS_REQ 地址 0x508000 | 0x509000 | 同上 |
| 共享页 7 页 | 10 页 | KMS_INFO + KMS_REQ + DISK_REQ2/DISK_RESP2/FS_RESP2 |
| KMS 驱动主循环：`sys_wait()` → 处理 | 先处理 → 仅 `count==0` 时 `sys_wait()` | `sys_notify` 只能唤醒 BLOCKED 进程，先等待会丢失通知 |
| shell 独立 `kms_putc`/`kms_flush` | libc `kms_write_flush` 作为 stdout write_fn | 所有用户程序（含 hello.elf）都通过 libc 输出，统一路径 |
| shell 手动链接 | shell 使用 libc.a | 统一构建，减少重复代码 |

### 已修复的 Bug

| Bug | 根因 | 修复 |
|-----|------|------|
| Shell 启动时 `>` 提示符不立刻显示 | KMS 主循环先 `sys_wait()` 再处理，早到的通知丢失 | 改为先 `process_commands()` 再在 `count==0` 时 `sys_wait()` |
| `run hello.elf` 后卡住 | `sys_exit` → `sys_notify(parent_pid)` 只匹配 `WAIT_NOTIFY`，但 `sys_waitpid` 设的是 `WAIT_CHILD` | `sys_notify` 同时匹配 `WAIT_NOTIFY` 和 `WAIT_CHILD` |
| `run hello.elf` 后 #GP 崩溃 | `proc_reap` 用 `pte & ~0xFFF` 提取物理地址，未清除 bit 63（NX 位），导致 `PHY_TO_PAGE` 越界访问 `frames[]` | PTE 物理地址提取改用 `pte & 0x000FFFFFFFFFF000` |
