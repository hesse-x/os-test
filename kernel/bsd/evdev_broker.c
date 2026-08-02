/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 */
// evdev kernel broker. Mirrors refact_evdev.md §5.
//  - control node /dev/input/control: driver_pid==0, ops.ioctl =
//  evdev_control_ioctl
//    (runs via sys_ioctl direct path, can alloc_fd/fd_install to return owner
//    write-fd).
//  - owner write-fd: held by evdev; write broadcasts to all client k fifos of
//  the instance.
//  - consumer fd: allocated on /dev/input/eventN open,
//  f_op=evdev_consumer_fops;
//    read/poll/ioctl(EVIOCG*|GRAB) handled here; EVIOCG* forwarded to
//    manager_pid (§6.3 Q).
#include "kernel/bsd/evdev_broker.h"

#include <xos/errno.h>
#include <xos/ioctl.h>
#include <xos/socket.h> // POLLIN

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h" // __memcpy/__memset/__strncpy/__strcmp
#include "kernel/bsd/devtmpfs.h"
#include "kernel/bsd/fops.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/input_abi.h"
#include "kernel/bsd/kfcntl.h"
#include "kernel/bsd/poll_types.h"
#include "kernel/bsd/proc.h"  // alloc_fd, fd_install, fd_lookup
#include "kernel/bsd/types.h" // struct file
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kpi.h" // copy_to_user, copy_from_user, kmalloc
#include "kernel/xcore/list.h"
#include "kernel/xcore/sched.h" // schedule, wake_with_event, sched_clock
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h"
#include "xos/syscall_nums.h" // __force

#define EVDEV_MAX_INSTANCES 8
#define EVDEV_CLIENT_KFIFO_CAP 64 // max 64 input_event per client
#define EVDEV_IOCTL_REPLY_TIMEOUT_NS 3000000000ULL // 3s

// UAPI: register request (passed in by evdev user-space via ioctl).
// evdev owns name/caps; register carries only minor/name.
struct input_register_arg {
  char name[64];  // "input/eventN"
  uint32_t minor; // device minor (internal evdev routing)
};

// owner write-fd private_data: back-pointer to instance + owner token.
struct input_producer {
  struct evdev_instance *inst;
  pid_t owner_pid;
};

static struct evdev_instance *g_instances[EVDEV_MAX_INSTANCES];
static spinlock g_inst_lock = SPINLOCK_INIT;

static struct evdev_instance *find_instance_by_name(const char *name) {
  for (int i = 0; i < EVDEV_MAX_INSTANCES; i++) {
    struct evdev_instance *inst = g_instances[i];
    if (inst && __strcmp(name, inst->name) == 0)
      return inst;
  }
  return NULL;
}

static struct evdev_instance *find_instance_by_minor(uint32_t minor) {
  for (int i = 0; i < EVDEV_MAX_INSTANCES; i++) {
    struct evdev_instance *inst = g_instances[i];
    if (inst && inst->minor == minor)
      return inst;
  }
  return NULL;
}

bool input_register_check_perm(xtask *proc) {
  (void)proc;
  return true; // §8: currently always true; future permission wiring only
               // changes here
}

// ---- control fd fops ----
// Control fd open: invoked by devtmpfs_open for /dev/input/control.
// input_control_fd allocation + f->private_data wiring is done in
// devtmpfs_open's control-node special case (devtmpfs.c identifies it via
// ops->ioctl == evdev_control_ioctl). So the control node needs no dev_ops.open
// callback (open only sets FD_DEV metadata; the special case fills
// private_data).

// Control node dev_ops (static singleton): /dev/input/control, driver_pid==0,
// ioctl = evdev_control_ioctl runs via sys_ioctl direct path.
static struct dev_ops evdev_control_ops;

void evdev_broker_init(void) {
  __memset(&evdev_control_ops, 0, sizeof(evdev_control_ops));
  evdev_control_ops.driver_pid = 0;
  evdev_control_ops.is_block = false;
  evdev_control_ops.minor = 0;
  __strncpy(evdev_control_ops.subsystem, "input",
            sizeof(evdev_control_ops.subsystem) - 1);
  evdev_control_ops.ioctl = evdev_control_ioctl;
  devtmpfs_create("input/control", &evdev_control_ops, NULL);
}

// Control fd close: §7.2 — iterate ctrl->instances, invalidate + remove. Called
// from f_op->close.
static int evdev_control_close(struct xtask *proc, struct file *f) {
  (void)proc;
  struct input_control_fd *ctrl = (struct input_control_fd *)f->private_data;
  if (!ctrl)
    return 0;

  // §7.2: invalidate + devtmpfs_remove every instance this evdev registered.
  // Blocked consumers see inst->dead → -ENODEV on read/ioctl recheck
  // (§5.4/§12.2).
  list_node *n = ctrl->instances.next;
  while (n && n != &ctrl->instances) {
    struct evdev_instance *inst =
        LIST_ENTRY(n, struct evdev_instance, ctrl_node);
    n = n->next;
    inst->dead = true;
    inst->ctrl = NULL;
    // Wake all blocked readers of this instance: iterate clients, __wake_up
    // each wq.
    spin_lock(&inst->client_lock);
    list_node *cn = inst->client_list.next;
    while (cn && cn != &inst->client_list) {
      struct evdev_client *client = LIST_ENTRY(cn, struct evdev_client, node);
      cn = cn->next;
      if (client->wq)
        __wake_up(client->wq, POLLIN);
    }
    spin_unlock(&inst->client_lock);
    devtmpfs_remove(inst->name); // broadcast "remove" uevent
  }

  // Remove instances owned by this ctrl from the global table (common case: no
  // live client after crash). Instances with live clients: consumer_close only
  // list_removes, does not kfree inst; nulling here prevents dangling pointers
  // under g_inst_lock.
  spin_lock(&g_inst_lock);
  for (int i = 0; i < EVDEV_MAX_INSTANCES; i++) {
    if (g_instances[i] && g_instances[i]->ctrl == ctrl)
      g_instances[i] = NULL;
  }
  spin_unlock(&g_inst_lock);

  kfree(ctrl);
  f->private_data = NULL;
  return 0;
}

const struct file_operations evdev_control_fops = {
    .close = evdev_control_close,
};

// ---- register: control node ioctl direct path ----
long evdev_control_ioctl(uint32_t cmd, void *arg) {
  if (cmd != INPUT_REGISTER)
    return -ENOTTY;

  struct input_register_arg req;
  __memset(&req, 0, sizeof(req));
  // Called via sys_ioctl INPUT_REGISTER early routing; arg is a user pointer.
  if (copy_from_user(&req, arg, sizeof(req)))
    return -EFAULT;

  if (!input_register_check_perm(current_task))
    return -EPERM;

  spin_lock(&g_inst_lock);
  if (find_instance_by_name(req.name)) {
    spin_unlock(&g_inst_lock);
    return -EEXIST;
  }
  int slot = -1;
  for (int i = 0; i < EVDEV_MAX_INSTANCES; i++) {
    if (!g_instances[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    spin_unlock(&g_inst_lock);
    return -ENOMEM;
  }
  struct evdev_instance *inst =
      (struct evdev_instance *)kmalloc(sizeof(struct evdev_instance));
  if (!inst) {
    spin_unlock(&g_inst_lock);
    return -ENOMEM;
  }
  __memset(inst, 0, sizeof(*inst));
  __strncpy(inst->name, req.name, sizeof(inst->name) - 1);
  inst->minor = req.minor;
  list_init(&inst->client_list);
  inst->manager_pid = current_task->pid;
  inst->dead = false;
  g_instances[slot] = inst;
  spin_unlock(&g_inst_lock);

  // Create /dev/input/eventN devtmpfs node (driver_pid==0 auto-broadcasts "add"
  // uevent). ops is statically allocated (single instance minor=0; multi-device
  // needs per-instance ops, see below).
  static struct dev_ops broker_ops[EVDEV_MAX_INSTANCES];
  struct dev_ops *ops = &broker_ops[slot];
  __memset(ops, 0, sizeof(*ops));
  ops->driver_pid = 0;
  ops->is_block = false;
  ops->minor = inst->minor;
  __strncpy(ops->subsystem, "input", sizeof(ops->subsystem) - 1);
  __strncpy(ops->devtype, "evdev", sizeof(ops->devtype) - 1);
  ops->open = evdev_consumer_open_cb;
  devtmpfs_create(inst->name, ops, NULL);

  // Link the new instance into the current control fd's ctrl->instances (crash
  // cleanup).
  {
    struct input_control_fd *ctrl = NULL;
    xtask *self = current_task;
    spin_lock(&self->proc->files->fd_lock);
    for (int i = 0; i < MAX_FD; i++) {
      struct file *cf = fd_lookup(self->proc->files, i);
      if (cf && cf->f_op == &evdev_control_fops && cf->private_data) {
        ctrl = (struct input_control_fd *)cf->private_data;
        break;
      }
    }
    spin_unlock(&self->proc->files->fd_lock);
    if (ctrl) {
      inst->ctrl = ctrl;
      list_push_back(&ctrl->instances, &inst->ctrl_node);
    }
  }

  // Allocate owner write-fd and install it in the caller.
  xtask *proc = current_task;
  spin_lock(&proc->proc->files->fd_lock);
  int fd = alloc_fd(proc->proc->files, 0);
  if (fd < 0) {
    spin_unlock(&proc->proc->files->fd_lock);
    devtmpfs_remove(inst->name);
    kfree(inst);
    return -EMFILE;
  }
  struct file *f = (struct file *)kmalloc(sizeof(struct file));
  if (!f) {
    spin_unlock(&proc->proc->files->fd_lock);
    devtmpfs_remove(inst->name);
    kfree(inst);
    return -ENOMEM;
  }
  __memset(f, 0, sizeof(*f));
  refcount_set(&f->f_count, 1);
  f->type = FD_DEV;
  f->flags = O_WRONLY;
  struct input_producer *prod =
      (struct input_producer *)kmalloc(sizeof(struct input_producer));
  if (!prod) {
    spin_unlock(&proc->proc->files->fd_lock);
    kfree(f);
    devtmpfs_remove(inst->name);
    kfree(inst);
    return -ENOMEM;
  }
  prod->inst = inst;
  prod->owner_pid = proc->pid;
  f->private_data = prod;
  f->f_op = &evdev_owner_fops;
  fd_install(proc->proc->files, fd, f);
  spin_unlock(&proc->proc->files->fd_lock);

  return (long)fd;
}

// ---- owner write-fd fops ----
static ssize_t evdev_owner_write(struct xtask *proc, struct file *f,
                                 const void *buf, size_t count) {
  struct input_producer *prod = (struct input_producer *)f->private_data;
  if (!prod || prod->owner_pid != proc->pid)
    return -EACCES;
  struct evdev_instance *inst = prod->inst;
  if (!inst || inst->dead)
    return -ENODEV;

  uint32_t nev = (uint32_t)(count / sizeof(input_event));
  if (nev == 0)
    return 0;

  // Batch broadcast: enqueue the whole batch per client, honoring SYN_DROPPED
  // frame semantics (§5.6).
  spin_lock(&inst->client_lock);
  list_node *n = inst->client_list.next;
  while (n && n != &inst->client_list) {
    struct evdev_client *client = LIST_ENTRY(n, struct evdev_client, node);
    n = n->next;
    bool any_new = false;
    for (uint32_t i = 0; i < nev; i++) {
      const input_event *ev =
          (const input_event *)((const uint8_t *)buf + i * sizeof(input_event));

      // Frame-aware SYN_DROPPED (§5.6): while in_frame, keep dropping until
      // next SYN_REPORT, then inject a single SYN_DROPPED at that frame
      // boundary.
      if (client->in_frame) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
          input_event drop;
          __memset(&drop, 0, sizeof(drop));
          drop.type = EV_SYN;
          drop.code = SYN_DROPPED;
          if (kfifo_in(&client->buffer, &drop))
            any_new = true;
          client->in_frame = false;
        }
        continue; // otherwise keep dropping remaining events of this frame
      }

      if (!kfifo_in(&client->buffer, ev)) {
        // drop-new: on full, set dropped + in_frame, discard this event.
        client->dropped++;
        client->in_frame = true;
      } else {
        any_new = true;
      }
    }
    if (any_new && client->wq)
      __wake_up(client->wq, POLLIN);
  }
  spin_unlock(&inst->client_lock);
  return (ssize_t)(nev * sizeof(input_event));
}

static int evdev_owner_close(struct xtask *proc, struct file *f) {
  (void)proc;
  struct input_producer *prod = (struct input_producer *)f->private_data;
  if (prod)
    kfree(prod);
  f->private_data = NULL;
  return 0;
}

const struct file_operations evdev_owner_fops = {
    .write = evdev_owner_write,
    .close = evdev_owner_close,
};

// ---- consumer path ----
static void evdev_client_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_with_event(target, WAIT_POLL);
}

// consumer open: invoked by devtmpfs_open for /dev/input/eventN. Allocates
// evdev_client, wires it into f->private_data, installs evdev_consumer_fops,
// links into inst->client_list.
int evdev_consumer_open_cb(xtask *proc, int fd) {
  struct file *f = fd_lookup(proc->proc->files, fd);
  if (!f || !f->inode || !f->inode->i_priv)
    return -ENODEV;
  struct dev_ops *ops = (struct dev_ops *)f->inode->i_priv;
  struct evdev_instance *inst = find_instance_by_minor(ops->minor);
  if (!inst || inst->dead)
    return -ENODEV;

  struct evdev_client *client =
      (struct evdev_client *)kmalloc(sizeof(struct evdev_client));
  if (!client)
    return -ENOMEM;
  __memset(client, 0, sizeof(*client));
  if (!kfifo_alloc(&client->buffer, EVDEV_CLIENT_KFIFO_CAP,
                   sizeof(input_event))) {
    kfree(client);
    return -ENOMEM;
  }
  client->inst = inst;
  client->owner_pid = proc->pid;
  client->wq = NULL; // Set below via file_wq_get before returning.
  client->dropped = 0;
  client->in_frame = false;
  list_init(&client->node);

  spin_lock(&inst->client_lock);
  list_push_back(&inst->client_list, &client->node);
  spin_unlock(&inst->client_lock);

  f->private_data = client;
  f->f_op = &evdev_consumer_fops;
  // Eagerly resolve the per-fd wq so owner write's __wake_up(client->wq)
  // reaches epoll/poll waiters — not just blocking read. Without this,
  // client->wq stays NULL until a blocking read (which never happens when
  // libinput monitors the fd via epoll), so posted events pile up in the
  // kfifo with no wakeup: input appears dead (bug.md). file_wq_get lazily
  // allocates f->wq here, and every later file_wq_get(consumer_fd) — by
  // sys_poll's add_wait_queue or epoll's ep_target_wq — returns the same
  // f->wq, so the wakeup target and the waiter's wq are one object.
  client->wq = file_wq_get(f);
  return 0;
}

static ssize_t evdev_consumer_read(struct xtask *proc, struct file *f,
                                   void *buf, size_t count) {
  (void)proc;
  struct evdev_client *client = (struct evdev_client *)f->private_data;
  if (!client || !client->inst || client->inst->dead)
    return -ENODEV;
  if (count == 0)
    return 0; // POSIX: read of zero bytes returns zero
  if (count < sizeof(input_event))
    return -EINVAL;

  if (kfifo_len(&client->buffer) == 0) {
    if (f->flags & O_NONBLOCK)
      return -EAGAIN;
    // Block: join per-file wq, schedule, retried after owner write's __wake_up
    // wakes us. Local convention follows ring.c:64-91.
    wait_queue_head *wq = file_wq_get(f);
    if (!wq)
      return -EAGAIN;
    wait_queue_t wait;
    wait.func = evdev_client_wake_cb;
    wait.data = current_task;
    wait.exclusive = 0;
    list_init(&wait.node);
    add_wait_queue(wq, &wait);
    // client->wq was resolved at open (evdev_consumer_open_cb) to this same
    // f->wq, so owner write already wakes us — nothing to assign here.

    while (kfifo_len(&client->buffer) == 0) {
      if (client->inst->dead) {
        remove_wait_queue(wq, &wait);
        return -ENODEV;
      }
      current_task->state = BLOCKED;
      current_task->wait_event = WAIT_POLL;
      schedule();
      if (f->flags & O_NONBLOCK) {
        remove_wait_queue(wq, &wait);
        return -EAGAIN;
      }
    }
    remove_wait_queue(wq, &wait);
  }

  uint32_t n = kfifo_out_batch(&client->buffer, buf,
                               (uint32_t)(count / sizeof(input_event)));
  if (n == 0)
    return 0;
  return (ssize_t)(n * sizeof(input_event));
}

static __poll evdev_consumer_poll(struct xtask *proc, struct file *f,
                                  int events) {
  (void)proc;
  struct evdev_client *client = (struct evdev_client *)f->private_data;
  if (!client || !client->inst || client->inst->dead)
    return 0;
  if (kfifo_len(&client->buffer) > 0)
    return events & POLLIN;
  return 0;
}

// EVIOCG*_with_EVIOCGRAB forwarded to manager evdev (§6.3 Q). f_op->ioctl runs
// in sys_ioctl before type dispatch (syscall.c:1878); broker manually replays
// the sys_req RECV_REQ + WAIT_REQ_REPLY pattern to forward to
// inst->manager_pid. Liveness pre-check: inst->dead or manager gone → immediate
// -ENODEV (§12.2).
static long evdev_consumer_ioctl(struct xtask *proc, struct file *f,
                                 uint32_t cmd, void *arg) {
  struct evdev_client *client = (struct evdev_client *)f->private_data;
  if (!client || !client->inst)
    return -ENODEV;
  struct evdev_instance *inst = client->inst;

  // Liveness pre-check (§5.4/§12.2).
  if (inst->dead)
    return -ENODEV;
  pid_t target_pid = inst->manager_pid;
  if (target_pid <= 0 || target_pid >= MAX_PROC)
    return -ENODEV;
  xtask *target = task_get(target_pid);
  if (target->pid != target_pid)
    return -ENODEV;

  uint16_t arg_size = _IOC_SIZE(cmd);
  uint8_t dir = _IOC_DIR(cmd);

  // Reuse sys_ioctl inline path layout: req_data[56] =
  // [cmd][arg≤48B][minor@52].
  uint8_t req_data[56];
  __memset(req_data, 0, 56);
  *(uint32_t *)req_data = cmd;
  if ((dir & _IOC_WRITE) && arg_size > 0 && arg_size <= 48) {
    if (copy_from_user(req_data + 4, arg, arg_size))
      return -EFAULT;
  }
  *(uint32_t *)(req_data + 52) = inst->minor;

  uint8_t msgbuf[RECV_MSG_SIZE];
  recv_msg *hdr = (recv_msg *)msgbuf;
  __memset(msgbuf, 0, RECV_MSG_SIZE);
  hdr->type = RECV_REQ;
  hdr->src = (uint32_t)proc->pid;
  __memcpy(hdr->data, req_data, 56);

  // Arm per-request reply state BEFORE enqueue (canonical order from sys_req /
  // sys_ioctl proxy): sys_resp publishes req_result/req_replied under the
  // caller's scheduler_lock; if we cleared after enqueue, a fast reply from
  // target on another CPU could land first and then be clobbered by our clear
  // here (lost wake → 3s -ETIMEDOUT; bug.md Bug 1 root cause of test_libudev
  // flakiness).
  proc->req_target_pid = target_pid;
  proc->req_reply_buf = arg;
  proc->req_reply_len = arg_size;
  proc->req_result = 0;
  proc->req_replied = 0;
  proc->wait_timed_out = 0;

  // Enqueue to target's recv queue (mirrors sys_req). target's ipc_recv() then
  // sets target->req_caller_pid = proc->pid so target's sys_resp returns to
  // this process.
  spin_lock(&target->recv_lock);
  uint32_t next = (target->recv_head + 1) % RECV_QUEUE_SIZE;
  if (next == target->recv_tail) {
    spin_unlock(&target->recv_lock);
    return -EBUSY;
  }
  __memcpy(target->recv_buf[target->recv_head], msgbuf, RECV_MSG_SIZE);
  target->recv_head = next;
  spin_unlock(&target->recv_lock);

  // Wake target if in WAIT_RECV.
  wake_with_event(target, WAIT_RECV);

  // Wake target's ipcfd wq (mirrors notify_and_wake §5.6): evdev's main loop
  // blocks in epoll_wait(WAIT_POLL); only the ipcfd wq's __wake_up triggers
  // ep_poll_callback and lets epoll_wait return. wake_with_event(WAIT_RECV)
  // only wakes sys_recv blockers, not the epoll path.
  if (target->ipcfd_file) {
    wait_queue_head *iwq = file_wq_get(target->ipcfd_file);
    if (iwq)
      __wake_up(iwq, POLLIN);
  }

  // Arm WAIT_REQ_REPLY: locked re-check of req_replied (sys_resp publishes
  // under the same scheduler_lock); if the reply already landed, don't sleep.
  if (sched_arm_timed_wait(proc, WAIT_REQ_REPLY,
                           sched_clock() + EVDEV_IOCTL_REPLY_TIMEOUT_NS,
                           &proc->req_replied))
    schedule();

  if (proc->wait_timed_out)
    return -ETIMEDOUT;
  return (long)proc->req_result;
}

static int evdev_consumer_close(struct xtask *proc, struct file *f) {
  struct evdev_client *client = (struct evdev_client *)f->private_data;
  if (!client)
    return 0;
  struct evdev_instance *inst = client->inst;
  if (inst) {
    spin_lock(&inst->client_lock);
    list_remove(&client->node);
    spin_unlock(&inst->client_lock);
  }
  kfifo_free(&client->buffer); // reclaim outside the lock
  kfree(client);               // reclaim outside the lock
  f->private_data = NULL;
  (void)proc;
  return 0;
}

const struct file_operations evdev_consumer_fops = {
    .read = evdev_consumer_read,
    .poll = evdev_consumer_poll,
    .ioctl = evdev_consumer_ioctl,
    .close = evdev_consumer_close,
};
