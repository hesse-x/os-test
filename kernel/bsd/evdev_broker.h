/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * evdev 内核态字符设备 broker（对齐 Linux drivers/input/evdev.c 的内核层）。
 * evdev 用户态进程经 /dev/input/control 的 INPUT_REGISTER ioctl 注册设备，
 * broker 在 /dev/input/eventN 呈标准 evdev
 * 语义（read/poll/EVIOCG*_与_EVIOCGRAB）， 持有 per-client kfifo 广播输入事件。
 */
#ifndef KERNEL_BSD_EVDEV_BROKER_H
#define KERNEL_BSD_EVDEV_BROKER_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/bsd/fops.h"
#include "kernel/bsd/kfifo.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h" // wait_queue_head
#include "kernel/xcore/xtask.h"      // pid_t, xtask

/* 每个消费者 fd 的状态。f->private_data 指向它。 */
struct evdev_client {
  kfifo buffer;   // per-consumer 事件环（SPSC，定长 input_event）
  list_node node; // 链入 inst->client_list
  struct evdev_instance *inst; // 回指所属实例
  wait_queue_head *wq; // per-fd 等待队列（= file_wq_get(consumer_fd)）
  uint32_t dropped;    // SYN_DROPPED 计数
  bool in_frame;       // 帧中间状态（SYN_DROPPED 帧边界用）
  pid_t owner_pid;     // 消费者 pid
};

/* 每个 eventN 实例。devtmpfs inode->i_priv 指向它。 */
struct evdev_instance {
  char name[64]; // "input/eventN"
  uint32_t minor;
  list_node client_list; // 消费者列表头
  spinlock client_lock;  // 保护 client_list（遍历持锁，释放锁外）
  pid_t manager_pid;     // 注册它的 evdev pid（=控制 fd 持有者）
  struct input_control_fd *ctrl; // 回指注册它的控制 fd（crash 清理用）
  list_node ctrl_node; // 链入 ctrl->instances（crash 清理遍历用）
  bool dead;           // 实例已失效（evdev crash 后）
};

/* 控制节点 fd（evdev 持有）。f->private_data 指向它。 */
struct input_control_fd {
  pid_t manager_pid;   // = current_task->pid at open
  list_node instances; // 此 fd 注册的所有 instance（crash 清理用）
};

/* 控制 fd 的 ioctl：register。在 sys_ioctl 的 dev_ops direct path 执行
 * （driver_pid==0），返回 owner write-fd。 */
long evdev_control_ioctl(uint32_t cmd, void *arg);

/* 初始化：创建 /dev/input/control 控制节点。由 bsd_init 调用。 */
void evdev_broker_init(void);

/* /dev/input/eventN 的 dev_ops.open（分配 evdev_client，装 consumer fops）。 */
int evdev_consumer_open_cb(xtask *proc, int fd);

/* owner write-fd 的 fops。read/poll 返回 -EINVAL，write 广播，close 释放
 * producer。 */
extern const struct file_operations evdev_owner_fops;

/* 消费者 fd 的 fops。read/poll/ioctl(EVIOCG*|GRAB)/close。 */
extern const struct file_operations evdev_consumer_fops;

/* 控制 fd 的 fops：close 遍历 instances 触发失效+remove（§7.2）。 */
extern const struct file_operations evdev_control_fops;

/* 权限校验占位（当前恒 true，§8）。 */
bool input_register_check_perm(xtask *proc);

#endif /* KERNEL_BSD_EVDEV_BROKER_H */
