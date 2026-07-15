# sysfs

> Linux 式 sysfs 伪文件系统，挂载 `/sys`，设备属性以真实可读文件暴露。
> 相关：`doc/design/kernel/mount.md`（挂载点穿越）、`doc/design/kernel/netlink.md`（uevent 广播）、`doc/design/udev.md`（libudev shim 读 sysfs）。

## 当前架构设计

### 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 属性暴露方式 | 真实 `/sys` 伪文件系统 | libudev/libinput 经 `open+read` 读属性，对齐 Linux；废弃 udevd 内存库虚拟 syspath |
| 2 | 是否引入完整 kobject/device 框架 | 否，最小属性树 + show 回调 | libinput 只需读少量文本属性，完整框架过度设计 |
| 3 | fd-I/O 分发模型 | `file_operations`（`f->f_op`） | 替代 `switch(f->type)` 膨胀；新设备类型只注册一个 fops，无需改每个 syscall handler |
| 4 | 路径层与 fd-I/O 层 | 两层独立 | 路径层走 fstype 回调（mount.md），fd-I/O 层走 `f->f_op`，互不干扰 |
| 5 | evdev 事件流 | SHM ring buffer + read/poll 语义 | 对齐 Linux `/dev/input/eventX`；生产者 mmap 零拷贝写，消费者 read/poll |
| 6 | ringbuf 溢出策略 | 覆盖最老 slot，慢读者 cursor 跳到 head | 对齐 Linux：客户端 buffer 溢出丢事件，生产者永不阻塞 |
| 7 | 新消费者起始游标 | open 时 cursor = 当前 head | 新消费者只收 open 后的事件，不读过期数据 |
| 8 | ringbuf mmap 权限 | 仅 driver owner 经 fall-through 映射；消费者 `-EPERM` | 防止消费者绕过内核直接 mmap SHM |

### 核心数据结构

`sysfs_attr`（kernel/bsd/sysfs.h : sysfs_attr）
- name : const char* — 属性文件名
- priv : void* — 设备上下文（`input_dev_props*` / `uevent_attr_priv*` / NULL）；对齐 Linux kobject 传参
- show : ssize_t(*)(char*, size_t, void*) — 读：格式化到 buf 返回字节数
- store : ssize_t(*)(const char*, size_t, void*) — 写（默认 NULL；uevent 属性可写，见「可写 uevent 属性」）

`uevent_attr_priv`（kernel/bsd/sysfs.h : uevent_attr_priv）— coldplug 重广播所需 devpath + subsystem
- devpath : char[32] — 设备 name（如 `input/event0`，= DEVPATH 值）
- subsystem : char[8] — 如 `input`
- 生命周期由 `dev_ops.uevent_priv` 持有（store 回调无法从 sysfs 节点位置反推完整 devpath——节点 basename 去掉了子系统前缀，且文件节点 inode->i_priv 只存 attr 不存 sysfs_node，故单独存；在 `sysfs_remove_dir` 前 kfree 防 UAF）

`input_dev_props`（kernel/bsd/sysfs.h : input_dev_props）— evdev 设备属性内核侧
- bustype / vendor / product / version : uint16_t — bus type 与 ID
- name : char[64] — 设备名

`sysfs_node`（kernel/bsd/sysfs.h : sysfs_node）— 目录或属性文件节点
- name : char[32]
- is_dir : bool
- attr_owned : bool — attr 是否 kmalloc（移除时需 free；每设备拷贝 attr 以挂各自 priv）
- parent / children / sibling : sysfs_node* — 内存树
- attr : sysfs_attr* — 文件属性；目录为 NULL
- ip : inode* — 关联 inode（lookup 时按需建）
- ino : uint32_t — 唯一 inode 号

`file_operations`（kernel/bsd/fops.h : file_operations）
- read / write / ioctl / poll / close / mmap — 6 个 fd-I/O 回调，NULL 表示不支持（read/write → `-ENOSYS`，ioctl → `-ENOTTY`）

`struct file` 加 `f_op` 字段（kernel/bsd/types.h : file）— `f->f_op` 为 NULL 时走原 `type` 分发（迁移期 fallback）。

### 关键流程

**挂载与路径解析：**
1. `bsd_init` 阶段内核主动 `mount("sysfs","/sys")`，`fs_data = sysfs_root`
2. `vfs_resolve("/sys/class/drm/card0/vendor")` 命中 `/sys` → `sysfs_fstype`，挂载内路径 `class/drm/card0/vendor`
3. `sysfs_lookup` 逐组件走路，从 `sysfs_root` 起 children 链线性匹配到叶节点，返回 `INODE_REGULAR`/`INODE_DIR` inode
4. `sysfs_getdents` 遍历目录节点的 children 链 `dir_emit`；`sysfs_stat` 返回 mode/ino

**属性文件 read：** `sys_open` 对 sysfs 挂载上的 `INODE_REGULAR` 设 `f->f_op = &sysfs_fops` → `sysfs_file_read` 分配 kbuf → 调 `attr->show(kbuf, len, attr->priv)` → `copy_to_user`。属性均 ≤1 页，单次全量返回，无 seek/分页。

**可写 uevent 属性（coldplug 重广播，对齐 Linux `echo add > /sys/.../uevent`）：**
- `sysfs_file_write`（sysfs.c : sysfs_file_write）：flags 检查 `O_WRONLY|O_RDWR`（防 `O_RDONLY` 写 → `-EBADF`，`sysfs_fops` 同时服务 read/write）→ `copy_from_user` kbuf → 调 `attr->store(kbuf, count, attr->priv)`，无 store → `-EIO`。
- `uevent_store(buf, len, priv)`（sysfs.c : uevent_store）：解析 action（只接受 `"add"`，否则 `-EINVAL`，remove/change 记 todo）→ `nl_uevent_broadcast("add", p->devpath, p->subsystem)` 重广播，走与热插拔完全相同的 netlink 路径。
- `sysfs_fops` 挂 `.write = sysfs_file_write`。`uevent_attr` 模板 `.show = NULL`（只写不读，Linux uevent 可读记 todo）+ `.store = uevent_store`；devtmpfs 逐设备拷贝模板填 `priv = uevent_attr_priv*`。

**dev_ops.uevent_priv 字段**（kernel/bsd/devtmpfs.h : dev_ops）：管理 uevent attr 的 priv 生命周期。`sys_dev_set_meta` 建 uevent attr 时 `kmalloc(uevent_attr_priv)` 填 devpath=name、subsystem=ops->subsystem → `ops->uevent_priv = priv`。`sysfs_remove_dir` 只 `kfree(attr)` 不释放 `attr->priv`，故两处 cleanup 的 `sysfs_remove_dir` **之前** kfree `ops->uevent_priv`：`sys_dev_set_meta` 幂等 guard + `devtmpfs_cleanup_pid`。释放顺序：kfree uevent_priv → sysfs_remove_dir（释放 attr 结构体）→ kfree subsys_priv → kfree ops。

**fd-I/O 分发：** sys_read/write/ioctl/poll/close/mmap 各从 ~14 个 `switch(f->type)` case 缩为 `if (f->f_op && f->f_op->read) return f->f_op->read(...);`。`sys_open` 设 `f->f_op`：sysfs 属性文件 → `sysfs_fops`；`INODE_DEV` 按 `ops->driver_pid`/`ip->shm` 分流到 ringbuf/dev_kernel/dev_ipc fops。

**evdev 两步注册（对齐 Linux allocate→fill→register）：**
1. `device_register_shm("input/event0", shm_fd, minor)` → `devtmpfs_create` 建 `/dev` 节点 + 绑 SHM，subsystem/devtype/subsys_priv/sysfs_dir 全空，**不推 uevent**
2. `device_set_meta("input/event0", "input", "evdev", &props)` → `sys_dev_set_meta`(96)：填 dev_ops.subsystem/devtype、kmalloc `input_dev_props` 填 subsys_priv、建 sysfs 子树（含可写 `uevent` 属性）、推 `nl_uevent_broadcast("add",...)`
3. udevd 只在 step 2 后感知设备，无竞态窗口

**sys_dev_set_meta 幂等性：** 重复调用（re-probe caps 更新 db / evdev 重注册）会泄漏 `subsys_priv` + 重复建 sysfs 文件。set_meta 入口幂等 guard（devtmpfs.c : sys_dev_set_meta）：`devtmpfs_lookup` 取 ops 后、建新前，先 kfree 旧 `uevent_priv` → `sysfs_remove_dir` 旧 `sysfs_dir` → kfree 旧 `subsys_priv`，再建新。重复推 add uevent 是幂等的（udevd 同 devnum 重探 caps 重写 db，原子覆盖语义保证最终一致）。

**ringbuf read/poll：** `ringbuf_fops.read` → `ring_read`（kernel/bsd/sysfs.c : ring_read）从 SHM ring 读，per-fd cursor（`f->offset`）推进；ring 空 + 非 `O_NONBLOCK` 阻塞于 `wait_queue`，被 evdev 的 `RINGBUF_WAKE` 唤醒。`ringbuf_poll` 在 cursor != head 时返回 POLLIN。

**ringbuf 生命周期通知：** `sys_open` 建 ringbuf fd 时 `ringbuf_notify_open` 经 `notify_and_wake` 向 `ops->driver_pid` 发 `RINGBUF_OPEN`；`sys_close` 时 `ringbuf_close` 发 `RINGBUF_CLOSE`。driver 自行记录/清除消费者 PID。

**设备移除清理：** `devtmpfs_cleanup_pid` 对每个被删设备，若 `ops->sysfs_dir` 则 `sysfs_remove_dir`（递归删子树）+ 释放 `subsys_priv`。

### 锁模型

| 锁 | 类型 | 保护范围 |
|----|------|----------|
| devtmpfs_lock | spinlock | dev_list/dir_list 链表(kmalloc 动态节点) + sysfs 节点树增删 |
| fat_lock | — | sysfs 与 FAT32 无交集，不涉及 fat 锁 |

sysfs 节点树在 `devtmpfs_lock` 下增删；show 回调读初始化后不变的只读字段，无额外锁。

### 设备属性树

**drm（内核态，`virtio_gpu.c` 注册，priv=NULL 读内核全局）：**

```
/sys/class/drm/card0/
  vendor device class driver enabled mode connector_status num_scanouts dev
```

值来自 `drm_pci_dev()`(vendor/device/class)、`dev_driver.name`、`g_drm.initialized`、`g_drm.fb_width×fb_height`、硬编码 connected、`g_virtio_gpu.config.num_scanouts`。不暴露 edid（GETPROPERTY 是 stub）。

**evdev（用户态驱动，priv=input_dev_props*）：**

```
/sys/class/input/event0/
  name          → props->name
  id/{bustype,vendor,product,version}  → props 对应字段(hex)
  uevent        → 可写(store=uevent_store)；写 "add" 触发 nl_uevent_broadcast 重广播(coldplug)
```

`SYS_DEV_SET_META` 为每设备 kmalloc 一份 `sysfs_attr` 拷贝并挂 `priv=iprops`（const 模板无 priv，共享会跨设备串值）。uevent attr 的 priv 挂 `uevent_attr_priv*`（devpath+subsystem），由 `dev_ops.uevent_priv` 持有。

### 系统调用/接口

`int64_t sys_dev_set_meta(name, subsystem, devtype, props)` — 编号 96（kernel/bsd/devtmpfs.c : sys_dev_set_meta）。按 name 找 devtmpfs dev_ops，幂等释放旧 uevent_priv/sysfs_dir/subsys_priv，copy_from_user 填 subsystem[8]/devtype[8]/`input_dev_props`，建 sysfs 子树（含可写 uevent 属性），推 uevent。name 不存在 → `-ENOENT`，copy 失败 → `-EFAULT`。

`device_register_shm(name, shm_fd, minor)` — 两步注册第一步（已有，3 参数不变）。

用户态 `struct dev_props { uint16_t bustype,vendor,product,version; char name[64]; }`（user/include/sys/device.h : dev_props）。

### 与其他模块的关系

- 依赖 `doc/design/kernel/mount.md`：sysfs 注册为 fstype，挂载点穿越 + getdents 枚举
- `doc/design/kernel/netlink.md`：设备就绪 uevent 经 `nl_uevent_broadcast` 广播
- `doc/design/udev.md`：libudev shim `udev_device_get_sysattr_value` open+read sysfs 真文件
- `doc/design/kbd.md` / `doc/design/evdev.md`：evdev 两步注册产出 sysfs 属性树

## 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| evdev 真实属性来源 | 当前属性值仍来自静态桩/硬编码（evdev.cc register_stubs 在 `#ifdef TEST`）。需让真实属性流入：短路径 xHCI 枚举提取 USB Device Descriptor idVendor/idProduct 进 devtmpfs 元数据；长路径用户态解析 HID report descriptor | 中 |
| capabilities/properties 属性树 | sysfs 暴露 evdev caps bitmap(EV_KEY) 与 prop bitmap（hex）；property 已由 udevd db（见 [udev.md](udev.md)）维护，caps 仍由 udevd 开 /dev 跑 EVIOCGBIT 探 | 中 |
| uevent 属性可读 | 现 `show=NULL`（只写不读）；Linux uevent 可读输出当前 uevent KV | 低 |
| uevent 接受 remove/change | `uevent_store` 只接受 `"add"`；补 remove/change action | 低 |
| ringbuf 多读者测试 | 需 fork + 共享 marker，验证 per-fd cursor 不互相干扰 | 低 |
| ringbuf 溢出测试 | 慢 reader cursor 跳 head 逻辑已实现，缺测试：evdev 写满 ring 验证 cursor 被绕过后跳到 head | 低 |
| ringbuf poll 阻塞测试 | wait_queue 已实现，缺测试：evdev 写事件后唤醒阻塞 poll | 低 |
| ringbuf 生命周期通知测试 | RINGBUF_OPEN/CLOSE 通知已实现，缺测试：验证 driver 收到通知 | 低 |
| seq_file / >页 属性 | 当前属性均小且静态，不引入 seq_file。若未来出现 >页 属性（如完整 EDID 二进制），需补 offset 传递或 seq 辅助 | 低 |
