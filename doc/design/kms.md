# KMS 用户态驱动设计

## 概述

将内核态 framebuffer 渲染（`kernel/fb.cc`）移至用户态 KMS 驱动进程。内核仅保留 `init_fb` 做 framebuffer 物理页映射和元信息保存，所有文本渲染、光标管理、滚动逻辑移到用户态。

**定位**：KMS 只负责 framebuffer 输出（back buffer → front buffer flip），不负责渲染。Terminal 作为 compositor 负责像素渲染（cell → glyph → 像素 → back buffer）。此架构与 Linux 模型一致：compositor 渲染像素，KMS/DRM 只做 scanout/flip。Buffer 由 KMS 分配（与 Linux DRM 模型一致），compositor 通过 attach 访问。

## 架构

```
Shell (PID 6)
  │
  │ stdout pipe
  ▼
Terminal (PID 5, compositor)
  │
  │ 1. 读 pipe → VT100 parse → 更新 cell buffer (内部)
  │ 2. 渲染 cell → glyph → 像素 → 写入 back buffer (display SHM)
  │ 3. 标记 dirty_full + 递增 generation → notify KMS
  ▼
KMS Driver (PID 4, IOPL=0)
  │
  │ 读 generation → memcpy back buffer → front buffer → 验证 generation
  ▼
Framebuffer 硬件 (front buffer)
```

KMS 是 IOPL=0 的普通用户态进程，不绑 IRQ。Framebuffer 物理页零拷贝映射到 KMS 进程地址空间 `0x700000`，KMS 驱动只做 memcpy flip。

## Display 协议

### 设计动机

旧协议是逐字符命令流（KMS_CMD_PUTC），terminal 每个字符发一条 16 字节消息到 kms_ring（容量 240 条）。一次 scroll 刷新 = 80×45 = 3600 条消息，远超 ring 容量，导致 terminal 在 `write_kms_ring` 里 busy-wait（`sched_yield` 循环），无法读 pipe 和 kbd_ring，整条流水线阻塞。

新协议让 terminal 直接渲染像素到共享 back buffer，KMS 只做 dirty 区域 flip。一次 scroll = 1 次 dirty 标记 + notify，彻底解耦生产与消费。

### Buffer 所有权

与 Linux DRM 模型一致，**KMS 创建 display SHM**（back buffer），terminal 通过 `sys_lookup_dev(DEV_KMS)` + `sys_shm_attach` 访问。KMS 拥有 buffer 生命周期，因为：

- KMS 是 flip 引擎，了解硬件约束
- 未来硬件 page flip 时 KMS 需要控制 buffer 物理地址对齐
- 与 Linux 模型一致：DRM 分配 framebuffer object，compositor 通过 GBM/DRM ioctl 请求分配

Terminal 从 display SHM header 读取 fb 元信息（width/height/pitch/bpp/rows/cols），**不再调 `sys_fb_info`**——KMS 是 fb_info 的唯一权威来源。

### 接口封装

#### driver/display.h（统一文件，双侧 API）

```c
// ===== display_shm_header（KMS 和 terminal 共享）=====

struct display_shm_header {
    // 元信息（KMS 写，terminal 读）
    uint32_t fb_width;          // 像素宽
    uint32_t fb_height;         // 像素高
    uint32_t fb_pitch;          // 字节/行
    uint32_t fb_bpp;            // 位/像素（当前仅支持 32）
    uint32_t rows;              // 文本行数 (fb_height / FONT_HEIGHT)
    uint32_t cols;              // 文本列数 (fb_width / FONT_WIDTH)
    uint32_t cursor_x;          // 光标列（保留，未来硬件光标）
    uint32_t cursor_y;          // 光标行（保留，未来硬件光标）

    // dirty 追踪（terminal 写，KMS 读后清零）
    uint32_t dirty_full;        // 1 = 全屏 dirty

    // 并发同步
    uint32_t generation;        // terminal flush 时递增，KMS flip 前后校验
    uint8_t  backend_sleeping;  // KMS 设 1 后进 recv，terminal 据此决定是否 notify
    uint8_t  reserved[3];

    // 保留
    uint8_t  reserved2[28];
};

// ===== Client API（terminal 侧）=====

// 初始化：attach display SHM，从 header 读 fb 元信息
int display_client_init();

// 渲染单个 cell 到 back buffer（ch 为 ASCII，fg/bg 为 32bit 颜色）
// 纯像素渲染，不处理特殊字符（\n/\r/\b/\t 由 VT100 层处理）
void display_client_render_cell(uint32_t row, uint32_t col,
                                uint8_t ch, uint32_t fg, uint32_t bg);

// 清空 back buffer（填 bg 颜色），标记 dirty_full
void display_client_clear(uint32_t bg);

// 在 back buffer 上执行 scroll（memmove 向上搬像素行，uint32_t 循环清末行）
void display_client_scroll_up(uint32_t bg);

// 设置光标位置（更新 header 元信息，供未来硬件光标）
void display_client_set_cursor(uint32_t x, uint32_t y);

// 标记 dirty_full + 递增 generation + 检查 backend_sleeping 决定是否 notify
// 内部守卫：dirty_full==0 时直接返回，不递增 generation 也不 notify
void display_client_flush();

// ===== Backend API（KMS 侧）=====

// 初始化：sys_fb_info 获取 fb 元信息 → sys_shm_create 创建 display SHM →
// 写入 header 元信息 → 注册 DEV_KMS
int display_backend_init();

// 轮询：检查 dirty_full，memcpy back → front，验证 generation
// 返回是否有工作（1=处理了 dirty，0=无 dirty）
int display_backend_poll();

// 等待：设 backend_sleeping=1 → double-check dirty → recv 超时等待
void display_backend_wait(uint32_t timeout_ms);
```

### Display SHM 布局

由 KMS 通过 `sys_shm_create` 创建，terminal 通过 `sys_lookup_dev(DEV_KMS)` + `sys_shm_attach` 访问。

```
page 0 (offset 0):      display_shm_header (64 bytes) + 保留 (4032 bytes)
page 1+ (offset 4096):  back buffer (fb_height × fb_pitch 字节，页对齐)
```

Header 占一整页（4096 字节），back buffer 从第二页开始，页对齐。为未来 huge page 映射预留空间。

### 并发同步：generation counter

Terminal 和 KMS 并发访问 back buffer，存在竞争窗口：terminal flush 后继续渲染新内容，同时 KMS 正在 memcpy 旧内容到 front buffer。

解法：**乐观锁（generation counter）**。

- Terminal `display_client_flush()` 操作顺序：**先扩展 dirty，再递增 generation**。KMS 看到 generation 变化时，dirty 一定已更新完毕
- Terminal 只扩展 dirty 标记（不清零），KMS 在 g1==g2 时清零 `dirty_full`
- KMS 在 memcpy 前读 `generation`（g1），memcpy 后再读一次（g2）
- 若 g1 == g2：清零 `dirty_full`，返回有工作
- 若 g1 != g2：不清零，返回有工作（主循环 `continue` 再进 poll，此时 dirty_full 仍为 1）
- 无阻塞，无锁竞争，scroll 等大范围 dirty 时最多多拷一次
- 比三缓冲省一半内存

### dirty 追踪

当前阶段只支持 `dirty_full`（全屏 dirty），不支持行级 dirty。理由：

- Shell 文本输出场景下，scroll 和满行 wrap 几乎总是全屏 dirty，行级 dirty 收益小
- Wayland 场景下天然是像素级 damage region，届时再加行级 dirty（`dirty_y_start`/`dirty_y_end` 像素行单位）是局部改动

### Bpp 支持

当前仅支持 32bpp。`display_client_init` 检查 `fb_bpp`，非 32 则报错。未来真机适配时再加 24bpp 路径。

### 光标渲染

Terminal（compositor）负责所有像素渲染，包括光标。光标在 back buffer 上渲染（反色块或下划线），KMS 纯 flip 不知道光标。`cursor_x`/`cursor_y` 保留在 header 中供未来硬件光标用。

## IPC 拓扑（更新后）

```
kbd_driver(PID3) ── SHM + notify ──→ terminal(PID5) ── pipe ──→ shell(PID6)
                                            │
                                            │ display SHM + notify
                                            ▼
                                     kms_driver(PID4)
                                            │
                                            │ memcpy flip
                                            ▼
                                     framebuffer (front buffer)
```

KMS 不再 attach kbd_driver SHM，与 kbd_driver 完全解耦。Terminal attach 两个 SHM：kbd SHM（键盘输入）+ display SHM（显示输出）。

## 组件改动

### Terminal 改动

- **删除**：`write_kms_ring()`、`kms_notify_driver()`、对 `kms_ring` 的直接操作、`font8x16` 字体表副本
- **新增**：`display_client_init()`、`display_client_render_cell()`、`display_client_clear()`、`display_client_scroll_up()`、`display_client_flush()`
- **`flush_dirty_cells()` 改造**：删除逐字符写 kms_ring 的逻辑，改为遍历 dirty 行的 cell → `display_client_render_cell()` 渲染到 back buffer → `display_client_flush()`
- **scroll 改为 back buffer memmove**：不再发 KMS_CMD_SCROLL 命令，terminal 直接在 back buffer 上 `memmove` 像素行 + `uint32_t` 循环清空末行 + 标记 `dirty_full`
- **fb_info 来源**：不再调 `sys_fb_info`，从 display SHM header 读取
- **字体表**：使用 `common/font.h`（共享字体表）
- **主循环**：`display_client_flush()` 每次迭代末尾调用，内部守卫（`dirty_full==0` 直接返回）避免无谓操作；只在有 dirty 时检查 `backend_sleeping` 并 notify KMS

### KMS 驱动改动

- **删除**：`fb_putc()`、`scroll_up()`、`advance_line()`、`font8x16` 字体表、kms_ring 消费循环、所有渲染逻辑、光标状态
- **删除**：对 kbd_driver SHM 的 attach（完全解耦）
- **新增**：`display_backend_init()`、`display_backend_poll()`、`display_backend_wait()`
- **启动流程**：`sys_fb_info` → `sys_shm_create(n)` 创建 display SHM → 写 header 元信息 → `sys_register_dev(DEV_KMS)` → 进入 flip 主循环
- **主循环**：

```c
while (1) {
    if (display_backend_poll()) {
        continue;  // 还有 dirty，继续处理
    }
    display_backend_wait(16);  // 无 dirty，等 16ms 或被 notify
}
```

- `display_backend_poll()` 内部：
  1. 读 `generation`（g1）
  2. 检查 `dirty_full == 1`，若无 dirty → 返回 0
  3. memcpy 整个 back buffer → front buffer
  4. 读 `generation`（g2）
  5. 若 g1 == g2：清零 `dirty_full`，返回 1
  6. 若 g1 != g2：不清零，返回 1（主循环 continue → 再进 poll 重拷）

- `display_backend_wait()` 内部（double-check 防 lost-wakeup）：
  1. 设 `backend_sleeping = 1`
  2. 再 poll 一次，若有 dirty → 清 sleeping → 返回
  3. 否则 `recv(timeout_ms)`
  4. 醒来后清 `backend_sleeping = 0`

### common/shm.h 改动

- **删除**：`KMS_CMD_*` 常量、`kms_msg`、`kms_ring`、`KMS_RING_OFFSET`、`KMS_RING_SIZE`
- **删除**：`driver_shm_header.kms_sleeping` 字段
- **保留**：`driver_shm_header`（精简后：`kbd_sleeping` + `consumer_sleeping`）、`kbd_ring`、`kbd_msg`、`KBD_RING_OFFSET`、`kms_fb_info`
- **注意**：`display_shm_header` 放在 `driver/display.h`，不在 `common/shm.h`

### driver/display.h（新增）

统一文件，包含 `display_shm_header` 结构体 + client API + backend API。只被 terminal 和 KMS 使用，不属于内核/用户态共享接口。

### common/font.h（新增）

`font8x16[96][16]` 字体表，从 KMS 和 terminal 中提取为共享资源。任何需要像素渲染的组件都可使用。

## Framebuffer 映射细节

- Front buffer 映射地址：`0x700000`（KMS 进程内，4KB 页映射，仅 KMS 可写）
- Back buffer：display SHM（KMS 创建，terminal attach），大小 = `fb_height × fb_pitch`，从第二页开始页对齐
- 物理页来源（front buffer）：`init_fb` 保存的 `fb.phys_addr` + `fb.size`
- 映射时机：`process_create_elf` 创建 KMS 进程时（`map_fb=true`）

## 内核改动

### fb.cc

- **保留** `init_fb`：映射 framebuffer 物理页到 higher-half 设备区 + 保存元信息到全局 `g_fb_info`

### kernel_main

- 创建 KMS 进程时 `map_fb=true`（映射 front buffer 物理页到 0x700000）
- 创建 terminal 进程时 `map_fb=false`（terminal 通过 display SHM 访问 back buffer，通过 header 读取 fb 元信息，不需要直接访问 front buffer，也不需要调 sys_fb_info）

## 启动阶段输出

- `init_fb` 前后均走串口输出（`serial_putc`/`serial_puts`）
- KMS 驱动启动前屏幕为黑，串口有输出
- 内核 panic 走串口

## 性能分析

### scroll 场景

当前方案：KMS 收到 `KMS_CMD_SCROLL` 后 front buffer memmove（1.92MB）+ 逐字符 `KMS_CMD_PUTC` 重绘所有行（25×80=2000 条消息解析 + 逐像素渲染）。

新方案：terminal back buffer memmove（1.92MB）+ 标记 dirty_full + KMS memcpy back→front（1.92MB）= 共 3.84MB 内存操作。

虽然总内存操作量翻倍，但当前方案的 2000 条消息解析 + 逐像素渲染开销远超 3.84MB 的两次 memcpy。新方案实际更快。

### 常规输出

当前方案：每字符一条 kms_ring 消息（16 字节）+ KMS 逐字符渲染。

新方案：terminal 直接写 back buffer 像素 + 一次 dirty_full + 一次 memcpy。减少消息开销和上下文切换。

## 已知限制

- **撕裂**：KMS memcpy 期间硬件同时在 scanout，可能产生半新半旧的撕裂帧。当前 QEMU 验证无影响，真机撕裂留待 VSync 扩展解决
- **全屏 dirty**：当前只支持 `dirty_full`，不支持行级 dirty。Wayland 场景下再加
- **32bpp only**：当前仅支持 32bpp，真机适配时再加 24bpp
- **无存活检测**：KMS/terminal 崩溃 = 屏幕冻结，当前阶段靠修复崩溃而非恢复机制

## 将来扩展

- **硬件 page flip**：UEFI GOP 无 CRTC 切换支持，当前只能软件 flip（memcpy）。若后续驱动真实 GPU，可改用硬件 page flip（写 CRTC 寄存器切换扫描地址，零拷贝）。KMS 拥有 buffer 分配权为此预留
- **VSync / VBLANK**：KMS 绑定显卡 IRQ，terminal 请求 flip 后等 vblank 回调，避免撕裂
- **行级 dirty**：`display_shm_header` 加 `dirty_y_start`/`dirty_y_end`（像素行单位），Wayland damage region 场景使用
- **多客户端**：display 协议支持多个 compositor 连接（当前一对一）
- **PAT / write-combining**：framebuffer 页映射加 PCD/PAT 标记，优化真机性能
- **Huge page 映射**：用户态 framebuffer 映射改用 2MB huge page，减少 TLB miss（back buffer 页对齐已预留）
- **24bpp 支持**：`display_client_render_cell` 加 24bpp 渲染路径

## 实现状态

已实现。

### 需修改的文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `driver/display.h` | 新增 | display 协议（header 结构体 + client/backend API） |
| `common/font.h` | 新增 | font8x16 字体表（从 KMS/terminal 提取） |
| `driver/terminal.cc` | 重构 | 删 kms_ring 操作，改用 display_client 渲染到 back buffer |
| `driver/kms_driver.cc` | 重构 | 删渲染逻辑/font/cursor/kbd SHM，改用 display_backend flip 循环 + 创建 display SHM |
| `common/shm.h` | 修改 | 删 kms_ring/kms_msg/KMS_CMD_*/kms_sleeping |
