/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// evdev kernel char-device broker (mirrors Linux drivers/input/evdev.c kernel
// layer). evdev user-space process registers devices via INPUT_REGISTER ioctl
// on /dev/input/control; broker exposes /dev/input/eventN with standard evdev
// semantics (read/poll/EVIOCG*_with_EVIOCGRAB) and per-client kfifo event
// broadcast.
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

// Per-consumer fd state. f->private_data points to it.
struct evdev_client {
  kfifo buffer;   // per-consumer event ring (SPSC, fixed-size input_event)
  list_node node; // linked into inst->client_list
  struct evdev_instance *inst; // back-pointer to owning instance
  wait_queue_head *wq;         // per-fd wait queue (= file_wq_get(consumer_fd))
  uint32_t dropped;            // SYN_DROPPED counter
  bool in_frame;               // mid-frame flag (SYN_DROPPED frame boundary)
  bool revoked;                // EVIOCREVOKE permanently disables this OFD
  pid_t owner_pid;             // consumer pid
};

// Per eventN instance. devtmpfs inode->i_priv points to it.
struct evdev_instance {
  char name[64]; // "input/eventN"
  uint32_t minor;
  list_node client_list; // consumer list head
  spinlock client_lock;  // guards client_list (iterate under lock, drop before
                         // release)
  pid_t manager_pid;     // pid of registering evdev (= control fd owner)
  struct input_control_fd *ctrl; // back-pointer to control fd (crash cleanup)
  list_node ctrl_node; // linked into ctrl->instances (crash cleanup traversal)
  bool dead;           // instance invalidated (after evdev crash)
};

// Control node fd (held by evdev). f->private_data points to it.
struct input_control_fd {
  pid_t manager_pid;   // = current_task->pid at open
  list_node instances; // all instances registered by this fd (crash cleanup)
};

// Control fd ioctl: register. Runs in sys_ioctl dev_ops direct path
// (driver_pid==0), returns owner write-fd.
long evdev_control_ioctl(uint32_t cmd, void *arg);

// Init: create /dev/input/control control node. Called from bsd_init.
void evdev_broker_init(void);

// dev_ops.open for /dev/input/eventN (allocates evdev_client, installs consumer
// fops).
int evdev_consumer_open_cb(xtask *proc, int fd);

// Owner write-fd fops. read/poll return -EINVAL, write broadcasts, close
// releases the producer.
extern const struct file_operations evdev_owner_fops;

// Consumer fd fops: read/poll/ioctl(EVIOCG*|GRAB)/close.
extern const struct file_operations evdev_consumer_fops;

// Control fd fops: close iterates instances, triggers invalidation + remove
// (§7.2).
extern const struct file_operations evdev_control_fops;

// Permission check placeholder (currently always true, §8).
bool input_register_check_perm(xtask *proc);

#endif // KERNEL_BSD_EVDEV_BROKER_H
