# virtio-gpu 驱动设计

virtio-gpu 设备的命令层 + buffer 模型。transport 见 [virtio.md](virtio.md)，DRM/KMS 见 [drm.md](drm.md)。

## 命令层

- ctrlq only（第一版不协商 cursor feature，不创建 cursorq）——无 cursor 需求，YAGNI
- 命令同步：全部同步等待（sleep + 中断 wake，单 in-flight 命令 + `cmd_lock`）。拒绝异步 callback：第一版单客户端无并发命令流，异步是提前优化
- 命令封装：每条命令 = (cmd struct, resp struct) 两个 descriptor，cmd device-readable、resp device-writable
- 命令：`CREATE_RESOURCE_2D` / `ATTACH_BACKING` / `TRANSFER_TO_HOST_2D` / `SET_SCANOUT` / `FLUSH`

实现：`kernel/driver/virtio_gpu.c` 命令部分 + `virtio_gpu.h`。

## buffer 模型

- `TRANSFER_TO_HOST_2D`（guest→host copy）+ `FLUSH`：拒绝 zero-copy（实机 GPU 思维迁移，virtio-gpu 阶段保持显式 transfer）
- `SET_SCANOUT` 设置 scanout 源（初始化时一次，绑定 resource 到 scanout 0）
- dirty-rect 优化：`TRANSFER_TO_HOST_2D` 带 x/y/width/height，目前传全帧

## ATTACH_BACKING 物理地址

`ATTACH_BACKING` 接受 guest 物理地址 + length 的 mem entry 数组。dumb buffer 由内核 `bfc_alloc_page_data` 分配连续物理页，`PHY_ADDR(vaddr)` 取物理地址，单 entry 描述整个 buffer。

## DRM/KMS 集成

见 [drm.md](drm.md)。

- `/dev/dri/card0` 字符设备 + DRM ioctl
- KMS 对象：1 CRTC + 1 plane + 1 connector + 1 encoder（1:1 硬编码）
- mode 固定 800×600@60（存成列表，预留修改路径）
- page flip 异步接口 + 立即 event（语义 C，见 [drm.md](drm.md)）

## 设备配置

`virtio_gpu_config`（device-specific config space）：`events_read`/`events_clear`/`num_scanouts`/`num_capsets`。`num_scanouts` 用于校验 scanout id。
