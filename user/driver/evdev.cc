/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// evdev user-space driver: real keyboard event source (xHCI HID → SHM ring →
// terminal consumer) + EVIOCG* ioctl query handler. Replaces the old kbd
// driver; terminal now opens /dev/input/event0.
#include "user/include/usb_hid.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>
#include <xos/input.h>
#include <xos/input_key.h>
#include <xos/ioctl.h>
#include <xos/ringbuf.h>
#include <xos/shm.h>

#define MAX_EVDEV_DEVICES 8
#define MAX_CONSUMERS 8
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

// Bound consumer pids (for notify). SHM is single shared ring on /dev/<name>
// inode.
static pid_t consumers[MAX_CONSUMERS];

static void consumer_add(pid_t pid) {
  for (int i = 0; i < MAX_CONSUMERS; i++) {
    if (consumers[i] == 0) {
      consumers[i] = pid;
      return;
    }
  }
}

static void consumer_remove_pid(pid_t pid) {
  for (int i = 0; i < MAX_CONSUMERS; i++) {
    if (consumers[i] == pid)
      consumers[i] = 0;
  }
}

// Driver-side SHM ring (single, shared via inode). The producer writes through
// this header; consumers read via the kernel ring_t path (per-fd f->offset).
static volatile ringbuf_header *g_ring;

/* 唤醒 fd: /dev/input/event0 的 ringbuf fd, 用于 ioctl RINGBUF_WAKE */
static int wake_fd = -1;

// Write one event to the shared ring + unconditional notify to all consumers.
// Overwrite-oldest policy: push never fails (no "full" check), and the wake
// below always runs — a dropped wake is what stalled the terminal.
static void broadcast_event(const input_event *ev) {
  if (!g_ring)
    return;
  fprintf(stderr, "evdev broadcast: type=%u code=%u value=%d\n",
          (unsigned)ev->type, (unsigned)ev->code, (int)ev->value);
  ringbuf_push(g_ring, ev);

  // Unconditional notify. If pid gone/exited, notify is no-op.
  for (int i = 0; i < MAX_CONSUMERS; i++) {
    pid_t p = consumers[i];
    if (p > 0)
      notify(p);
  }

  /* 唤醒 ringbuf poll/epoll 等待者 (terminal/libinput) */
  if (wake_fd >= 0) {
    int r = ioctl(wake_fd, RINGBUF_WAKE, 0);
    if (r < 0)
      fprintf(stderr, "evdev: RINGBUF_WAKE failed errno=%d\n", errno);
  }
}

static void handle_req(struct recv_msg *msg) {
  if (msg->type != RECV_REQ)
    return;
  uint32_t opcode = *(uint32_t *)msg->data;

  int32_t result = 0;
  if (opcode == (uint32_t)INPUT_BIND) {
    // Direction A: consumer just registers itself. SHM is accessed via
    // open("/dev/input/event0") + mmap(MAP_SHARED, fd) — no fd passing.
    consumer_add((pid_t)msg->src);
    result = 0;
  } else if (opcode == (uint32_t)INPUT_UNBIND) {
    consumer_remove_pid((pid_t)msg->src);
    result = 0;
  } else {
    result = -EINVAL;
  }
  // INPUT_BIND/UNBIND are _IOWR('I', ...): the kernel copies the 8-byte arg
  // (struct input_bind_arg { int shm_fd; int result; }) back to the caller on
  // reply. The caller (terminal/test) judges success by bind_arg.result, so we
  // must write result into offset 4 and return it as a full 8-byte reply — not
  // just pass result as status with reply_len=0 (that leaves bind_arg.result at
  // its caller-initialized -1 and the caller loops forever).
  struct input_bind_arg reply;
  reply.shm_fd = -1;
  reply.result = result;
  sys_resp(&reply, sizeof(reply), result);
}

// on_key_event: each call fills one event, returns 1=has event / 0=HID empty.
static int on_key_event(input_event *ev) {
  struct key_event ke;
  if (get_keycode(&ke) != 0)
    return 0; // HID ring empty
  uint64_t ns = sys_gettime();
  ev->tv_sec = ns / 1000000000ULL;
  ev->tv_usec = (ns % 1000000000ULL) / 1000;
  ev->type = EV_KEY;
  ev->code = ke.key;      // KEY_A, KEY_B, ... (evdev-aligned in input_key.h)
  ev->value = ke.pressed; // 1=press, 0=release
  return 1;
}

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

// Advertise EV_KEY support + the set of keys the terminal can map to ASCII.
static void init_caps(struct evdev_device *dev) {
  memset(dev->caps_bitmap, 0, sizeof(dev->caps_bitmap));
  // ev=0 (EV_SYN-type) bitmap advertises supported event types: set EV_KEY.
  dev->caps_bitmap[EV_SYN][EV_KEY / 8] |= (1u << (EV_KEY % 8));
  // ev=1 (EV_KEY-type) bitmap advertises supported key codes.
  static const uint16_t keys[] = {
      KEY_ESC,      KEY_1,          KEY_2,          KEY_3,
      KEY_4,        KEY_5,          KEY_6,          KEY_7,
      KEY_8,        KEY_9,          KEY_0,          KEY_MINUS,
      KEY_EQUAL,    KEY_BACKSPACE,  KEY_TAB,        KEY_Q,
      KEY_W,        KEY_E,          KEY_R,          KEY_T,
      KEY_Y,        KEY_U,          KEY_I,          KEY_O,
      KEY_P,        KEY_LEFTBRACE,  KEY_RIGHTBRACE, KEY_ENTER,
      KEY_LEFTCTRL, KEY_A,          KEY_S,          KEY_D,
      KEY_F,        KEY_G,          KEY_H,          KEY_J,
      KEY_K,        KEY_L,          KEY_SEMICOLON,  KEY_APOSTROPHE,
      KEY_GRAVE,    KEY_LEFTSHIFT,  KEY_BACKSLASH,  KEY_Z,
      KEY_X,        KEY_C,          KEY_V,          KEY_B,
      KEY_N,        KEY_M,          KEY_COMMA,      KEY_DOT,
      KEY_SLASH,    KEY_RIGHTSHIFT, KEY_LEFTALT,    KEY_SPACE,
      KEY_CAPSLOCK,
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    uint16_t k = keys[i];
    dev->caps_bitmap[EV_KEY][k / 8] |= (1u << (k % 8));
  }
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;

  // 1. Open HID node + mmap kernel HID SHM (triggers usb_hid_kbd_open,
  // adding our pid into xhci kbd_openers[]).
  int hid_fd = open("/dev/usb_hid_kbd", O_RDWR);
  void *hid_shm =
      mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, hid_fd, 0);
  get_keycode_init(hid_shm);

  // 2. Build event0 device record (minor=0): real keyboard identity + caps.
  struct evdev_device *dev = &devices[0];
  dev->minor = 0;
  strncpy(dev->name, "evdev keyboard", sizeof(dev->name) - 1);
  dev->id.bustype = BUS_USB;
  dev->id.vendor = 0x0001;
  dev->id.product = 0x0001;
  dev->id.version = 0x0001;
  dev->prop_bitmap = 0;
  dev->grabbed = false;
  dev->grab_client = 0;
  init_caps(dev);
  num_devices = 1;

  // 3. Create output SHM ring (driver-owned) + bind to /dev/input/event0 inode.
  // 3. Create output SHM ring (driver-owned) + bind to /dev/input/event0 inode.
  //    Layout: ringbuf_header at offset 0, ring data area at offset 128.
  //    Consumers read via the kernel ring_t path (open + read/poll); the
  //    producer pushes with ringbuf_push() (overwrite-oldest, never fails).
  int shm_fd = memfd_create("input_ring", 0);
  ftruncate(shm_fd, 4096);
  void *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

  for (int i = 0; i < 4096; i++)
    ((volatile uint8_t *)shm)[i] = 0;

  // ringbuf_header for kernel ring_t (read/poll) + user-space ringbuf_push.
  volatile ringbuf_header *rb = (volatile ringbuf_header *)shm;
  rb->magic = RINGBUF_MAGIC;
  rb->version = 1;
  rb->capacity = INPUT_RING_CAPACITY_DEFAULT;
  rb->head = 0;
  rb->data_offset = 128;
  rb->elem_size = sizeof(input_event);

  g_ring = rb;

  device_register_shm("input/event0", shm_fd, 0);

  /* Open /dev/input/event0 for RINGBUF_WAKE ioctl (wake ringbuf
   * poll/epoll consumers like terminal/libinput). */
  wake_fd = open("/dev/input/event0", O_RDWR);
  if (wake_fd < 0)
    fprintf(stderr, "evdev: wake open failed errno=%d\n", errno);

  {
    struct dev_props props;
    memset(&props, 0, sizeof(props));
    props.bustype = BUS_USB;
    props.vendor = 0x0001;
    props.product = 0x0001;
    props.version = 0x0001;
    strncpy(props.name, "evdev keyboard", 63);
    props.name[63] = '\0';
    device_set_meta("input/event0", "input", "evdev", &props);
  }

  // 4. Main loop: recv() → EINTR: drain HID reports → ring + notify;
  //    RECV_REQ: INPUT_BIND/UNBIND (type='I') or inline EVIOCG* (type='E');
  //    RECV_IOCTL: variable-length EVIOCG* (arg>48B).
  while (1) {
    struct recv_msg msg;
    // Variable-length RECV_IOCTL requests carry their arg data here: the kernel
    // copies the kmalloc'd arg into data_buf and frees the kernel buffer before
    // returning. NULL would make the kernel reject those with EINVAL. 256B
    // covers any arg we serve (read getters only; size = _IOC_SIZE(cmd)).
    uint8_t data_buf[256];
    int rc = recv(&msg, data_buf, sizeof(data_buf), 0);
    fprintf(stderr, "evdev recv: rc=%d errno=%d\n", rc, errno);

    if (rc < 0) {
      if (errno == EINTR) {
        // ISR woke us — drain HID reports
        input_event ev;
        int nevents = 0;
        while (on_key_event(&ev)) {
          broadcast_event(&ev);
          nevents++;
        }
        fprintf(stderr, "evdev EINTR drain: nevents=%d\n", nevents);
        // Send EV_SYN/SYN_REPORT after batch — libinput/evdev requires it
        // to commit the event sequence.
        if (nevents > 0) {
          input_event syn_ev;
          memset(&syn_ev, 0, sizeof(syn_ev));
          uint64_t syn_ns = sys_gettime();
          syn_ev.tv_sec = syn_ns / 1000000000ULL;
          syn_ev.tv_usec = (syn_ns % 1000000000ULL) / 1000;
          syn_ev.type = EV_SYN;
          syn_ev.code = 0; // SYN_REPORT
          syn_ev.value = 0;
          broadcast_event(&syn_ev);
        }
      }
      continue;
    }

    if (msg.type == RECV_REQ) {
      uint32_t opcode = *(uint32_t *)msg.data;
      // type=='I' → INPUT_BIND/UNBIND; type=='E' → inline EVIOCG* ioctl.
      if (_IOC_TYPE(opcode) == 'I') {
        handle_req(&msg);
      } else {
        // inline path: req_data = [cmd][arg<=48B][minor@52]
        uint32_t cmd = opcode;
        uint32_t minor = *(uint32_t *)(msg.data + 52);
        int32_t grab_val =
            (_IOC_DIR(cmd) & _IOC_WRITE) ? *(int32_t *)(msg.data + 4) : 0;
        handle_ioctl(cmd, minor, (pid_t)msg.src, grab_val);
      }
    } else if (msg.type == RECV_IOCTL) {
      handle_ioctl(msg.ioctl.cmd, msg.ioctl.minor, (pid_t)msg.src, 0);
    }
  }

  return 0;
}
