# KMS 用户态驱动设计

## 概述

将内核态 framebuffer 渲染（`kernel/fb.cc`）移至用户态 KMS 驱动进程。内核完全不参与 framebuffer 管理，文本渲染、光标管理、滚动逻辑、显示设备初始化均在用户态完成。

**定位**：KMS 只负责 framebuffer 输出（back buffer → front buffer flip）和显示设备初始化（PCI VBE modeset），不负责渲染。Terminal 作为 compositor 负责像素渲染（cell → glyph → 像素 → back buffer）。此架构与 Linux 模型一致：compositor 渲染像素，KMS/DRM 只做 scanout/flip。Buffer 由 KMS 分配（与 Linux DRM 模型一致），compositor 通过 attach 访问。

**FB 来源**：KMS 驱动通过 PCI ECAM 枚举 bochs-display 设备，mmap BAR0（VBE MMIO 寄存器）和 BAR1（帧缓冲内存），自主初始化显示设备。不再依赖 UEFI GOP 传递 framebuffer 物理地址。详见 [Phase 3: PCI VBE 原生显示驱动](#phase-3-pci-vbe-原生显示驱动)。

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

KMS 是 IOPL=0 的普通用户态进程，不绑 IRQ。Framebuffer 由 KMS 驱动通过 PCI BAR1 `mmap(MAP_PHYSICAL)` 获取，KMS 驱动只做 memcpy flip。

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

// 初始化：PCI 遍历发现 bochs-display → mmap BAR0(VBE regs) + BAR1(fb) →
// VBE modeset 800x600x32 → sys_shm_create 创建 display SHM →
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
- **启动流程**：PCI 遍历(寻找 class=0x0300, vendor=0x1234, device=0x1111) → mmap BAR0(VBE regs) → mmap BAR1(fb) → VBE modeset(800x600x32) → `sys_shm_create(n)` 创建 display SHM → 写 header 元信息 → `device_register(DEV_KMS)` → 进入 flip 主循环
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
- **删除**：`driver_shm_header.kms_sleeping` 字段
- **保留**：`driver_shm_header`（精简后：`kbd_sleeping` + `consumer_sleeping`）、`kbd_ring`、`kbd_msg`、`KBD_RING_OFFSET`
- **删除**：`kms_fb_info`（fb 元信息改由 display_shm_header 提供）
- **注意**：`display_shm_header` 放在 `driver/display.h`，不在 `common/shm.h`

### driver/display.h（新增）

统一文件，包含 `display_shm_header` 结构体 + client API + backend API。只被 terminal 和 KMS 使用，不属于内核/用户态共享接口。

### common/font.h（新增）

`font8x16[96][16]` 字体表，从 KMS 和 terminal 中提取为共享资源。任何需要像素渲染的组件都可使用。

## Framebuffer 映射细节

- Front buffer：KMS 驱动通过 `sys_pci_dev_info` 读取 bochs-display BAR1 的物理地址和大小，通过 `mmap(MAP_PHYSICAL, bar1_phys, fb_size)` 映射到 KMS 进程地址空间
- 映射标志：`MAP_PHYSICAL` + NX（不可执行），back buffer 用 `MAP_SHARED`
- Back buffer：display SHM（KMS 通过 `sys_shm_create` 创建，terminal 通过 `mmap(fd, MAP_SHARED)` 访问），大小 = `fb_height × fb_pitch`，从 display SHM 第二页开始页对齐
- VBE 寄存器：KMS 驱动通过 `sys_pci_dev_info` 读取 BAR0 的物理地址和大小，通过 `mmap(MAP_PHYSICAL, bar0_phys, 4096)` 映射到 KMS 进程地址空间
- **内核完全不做 framebuffer 映射**，不再有 `init_fb`、`g_fb_info`、`map_fb` 参数

## 内核改动

### fb.cc

**删除** `init_fb`。内核不再映射 framebuffer 物理页，不再保存 `g_fb_info`。

### kernel_main

- 删除 `init_fb(bi)` 调用
- 删除 `process_create_elf` 的 `map_fb` 参数——KMS 驱动自行 mmap BAR
- KMS 驱动不再需要内核预映射 framebuffer

### common/syscall.h & kernel/trap.cc

- **删除** `SYS_FB_INFO` syscall（编号 #11，后续保持编号连续）
- **删除** `sys_fb_info()` 实现
- `kms_fb_info` 结构体不再由内核维护

### boot/stub.c

- **删除** `read_gop()` 调用——EFI stub 不再读 GOP 信息填入 `boot_info`
- `boot_info.fb_addr` 等 fb 字段不再使用

## 启动阶段输出

- KMS 驱动启动前屏幕为黑，串口有输出（QEMU `-device bochs-display` 默认禁能，KMS 驱动做完 VBE modeset 后屏幕激活）
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
- **硬编码分辨率**：800×600×32，后续可通过配置参数支持可变分辨率
- **QEMU bochs-display only**：当前只支持 PCI 1234:1111，真实硬件需额外驱动
- **I/O 端口未使用**：Bochs VBE 控制寄存器走 BAR0 MMIO，避免了 sys_ioperm 依赖

## 将来扩展

- **硬件 page flip**：当前只能软件 flip（memcpy），bochs-display 无 CRTC 切换支持。若后续驱动真实 GPU，可改用硬件 page flip（写 CRTC 寄存器切换扫描地址，零拷贝）
- **VSync / VBLANK**：KMS 绑定显卡 IRQ，terminal 请求 flip 后等 vblank 回调，避免撕裂
- **行级 dirty**：`display_shm_header` 加 `dirty_y_start`/`dirty_y_end`（像素行单位），Wayland damage region 场景使用
- **可变分辨率**：`display_shm_header` 扩充分辨率协商字段，支持 compositor 请求切换分辨率
- **多客户端**：display 协议支持多个 compositor 连接（当前一对一）
- **PAT / write-combining**：framebuffer 页映射加 PCD/PAT 标记，优化真机性能
- **Huge page 映射**：用户态 framebuffer 映射改用 2MB huge page，减少 TLB miss（back buffer 页对齐已预留）
- **24bpp 支持**：`display_client_render_cell` 加 24bpp 渲染路径
- **其他真实显卡**：基于 BAR mmap 模式增加 AMD/Intel 驱动支持，mmap(MAP_PHYSICAL) + 寄存器 MMIO 模式可复用

## 实现状态

### Phase 1（display SHM + flip 循环协议）

已实现。

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `driver/display.h` | 新增 | display 协议（header 结构体 + client/backend API） |
| `common/font.h` | 新增 | font8x16 字体表（从 KMS/terminal 提取） |
| `driver/terminal.cc` | 重构 | 删 kms_ring 操作，改用 display_client 渲染到 back buffer |
| `driver/kms_driver.cc` | 重构 | 删渲染逻辑/font/cursor/kbd SHM，改用 display_backend flip 循环 + 创建 display SHM |
| `common/shm.h` | 修改 | 删 kms_ring/kms_msg/KMS_CMD_*/kms_sleeping |

### Phase 2（`/dev/kms` fd 化）

已实现。

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `driver/display.h` | 修改 | `display_client_init()` 改为 `open("/dev/kms")` + `mmap(fd, MAP_SHARED)`；`display_client_flush()` 改为 `notify_fd(fd)` |
| `driver/kms_driver.cc` | 不变 | KMS 驱动侧无需感知 fd 模型变化 |

### Phase 3（PCI VBE 原生显示驱动）

已实现。

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `run.sh` | 修改 | `-vga std` → `-vga none -device bochs-display` |
| `boot/stub.c` | 修改 | 删除 `read_gop()` 调用 |
| `kernel/fb.cc` | 删除 | `init_fb` 不再需要 |
| `kernel/fb.h` | 删除 | |
| `kernel/mem/alloc.cc` | 修改 | 删 `init_fb(bi)` 调用 |
| `kernel/trap.cc` | 修改 | 删 `sys_fb_info()` 实现，slot 11 置 nullptr |
| `common/syscall.h` | 修改 | 删 `SYS_FB_INFO` 定义 |
| `kernel/proc.cc` | 修改 | 删 `map_fb` 参数 |
| `kernel/proc.h` | 修改 | 删 `map_fb` 参数声明 |
| `driver/display.h` | 修改 | `display_backend_init()` 改为 PCI ECAM 发现 bochs-display + BAR mmap + VBE modeset |
| `driver/kms_driver.cc` | 重构 | 删除旧 `sys_fb_info` / `shm_attach` 依赖，改用 PCI + BAR mmap |

---

## Phase 2：设备发现 `/dev/kms` + fd 统一接口

### 设计动机

与 kbd 的 fd 化动机一致——用户侧统一通过 `open("/dev/kms")` 发现设备、`req(fd)` 控制、`mmap(fd)` 取 SHM、`poll(fd)` 等事件。

KMS 场景下消费者当前是 terminal（未来是独立的合成器进程）：

```
consumer  open("/dev/kms") → fd
              │
              ├─ req(fd, DISPLAY_REQ_*)
              ├─ mmap(fd, ...) → display SHM
              └─ poll(fd, POLLIN)

driver(KMS 进程)  不 open 自己，照常事件循环
```

### 与 kbd fd 模型的对称性

| 层面 | kbd | kms |
|------|-----|-----|
| 设备节点 | `/dev/kbd` | `/dev/kms` |
| 消费者 | terminal（→ 合成器） | terminal（→ 合成器） |
| 驱动进程 | kbd_driver | kms_driver |
| 控制通道 | `req(fd, KBD_REQ_BIND, ...)` | `req(fd, DISPLAY_REQ_*, ...)` |
| 数据通道 | `mmap(fd, ...)` → kbd_ring | `mmap(fd, ...)` → display SHM back buffer |
| 事件等待 | `poll(fd, POLLIN)` 等按键 | 驱动侧不变，消费者(compositor) poll |
| 驱动侧 | 不 open 自己，正常事件循环 | 不 open 自己，正常 flip 循环 |
| 驱动切换 | 合成器 detect POLLERR → close →重连 | 合成器 detect POLLERR → close →重连 |

### 驱动侧的视角

KMS 驱动进程完全不需要感知 fd 模型的变化。它的代码不动：

```c
// kms_driver.cc — 变化极微，可能仅注册方式
void kms_main() {
    display_backend_init();    // shm_create → 写 header → 进入 flip 循环
    // 内部 device_register(DEV_KMS) 保留，供 open("/dev/kms") 查到 PID
    while (1) {
        if (display_backend_poll()) continue;
        display_backend_wait(16);
    }
}
```

`device_register(DEV_KMS)` 保留——driver 注册 dev_table，不是暴露给用户的 API，而是内核内部 open 做 PID 翻译的后端。

### 消费者侧的变化

当前 `display.h` 中的 `display_client_init()`：

```c
// 旧：device_lookup + shm_attach
static inline int display_client_init() {
    while ((display_kms_pid = device_lookup(DEV_KMS)) <= 0)
        recv(&m, NULL, 0, 1);
    while (shm_attach(display_kms_pid, &shm_ptr) < 0)
        recv(&m, NULL, 0, 1);
    display_hdr = shm_ptr;
    display_back_buffer = shm_ptr + DISPLAY_BACK_BUFFER_OFFSET;
}
```

改为 fd 模式：

```c
// 新：open + mmap
static inline int display_client_init() {
    int fd;
    while ((fd = open("/dev/kms", O_RDWR)) < 0)
        recv(&m, NULL, 0, 1);                 // 等 KMS 驱动就绪

    // mmap display SHM via MAP_SHARED (fd → target_pid → sys_shm_attach)
    // size=0 is ignored — MAP_SHARED maps the entire driver SHM region
    void *shm_ptr = mmap(NULL, 0,
                         PROT_READ|PROT_WRITE,
                         MAP_SHARED, fd, 0);
    display_hdr = shm_ptr;
    display_back_buffer = shm_ptr + DISPLAY_BACK_BUFFER_OFFSET;
    display_dev_fd = fd;                     // 缓存 fd 供后续 flush 用
    return 0;
}
```

`display_client_flush()`：

```c
// 旧：notify(display_kms_pid)
static inline void display_client_flush() {
    if (display_hdr->dirty_full == 0) return;
    display_hdr->generation++;
    if (display_hdr->backend_sleeping)
        notify(display_kms_pid);    // ← 查 pid
}

// 新：notify_fd(fd)
static inline void display_client_flush() {
    if (display_hdr->dirty_full == 0) return;
    display_hdr->generation++;
    if (display_hdr->backend_sleeping)
        notify_fd(display_dev_fd);     // ← 传 fd
}
```

### 驱动切换

当 KMS 驱动异常退出时：

```
合成器:
  req(fd, ...) → ESRCH，或 poll(fd) → POLLERR
  → close(fd)

init 重新 spawn kms_driver:
  → kms_driver 启动 → display_backend_init()
  → shm_create（新 SHM，新物理页）
  → device_register(DEV_KMS)

合成器重试循环:
  while ((fd = open("/dev/kms")) < 0)
      poll(NULL, 0, 100);
  mmap(fd, ...)     ← 新 SHM
  // 回到正常合成循环
```

与 kbd 一致：**SHM 归驱动进程**，驱动切换时释放旧 SHM、创建新 SHM，消费者通过重连 open + mmap 拿到新 SHM。

### 对 display.h 的影响

`driver/display.h` 中的 API 签名更改为使用 fd：

```c
// Client API（terminal / 合成器侧）
int display_client_init();           // 内部改为 open + mmap

// 以下不变——只操作 shm 指针，不涉及通信方式
void display_client_render_cell(...);
void display_client_clear(uint32_t bg);
void display_client_scroll_up(uint32_t bg);
void display_client_set_cursor(uint32_t x, uint32_t y);
void display_client_flush();         // 内部改为 notify(fd)

// Backend API（KMS 驱动侧）——完全不变
int  display_backend_init();
int  display_backend_poll();
void display_backend_wait(uint32_t timeout_ms);
```

Backend API 完全不需要改动——KMS 驱动进程不 open 自己，`device_register(DEV_KMS)` 保留为内核内部机制。`shm_create` 也保留为自己创建 SHM，不需要换成 mmap。

### 与 display SHM 语义的兼容性

display SHM 的一个特殊性是：**KMS 创建 SHM**，消费者 attach/mmap。kbd 则是 **kbd_driver 创建 SHM**，消费者 attach/mmap。方向相同：

```
kbd:  kbd_driver→shm_create → consumer→mmap(fd)
kms:  kms_driver→shm_create → consumer→mmap(fd)
                           ↑
                     完全一致
```

所以 KMS 的 fd 化没有额外困难，与 kbd 的模式完全对称。

---

## Phase 3：PCI VBE 原生显示驱动

### 设计动机

移除 UEFI GOP 依赖，framebuffer 的发现与初始化全部走 PCI 总线。KMS 驱动通过 PCI ECAM 发现 bochs-display 设备，mmap BAR0（VBE 寄存器）和 BAR1（帧缓冲内存），自主完成显示初始化。

### QEMU 配置

```bash
# run.sh 改动：不再使用 UEFI GOP 提供的 framebuffer
-vga none -device bochs-display
```

`bochs-display` 是 QEMU 的纯 PCI 显示设备，无 VGA 兼容层，无 I/O 端口寄存器，所有控制通过 BAR0 MMIO 完成。BAR1 提供线性帧缓冲内存。

### PCI 设备信息

| 属性 | 值 |
|------|-----|
| vendor_id | 0x1234 (QEMU) |
| device_id | 0x1111 (Bochs VBE) |
| class_code | 0x0300 (Display Controller / VGA) |
| BAR0 | VBE MMIO 寄存器（4KB） |
| BAR1 | 线性帧缓冲（~128MB，物理地址由 QEMU 分配） |

### KMS 驱动初始化流程

```
kms_driver 启动
  │
  ├─ 1. PCI 设备发现
  │      for bus 0..255:
  │        for dev 0..31:
  │          for func 0..7:
  │            sys_pci_dev_info(bus, dev, func, &info)
  │            if info.class_code == 0x0300
  │               && info.vendor_id == 0x1234
  │               && info.device_id == 0x1111 → 匹配
  │              记录 (bus, dev, func) 和 BAR0/BAR1 物理地址
  │
  ├─ 2. mmap BAR0（VBE 寄存器）
  │      vbe_regs = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
  │                      MAP_PHYSICAL, -1, bar0_phys);
  │
  ├─ 3. mmap BAR1（帧缓冲）
  │      front_fb = mmap(NULL, bar1_size, PROT_READ|PROT_WRITE,
  │                      MAP_PHYSICAL, -1, bar1_phys);
  │
  ├─ 4. VBE 初始化（写 MMIO 寄存器）
  │      writew(vbe_regs + VBE_DISPI_INDEX_XRES,  800);
  │      writew(vbe_regs + VBE_DISPI_INDEX_YRES,  600);
  │      writew(vbe_regs + VBE_DISPI_INDEX_BPP,   32);
  │      writew(vbe_regs + VBE_DISPI_INDEX_ENABLE,
  │             VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
  │
  ├─ 5. 读回确认分辨率
  │      width  = readw(vbe_regs + VBE_DISPI_INDEX_XRES);  // 800
  │      pitch  = width * 4;                                // 3200
  │      height = readw(vbe_regs + VBE_DISPI_INDEX_YRES);  // 600
  │
  ├─ 6. 创建 display SHM
  │      shm_create(4096 + pitch * height, &shm_ptr);
  │      hdr = (display_shm_header *)shm_ptr;
  │      hdr->fb_width  = 800;
  │      hdr->fb_height = 600;
  │      hdr->fb_pitch  = 3200;
  │      hdr->fb_bpp    = 32;
  │      hdr->rows      = 600 / 16;   // FONT_HEIGHT
  │      hdr->cols      = 800 / 8;    // FONT_WIDTH
  │
  ├─ 7. device_register(DEV_KMS)
  │
  └─ 8. Flip 主循环（不变）
         while (1) {
           if (display_backend_poll()) continue;
           display_backend_wait(16);
         }
```

### VBE MMIO 寄存器常量定义

```c
// bochs-display VBE MMIO 寄存器（通过 BAR0 访问）
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0x0A
#define VBE_DISPI_INDEX_REG_COUNT   0x0B

// 寄存器访问：BAR0 中每寄存器占 2 字节（uint16_t），连续排列
// 寄存器 index 0x00 → offset 0x00，index 0x01 → offset 0x02，以此类推
#define VBE_DISPI_MMIO_OFFSET(idx)  ((idx) * 2)
```

注：`-device bochs-display` 的 BAR0 采用直接 MMIO 模式（寄存器按 index 连续排列，非索引/数据端口模型），`readw`/`writew` 可直接按 offset 访问。

### display_backend_poll() — 更新

flip 循环的 front buffer 来源从 `mmap(MAP_PHYSICAL)` 得到的 BAR1 映射变为直接使用 `front_fb` 指针：

```c
int display_backend_poll() {
    uint32_t g1 = display_hdr->generation;
    if (!display_hdr->dirty_full)
        return 0;

    // 从 display SHM 的 back buffer 拷贝到 BAR1 front buffer
    memcpy(front_fb, back_buffer, fb_size);

    uint32_t g2 = display_hdr->generation;
    if (g1 == g2) {
        display_hdr->dirty_full = 0;  // 没有新 dirty，清零
    }
    // g1 != g2：不清零，让主循环 continue 再拷一次
    return 1;
}
```

### Display SHM 布局（不变）

```
page 0 (offset 0):      display_shm_header (64 bytes) + 保留 (4032 bytes)
page 1+ (offset 4096):  back buffer (fb_height × fb_pitch 字节，页对齐)
```

### 与 Phase 1/2 的关系

- **Phase 1 的 display 协议（`display_shm_header` + back buffer + generation counter）完全不变**——terminal 无感知
- **Phase 2 的 fd 化**与 Phase 3 正交，可独立推进或合并实现
- Phase 3 唯一改变的是 `display_backend_init()` 和 `display_backend_poll()` 中 front buffer 的来源

### 涉及文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `run.sh` | 修改 | `-vga std` → `-vga none -device bochs-display` |
| `boot/stub.c` | 修改 | 删除 `read_gop()` 调用 |
| `kernel/fb.cc` | 删除 | `init_fb` 不再需要 |
| `kernel/fb.h` | 删除 | |
| `kernel/mem/alloc.cc` | 修改 | 删 `init_fb(bi)` 调用 |
| `kernel/trap.cc` | 修改 | 删 `sys_fb_info()` 实现 |
| `common/syscall.h` | 修改 | 删 `SYS_FB_INFO` 定义，保持编号连续 |
| `kernel/proc.cc` | 修改 | 删 `map_fb` 参数 |
| `kernel/proc.h` | 修改 | 删 `map_fb` 参数声明 |
| `driver/kms_driver.cc` | 重构 | PCI 遍历 + BAR mmap + VBE modeset + front buffer 来源切换 |
| | | 不再需要 `map_fb=true` 内核预映射 |

### 后续扩展

- **可变分辨率**：从 display_shm_header 或配置文件读取分辨率参数，传递给 `display_backend_init()` 而非硬编码
- **真实硬件（AMD/Intel）**：基于 PCI 发现 + BAR mmap + MMIO 寄存器访问的同一模式，替换 VBE 初始化逻辑为对应硬件的初始化序列
- **24bpp 支持**：VBE 寄存器设 BPP=24，调整 pitch 计算和 memcpy 路径
