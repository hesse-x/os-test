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

接入目标状态：交叉编译 libdrm + Mesa(kms_swrast) → tinywl（wlroots example compositor）能以 drm-backend 输出 → 走内核 KMS。

### 阶段 1：内核 KMS 补齐（已完成 ✅）

**已实现且通过回归测试（19/19 PASS）：**

`drm_ioctl()` switch 中的处理函数：

| ioctl | 说明 | 测试 |
|---|---|---|
| `ADDFB2` | `bpp_from_format()` + `drm_ioctl_addfb2()`，校验 pixel_format/flags/handle，分配 fb_id 并 bump dumb refcount | test_drm_addfb2_getfb |
| `GET_MAGIC` | 自增 `g_drm.magic_counter`，返回 magic 值 | test_drm_get_magic |
| `AUTH_MAGIC` | 简化模型：master 权限下任意 magic 均通过 | test_drm_auth_magic |
| `GEM_CLOSE` | dumb handle refcount--，语义同 DESTROY_DUMB | test_drm_gem_close |
| `GETFB` | 按 fb_id 查 framebuffer，返回 width/height/pitch/bpp/depth/handle | test_drm_addfb2_getfb |
| `DRM_CAP_ADDFB2_MODIFIERS` | GET_CAP 新增 case 0x10，返回 0（不支持 modifier） | test_drm_cap_addfb2_modifiers |

测试位于 `user/test/test_drm_ioctl.c`，用 Unity 框架覆盖正向流程（GET_MAGIC → AUTH_MAGIC，CREATE_DUMB + ADDFB2 → GETFB 回读，GEM_CLOSE 清理），注册在 test_runner 中。

**已知剩余问题：**

- **`count_fbs` 填充** — `drmModeGetResources` 返回 `count_fbs=0`，libdrm 因此不会尝试遍历 fb_id_ptr。当前 terminal 直接用已知 fb_id 操作，不受影响；wlroots/tinywl 接入阶段需补齐

### 阶段 2：wlroots 完整支持

wlroots 的 drm-backend 核心差异：

| 维度 | 说明 |
|---|---|
| **Atomic 默认强制** | 初始化检测 `DRM_CAP_ATOMIC`，返回 0 则走 legacy（`SETCRTC`+`PAGE_FLIP`），但 legacy 路径较少测试 |
| **IN_FORMATS blob** | plane 必须注册 `IN_FORMATS` 属性 blob 声明支持的 format/modifier（wlroots 用此分配 dumb buffer） |
| **属性枚举** | 必须注册 KMS 对象的标准属性（`CRTC_ID`、`CONNECTOR_ID`、`FB_ID`、`SRC_*` 等），wlroots 通过 `OBJ_GETPROPERTIES` 遍历 |

wlroots drm-backend 需实现的 ioctl：

| 接口 | 说明 |
|---|---|
| `DRM_IOCTL_MODE_CURSOR2` | wlroots 用 hardware cursor 实现指针渲染，需 `drmModeSetCursor2`（含 hotspot） |
| `DRM_IOCTL_MODE_GETPROPERTY` | 查询 connector/CRTC/plane 属性。connector：`EDID`（空 blob）、`DPMS`（ON）。plane：`IN_FORMATS`、`CRTC_ID`、`SRC_*`、`FB_ID`。CRTC：`MODE_ID`、`ACTIVE`（仅 atomic 用，可选） |
| `DRM_IOCTL_MODE_GETPROPBLOB` | 属性 blob 读取（至少支持 `IN_FORMATS`） |
| `DRM_IOCTL_MODE_SETPROPERTY` | 属性设置（DPMS On/Off 可接受无操作） |
| `DRM_IOCTL_MODE_OBJ_GETPROPERTIES` / `SETPROPERTY` | 通用对象属性枚举/设置 |
| **DRM master 模型完善** | 多个 fd 间的 master/lease、auth 校验、drop master 时清理 |

**非必须（可以不实现）：**

| 接口 | 理由 |
|---|---|
| `DRM_IOCTL_MODE_ATOMIC` | wlroots 初始化设 `WLROOTS_BACKEND_DRM_ATOMIC=0` 或内核 `GET_CAP(DRM_CAP_ATOMIC)=0` 即走 legacy |
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
