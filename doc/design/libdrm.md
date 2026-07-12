# libdrm.so — 用户态 DRM 接口层

## 当前架构设计

### 概述

用户态程序通过 `libdrm.so` 共享库的标准 `xf86drm.h` / `xf86drmMode.h` API 操作 `/dev/dri/card0` 上的 DRM/KMS ioctl。内核 DRM/KMS 实现见 [kernel/drm.md](kernel/drm.md)，virtio-gpu 命令层见 [virtio_gpu.md](virtio_gpu.md)，virtio transport 见 [virtio.md](virtio.md)。

```
用户态程序（terminal / modetest）
   │  libdrm API: drmModeGetResources, drmModeAddFB, drmModeSetCrtc, ...
   ├─ libdrm.so（xf86drm.c / xf86drmMode.c）
   │    │  ioctl() / open() / mmap() / poll() / read()
   │    ▼
内核 DRM/KMS 层（virtio_gpu.c + drm_internal.h）
   │  virtio-gpu 命令
   ▼
virtio-gpu 命令层 → virtio-pci transport
```

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | libdrm 形态 | **共享库**（libdrm.so） | 多用户态程序共享一份 .text；标准化接口，后续 Mesa/Weston 均依赖此层 |
| 2 | 终端设备打开 | **直接 `open("/dev/dri/card0")`** | `drmOpen()` 依赖 sysfs 路径，直接 open 更少故障点；保留 `drmOpen()` fallback 路径供验证程序使用 |
| 3 | DUMB 操作 | **保留 `drmIoctl()`** | CREATE_DUMB / MAP_DUMB / DESTROY_DUMB 无 libdrm wrapper，`drmIoctl()` 封装了 EINTR 重试优于裸 `ioctl()` |
| 4 | 分辨率发现 | **运行时 `drmModeGetConnector` 动态读取** | 不硬编码 800×600；terminal 初始化时通过 connector mode 列表取 preferred mode |
| 5 | Atomic vs Legacy | **强制 legacy**（`DRM_CAP_ATOMIC=0`） | 避免 atomic TEST_ONLY 状态机复杂性，virtio-gpu 无硬件 test-only 语义 |
| 6 | 属性注册 | **动态注册**（`drm_property_create_*` + blob alloc/free） | 便于将来 atomic/lease 扩展，属性 id 不再硬编码 |
| 7 | 软件光标 | **dumb buffer 上 overlay** 而非硬件 cursor | QEMU virtio-gpu cursorq 支持不完整，软件实现完全可控 |
| 8 | Master 模型 | **per-fd 互斥 + DROP_MASTER 清理** | wlroots 内部多 fd 场景需要互斥 |
| 9 | EDID | **从当前 mode 实时生成 128 字节最小 EDID 1.3** | wlroots 走真实 EDID 路径而非 fallback |
| 10 | 支持的 pixel format | **4 formats: XRGB8888 + ARGB8888 + XBGR8888 + ABGR8888 × LINEAR** | 覆盖 Mesa/wlroots 常见 format 尝试，避免 format 不匹配的 debug 成本 |

### 核心数据结构

用户态程序通过 libdrm 类型操作，内核侧完整数据结构见 `kernel/driver/drm_internal.h`。

**用户态可见结构**（来自 `xf86drmMode.h`）：

- `drmModeRes` — card 资源：count_connectors / count_crtcs / count_encoders / count_fbs + 对应 ID 数组指针
- `drmModeConnector` — 连接器：connection（DRM_MODE_CONNECTED / UNCONNECTED）、count_modes + modes[] 模式列表、count_props + props[] + prop_values[]
- `drmModeModeInfo` — 显示模式：hdisplay / vdisplay / vrefresh / clock + 时序参数
- `drmModeCrtc` — CRTC：crtc_id、fb_id（当前绑定的 fb）、mode（当前模式）、x/y 偏移
- `drmModeFB` — framebuffer：fb_id、width / height、pitch、bpp、depth、handle（dumb handle）
- `drmModePlane` — plane：count_formats + formats[]、crtc_id / fb_id（当前绑定）

**内核侧 key 结构**（`kernel/driver/drm_internal.h`，详见 `kernel/drm.md`）：

- `drm_property` — 属性定义（type: RANGE/ENUM/BLOB/OBJECT, name, flags, enum 值列表）
- `drm_blob` — blob 数据（blob_id, length, data, refcount）
- `drm_object_props` — 对象属性绑定（prop_ids[] + prop_values[] 数组，spinlock）
- `drm_file` — per-fd 状态（is_master, authenticated_magic, created_fb_ids[], created_dumb_handles[]）
- `drm_cursor` — 软件光标（enabled, x/y position, hotspot, 64×64 ARGB buffer）

### 关键流程

#### terminal 显示初始化（display_client_init）

路径：`user/driver/display.h : display_client_init`

1. `open("/dev/dri/card0", O_RDWR)` — 阻塞直到设备就绪
2. `drmSetMaster(fd)` — 获取 master 权限
3. `drmModeGetResources(fd)` → `res` — 查询 connector/crtc/encoder/fb 数量与 ID
4. 遍历 `res->connectors[]`，`drmModeGetConnector(fd, id)` 找到连接状态为 `DRM_MODE_CONNECTED` 且有 mode 的 connector
5. 取 `conn->modes[0]`（preferred mode）作为显示分辨率
6. `drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, ...)` — 分配 dumb buffer
7. `drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, ...)` — 获取 mmap offset
8. `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset)` — 映射到用户态
9. `drmModeAddFB(fd, w, h, 24, 32, pitch, handle, &fb_id)` — 注册 framebuffer
10. `drmModeSetCrtc(fd, 1, fb_id, 0, 0, &conn_id, 1, &mode)` — 绑定 CRTC 显示

#### terminal flush（display_client_flush）

路径：`user/driver/display.h : display_client_flush`

`drmModePageFlip(fd, 1, fb_id, 0, NULL)` → 内核将 cursor overlay 合成到 dumb buffer → `virtio_gpu_transfer_2d` 全帧传输 → `virtio_gpu_flush` 刷新显示。

#### drmOpen 路径（验证程序用）

```
drmOpen("drm", NULL)
  → drmOpenByName → drmOpenDevice("/dev/dri/card0")
    → stat → open → drmGetVersion (DRM_IOCTL_VERSION 返回 name="drm")
    → 匹配 → 返回 fd
```

fallback：`drmOpen` 失败时直接 `open("/dev/dri/card0")`（terminal 始终使用 direct open，只有 `drm_test_link` 等验证程序使用 `drmOpen`）。

#### 软件光标合成

内核 `drm_ioctl_cursor2` 接收 CURSOR2(BO) 设置/更新光标位图，或 CURSOR2(MOVE) 移动光标位置。`drm_ioctl_page_flip` 中在大帧传输前调用 `drm_cursor_overlay()` 将光标 64×64 ARGB 位图按 hotspot 位置 alpha blend 到 framebuffer 的 dumb buffer 上。支持透明像素跳过、不透明像素替换、半透明 alpha blend。

#### Master 互斥

- `SET_MASTER`: per-fd 互斥：当前 fd 已是 master 则幂等，有其他 fd 持有 master 则返回 -EBUSY
- `DROP_MASTER`: 清除当前 fb、取消 pending page flip event、禁用 cursor；不释放 dumb buffer / framebuffer（其生命周期由 refcount 管理）
- `drm_close`: 释放当前 fd 创建的 fb 和 dumb，drop master 并清理
- `AUTH_MAGIC`: 仅 master fd 可认证，且 magic 必须是此前通过 `GET_MAGIC` 分配的值

### 属性模型

内核为每个 KMS 对象注册属性集合，用户态通过 `drmModeGetProperty` / `drmModeGetPropertyBlob` / `drmModeObjectGetProperties` 查询：

| 对象 | 属性 | 类型 | 说明 |
|------|------|------|------|
| Connector (id=2) | DPMS | ENUM (On/Standby/Suspend/Off) | 当前值 = DRM_MODE_DPMS_ON |
| | EDID | BLOB | 128 字节 EDID 1.3，从当前分辨率实时生成 |
| Plane (id=4) | IN_FORMATS | BLOB | 4 formats × LINEAR modifier（XRGB8888, ARGB8888, XBGR8888, ABGR8888） |
| | CRTC_ID | OBJECT (type=CRTC) | 默认 0 |
| | FB_ID | OBJECT (type=FB) | 默认 0 |
| | SRC_X / SRC_Y / SRC_W / SRC_H | RANGE (0～0xFFFFFFFF) | IMMUTABLE |
| CRTC (id=1) | ACTIVE | RANGE (0～1) | 默认 0 |
| | MODE_ID | BLOB | 默认 0 |

### 锁模型

见 `kernel/drm.md` 锁模型部分。用户态不直接操作这些锁。

### 系统调用/接口

#### libdrm.so 导出符号

通过 `build_script/libdrm/libdrm.map` 版本脚本控制，仅导出 `drm*` glob（~60-80 个符号）。

核心连接管理：`drmOpen`, `drmClose`, `drmIoctl`, `drmSetMaster`, `drmDropMaster`, `drmGetVersion`, `drmGetCap`, `drmSetClientCap`, `drmGetMagic`, `drmAuthMagic`

KMS 对象操作（`xf86drmMode.c`）：`drmModeGetResources` / `FreeResources`、`drmModeGetConnector` / `FreeConnector`、`drmModeGetCrtc` / `FreeCrtc`、`drmModeGetEncoder` / `FreeEncoder`、`drmModeGetPlaneResources` / `drmModeGetPlane` / `FreePlane`、`drmModeAddFB` / `drmModeAddFB2` / `drmModeRmFB` / `drmModeGetFB` / `drmModeGetFB2`、`drmModePageFlip`、`drmModeCreateDumbBuffer` / `drmModeDestroyDumbBuffer` / `drmModeMapDumbBuffer`、`drmModeGetProperty` / `FreeProperty` / `drmModeGetPropertyBlob` / `FreePropertyBlob`、`drmModeObjectGetProperties`

#### DRM ioctl 覆盖（terminal 使用路径）

| 用户态调用 | 内核 ioctl |
|-----------|-----------|
| `drmSetMaster(fd)` | `DRM_IOCTL_SET_MASTER` |
| `drmModeGetResources(fd)` | `DRM_IOCTL_MODE_GETRESOURCES`（两次调用：计数 + 填充 ID 缓冲） |
| `drmModeGetConnector(fd, id)` | `DRM_IOCTL_MODE_GETCONNECTOR` |
| `drmIoctl(fd, CREATE_DUMB, ...)` | `DRM_IOCTL_MODE_CREATE_DUMB` |
| `drmIoctl(fd, MAP_DUMB, ...)` | `DRM_IOCTL_MODE_MAP_DUMB` |
| `drmModeAddFB(fd, ...)` | `DRM_IOCTL_MODE_ADDFB` |
| `drmModeSetCrtc(fd, ...)` | `DRM_IOCTL_MODE_SETCRTC` |
| `drmModePageFlip(fd, ...)` | `DRM_IOCTL_MODE_PAGE_FLIP` |

完整内核 ioctl 列表及状态见 `kernel/drm.md`。

### 与内核 DRM 模块的关系

- 用户态通过 libdrm API → `ioctl()` syscall → 内核 `drm_ioctl()` dispatch（`virtio_gpu.c`）
- `mmap` 直接走内核 `dev_ops.mmap`，无 libdrm wrapper
- `poll` + `read` 用于 page flip event 读取（eventfd 路径）
- 内核设备 `/dev/dri/card0` 为内核态设备（`driver_pid=0`），不走 IPC 代理
- 详见 `kernel/drm.md` 第 8 节"设备注册与分发路径"

### 构建结构

libdrm.so CMake target（`user/CMakeLists.txt`）：

- target 名：`drm_so`（`add_user_lib(SHARED)`)
- 源文件：`xf86drm.c` / `xf86drmMode.c` / `xf86drmHash.c` / `xf86drmRandom.c` / `xf86drmSL.c`
- 输出：`libdrm.so`（soname），`DT_NEEDED libc.so`
- 版本脚本：`build_script/libdrm/libdrm.map`（仅导出 `drm*` 符号）
- 编译：`-fPIC`（SHARED 自动追加），`-fvisibility=hidden`
- 依赖：`c_so`（libc.so 先构建）、`libdrm_fourcc_table`（fourcc code 表）

terminal 动态 ELF 最终 `DT_NEEDED` 链：`libdrm.so` + `libinput.so` + `libm.so` + `libc.so`

---

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| modetest 构建目标 | `user/test/modetest.c` 已有源码但未接入 CMake 构建，需新增 `add_user_elf(modetest)` 和 `add_user_dyn_elf(modetest_dyn)` 目标 | 低 |
| hello_drm_dyn 构建目标 | `user/test/hello_drm_dyn.c` 已有源码但未接入 CMake 构建，需新增 `add_user_dyn_elf(hello_drm_dyn)` 目标 | 低 |
| SETPROPERTY / OBJ_SETPROPERTY | 当前返回 `-ENOSYS`，wlroots 调用 `drmModeObjectSetProperty` 设置 DPMS 或 IN_FORMATS 时会失败。需实现属性设置路径（DPMS 可接受 no-op，IN_FORMATS 拒绝不支持的 format） | 中 |
| GEM_FLINK / GEM_OPEN | Mesa kms_swrast 可能调用 `drmModeGetFB` 后走 flink 路径获取全局 handle，当前返回 `-ENOSYS` | 低 |
| PRIME / SYNCOBJ / ATOMIC / LEASE | 阶段 3 高级特性，当前返回 `-ENOSYS`，非 wlroots legacy 路径所必须 | 低 |
| tinywl 交叉编译与验证 | 交叉编译 libdrm + Mesa(kms_swrast) + wlroots + tinywl，部署到 disk.img 启动验证完整 compositor 链路 | 中 |
| drmClose 替代 close | terminal 当前使用 `close(fd)` 而非 `drmClose(fd)`，后者会额外调用 `DROP_MASTER` 并触发资源清理 | 低 |
