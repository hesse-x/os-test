# KMS 内核态显示驱动

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | KMS 在内核还是用户态 | 内核 | 与 Linux DRM/KMS 模型一致；flip 是同步 memcpy，放内核消除上下文切换开销 |
| 2 | 完整 DRM vs 最小 KMS | 最小 KMS（buffer 创建 + flip） | 当前只需 scanout，不需要 CRTC/Plane/Connector/GEM/fence/多客户端仲裁 |
| 3 | 谁分配 back buffer | 内核 | 与 Linux DRM 一致：内核了解硬件约束、拥有 buffer 生命周期；compositor 通过 ioctl 请求分配 |
| 4 | flip 机制 | 同步 memcpy back→front，支持 dirty-rect 行级拷贝 | 简单正确；异步 flip 需要显卡 IRQ + vblank 机制，当前不存在；QEMU 模拟环境下硬件 page-flip 无实际收益 |
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
  fb_rows : uint32_t — 文本行数 = fb_height / FONT_HEIGHT
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
2. 解析 `display_ioctl_flip_arg` 中的 `dirty_row_start` / `dirty_row_end`
3. 无效范围（start >= end 或 end > rows）退化为全帧拷贝
4. 有效 dirty 范围时行级 memcpy，只拷贝脏行区域

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
- `display_client_flush(dirty_row_start, dirty_row_end)` — `ioctl(fd, KMS_IOCTL_FLIP, &arg)` 请求内核 flip，附带 dirty 行范围

Terminal 调用 `display_client_init()` 初始化，渲染到 mmap'd back buffer，flush 时请求内核 flip。光标在 back buffer 上渲染（反色块），内核不知道光标。

**Scroll 瓶颈**：当前 scroll 路径（`cell_putc` 触发）每次 scroll 都全帧 flip（~3.6ms）。这是 terminal 侧的问题，Wayland 后 terminal 变为普通 client 自管 surface，compositor 天然只做一次 flip，此问题自然消失。当前性能体感可接受，不单独优化。

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

## 性能优化

### 性能基线与演进

测试条件：`--perf` 构建，TSC 计时，`serial_printf` 输出。分辨率 800×600×32，全帧 1.92MB。

#### 基线（优化前）

FLIP 无条件全帧 memcpy 1.92MB，Terminal read 缓冲区 256 字节。

| 场景 | 平均耗时 | 最小 | 最大 | 拷贝量 |
|------|---------|------|------|--------|
| 全帧 FLIP | 6987 us | 1016 us | 11531 us | 1.92MB |

#### Phase 1 已完成：dirty-rect FLIP + read buffer 扩大

**已实施优化**：

1. **行级 dirty-rect FLIP** — `display_ioctl_flip_arg`（8B: dirty_row_start + dirty_row_end）传入脏行范围，内核只拷贝脏行区域。无效范围退化为全帧拷贝，兼容旧客户端。
2. **Terminal read 缓冲区 256→4096** — 减少 flip 频率，多个输出行合并到同一次 flip。
3. **每 4 行主动 flush** — `vt100_feed` 循环中 `dirty_row_end - dirty_row_start >= 4` 时提前 flush，保持交互响应性。

**实测性能**（Phase 1 完成后）：

| 场景 | 平均耗时 | 最小 | 最大 | 拷贝量 |
|------|---------|------|------|--------|
| partial flip（2-4 行脏） | **107 us** | 19 us | 301 us | ~50-200KB |
| full flip（scroll/clear） | 3666 us | 834 us | 13510 us | ~1.89MB |

**partial flip 比基线快 ~65 倍。**

Partial flip 行分布：rows=4（10次），rows=1（5次），rows=3（2次），rows=2（1次）。

**分析**：
- 日常交互（命令输入、回显）触发 partial flip，仅拷贝 2-4 行（~50-200KB），耗时 ~100us
- scroll/clear 仍触发全帧 flip，但频率远低于逐字符 partial flip
- full flip 平均耗时比基线低近一半（3666 vs 6987），可能因分支路径优化和缓存局部性差异；max 偏高（13510 us）属采样噪声
- MMIO 写入带宽是全帧 flip 的硬上限

#### Phase 1 涉及的改动

| 文件 | 改动 |
|------|------|
| common/display.h | 新增 `display_ioctl_flip_arg`（8B: dirty_row_start + dirty_row_end） |
| common/ioctl.h | `KMS_IOCTL_FLIP` 从 `_IO('K',2)` 改为 `_IOWR('K',2,char[8])`，兼容 `_IOC_NR==2` |
| kernel/display.h | `display_state` 新增 `fb_rows` 字段 |
| kernel/display.c | FLIP 路径解析 flip_arg，有效 dirty 范围时行级 memcpy，无效时退化为全帧拷贝；legacy `DISPLAY_REQ_FLIP` 传入零初始化 flip_arg 保持向后兼容 |
| driver/display.h | `display_client_flush(dirty_row_start, dirty_row_end)` 传入 dirty 范围 |
| driver/terminal.cc | `flush_dirty_cells` 保存 dirty 范围传给 flush；初始化清屏传 `(0, display_rows)`；read buffer 从 256 扩至 4096 并每 4 行主动 flush |

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| 可变分辨率 | CREATE_BUF 验证改为查 mode 列表，支持 compositor 请求切换分辨率 | 中 |
| 真实 GPU 驱动 | AMD Oland (1002:6611) 或其他真实显卡；真实 GPU 下硬件 page-flip（Y_OFFSET / CRTC 地址切换）、vblank 同步、vsync 等才有实际意义 | 中 |
| 异步 flip + VSync/VBLANK | ioctl(FLIP) 立即返回，内核 deferred flip（下次 vblank 时）；需要显卡 IRQ + vblank callback 机制；真实 GPU 场景 | 低 |
| 多 buffer（double/triple buffering） | CREATE_BUF 支持创建多个 dumb buffer，MAP_BUF handle→offset；真实 GPU 场景 | 低 |
| 完整 DRM | CRTC/Plane/Connector 模型 + GEM buffer manager + fence/sync + 多客户端仲裁；真实 GPU 场景 | 低 |
