# virtio transport 设计

virtio-gpu 的 PCI transport 层。命令层见 [virtio_gpu.md](virtio_gpu.md)，DRM/KMS 见 [drm.md](drm.md)。

## virtio-pci modern

- PCI capability 结构（common config / notify / ISR / device-config），通过 vendor-specific cap（`PCI_CAP_ID_VNDR=0x09`）链遍历定位
- 匹配规则：`vendor_id==0x1AF4 && device_id==0x1050 && subsystem_id==2`（virtio-gpu-pci 是 non-transitional 设备，subsystem id=2 区分 virtio-gpu 与其它 virtio 设备）
- 拒绝 MMIO transport（只支持 PCI modern；transitional/legacy transport 不实现）

实现：`kernel/driver/virtio_pci.c` + `virtio_pci.h`。

## split virtqueue

- spec baseline split queue（desc/avail/used 三环），所有合规 device 支持
- free list 管理：`next_free[i]` 链表 + `free_head`/`free_cnt`
- `vring_add_buf` 发布 chain，`vring_kick` 更新 `avail->idx`（caller 再写 notify BAR）
- `vring_poll_used` 消费 used 环并释放 desc

实现：`kernel/driver/virtio_ring.c` + `virtio_ring.h`。

选 split 不选 packed：字符显示场景无性能需求（800×600×32@60Hz ≈ 115 MB/s，split queue 完全够），且 QEMU virtio-gpu 的 packed queue 历史有 bug，split 是最稳路径。

## feature 协商

- 只协商 `VIRTIO_F_VERSION_1`（virtio 1.x modern 必需）
- 写 `driver_feature_select/driver_feature`，置 `FEATURES_OK`，回读 status 确认 device 接受

## 中断

- 单 MSI-X 向量（ctrlq + config change 共用）
- ISR 读 capability 区分 queue interrupt vs config change
- 命令等待用 `BLOCKED + WAIT_RECV + wake_with_event`（见 [schedule.md](schedule.md)）
- `lapic_eoi` 在 ISR 末尾调用一次

## 设备状态机

`ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK`。`DRIVER_OK` 在 ctrlq 初始化 + ISR 注册 + MSI-X unmask 之后才置位。
