# KMS 内核态驱动设计

## 概述

将 KMS 显示驱动从用户态进程搬入内核，与 Linux/Mac 模型对齐：内核负责显示设备初始化、back buffer 分配、page flip；用户态 compositor（terminal）负责像素渲染。

**动机**：
- 与 Linux DRM/KMS 模型一致——Linux 的 KMS 在内核，compositor 在用户态通过 ioctl 请求 flip
- 性能优化——消除 KMS 用户进程的 IPC 上下文切换开销，flip 在内核同步完成

**定位**：KMS 只负责 framebuffer 输出（back buffer → front buffer flip）和显示设备初始化，不负责渲染。Terminal 作为 compositor 负责像素渲染（cell → glyph → 像素 → back buffer），通过 req(fd, FLIP) 请求内核做 flip。此架构与 Linux DRM 模型一致：compositor 渲染像素，内核 KMS 做 scanout/flip。

**不做完整 DRM**：当前实现最小 KMS 功能集（buffer 创建 + flip），不支持 DRM 的 CRTC/Plane/Connector 模型、GEM buffer manager、fence/sync、多客户端仲裁。未来需要时再扩展。

**依赖**：本设计建立在 VFS 重构（devtmpfs + dev_ops）之后。详见 [vfs.md](vfs.md)。

## 架构

```
Shell (PID 6)
  │
  │ stdout pipe
  ▼
Terminal (PID 5, compositor)
  │
  │ 1. 读 pipe → VT100 parse → 更新 cell buffer (内部)
  │ 2. 渲染 cell → glyph → 像素 → 写入 back buffer (mmap'd)
  │ 3. req(fd, DISPLAY_REQ_FLIP) → 内核同步 memcpy back → front
  ▼
内核 KMS (kernel/display.cc)
  │
  │ 同步 memcpy back buffer → front buffer
  ▼
Framebuffer 硬件 (front buffer)
```

KMS 是内核子系统，不是独立进程。Terminal 通过 `/dev/kms` 设备节点与内核 KMS 交互：

```
open("/dev/kms") → fd (devtmpfs inode, dev_ops.kernel_owned=true)
  │
  ├─ req(fd, DISPLAY_REQ_CREATE_BUF, {width, height, bpp})
  │    → 内核分配 back buffer，返回 {pitch, size, rows, cols}
  │
  ├─ mmap(fd, size)
  │    → 映射内核分配的 back buffer（纯像素数据，无 header）
  │
  └─ req(fd, DISPLAY_REQ_FLIP)
     → 内核同步 memcpy back → front，返回成功
```

## Display 协议

### 与 Linux DRM 的对应关系

| Linux DRM | 本 OS | 说明 |
|-----------|-------|------|
| `/dev/dri/card0` | `/dev/kms` | devtmpfs 设备节点 |
| `DRM_IOCTL_MODE_CREATE_DUMB` | `req(fd, DISPLAY_REQ_CREATE_BUF)` | compositor 指定分辨率，内核分配 buffer |
| `DRM_IOCTL_MODE_MAP_DUMB` | `mmap(fd, size)` | 映射纯像素 buffer（当前只有一个 buffer，无需 handle→offset 间接层） |
| `DRM_IOCTL_MODE_PAGE_FLIP` | `req(fd, DISPLAY_REQ_FLIP)` | 同步 flip（未来扩展异步+vblank） |
| 内核 DRM driver 分配 buffer | 内核 display.cc 分配 | 物理页分配，compositor mmap |
| 内核 CRTC page flip | 内核 memcpy back→front | 当前软件 flip，未来硬件 page flip |

### req 协议定义

```c
// driver/display.h

// ===== Display request 常量 =====
#define DISPLAY_REQ_CREATE_BUF  1   // 创建 back buffer
#define DISPLAY_REQ_FLIP        2   // 请求 page flip

// ===== CREATE_BUF request =====
struct display_create_buf_req {
    uint32_t width;      // compositor 指定（当前只接受 800）
    uint32_t height;     // compositor 指定（当前只接受 600）
    uint32_t bpp;        // compositor 指定（当前只接受 32）
};

// ===== CREATE_BUF response =====
struct display_create_buf_resp {
    uint32_t pitch;      // 内核返回（width * bytes_per_pixel）
    uint32_t size;       // 内核返回（pitch * height）
    uint32_t rows;       // 内核返回（height / FONT_HEIGHT）
    uint32_t cols;       // 内核返回（width / FONT_WIDTH）
    int32_t  result;     // 0=成功, -EINVAL=不支持分辨率
};

// ===== FLIP request =====
// 无额外参数，req type = DISPLAY_REQ_FLIP 即可

// ===== FLIP response =====
struct display_flip_resp {
    int32_t result;      // 0=成功
};
```

### Buffer 所有权

与 Linux DRM 模型一致，**内核分配 back buffer**。Compositor 通过 `req(fd, CREATE_BUF)` 请求分配，通过 `mmap(fd)` 映射访问。

- 内核是 flip 引擎，了解硬件约束，拥有 buffer 生命周期
- 与 Linux 一致：DRM 分配 framebuffer object，compositor 通过 ioctl 请求分配
- Compositor 不调 `sys_fb_info`——元信息从 CREATE_BUF response 获取

### Buffer 布局

纯像素 buffer，无 header 嵌入。与 Linux DRM `CREATE_DUMB` 一致：buffer 是纯像素数据，元信息来自 ioctl/response。

```
back buffer: fb_height × fb_pitch 字节，纯像素数据（32bpp RGBA）
```

 mmap(fd) 直接返回指向 buffer 起始地址的指针，无 header 页。

### 分辨率协商

Compositor 在 CREATE_BUF req 中指定 width/height/bpp。内核验证是否支持，当前只接受 800×600×32，其他返回 `-EINVAL`。

未来扩展可变分辨率时：
- 内核维护支持的 mode 列表
- Compositor 可 req(fd, DISPLAY_REQ_GET_MODES) 查询
- CREATE_BUF 验证逻辑改为查 mode 列表

### Bpp 支持

当前仅支持 32bpp。CREATE_BUF req 传 bpp=32，内核验证非 32 则返回 `-EINVAL`。未来真机适配时再加 24bpp。

### 光标渲染

Compositor（terminal）负责所有像素渲染，包括光标。光标在 back buffer 上渲染（反色块或下划线），内核 KMS 只做 flip，不知道光标。

## req 分发路径

基于 VFS devtmpfs + dev_ops 模型。`/dev/kms` 是 devtmpfs inode（`INODE_DEV`），`dev_ops` 标记为内核设备：

```c
// kernel/devtmpfs.c — KMS 设备注册
struct dev_ops kms_dev_ops = {
    .driver_pid = 0,           // 0 = 内核设备，不走 IPC 代理
    .device_type = DEV_KMS,
};
dev_create("/dev/kms", S_IFCHR, &kms_dev_ops);
```

`sys_req` 分发逻辑：

```c
// kernel/trap.c — sys_req 内部
if (fd->type == FD_DEV) {
    dev_ops *ops = (dev_ops*)fd->inode->i_priv;
    if (ops->driver_pid == 0) {
        // 内核设备：直接处理，不走 IPC
        return kernel_dev_req_dispatch(fd, req_type, req_data, req_len,
                                       resp_data, resp_len);
    } else {
        // 用户态驱动：IPC 代理转发（现有逻辑）
        return ipc_proxy_req(fd, ...);
}
```

`kernel_dev_req_dispatch` 内部根据 `ops->dev_type` 分发：

```c
int kernel_dev_req_dispatch(...) {
    if (ops->device_type == DEV_KMS) {
        return display_req_handler(req_type, req_data, req_len, resp_data, resp_len);
    }
    return -ENODEV;
}
```

`sys_mmap` 同理：发现 `driver_pid == 0` 的 FD_DEV → 映射内核分配的 display buffer 物理页到用户 PML4。

## IPC 拓扑

```
kbd_driver(PID3) ── SHM + req ──→ terminal(PID5) ── pipe ──→ shell(PID6)
                                        │
                                        │ open("/dev/kms") → fd
                                        │ req(fd, CREATE_BUF) → back buffer 元信息
                                        │ mmap(fd) → back buffer 映射
                                        │ req(fd, FLIP) → 内核 flip
                                        ▼
                                 内核 KMS (display.cc)
                                        │
                                        │ 同步 memcpy
                                        ▼
                                 framebuffer (front buffer)
```

KMS 不再是用户进程，与 kbd_driver 完全解耦。Terminal 交互对象从两个用户进程变为：kbd_driver（用户态 SHM）+ 内核 KMS（req + mmap）。

## 内核实现

### kernel/display.c + kernel/display.h（新增）

```c
// kernel/display.h

// 内核 display 子系统
struct display_state {
    uint8_t  *front_fb;        // front buffer 内核虚拟地址（BAR1 映射或 init_fb 映射）
    uint8_t  *back_buffer;     // back buffer 内核虚拟地址（内核分配）
    uint64_t  back_buffer_phys; // back buffer 物理地址（mmap 映射时需要）
    uint32_t  fb_width;        // 当前 mode
    uint32_t  fb_height;
    uint32_t  fb_pitch;
    uint32_t  fb_bpp;
    uint32_t  fb_size;         // pitch * height
    uint32_t  rows;
    uint32_t  cols;
    bool      initialized;     // back buffer 是否已分配
};

// 初始化：映射 front buffer（当前从 bootloader fb_info，未来 PCI VBE）
void display_init(boot_info_t *bi);

// req 处理：CREATE_BUF / FLIP
int display_req_handler(uint32_t req_type, void *req_data, uint32_t req_len,
                        void *resp_data, uint32_t resp_len);

// mmap 处理：映射 back buffer 到用户 PML4
int display_mmap_handler(proc_t *proc, uint64_t size, uint64_t *out_vaddr);

// devtmpfs 注册
void display_dev_register();
```

### display_req_handler 实现

```c
int display_req_handler(uint32_t req_type, ...) {
    switch (req_type) {
    case DISPLAY_REQ_CREATE_BUF: {
        display_create_buf_req *req = (display_create_buf_req *)req_data;
        // 验证分辨率（当前硬编码只接受 800×600×32）
        if (req->width != 800 || req->height != 600 || req->bpp != 32)
            return -EINVAL;
        // 分配 back buffer
        g_display.back_buffer = alloc_contiguous_pages(g_display.fb_size);
        g_display.back_buffer_phys = vaddr_to_phys(g_display.back_buffer);
        g_display.initialized = true;
        // 构造 response
        display_create_buf_resp *resp = (display_create_buf_resp *)resp_data;
        resp->pitch  = g_display.fb_pitch;
        resp->size   = g_display.fb_size;
        resp->rows   = g_display.fb_height / FONT_HEIGHT;
        resp->cols   = g_display.fb_width / FONT_WIDTH;
        resp->result = 0;
        return 0;
    }
    case DISPLAY_REQ_FLIP: {
        if (!g_display.initialized)
            return -ENOENT;
        // 同步 memcpy back → front
        memcpy(g_display.front_fb, g_display.back_buffer, g_display.fb_size);
        display_flip_resp *resp = (display_flip_resp *)resp_data;
        resp->result = 0;
        return 0;
    }
    default:
        return -EINVAL;
    }
}
```

### display_mmap_handler 实现

```c
int display_mmap_handler(proc_t *proc, uint64_t size, uint64_t *out_vaddr) {
    if (!g_display.initialized)
        return -ENOENT;
    // 映射 back buffer 物理页到用户地址空间
    *out_vaddr = map_physical_pages_to_user(proc, g_display.back_buffer_phys,
                                            g_display.fb_size);
    return 0;
}
```

### kernel/fb.c（保留，职责缩减）

`init_fb` 保留——映射 front buffer 物理页到内核地址空间（从 bootloader 获取 fb 物理地址）。未来 PCI VBE Phase 后，`init_fb` 改为从 PCI BAR1 获取。

```c
// kernel/fb.c — 只做 front buffer 物理页映射
void init_fb(boot_info_t *bi) {
    // 从 bootloader 获取 framebuffer 物理地址
    uint64_t fb_phys = bi->fb_addr;
    uint64_t fb_size = bi->fb_size;
    // 映射到内核 high-half 地址空间（pdpt_hh，2MB huge pages）
    front_fb_vaddr = map_physical_to_kernel(fb_phys, fb_size);
    // 保存到 g_display
    g_display.front_fb = front_fb_vaddr;
    g_display.fb_width  = bi->fb_width;
    g_display.fb_height = bi->fb_height;
    g_display.fb_pitch  = bi->fb_pitch;
    g_display.fb_bpp    = bi->fb_bpp;
    g_display.fb_size   = fb_size;
}
```

### sys_fb_info — 删除

`SYS_FB_INFO` syscall 不再需要——内核内部直接有 `g_display`，元信息通过 CREATE_BUF req response 传递给 compositor，不需要单独 syscall。

### sys_req — 扩展

`sys_req` 增加 `driver_pid == 0` 的内核设备分发路径。详见 [req 分发路径](#req-分发路径)。

### sys_mmap — 扩展

`sys_mmap` 对 FD_DEV（`driver_pid == 0`）增加内核设备 mmap 路径，调用 `display_mmap_handler` 映射 back buffer。

## 组件改动

### Terminal 改动

- **`display_client_init()`**：改为 `open("/dev/kms")` + `req(fd, CREATE_BUF, {800,600,32})` + 存元信息到本地变量 + `mmap(fd, size)`
- **`display_client_flush()`**：改为 `req(fd, DISPLAY_REQ_FLIP)`（同步 flip，内核 memcpy 后返回）
- **删除**：对 `display_shm_header` 的依赖（`dirty_full` / `generation` / `backend_sleeping`）
- **保留不变**：`display_client_render_cell()`、`display_client_clear()`、`display_client_scroll_up()`、`display_client_set_cursor()`——仍操作 mmap'd back buffer 指针
- **scroll**：terminal 直接在 back buffer 上 `memmove` 像素行（不变）
- **光标**：terminal 在 back buffer 上渲染光标像素（不变）
- **元信息来源**：从 CREATE_BUF response 存本地变量，不从 buffer header 读

### driver/display.h（重构）

统一文件，包含 req 常量 + request/response 结构体 + client API。**Backend API 删除**（内核内部，不暴露）。

```c
// ===== Display request 常量 =====
#define DISPLAY_REQ_CREATE_BUF  1
#define DISPLAY_REQ_FLIP        2

// ===== Request/Response 结构体 =====
struct display_create_buf_req { ... };
struct display_create_buf_resp { ... };
struct display_flip_resp { ... };

// ===== Client API（compositor 侧）=====

// 初始化：open("/dev/kms") + req(CREATE_BUF) + mmap(fd)
// 元信息存到本地变量，back_buffer 指针从 mmap 获得
int display_client_init();

// 渲染单个 cell 到 back buffer（纯像素操作，不涉及通信）
void display_client_render_cell(uint32_t row, uint32_t col,
                                uint8_t ch, uint32_t fg, uint32_t bg);

// 清空 back buffer（填 bg 颜色）
void display_client_clear(uint32_t bg);

// 在 back buffer 上执行 scroll（memmove 像素行）
void display_client_scroll_up(uint32_t bg);

// 设置光标位置（纯本地状态，供 VT100 层使用）
void display_client_set_cursor(uint32_t x, uint32_t y);

// 请求内核 flip：req(fd, DISPLAY_REQ_FLIP)
// 同步 memcpy back→front，调用返回时 flip 已完成
void display_client_flush();
```

### common/font.h（保留）

`font8x16[96][16]` 字体表，terminal 像素渲染使用。KMS 进程删除后，只有 terminal 使用，但仍作为共享资源保留（未来 Wayland compositor 也需要）。

### driver/kms_driver.cc（删除）

KMS 用户态进程不再存在，整文件删除。

### common/shm.h（清理）

- **删除**：`kms_fb_info` 结构体（fb 元信息改由 CREATE_BUF response 提供）
- **保留**：`driver_shm_header`（kbd SHM）、`kbd_ring`、`kbd_msg`

### init/init.c（修改）

- **删除**：`spawn_service("/driver/kms.dev")` 和 `wait_dev_ready("/dev/kms")` —— KMS 不再是用户进程
- `/dev/kms` 由内核在启动时通过 devtmpfs 注册，compositor `open("/dev/kms")` 即可获取

### CMakeLists.txt（修改）

- **删除**：`kms_driver.elf` 构建目标
- **新增**：`kernel/display.cc` 作为内核源文件

## 性能分析

### 与旧方案对比

旧方案（KMS 用户进程）：
- Terminal flush：写 `dirty_full` + 递增 `generation` + `notify_fd` → KMS 进程唤醒 → schedule → switch_to → KMS 读 generation → memcpy → schedule → switch_to → 返回 terminal
- 一次 flip = 2 次上下文切换 + IPC 通知开销

新方案（内核 KMS）：
- Terminal flush：`req(fd, FLIP)` → 内核 trap → memcpy → trapret
- 一次 flip = 0 次上下文切换，纯 syscall 开销

### 常规输出

Terminal 渲染像素到 back buffer + `req(fd, FLIP)` 同步 flip。减少消息开销和上下文切换。

### scroll 场景

Terminal back buffer memmove（1.92MB）+ `req(fd, FLIP)` 内核 memcpy back→front（1.92MB）= 共 3.84MB 内存操作 + 1 次 syscall。无上下文切换。

## 已知限制

- **撕裂**：内核 memcpy 期间硬件同时在 scanout，可能产生半新半旧的撕裂帧。当前 QEMU 验证无影响，真机撕裂留待 VSync 扩展解决
- **32bpp only**：当前仅支持 32bpp，真机适配时再加 24bpp
- **硬编码分辨率**：800×600×32，CREATE_BUF 验证只接受此值
- **QEMU bochs-display only**：当前只支持 PCI 1234:1111（PCI VBE Phase 后），真机需额外驱动
- **单 buffer**：只支持一个 back buffer per fd，不支持多 buffer（double/triple buffering）
- **单 compositor**：只支持一个 compositor 连接 `/dev/kms`

## 实现分步

### Step 1：KMS 搬入内核（保持 bootloader fb）

KMS 用户态进程功能搬入内核，front buffer 来源仍从 bootloader fb_info。Terminal 改用 req + mmap 模型。

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `kernel/display.c` | 新增 | display 子系统（buffer 分配 + flip + req 处理 + mmap 处理） |
| `kernel/display.h` | 新增 | display_state 结构体 + 接口声明 |
| `kernel/fb.c` | 修改 | 保留 init_fb（front buffer 映射），删 g_fb_info/sys_fb_info 相关 |
| `kernel/fb.h` | 修改 | 精简，只保留 init_fb 声明 |
| `kernel/trap.c` | 修改 | 删 sys_fb_info；扩展 sys_req/sys_mmap 支持内核设备分发 |
| `kernel/proc.h` | 修改 | struct file 支持内核设备 fd（基于 VFS dev_ops） |
| `kernel/kernel.c` | 修改 | 加 display_init(bi) 调用 |
| `common/syscall.h` | 修改 | 删 SYS_FB_INFO |
| `driver/display.h` | 重构 | 删 display_shm_header/backend API，加 req 常量 + struct + 新 client API |
| `driver/kms_driver.cc` | 删除 | 整文件删除 |
| `driver/terminal.cc` | 修改 | 改用 req(fd, CREATE_BUF) + mmap(fd) + req(fd, FLIP) |
| `init/init.c` | 修改 | 删 KMS spawn + wait |
| CMakeLists.txt | 修改 | 删 kms_driver.elf，加 kernel/display.cc |

### Step 2：PCI VBE 原生显示驱动

内核自主初始化显示设备（PCI ECAM 遍历 + BAR mmap + VBE modeset），移除 bootloader GOP 依赖。

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `kernel/display.c` | 修改 | display_init 改为 PCI 遍历 + BAR1 mmap + VBE modeset |
| `kernel/fb.c` | 修改 | init_fb 改为从 PCI BAR1 获取 fb 物理地址（或合并到 display_init） |
| `run.sh` | 修改 | `-vga std` → `-vga none -device bochs-display` |
| `boot/stub.c` | 修改 | 删除 `read_gop()` 调用 |
| `kernel/kernel.cc` | 修改 | display_init 不再依赖 boot_info fb 字段 |

PCI VBE 寄存器常量定义（Step 2 时使用）：

```c
// kernel/display.h — VBE MMIO 寄存器（bochs-display BAR0）
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
#define VBE_DISPI_MMIO_OFFSET(idx)  ((idx) * 2)
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40
```

## 将来扩展

- **异步 flip + VSync/VBLANK**：req(fd, FLIP) 立即返回，内核 deferred flip（下次 vblank 时）。需要显卡 IRQ + vblank callback 机制
- **硬件 page flip**：当前只能软件 flip（memcpy），若后续驱动真实 GPU，可改用硬件 page flip（写 CRTC 寄存器切换扫描地址，零拷贝）
- **行级 dirty / damage region**：Wayland compositor 场景下 req(fd, FLIP) 可附带 dirty 区域参数，内核只拷 dirty 部分
- **多 buffer**：req(fd, CREATE_BUF) 支持创建多个 dumb buffer，req(fd, MAP_BUF, handle) 返回 mmap offset，支持 double/triple buffering
- **完整 DRM**：CRTC/Plane/Connector 模型 + GEM buffer manager + fence/sync + 多客户端仲裁
- **可变分辨率**：CREATE_BUF 验证改为查 mode 列表，支持 compositor 请求切换分辨率
- **PAT / write-combining**：framebuffer 页映射加 PCD/PAT 标记，优化真机性能
- **Huge page 映射**：framebuffer 映射改用 2MB huge page，减少 TLB miss
- **24bpp 支持**：CREATE_BUF bpp 参数支持 24，内核分配 buffer 调整 pitch
- **其他真实显卡**：基于 PCI 发现 + BAR mmap + MMIO 寄存器访问模式，替换 VBE 初始化逻辑
