/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <freebsd/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/device.h>
#include <sys/ipc.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>

#define MAX_EVDEV_DEVICES 8
// Reply buffer for inline RECV_REQ path and the data evdev hands to sys_resp.
// Must hold the largest getter we serve: EVIOCGBIT(EV_KEY, KEY_CNT) = 96B, plus
// headroom for future EVIOCGABS (struct input_absinfo across ABS_CNT axes) etc.
#define REPLY_BUF_SIZE 256

struct evdev_device {
  uint16_t minor;
  char name[64];
  struct input_id id;
  // Per-event-type capability bitmaps, indexed [ev][byte]. Each ev in
  // [0,EV_CNT) gets its own bitmap; EVIOCGBIT(ev,len) copies caps_bitmap[ev]
  // into the reply, truncated to len. KEY/ABS/etc. bitmaps can be up to
  // KEY_CNT/ABS_CNT bytes; 96B covers KEY_CNT (the largest). EV_CNT=32.
  uint8_t caps_bitmap[EV_CNT][96];
  uint32_t prop_bitmap;
  bool grabbed;
  pid_t grab_client;
};

static struct evdev_device devices[MAX_EVDEV_DEVICES];
static int num_devices;

static struct evdev_device *find_device(uint32_t minor) {
  for (int i = 0; i < num_devices; i++) {
    if (devices[i].minor == minor)
      return &devices[i];
  }
  return NULL;
}

#ifdef TEST
static void init_stub(struct evdev_device *dev, uint16_t minor,
                      const char *name, struct input_id id, uint32_t prop) {
  dev->minor = minor;
  strncpy(dev->name, name, sizeof(dev->name) - 1);
  dev->id = id;
  memset(dev->caps_bitmap, 0, sizeof(dev->caps_bitmap));
  dev->prop_bitmap = prop;
  dev->grabbed = false;
  dev->grab_client = 0;
}

static void register_stubs(void) {
  struct input_id kbd_id = {BUS_USB, 0x0001, 0x0001, 0x0001};
  init_stub(&devices[0], 0, "evdev keyboard", kbd_id, 0);
  // Advertise EV_KEY support: set bit EV_KEY in the EV_SYN-type bitmap (ev=0).
  devices[0].caps_bitmap[EV_SYN][EV_KEY / 8] |= (1u << (EV_KEY % 8));
  // Advertise KEY_A: set bit KEY_A (=30) in the EV_KEY-type bitmap (ev=1).
  // KEY_A=30 → byte 3, bit 6.
  devices[0].caps_bitmap[EV_KEY][KEY_A / 8] |= (1u << (KEY_A % 8));

  struct input_id probe_id = {0, 0, 0, 0};
  init_stub(&devices[1], 1, "evdev test dev", probe_id, 0);
  num_devices = 2;
  device_register_shm("input/event0", -1, 0);
  device_register_shm("input/event1", -1, 1);
}
#endif

// Core ioctl handler shared by the inline (RECV_REQ, arg<=48B) and variable-
// length (RECV_IOCTL, arg>48B) proxy paths. The caller extracts cmd/minor/src
// (and grab_val for the inline-only EVIOCGRAB) from whichever recv_msg variant
// arrived; this function is agnostic to the transport.
static void handle_ioctl(uint32_t cmd, uint32_t minor, pid_t src,
                         int32_t grab_val) {
  uint8_t nr = _IOC_NR(cmd);
  uint16_t size = _IOC_SIZE(cmd);
  // Reply data buffer (pure data — result is passed separately to resp()).
  uint8_t data[REPLY_BUF_SIZE];
  memset(data, 0, REPLY_BUF_SIZE);
  int32_t result = 0;
  uint16_t data_len = 0;

  struct evdev_device *dev = find_device(minor);
  if (!dev) {
    resp(NULL, 0, -ENODEV);
    return;
  }

  // EVIOCGRAB itself bypasses grab check (holder must be able to release)
  if (cmd != (uint32_t)EVIOCGRAB) {
    if (dev->grabbed && dev->grab_client != src) {
      resp(NULL, 0, -EBUSY);
      return;
    }
  }

  switch (nr) {
  case 0x01: // EVIOCGVERSION
    *(int32_t *)data = EV_VERSION;
    data_len = size;
    break;
  case 0x02: // EVIOCGID
    memcpy(data, &dev->id, sizeof(struct input_id));
    data_len = size;
    break;
  case 0x06: // EVIOCGNAME(len)
  {
    uint16_t copy_len = strlen(dev->name) + 1;
    if (copy_len > size)
      copy_len = size;
    memcpy(data, dev->name, copy_len);
    data_len = size;
    break;
  }
  case 0x09: // EVIOCGPROP(len)
  {
    uint16_t copy_len = sizeof(dev->prop_bitmap);
    if (copy_len > size)
      copy_len = size;
    memcpy(data, &dev->prop_bitmap, copy_len);
    data_len = size;
    break;
  }
  default:
    if (nr >= 0x20 && nr < 0x40) { // EVIOCGBIT(ev, len)
      uint8_t ev = nr - 0x20;
      if (ev < EV_CNT) {
        uint16_t bitmap_len = sizeof(dev->caps_bitmap[ev]); // = 96
        uint16_t copy_len = bitmap_len;
        if (copy_len > size)
          copy_len = size;
        memcpy(data, dev->caps_bitmap[ev], copy_len);
      }
      data_len = size;
    } else if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(abs)
      result = -ENOSYS;
    } else if (nr == 0x90) { // EVIOCGRAB
      // grab_val is supplied by the caller (inline path reads it from req_data;
      // the variable-length path never carries EVIOCGRAB, whose _IOC_SIZE ==
      // sizeof(int) <= 48 always goes inline).
      if (grab_val) {
        dev->grabbed = true;
        dev->grab_client = src;
      } else {
        dev->grabbed = false;
        dev->grab_client = 0;
      }
      // EVIOCGRAB is write-only: no data returned.
    } else {
      result = -ENOSYS;
    }
    break;
  }

  resp(data, data_len, result);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;

#ifdef TEST
  register_stubs();
#endif

  while (1) {
    struct recv_msg msg;
    // Variable-length RECV_IOCTL requests carry their arg data here: the kernel
    // copies the kmalloc'd arg into data_buf and frees the kernel buffer before
    // returning (see kernel/xcore/ipc.c sys_recv). NULL would make the kernel
    // reject those with EINVAL and drop the request. 256B covers any arg we
    // serve (read getters only; size is _IOC_SIZE(cmd), capped by libc ioctl).
    uint8_t data_buf[256];
    int rc = recv(&msg, data_buf, sizeof(data_buf), 0);
    if (rc < 0)
      continue;

    uint32_t cmd;
    uint32_t minor;
    pid_t src = (pid_t)msg.src;
    int32_t grab_val = 0;
    if (msg.type == RECV_REQ) {
      // inline path: req_data = [cmd][arg<=48B][minor@52]
      cmd = *(uint32_t *)msg.data;
      minor = *(uint32_t *)(msg.data + 52);
      if (_IOC_DIR(cmd) & _IOC_WRITE)
        grab_val = *(int32_t *)(msg.data + 4);
    } else if (msg.type == RECV_IOCTL) {
      // variable-length path: arg data already delivered via data_buf above;
      // pure-read getters (the only ones that exceed 48B) don't read it back.
      cmd = msg.ioctl.cmd;
      minor = msg.ioctl.minor;
    } else {
      continue;
    }
    handle_ioctl(cmd, minor, src, grab_val);
  }

  return 0;
}
