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
#define REPLY_BUF_SIZE 56

struct evdev_device {
  uint16_t minor;
  char name[64];
  struct input_id id;
  uint32_t caps_bitmap[4];
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
                      const char *name, struct input_id id, uint32_t caps0,
                      uint32_t caps1, uint32_t prop) {
  dev->minor = minor;
  strncpy(dev->name, name, sizeof(dev->name) - 1);
  dev->id = id;
  dev->caps_bitmap[0] = caps0;
  dev->caps_bitmap[1] = caps1;
  dev->prop_bitmap = prop;
  dev->grabbed = false;
  dev->grab_client = 0;
}

static void register_stubs(void) {
  struct input_id kbd_id = {BUS_USB, 0x0001, 0x0001, 0x0001};
  init_stub(&devices[0], 0, "evdev keyboard", kbd_id, (1u << EV_KEY),
            (1u << KEY_A), 0);
  struct input_id probe_id = {0, 0, 0, 0};
  init_stub(&devices[1], 1, "evdev test dev", probe_id, 0, 0, 0);
  num_devices = 2;
  device_register_shm("input/event0", -1, 0);
  device_register_shm("input/event1", -1, 1);
}
#endif

static void handle_req(struct recv_msg *msg) {
  if (msg->type != RECV_REQ)
    return;

  uint32_t cmd = *(uint32_t *)msg->data;
  uint32_t minor = *(uint32_t *)(msg->data + 52);
  pid_t src = (pid_t)msg->src;

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
      uint16_t bitmap_len = ev < 4 ? sizeof(dev->caps_bitmap[ev]) : 0;
      if (ev < 4) {
        uint16_t copy_len = bitmap_len;
        if (copy_len > size)
          copy_len = size;
        memcpy(data, &dev->caps_bitmap[ev], copy_len);
      }
      data_len = size;
    } else if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(abs)
      result = -ENOSYS;
    } else if (nr == 0x90) { // EVIOCGRAB
      int32_t grab_val = 0;
      if (_IOC_DIR(cmd) & _IOC_WRITE) {
        grab_val = *(int32_t *)(msg->data + 4);
      }
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
    int rc = recv(&msg, NULL, 0, 0);
    if (rc < 0)
      continue;
    if (msg.type == RECV_REQ)
      handle_req(&msg);
  }

  return 0;
}
