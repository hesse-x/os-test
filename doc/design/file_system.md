# 内核 FAT32 文件系统

> FAT32 已从用户态 fs_driver 搬入内核。完整 VFS + inode+page cache+devtmpfs 设计见 [vfs.md](vfs.md)。

>
> 旧版 fs_driver（driver/fs_driver.cc，4167 行）已删除。旧版设计（异步事件循环+pending_op+kernel_msg_send IPC 代理）不再适用。
>
> 本文件仅保留 FAT32 磁盘布局参考和目录结构。

> 磁盘布局详见 [fat32.md](fat32.md)，VFS/inode/page cache 设计详见 [vfs.md](vfs.md)

