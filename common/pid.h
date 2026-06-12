#ifndef PID_H
#define PID_H

// 启动时内核按顺序创建的进程 PID（与 kernel_main 中 process_create_elf 调用顺序一致）
#define DISK_DRIVER_PID  2
#define KBD_DRIVER_PID   3
#define KMS_DRIVER_PID   4
#define SHELL_PID        5
#define FS_DRIVER_PID    6

#endif // PID_H
