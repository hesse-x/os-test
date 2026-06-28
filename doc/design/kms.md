# KMS 内核态显示驱动

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | KMS 在内核还是用户态 | 内核 | 与 Linux DRM/KMS 模型一致；flip 是同步 memcpy，放内核消除上下文切换开销 |
| 2 | 完整 DRM vs 最小 KMS | 最小 KMS（buffer 创建 + flip） | 当前只需 scanout，不需要 CRTC/Plane/Connector/GEM/fence/多客户端仲裁 |
| 3 | 谁分配 back buffer | 内核 | 与 Linux DRM 一致：内核了解硬件约束、拥有 buffer 生命周期；compositor 通过 ioctl 请求分配 |
| 4 | flip 机制 | 同步 memcpy back→front | 简单正确；异步 flip 需要显卡 IRQ + vblank 机制，当前不存在 |
| 5 | 分辨率 | 硬编码 800×600×32 | QEMU bochs-display 只验证此值；可变分辨率留待扩展 |
| 6 | 显卡发现方式 | PCI vendor:device ID（0x1234:0x1111） | bochs-display class_code=0x0380（Display/Other），不是 0x0300（VGA Compatible），不能用 class 查找 |
| 7 | BAR0 缓存属性 | framebuffer 用 WC（write-combining） | `pci_enable_device_wc` 指定 BAR；WC 允许写合并成 burst，真机 flip 性能好于 UC |
| 8 | 内核设备分发路径 | ioctl（非 sys_req） | `/dev/kms` 注册为内核设备（`driver_pid=0`），`sys_ioctl` 直接调 `ops->ioctl`，无需 IPC 代理 |

### 核心数据结构

display_state（kernel/display.h : display_state）
  front_fb : uint8_t __iomem* — front buffer MMIO 地址（PCI BAR1）
  back_buffer : uint8_t* — back buffer 内核虚拟地址
  back_buffer_phys : uint64_t — back buffer 物理地址
  back_buffer_npages : uint64_t — back buffer 页数
  vbe_mmio : uint16_t __iomem* — BAR0 VBE DISPI 寄存器基址
  pci_dev : pci_device* — PCI 设备引用
  fb_width/height/pitch/bpp/size : uint32_t — 当前 mode
  initialized : bool — back buffer 是否已分配

全局实例：`g_display`（kernel/display.c）

### 与 Linux DRM 的对应关系

| Linux DRM | 本 OS | 说明 |
|-----------|-------|------|
| `/dev/dri/card0` | `/dev/kms` | devtmpfs 设备节点 |
| `DRM_IOCTL_MODE_CREATE_DUMB` | `ioctl(fd, KMS_IOCTL_CREATE_BUF)` | compositor 指定分辨率，内核分配 buffer |
| `DRM_IOCTL_MODE_MAP_DUMB` | `mmap(fd, size)` | 映射纯像素 buffer |
| `DRM_IOCTL_MODE_PAGE_FLIP` | `ioctl(fd, KMS_IOCTL_FLIP)` | 同步 flip（未来扩展异步+vblank） |
| 内核 DRM driver 分配 buffer | 内核 display.c 分配 | 物理页分配，compositor mmap |
| 内核 CRTC page flip | 内核 memcpy back→front | 当前软件 flip |

### ioctl 协议

KMS ioctl 常量定义在 common/ioctl.h：

- `KMS_IOCTL_CREATE_BUF` — `_IOWR('K', 1, char[32])`，统一 32B 结构体
- `KMS_IOCTL_FLIP` — `_IOWR('K', 2, char[8])`，dirty-rect 扩展后 8B 结构体（见下文优化项）

display_ioctl_create_buf_arg（common/display.h : display_ioctl_create_buf_arg）
  width/height/bpp : uint32_t — compositor 输入（当前只接受 800/600/32）
  pitch/size/rows/cols : uint32_t — 内核输出
  result : int32_t — 0=成功, -EINVAL=不支持分辨率

### 设备注册与分发路径

`/dev/kms` 通过 devtmpfs 注册为内核设备：

- kernel/display.c : `display_dev_register()` 调用 `devtmpfs_create("kms", DEV_KMS, &kms_dev_ops)`
- `kms_dev_ops.driver_pid = 0`（内核设备，不走 IPC 代理）
- `kms_dev_ops.ioctl = display_ioctl`，`kms_dev_ops.mmap = display_mmap_handler_ioctl`

分发路径：
1. 用户 `ioctl(fd, cmd, arg)` → `sys_ioctl` 发现 FD_DEV 且 `ops->driver_pid == 0`
2. 直接调 `ops->ioctl(cmd, arg)` → `display_ioctl` 在内核同步处理
3. 用户 `mmap(fd, size)` → `sys_mmap` 发现内核设备 → `display_mmap_handler_ioctl` 映射 back buffer 物理页

### 关键流程

**display_init**（kernel/display.c : display_init）— PCI 发现 + VBE modeset：

1. `pci_find_device_by_id(0x1234, 0x1111)` 查找 bochs-display
2. `pci_enable_device_wc(dev, fb_bar_idx)` — 映射所有 BAR，framebuffer BAR 用 WC
3. 动态识别 BAR：最大 MMIO = framebuffer，小 BAR 且含 VBE DISPI ID（0xB0C5）= VBE 寄存器
4. VBE modeset 800×600×32：先禁用 → 写 XRES/YRES/BPP → 启用（ENABLED | LFB_ENABLED）
5. 填充 `g_display`

VBE MMIO 寄存器通过 BAR0 + 偏移 0x500 + index×2 访问（`VBE_DISPI_MMIO_OFFSET` 宏），必须用 `uint8_t __iomem*` 基址 + 字节偏移再 cast，不能直接用 `uint16_t*` 数组索引（偏移会翻倍）。

**CREATE_BUF 流程**（kernel/display.c : display_ioctl）：

1. 验证 width=800, height=600, bpp=32
2. `alloc_contiguous_pages(fb_size)` 分配 back buffer
3. 填充 pitch/size/rows/cols 到 arg
4. 设 `g_display.initialized = true`

**FLIP 流程**（kernel/display.c : display_ioctl）：

1. 检查 `g_display.initialized`
2. `memcpy(front_fb, back_buffer, fb_size)` — 同步复制

**mmap 流程**（kernel/display.c : display_mmap_handler_ioctl）：

1. 检查 `g_display.initialized`
2. 将 back buffer 物理页映射到用户 PML4，创建 `mmap_region_t`

### IPC 拓扑

```
kbd_driver(PID3) ── SHM + ioctl ──→ terminal(PID5) ── pipe ──→ shell(PID6)
                                        │
                                        │ open("/dev/kms") → fd
                                        │ ioctl(fd, CREATE_BUF) → back buffer 元信息
                                        │ mmap(fd) → back buffer 映射
                                        │ ioctl(fd, FLIP) → 内核 flip
                                        ▼
                                 内核 KMS (display.c)
                                        │
                                        │ PCI BAR1 front buffer
                                        │ 同步 memcpy
                                        ▼
                                 bochs-display (PCI 1234:1111)
```

### 启动时序

kernel/kernel.c : kernel_main 中调用顺序：

```
pci_init() → display_init() → ahci_init() → vfs_init()（含 display_dev_register）
```

`display_init()` 在 `pci_init()` 之后，与 AHCI/xHCI 模式一致（PCI 只枚举，各驱动自己 init）。启动期间（display_init 前）只有串口输出，无 framebuffer。

init/init.c 只 spawn kbd_driver 和 terminal，不 spawn KMS 进程。`/dev/kms` 由内核在 vfs_init 时注册。

### Terminal 集成

driver/display.h 提供 client API：

- `display_client_init()` — `open("/dev/kms")` + `ioctl(fd, KMS_IOCTL_CREATE_BUF)` + `mmap(fd, size)`
- `display_client_render_cell(row, col, ch, fg, bg)` — 像素渲染到 back buffer
- `display_client_clear(bg)` / `display_client_scroll_up(bg)` / `display_client_set_cursor(x, y)`
- `display_client_flush()` — `ioctl(fd, KMS_IOCTL_FLIP)` 请求内核 flip

Terminal 调用 `display_client_init()` 初始化，渲染到 mmap'd back buffer，flush 时请求内核 flip。光标在 back buffer 上渲染（反色块），内核不知道光标。

### MMIO 访问 helpers

arch/x64/utils.h 提供：
- `mmio_read16(addr)` / `mmio_write16(addr, val)` — 16-bit MMIO 读写
- `readl(addr)` / `writel(addr, val)` — 32-bit MMIO 读写

### 组件清单

| 文件 | 角色 |
|------|------|
| kernel/display.c + kernel/display.h | 内核 KMS 子系统（PCI 发现 + VBE modeset + back buffer 分配 + flip + mmap） |
| common/display.h | ioctl 常量 + 结构体定义 |
| common/ioctl.h | KMS_IOCTL_CREATE_BUF / KMS_IOCTL_FLIP 命令编码 |
| driver/display.h | Client API（compositor 侧 inline 函数） |
| driver/font.h | 8x16 字体表 |

### 与其他模块的关系

- **PCIe**：display_init 依赖 `pci_find_device_by_id` 和 `pci_enable_device_wc`。详见 [pcie.md](pcie.md)
- **VFS/devtmpfs**：`/dev/kms` 通过 devtmpfs 注册，FD_DEV 分发路径。详见 [vfs.md](vfs.md)
- **Terminal**：作为 compositor 通过 `/dev/kms` fd 与内核 KMS 交互。详见 [terminal.md](terminal.md)
- **IPC**：内核设备走 ioctl 直调，不走 sys_req IPC 代理。详见 [ipc.md](ipc.md)

---

## 性能优化方案

### 1 优化背景与目标

#### 1.1 当前瓶颈分析

Terminal 主循环每轮执行：`read(master_fd, buf, 256)` → `vt100_feed` 逐字节 → `flush_dirty_cells()` → `display_client_flush()`。瓶颈有两个维度：

**瓶颈 A：FLIP 是全帧 memcpy**

`display_ioctl` FLIP 路径（display.c:177）无条件执行 `memcpy(front_fb, back_buffer, fb_size)`，复制整 1.92MB（800×600×4），即使只变化了 1 行。Terminal 的 dirty tracking 仅在用户态决定"哪些行需要重绘到 back buffer"，内核侧无法利用此信息。

**瓶颈 B：read 缓冲区过小**

Terminal 的 read 缓冲区仅 256 字节（terminal.cc:500 `char buf[256]`）。测试输出大量文本时，每轮循环只读 256 字节就触发一次 flip。假设输出 10KB：~40 轮循环 × 1.92MB memcpy = ~77MB 无效拷贝。

**瓶颈 C：每轮循环都调 flush**

`flush_dirty_cells()` 在主循环末尾无条件调用（terminal.cc:529）。虽然有 dirty_row_start/end 守卫（无变化时跳过），但只要 `read` 返回了数据，就一定有脏行，就一定触发 flip。

#### 1.2 性能量化基线与目标

| 指标 | 当前值 | 优化目标（Phase 1） | 优化目标（Phase 2） |
|------|--------|-------------------|-------------------|
| 单次 FLIP 拷贝量 | 1.92MB（全帧） | ~51KB（单行脏） | 0（寄存器写入） |
| 10KB 输出 flip 次数 | ~40 | ~3 | ~3 |
| 10KB 输出总拷贝量 | ~77MB | ~150KB | 0 |
| 全屏刷新拷贝量 | 1.92MB | 1.92MB | 0（寄存器写入） |

### 2 优化项一：行级 Dirty-Rect FLIP

#### 2.1 设计原理

将 terminal 已有的 dirty_row_start / dirty_row_end 传递给内核 FLIP，内核只拷贝脏行范围内的像素数据，而非整帧。

**关键约束**：dirty 粒度为行级（与 terminal 现有 dirty tracking 一致），不引入更细的列级或像素级 dirty。理由：
- Terminal 的 cell buffer 天然按行组织，行级 dirty 已充分减少拷贝
- 列级 dirty 需要内核逐行逐列跳读，memcpy 无法对齐连续内存，收益不及复杂度
- 行级 dirty 对 memcpy 友好：连续 `pitch × dirty_rows` 字节

#### 2.2 接口变更

**common/ioctl.h** — FLIP 命令编码变更：

```c
// 旧: KMS_IOCTL_FLIP  _IO('K', 2)          — 无参数
// 新: KMS_IOCTL_FLIP  _IOWR('K', 2, char[8]) — 8B 参数
```

命令号编码包含 size 字段，`_IO('K', 2)` = `0x00004B02`，`_IOWR('K', 2, char[8])` = `0x20084B02`，编码不同。但 `sys_ioctl` 通过 `_IOC_NR(cmd)` 分发，nr 均为 2，不受影响。内核 `display_ioctl` 同时匹配新旧编码：

```c
bool is_flip = (cmd == KMS_IOCTL_FLIP || cmd == DISPLAY_REQ_FLIP);
```

改为：

```c
bool is_flip = ((_IOC_NR(cmd) == 2) || cmd == DISPLAY_REQ_FLIP);
```

**common/display.h** — 新增 FLIP 参数结构体：

```c
// FLIP argument (8 bytes)
struct display_ioctl_flip_arg {
    uint32_t dirty_row_start;  // 脏行起始（含），= rows 表示全帧
    uint32_t dirty_row_end;    // 脏行结束（不含），= 0 表示全帧
};
```

**兼容策略**：dirty_row_start >= dirty_row_end 时退化为全帧拷贝（向后兼容旧客户端传 0/0 或不传参数的场景）。

#### 2.3 内核实现

**kernel/display.h** — display_state 新增字段：

```c
struct display_state {
    // ... existing fields ...
    uint32_t fb_rows;          // 总行数 = fb_height / FONT_HEIGHT
};
```

**kernel/display.c** — display_ioctl FLIP 路径改造：

```c
// FLIP path
if (is_flip) {
    if (!g_display.initialized)
        return -ENOENT;

    struct display_ioctl_flip_arg *farg = (struct display_ioctl_flip_arg *)arg;
    uint32_t row_start = farg->dirty_row_start;
    uint32_t row_end   = farg->dirty_row_end;
    uint32_t rows      = g_display.fb_rows;
    uint32_t pitch     = g_display.fb_pitch;

    // 全帧退化和安全边界
    if (row_start >= row_end || row_end > rows) {
        row_start = 0;
        row_end = rows;
    }

    if (row_start == 0 && row_end == rows) {
        // 全帧拷贝（含用户态 clear/scroll 场景）
        __memcpy((void __force *)g_display.front_fb,
                 g_display.back_buffer, g_display.fb_size);
    } else {
        // 行级拷贝
        uint32_t y_start = row_start * FONT_HEIGHT;
        uint32_t y_end   = row_end * FONT_HEIGHT;
        uint32_t copy_bytes = (y_end - y_start) * pitch;
        __memcpy((void __force *)(g_display.front_fb + y_start * pitch),
                 g_display.back_buffer + y_start * pitch,
                 copy_bytes);
    }
    return 0;
}
```

**边界安全**：
- `row_end > rows` 时退化为全帧（防御畸形参数）
- `y_end` 上限受 `row_end <= rows` 约束，不会越界 `fb_height`
- 内核侧使用 `FONT_HEIGHT`（来自 common/font_metrics.h）将行号转为像素行

#### 2.4 Terminal 适配

**driver/display.h** — display_client_flush 签名变更：

```c
// 旧: static inline void display_client_flush()
// 新:
static inline void display_client_flush(uint32_t dirty_row_start,
                                         uint32_t dirty_row_end) {
    struct display_ioctl_flip_arg arg;
    arg.dirty_row_start = dirty_row_start;
    arg.dirty_row_end   = dirty_row_end;
    ioctl(display_dev_fd, KMS_IOCTL_FLIP, &arg);
}
```

**driver/terminal.cc** — flush_dirty_cells 传递 dirty 范围：

```c
static void flush_dirty_cells() {
    if (dirty_row_start >= dirty_row_end) return;

    for (int row = dirty_row_start; row < dirty_row_end; row++) {
        for (int col = 0; col < vt.cols; col++) {
            struct cell *c = &cells[row * vt.cols + col];
            display_client_render_cell(row, col, c->ch, c->fg_color, c->bg_color);
        }
    }

    display_client_set_cursor(vt.cursor_x, vt.cursor_y);

    int rs = dirty_row_start;
    int re = dirty_row_end;
    dirty_row_start = vt.rows;
    dirty_row_end = 0;

    display_client_flush(rs, re);  // 传 dirty 范围
}
```

**scroll / clear 场景**：terminal 的 `cell_putc` scroll 和 `\033[2J` clear 已经将 `dirty_row_start=0, dirty_row_end=vt.rows`，自然走全帧拷贝路径，无需特殊处理。

#### 2.5 性能量化

| 场景 | 当前拷贝量 | 优化后拷贝量 | 倍数 |
|------|-----------|-------------|------|
| 单字符输入（1 行脏） | 1.92MB | 51.2KB（1×16×3200） | 37x |
| ls 输出 20 行 | 1.92MB × 20 | 1.024MB（20×51.2KB） | ~1.9x |
| 全屏 scroll | 1.92MB | 1.92MB | 1x（无改善） |
| clear screen | 1.92MB | 1.92MB | 1x（无改善） |

增量更新场景（键盘输入、逐行输出）收益显著；全屏场景无改善，需硬件 page-flip 解决。

### 3 优化项二：Terminal 读取缓冲区扩大

#### 3.1 设计原理

将 terminal 主循环的 `read` 缓冲区从 256 字节扩大到 4096 字节，减少主循环迭代次数和 flip 频率。

256 字节意味着每次 `read` 最多处理约 3-4 行文本（每行 ~70 字符 + newline），每次 read 后触发一次 flip。扩大到 4096 字节后，一次 read 可处理约 50 行文本，dirty tracking 将这些行合并到一个 dirty range，一次 flip 完成。

#### 3.2 实现变更

**driver/terminal.cc:500** — 缓冲区扩大：

```c
// 旧: char buf[256];
// 新:
char buf[4096];
int64_t n = read(master_fd, buf, sizeof(buf));
```

仅此一处改动，无接口变更。

#### 3.3 性能量化

| 指标 | 256B buffer | 4096B buffer |
|------|------------|-------------|
| 10KB 输出循环次数 | ~40 | ~3 |
| 10KB 输出 flip 次数 | ~40 | ~3 |
| 10KB 输出总拷贝量（无 dirty-rect） | ~77MB | ~5.8MB |
| 10KB 输出总拷贝量（有 dirty-rect） | ~1.5MB | ~150KB |

read 缓冲区扩大与 dirty-rect 优化叠加效果：大缓冲区让更多行合并到同一次 flip，dirty-rect 让单次 flip 只拷贝变化行。

#### 3.4 约束

4096 字节不会导致延迟感知问题：terminal 主循环是无阻塞 busy loop，`read` 在 O_NONBLOCK 下立即返回已可用数据（最多 4096 字节），不会等待缓冲区填满。

### 4 优化项三：硬件 Page-Flip（VBE Y_OFFSET）

#### 4.1 bochs-display 硬件原理

QEMU bochs-display 实现 VBE DISPI 接口，支持虚拟帧缓冲大于可见区域，通过 `Y_OFFSET` 寄存器选择硬件扫描起始行：

| 寄存器 | 作用 | 当前值 | 优化后值 |
|--------|------|--------|---------|
| `VBE_DISPI_INDEX_VIRT_HEIGHT` | 虚拟帧缓冲总行数 | 未设（=YRES=600） | 1200 |
| `VBE_DISPI_INDEX_Y_OFFSET` | 硬件扫描起始行 | 0 | 0 或 600 |

原理：设 `VIRT_HEIGHT=1200` 后，framebuffer BAR 空间逻辑上分为上下两个 600 行区域。硬件 CRTC 从 `Y_OFFSET` 行开始扫描输出。写 `Y_OFFSET` 寄存器即可瞬间切换显示内容，无需拷贝像素数据。

**BAR 空间验证**：QEMU bochs-display 默认 framebuffer BAR 为 16MB，当前 800×600×32 = 1.92MB，VIRT_HEIGHT=1200 后需 3.84MB，空间充裕。

#### 4.2 虚拟帧缓冲布局

```
framebuffer BAR (WC mapping)
┌─────────────────────────┐ Y_OFFSET=0 时硬件扫描此区域
│  scan buffer (0-599行)   │ ← front_fb = fb_vaddr
│  800×600×4 = 1.92MB     │
├─────────────────────────┤
│  render buffer (600-1199行) │ ← back_buffer = fb_vaddr + 1.92MB
│  800×600×4 = 1.92MB     │    用户态 mmap 映射此区域
└─────────────────────────┘
```

**flip 操作**：写 `Y_OFFSET = 600` → 硬件立即从第 600 行开始扫描 → render buffer 变为 front。
再次 flip 时写 `Y_OFFSET = 0`，依此交替。

#### 4.3 缓冲区同步策略

硬件 page-flip 后，原 front buffer（上一次的 scan buffer）变为可用 render buffer，但其中保存的是**上一帧的像素**。下一帧渲染前需要确保：

| 策略 | 做法 | 优点 | 缺点 |
|------|------|------|------|
| A: render-all | terminal 每帧重绘所有脏行到新 render buffer | 简单，与现有 dirty tracking 天然兼容 | 无 |
| B: copy-after-flip | flip 后先 memcpy front→back（拷旧帧），再只渲染增量 | flip 后 dirty 可增量 | 首帧多一次全帧拷贝，复杂度高 |
| C: render-to-both | 渲染时同时写两个 buffer | flip 后无需拷贝 | 写带宽翻倍，破坏 WC 合并 |

**推荐策略 A（render-all）**：terminal 已有完整的 cell buffer + dirty tracking，每次 `flush_dirty_cells()` 都会重绘所有脏行到 back buffer。硬件 flip 后 render buffer 切换，下一帧脏行自然写入新 render buffer。不需要任何额外同步逻辑。

**特例处理**：

- **scroll**：`display_client_scroll_up` 需要在新 render buffer 上执行。当前 scroll 实现是逐字节搬移（display.h:94-108），直接操作 `display_back_buffer` 指针。硬件 flip 后此指针自动指向新 render buffer，scroll 行为正确。但 scroll 后新 render buffer 上半部分是旧数据（上一帧 front buffer 的像素），而 cell buffer 已被 shift。解决方案：scroll 后设 `dirty_row_start=0, dirty_row_end=rows`，全量重绘，确保 render buffer 与 cell buffer 一致。
- **clear**：`display_client_clear` 直接填充整个 back buffer，无需特殊处理。

#### 4.4 接口变更

**common/display.h** — CREATE_BUF 返回新增字段：

```c
struct display_ioctl_create_buf_arg {
    // input (unchanged)
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    // output (extended)
    uint32_t pitch;
    uint32_t size;
    uint32_t rows;
    uint32_t cols;
    int32_t  result;
    uint32_t render_offset;   // 新增：render buffer 在 framebuffer 中的字节偏移
    uint32_t _pad;            // 保持 32B 对齐
};
```

结构体总大小从 28B 扩展到 32B，仍在 `char[32]` ioctl 编码范围内。

**common/ioctl.h** — FLIP 返回新增字段：

```c
struct display_ioctl_flip_arg {
    uint32_t dirty_row_start;
    uint32_t dirty_row_end;
};
// 硬件 flip 后内核返回当前 Y_OFFSET，用户态据此计算 render buffer 地址
// 扩展为 16B:
struct display_ioctl_flip_arg {
    uint32_t dirty_row_start;   // input: 脏行起始
    uint32_t dirty_row_end;     // input: 脏行结束
    uint32_t current_offset;    // output: 当前 scan offset（字节）
    uint32_t _pad;
};
```

ioctl 编码相应更新：`_IOWR('K', 2, char[16])`。

#### 4.5 内核实现

**kernel/display.h** — display_state 改造：

```c
struct display_state {
    uint8_t __iomem *fb_vaddr;       // framebuffer BAR 完整虚拟地址
    uint16_t __iomem *vbe_mmio;      // VBE DISPI 寄存器基址
    struct pci_device *pci_dev;
    uint32_t fb_width;               // 800
    uint32_t fb_height;              // 600（可见高度）
    uint32_t fb_pitch;               // 3200
    uint32_t fb_bpp;                 // 32
    uint32_t fb_size;                // 1.92MB（单 buffer 大小）
    uint32_t fb_rows;                // 37
    uint32_t current_y_offset;       // 当前 Y_OFFSET（0 或 fb_height）
    bool     hw_flip;                // 硬件 flip 模式
    bool     initialized;

    // 软件回退路径（hw_flip=false 时使用）
    uint8_t  *back_buffer;
    uint64_t  back_buffer_phys;
    uint64_t  back_buffer_npages;
};
```

**kernel/display.c** — display_init modeset 变更：

```c
// 在 VBE modeset 中增加:
mmio_write16(VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_VIRT_HEIGHT), 1200);
g_display.hw_flip = true;
g_display.fb_rows = g_display.fb_height / FONT_HEIGHT;
```

**kernel/display.c** — display_ioctl FLIP 路径：

```c
if (is_flip) {
    if (!g_display.initialized) return -ENOENT;

    struct display_ioctl_flip_arg *farg = (struct display_ioctl_flip_arg *)arg;
    uint32_t row_start = farg->dirty_row_start;
    uint32_t row_end   = farg->dirty_row_end;
    uint32_t rows      = g_display.fb_rows;

    if (row_start >= row_end || row_end > rows) {
        row_start = 0;
        row_end = rows;
    }

    if (g_display.hw_flip) {
        // 硬件 page-flip：切换 Y_OFFSET
        uint32_t new_y = (g_display.current_y_offset == 0) ? g_display.fb_height : 0;
        uint8_t __iomem *mmio = (uint8_t __iomem *)g_display.vbe_mmio;
        mmio_write16((uint16_t __iomem *)(mmio + VBE_DISPI_MMIO_OFFSET(VBE_DISPI_INDEX_Y_OFFSET)),
                     (uint16_t)new_y);
        g_display.current_y_offset = new_y;
        farg->current_offset = new_y * g_display.fb_pitch;
    } else {
        // 软件回退：dirty-rect memcpy
        if (row_start == 0 && row_end == rows) {
            __memcpy((void __force *)g_display.fb_vaddr,
                     g_display.back_buffer, g_display.fb_size);
        } else {
            uint32_t y_start = row_start * FONT_HEIGHT;
            uint32_t y_end   = row_end * FONT_HEIGHT;
            __memcpy((void __force *)(g_display.fb_vaddr + y_start * g_display.fb_pitch),
                     g_display.back_buffer + y_start * g_display.fb_pitch,
                     (y_end - y_start) * g_display.fb_pitch);
        }
        farg->current_offset = 0;
    }
    return 0;
}
```

**kernel/display.c** — display_mmap_handler 改造：

```c
uint64_t display_mmap_handler(struct task_t *proc, size_t size) {
    if (!g_display.initialized) return 0;

    if (g_display.hw_flip) {
        // 硬件 flip 模式：映射 framebuffer BAR 的非扫描半区
        uint32_t render_offset = (g_display.current_y_offset == 0)
                                 ? g_display.fb_size    // 扫描区=0, 渲染区=后半
                                 : 0;                   // 扫描区=后半, 渲染区=前半
        // 计算 BAR 物理地址偏移
        uint64_t bar_phys = g_display.pci_dev->bar[fb_bar_idx].phys + render_offset;
        uint64_t vaddr = proc->mm->mmap_brk;
        uint64_t *pml4 = phys_to_virt(proc->mm->cr3);
        size_t npages = g_display.fb_size / PAGE_SIZE;
        uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX | PTE_PCD;

        for (size_t i = 0; i < npages; i++) {
            if (!map_user_page_direct(pml4, vaddr + i * PAGE_SIZE,
                                      bar_phys + i * PAGE_SIZE, pte_flags))
                // cleanup and return 0 ...
        }
        // mmap_region_t 记录 ...
        return vaddr;
    } else {
        // 软件回退：映射 back_buffer 物理页（原有逻辑）
        // ...
    }
}
```

**PTE 标志注意**：BAR 空间用户态映射应加 `PTE_PCD`（Cache Disable）或使用 WC PAT 模式。用户态写入 WC 映射的 BAR 空间时，CPU 写合并（write-combining）合并连续写为 burst 事务，渲染性能远优于 UC。

#### 4.6 用户态适配

**driver/display.h** — display_client_flush 改造：

```c
static uint32_t display_render_offset;  // render buffer 在 framebuffer 中的偏移

static inline void display_client_flush(uint32_t dirty_row_start,
                                         uint32_t dirty_row_end) {
    struct display_ioctl_flip_arg arg;
    arg.dirty_row_start = dirty_row_start;
    arg.dirty_row_end   = dirty_row_end;
    ioctl(display_dev_fd, KMS_IOCTL_FLIP, &arg);
    // flip 后内核返回当前 scan offset，render buffer 切换
    display_render_offset = arg.current_offset;
}
```

**mmap 时机问题**：当前 `display_client_init` 在 CREATE_BUF 后立即 mmap，此时已知 render_offset。硬件 flip 后 render buffer 地址在 BAR 内偏移，但用户态 mmap 是一次性映射。解决方案：

- **方案 1（推荐）**：mmap 映射整个 framebuffer BAR 的两倍区域（3.84MB），用户态通过 `display_back_buffer + offset` 切换 render buffer 地址。无需重新 mmap。
- **方案 2**：每次 flip 后 `munmap` + `mmap` 重新映射新 render buffer。开销大，不推荐。

```c
static inline int display_client_init() {
    // ... open, ioctl CREATE_BUF ...

    // mmap 整个双缓冲区域
    void *buf = mmap(NULL, arg.size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) return -1;

    // 初始 render buffer 在后半区
    display_render_offset = arg.render_offset;
    display_back_buffer = (uint8_t *)buf + display_render_offset;
    return 0;
}

static inline void display_client_flush(uint32_t dirty_row_start,
                                         uint32_t dirty_row_end) {
    struct display_ioctl_flip_arg arg;
    arg.dirty_row_start = dirty_row_start;
    arg.dirty_row_end   = dirty_row_end;
    ioctl(display_dev_fd, KMS_IOCTL_FLIP, &arg);

    // flip 后切换 render buffer 地址
    display_back_buffer = (uint8_t *)display_back_buffer_base + arg.current_offset;
}
```

**display_client_scroll_up 适配**：硬件 flip 后 scroll 需要操作新 render buffer。由于 `display_back_buffer` 指针在 flush 后已切换，scroll 自动操作正确区域。但 scroll 后需全量重绘（见 4.3 策略 A），terminal 的 scroll 逻辑已设 `dirty_row_start=0, dirty_row_end=vt.rows`，满足要求。

#### 4.7 适用场景分析

| 场景 | 硬件 page-flip 收益 | dirty-rect 叠加收益 |
|------|--------------------|--------------------|
| 单字符输入 | 极高（0 拷贝 vs 1.92MB） | 无（硬件 flip 已零拷贝） |
| ls 输出 20 行 | 极高 | 无 |
| 全屏 scroll | 极高（0 拷贝 vs 1.92MB） | 无 |
| clear screen | 极高 | 无 |

硬件 page-flip 在所有场景下都是零拷贝，dirty-rect 优化在硬件 flip 模式下无额外收益。但 dirty-rect 在软件回退路径下仍然有效，且实现简单，建议保留。

### 5 实施顺序与依赖关系

```
Phase 1（立即可做，收益最大）：
  ├── 优化项二：read buffer 256→4096（1 行改动，独立）
  └── 优化项一：dirty-rect FLIP（接口+内核+terminal，3 文件改动）

Phase 2（依赖 Phase 1 的接口扩展，全屏场景收益显著）：
  └── 优化项三：硬件 page-flip（display_init+mmap+flip 改造，需 QEMU 验证）
```

**Phase 1 独立可部署**：dirty-rect + 扩大 read buffer 即可解决增量更新场景的性能问题。

**Phase 2 前置验证**：实施前需在 QEMU 中验证 `VBE_DISPI_INDEX_VIRT_HEIGHT` + `Y_OFFSET` 切换行为（见 6.1）。

**实施清单**：

| 阶段 | 文件 | 改动 |
|------|------|------|
| P1 | common/display.h | 新增 `display_ioctl_flip_arg` |
| P1 | common/ioctl.h | `KMS_IOCTL_FLIP` 编码改为 `_IOWR('K', 2, char[8])` |
| P1 | kernel/display.h | 新增 `fb_rows` 字段 |
| P1 | kernel/display.c | FLIP 路径行级拷贝，nr 匹配逻辑 |
| P1 | driver/display.h | `display_client_flush(dirty_row_start, dirty_row_end)` |
| P1 | driver/terminal.cc | flush 传 dirty 范围 + `buf[4096]` |
| P2 | kernel/display.h | display_state 增加 hw_flip/current_y_offset，移除 back_buffer |
| P2 | kernel/display.c | display_init VIRT_HEIGHT=1200，FLIP 硬件切换，mmap BAR 偏移 |
| P2 | driver/display.h | init mmap 双倍区域 + flush 切换 buffer 指针 |
| P2 | common/display.h | CREATE_BUF 返回 render_offset |

### 6 风险与兼容性

#### 6.1 QEMU bochs-display Y_OFFSET 行为验证

VBE DISPI Y_OFFSET 是 QEMU bochs-display 的文档化功能，但需验证：
- Y_OFFSET 切换是否在同一帧内生效（vs 下一帧）
- VIRT_HEIGHT=1200 后 BAR 空间是否完整可写
- Y_OFFSET 非 0 对齐时是否有硬件约束

**验证方法**：在 display_init 中临时加 VIRT_HEIGHT=1200 + Y_OFFSET=600，观察 QEMU 窗口是否显示后半段内容。

#### 6.2 ioctl 编码兼容性

FLIP 命令从 `_IO('K', 2)` 改为 `_IOWR('K', 2, char[8])`，编码值变化。`sys_ioctl` 通过 `_IOC_NR` 提取命令号分发，nr 不变（=2），分发逻辑不受影响。但 `sys_ioctl` 的 copy_from_user/copy_to_user 逻辑需正确处理 size=0（旧）和 size=8（新）两种情况。

**内核侧兼容**：`display_ioctl` 通过 `_IOC_NR(cmd) == 2` 匹配 FLIP，`_IOC_SIZE(cmd)` 为 0 时（旧客户端无参数）退化为全帧拷贝。

#### 6.3 BAR 空间用户态映射属性

当前 mmap 映射的 back_buffer 物理页使用 `PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX`。硬件 flip 模式下映射 BAR 空间，需考虑：

- **WC 映射**：理想情况下用户态映射也应 WC，但 WC 需 PAT MSR 配置。当前 `pci_enable_device_wc` 仅对内核侧 PTE 设置 WC。
- **UC 映射**（`PTE_PCD`）：安全回退，写性能略低于 WC 但远优于普通缓存映射（避免 cache line 读分配开销）。
- **推荐**：Phase 2 先用 UC（`PTE_PCD`），WC PAT 作为后续优化。

#### 6.4 软件回退

硬件 flip 模式依赖 QEMU bochs-display。未来支持真实 GPU（如 AMD Oland）时，若无 VBE Y_OFFSET 机制，应自动回退到软件 memcpy + dirty-rect 路径。`display_state.hw_flip` 标志控制切换。

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| ~~行级 dirty / damage region~~ | FLIP 附带 dirty 行范围参数，内核只拷 dirty 部分 | ~~低~~ → **Phase 1 已设计** |
| ~~硬件 page flip~~ | 写 Y_OFFSET 寄存器切换扫描地址替代 memcpy，零拷贝 | ~~低~~ → **Phase 2 已设计** |
| 异步 flip + VSync/VBLANK | ioctl(FLIP) 立即返回，内核 deferred flip（下次 vblank 时）；需要显卡 IRQ + vblank callback 机制 | 中 |
| 多 buffer（double/triple buffering） | CREATE_BUF 支持创建多个 dumb buffer，MAP_BUF handle→offset | 中 |
| 可变分辨率 | CREATE_BUF 验证改为查 mode 列表，支持 compositor 请求切换分辨率 | 中 |
| 24bpp 支持 | CREATE_BUF bpp 参数支持 24，调整 pitch 计算 | 低 |
| 真实 GPU 驱动 | AMD Oland (1002:6611) 或其他真实显卡，替换 VBE 初始化逻辑 | 低 |
| Huge page 映射 | framebuffer BAR 映射改用 2MB huge page，减少 TLB miss | 低 |
| 用户态 WC PAT 映射 | 硬件 flip mmap 使用 WC 而非 UC，提升渲染写带宽 | 低 |
| 完整 DRM | CRTC/Plane/Connector 模型 + GEM buffer manager + fence/sync + 多客户端仲裁 | 低 |
