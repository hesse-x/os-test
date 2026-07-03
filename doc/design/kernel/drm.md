# DRM/KMS 内核态驱动（virtio-gpu 后端）

## 概述

显示路径：terminal（用户态，`driver/display.h`）通过 `/dev/dri/card0` 上的 DRM ioctl → 内核 DRM/KMS 层 → virtio-gpu 命令层 → virtio-pci modern transport。virtio transport 见 [virtio.md](virtio.md)，virtio-gpu 命令层见 [virtio_gpu.md](virtio_gpu.md)。

## 架构

```
terminal (用户态, driver/display.h)
   │  DRM ioctl on /dev/dri/card0
   ▼
DRM/KMS 层 (kernel/driver/virtio_gpu.c DRM 部分 + drm_internal.h)
   │  virtio-gpu 命令
   ▼
virtio-gpu 命令层 (kernel/driver/virtio_gpu.c 命令部分)
   │  split virtqueue + MSI-X
   ▼
virtio-pci modern transport (kernel/driver/virtio_pci.c + virtio_ring.c)
```

- **传输层**：virtio-pci modern + split virtqueue（`virtio_pci.c`/`virtio_ring.c`）
- **命令层**：virtio-gpu ctrlq（`virtio_gpu.c` 命令部分）
- **DRM/KMS**：A+B 层次（`virtio_gpu.c` DRM 部分 + `drm_internal.h`）
- **设备节点**：`/dev/dri/card0`（内核态，`driver_pid=0`，devtmpfs 子目录 `dri/`）

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | DRM 在内核还是用户态 | 内核 | 与 Linux DRM/KMS 模型一致；virtio-gpu 命令需同步等待中断，内核态 sleep/wake 最直接 |
| 2 | 完整 DRM vs 最小 KMS | A+B 层次（dumb buffer + framebuffer + CRTC/connector/encoder/plane 静态拓扑 + page flip） | terminal 只需 scanout，但走标准 DRM ioctl 路径以便 libdrm/mesa 接入 |
| 3 | 谁分配 dumb buffer | 内核 | 与 Linux DRM 一致：内核分配 buffer、管理生命周期；用户态通过 CREATE_DUMB + MAP_DUMB + mmap 访问 |
| 4 | flip 机制 | PAGE_FLIP（transfer 全帧 + flush，flags=0 fire-and-forget） | virtio-gpu 无真实 vblank，立即 event 无意义；terminal flush 路径用同步语义最简单 |
| 5 | 分辨率 | 硬编码 800×600×32 | virtio-gpu `num_scanouts` 读出的固定 mode；可变分辨率留待 EDID 协商（阶段 2）。当前硬编码但**保留修改路径**：ioctl 用标准 DRM struct 不缩水、内核"当前 mode"存成字段不散落成常量、`drmModeGetConnector` 返回 mode 列表（即使只有 1 项）、`drmModeSetCrtc` 走"在支持列表里查找匹配 mode → 应用"路径——将来加 EDID/多分辨率时只扩展 mode_list 来源，set 路径不变 |
| 6 | 显卡发现方式 | PCI vendor:device ID（0x1AF4:0x1050）+ subsystem id=2 | virtio-gpu-pci 是 non-transitional 设备，device_id=0x1050；用 subsystem id=2 区分 virtio-gpu 与其它 virtio 设备 |
| 7 | dumb buffer 物理内存 | `bfc_alloc_page_data`（连续物理页） | ATTACH_BACKING 需 guest 物理地址；TRANSFER_TO_HOST_2D 由 host 拷贝，连续物理页最简单 |
| 8 | 内核设备分发路径 | ioctl（非 sys_req） | `/dev/dri/card0` 注册为内核设备（`driver_pid=0`），`sys_ioctl` 直接调 `ops->ioctl`，无需 IPC 代理 |
| 9 | mmap 机制 | MAP_DUMB 返回 `offset = handle << 12`，mmap 解码 handle | 复用现有 `dev_ops.mmap` + `mmap_region_t` 清理机制；offset 编码避免新增查找表 |

## DRM ioctl 覆盖

内核实现入口为 `drm_ioctl(uint32_t cmd, void *arg)`（`virtio_gpu.c`）。

### 必须实现（terminal 显示路径核心）

| ioctl | 作用 |
|---|---|
| `DRM_IOCTL_VERSION` | 返回 driver/version，libdrm 探测 |
| `DRM_IOCTL_GET_CAP` | 返回能力位（`DRM_CAP_DUMB_BUFFER` 等） |
| `DRM_IOCTL_SET_CLIENT_CAP` | 实现 `DRM_CLIENT_CAP_UNIVERSAL_PLANES` |
| `DRM_IOCTL_SET_MASTER` / `DROP_MASTER` | master 权限，严格对齐 Linux |
| `DRM_IOCTL_MODE_GETRESOURCES` | 列出 CRTC/connector/encoder/fb id |
| `DRM_IOCTL_MODE_GETPLANERES` | 列出 plane id |
| `DRM_IOCTL_MODE_GETPLANE` | plane 详情 |
| `DRM_IOCTL_MODE_GETCRTC` | CRTC 详情 + 当前 mode/fb |
| `DRM_IOCTL_MODE_SETCRTC` | 设置 CRTC 的 fb + mode |
| `DRM_IOCTL_MODE_GETCONNECTOR` | connector 详情 + mode 列表 |
| `DRM_IOCTL_MODE_GETENCODER` | encoder 详情 |
| `DRM_IOCTL_MODE_CREATE_DUMB` | 分配 dumb buffer |
| `DRM_IOCTL_MODE_MAP_DUMB` | 返回 mmap offset |
| `DRM_IOCTL_MODE_DESTROY_DUMB` | 释放 dumb buffer |
| `DRM_IOCTL_MODE_ADDFB` | dumb 包成 framebuffer（旧式，terminal 当前使用） |
| `DRM_IOCTL_MODE_ADDFB2` | **Mesa kms_swrast 必须**（只用 ADDFB2，不用旧式 ADDFB） |
| `DRM_IOCTL_GEM_CLOSE` | **Mesa kms_swrast 必须**（ADDFB2 返回的 handle 用完需关闭） |
| `DRM_IOCTL_GET_MAGIC` | **Mesa/Weston 必须**（libdrm 初始化时调用） |
| `DRM_IOCTL_AUTH_MAGIC` | **Mesa/Weston 必须**（drm auth 流程） |
| `DRM_IOCTL_MODE_GETFB` / `GETFB2` | Mesa kms_swrast 初始化时可能调用 |
| `DRM_IOCTL_MODE_RMFB` | 销毁 framebuffer |
| `DRM_IOCTL_MODE_PAGE_FLIP` | 异步 flip + event |
| `DRM_IOCTL_MODE_DIRTYFB` | dirty rect 标记（复用现有 dirty-rect 优化） |

### 必须 stub（返回 -ENOSYS，不致命）

- `DRM_IOCTL_MODE_GETPROPERTY` / `GETPROPROB` / `SETPROPERTY` — KMS 属性（阶段 2 实现）
- `DRM_IOCTL_MODE_OBJ_GETPROPERTIES` / `SETPROPERTY` — 对象属性（阶段 2 实现）
- `DRM_IOCTL_MODE_CURSOR` / `CURSOR2` — cursor plane（阶段 2 实现，Weston 需要）
- `DRM_IOCTL_PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE` — GEM 跨进程（阶段 3）
- `DRM_IOCTL_GEM_FLINK` / `GEM_OPEN` — GEM flink（阶段 3）
- `DRM_IOCTL_SYNCOBJ_*` — sync object（阶段 3）
- `DRM_IOCTL_MODE_ATOMIC` — atomic commit（阶段 3）
- `DRM_IOCTL_MODE_CREATE_LEASE` / LIST_LESSEES / GET_LEASE / REVOKE_LEASE — DRM lease（阶段 3）

注意：stub 不要错误地返回 0 + 空 struct，那会让 libdrm 误判能力。

## dumb buffer + framebuffer 模型

独立对象、各自 refcount：

- `CREATE_DUMB` → 分配 `drm_dumb_buffer`（handle，refcount=1），同时创建 virtio-gpu 2D resource + attach backing
- `MAP_DUMB` → 返回 `offset = handle << 12`（mmap 解码用）
- `ADDFB` → 创建 `drm_framebuffer`（fb_id，引用 dumb handle，dumb refcount++）
- `RMFB` → fb refcount--，dumb refcount--
- `DESTROY_DUMB` → dumb refcount--，=0 才释放

数据结构定义在 `kernel/driver/drm_internal.h`，全局实例 `g_drm`。

## KMS 对象拓扑

1:1 硬编码的最小对象树：

- 1 CRTC（id=1）
- 1 plane（id=4）
- 1 connector（id=2，VIRTUAL 类型）
- 1 encoder（id=3，VIRTUAL 类型）
- mode 固定 800×600@60（存成列表，预留修改路径）

## mmap 机制

- `MAP_DUMB` 返回 `offset = handle << 12`
- `mmap(fd, size, ..., offset)` → `drm_mmap_handler(proc, size)` 按 size 匹配 dumb buffer，映射其物理页到用户空间
- 复用 `mmap_region_t` 清理机制

## page flip 语义

- `PAGE_FLIP`（flags=0）：transfer 全帧 + flush，立即返回（fire-and-forget）
- `PAGE_FLIP`（flags=DRM_MODE_PAGE_FLIP_EVENT）：transfer + flush 后置 `event_pending`，`poll(POLLIN)` 可读，`read()` 返回 `drm_event_vblank`

## 设备注册与分发路径

`/dev/dri/card0` 由 `drm_dev_register()` 创建（在 `virtio_gpu_init` 末尾调用，`driver_init` 期间），注册为内核设备（`driver_pid=0`）。`sys_ioctl` 对内核设备直接调 `ops->ioctl`；`sys_mmap` 对内核设备调 `ops->mmap`；`sys_poll`/`sys_read` 同理走 `ops->poll`/`ops->read`（page flip event 通路）。

## 关键流程

### terminal 初始化（display_client_init）

1. `open("/dev/dri/card0")`
2. `SET_MASTER` + `SET_CLIENT_CAP(UNIVERSAL_PLANES)`
3. `CREATE_DUMB(800×600×32)` → dumb handle + pitch + size
4. `MAP_DUMB` → offset
5. `mmap(size, ..., fd, offset)` → back buffer
6. `ADDFB` → fb_id
7. `SETCRTC(crtc_id=1, fb_id, mode)` → 内核调 `virtio_gpu_set_scanout` 把 resource 绑到 scanout 0

### terminal flush（display_client_flush）

`PAGE_FLIP(crtc_id=1, fb_id, flags=0)` → 内核 `virtio_gpu_transfer_2d`（全帧）+ `virtio_gpu_flush`，立即返回。

## Mesa kms_swrast 接入计划

Mesa 的 `kms_swrast` gallium 驱动（软件渲染）只走标准 KMS ioctl，不走 `DRM_VIRTGPU_*` 私有 ioctl。它通过 core libdrm 的 `xf86drm.c`/`xf86drmMode.c` 直接调 `/dev/dri/card0` 上的 KMS ioctl，CPU 渲染到 dumb buffer，page flip 到显示。

接入目标状态：交叉编译 libdrm + Mesa(kms_swrast) → Westo 或其他 Wayland compositor 能以 drm-backend 输出 → 走内核 KMS。

### 阶段 1：内核 KMS 补齐（当前项）

**确认已有且已验证：** RESOURCES/CRTC/CONNECTOR/ENCODER/PLANE 查询、CREATE_DUMB/MAP_DUMB/DESTROY_DUMB、ADDFB/RMFB、PAGE_FLIP、DIRTYFB、SET_MASTER/DROP_MASTER、GET_CAP/SET_CLIENT_CAP

**新增：**

| 接口 | 优先级 | 说明 |
|---|---|---|
| `ADDFB2` | P0 必须 | Mesa 只用 `drmModeAddFB2`，永远不会走旧式 ADDFB。结构体 `drm_mode_fb_cmd2` 已在 `common/drm.h` 定义，补 `case DRM_IOCTL_MODE_ADDFB2` 分发即可 |
| `GET_MAGIC` | P0 必须 | libdrm `drmGetMagic()` 获取 magic 编号。分配自增的 32-bit magic 存入进程关联表 |
| `AUTH_MAGIC` | P0 必须 | libdrm `drmAuthMagic()` 认证。master 进程验证 magic 并标记 fd 为 authenticated。未 auth 的 fd 拒绝 PRIME/一些操作 |
| `GEM_CLOSE` | P0 必须 | Mesa 在 `kms_swrast_display_init` 中 ADDFB2 后立即 `drmModeCloseFB`（走 GEM_CLOSE 释放 handle）。当前 dumb buffer 有 refcount 机制，GEM_CLOSE 做 dumb refcount-- 即可 |
| `GETFB` / `GETFB2` | P1 需要 | Mesa `make_current` 等路径通过 `drmModeGetFB2` 查询当前 fb 信息。可以先用 GETFB，返回名称/handle/pitch |

**需要确认的遗留问题：**

- **Page flip event read 路径** — PAGE_FLIP 带 `DRM_MODE_PAGE_FLIP_EVENT` 标志时，需在完成时往 drm fd 写 `drm_event_vblank`，libdrm/Weston 通过 `poll(POLLIN)` + `read()` 收取。当前 event_pending + drm_poll 路径需验证
- **`ADDFB2 像素格式`** — Mesa kms_swrast 默认用 `DRM_FORMAT_XRGB8888`（=0x34325258）。确认内核 DRM 不拒绝该格式
- **drmModeGetResources 返回值** — libdrm 的 drmModeGetResources 期望 `count_fbs` 可正确遍历。当前 ADDFB 把 fb_id 放入 `g_drm.fbs[]`，但 `count_fbs` 和 `fb_id_ptr` 的填充路径需确认

### 阶段 2：Weston 完整支持

Mesa kms_swrast 驱动跑通后，Weston 的 drm-backend 还需：

| 接口 | 说明 |
|---|---|
| `DRM_IOCTL_MODE_CURSOR2` | Weston 用 hardware cursor 实现指针渲染，需 `drmModeSetCursor2`（含 hotspot） |
| `DRM_IOCTL_MODE_GETPROPERTY` | 查询 connector/CRTC/plane 支持的属性。至少实现 connector 属性：`EDID`、`DPMS` |
| `DRM_IOCTL_MODE_GETPROPROB` | 读取属性 blob（如 EDID 数据）。当前 stub 返回 -ENOSYS 可能使 Weston 退化为无 EDID 模式，但不致命 |
| `DRM_IOCTL_MODE_SETPROPERTY` | 设置 connector 属性（如 DPMS On/Off） |
| `DRM_IOCTL_MODE_OBJ_GETPROPERTIES` / `SETPROPERTY` | 通用对象属性查询/设置（atomic 的前提） |
| **DRM master 模型完善** | 多个 fd 间的 master/lease、auth 校验、drop master 时清理 |

**非必须（可以不实现）：**

| 接口 | 理由 |
|---|---|
| `DRM_IOCTL_MODE_ATOMIC` | Weston 可退化为 legacy 模式（`--use-legacy`） |
| `DRM_IOCTL_SYNCOBJ_*` | 非 atomic 模式下不需要 |
| `DRM_IOCTL_PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE` | 单进程显示不需要 dma-buf 共享 |

### 阶段 3：高级特性

- `PRIME` — dma-buf 跨进程共享
- `SYNCOBJ` — fence 同步
- `ATOMIC` — atomic mode setting
- `LEASE` — DRM lease（虚拟机多显示/沙箱）
- `GEM_FLINK` / `GEM_OPEN` — 全局名称共享

### 交叉编译依赖

```
libdrm (core only: xf86drm.c + xf86drmMode.c + libdrm_macros)
  └── 关掉所有 driver-specific：-Damdgpu=false -Dintel=false -Dradeon=false
      -Dnouveau=false -Dvmwgfx=false -Detnaviv=false -Dexynos=false
      -Dfreedreno=false -Domap=false -Dtegra=false -Dvc4=false
  注：不需要 libdrm_virtio（3D 加速才需要）

Mesa (kms_swrast gallium driver)
  ├── 依赖：libdrm, libexpat（xmlconfig）
  ├── 配置：-Dgallium-drivers=swrast -Dvulkan-drivers=[]
  └── 输出：libGL.so + libEGL.so + libgbm.so
```

## 与其他模块的关系

- [virtio.md](virtio.md)：virtio transport（virtio-pci modern + split virtqueue）
- [virtio_gpu.md](virtio_gpu.md)：virtio-gpu 命令层 + buffer 模型
- [vfs.md](vfs.md)：devtmpfs 子目录支持（`dri/card0`）
- [proc.md](proc.md)：DRM open/close 绑定进程，mmap_region_t 生命周期随进程
- [ipc.md](ipc.md)：DRM 为内核设备（`driver_pid=0`），不走 IPC 代理
