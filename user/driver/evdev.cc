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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>
#include <xos/input.h>
#include <xos/input_key.h>
#include <xos/ioctl.h>
#include <xos/shm.h>

#define MAX_EVDEV_DEVICES 8
// Reply buffer for inline RECV_REQ path and the data evdev hands to sys_resp.
// Must hold the largest getter we serve: EVIOCGBIT(EV_KEY, KEY_CNT) = 96B, plus
// headroom for future EVIOCGABS (struct input_absinfo across ABS_CNT axes) etc.
#define REPLY_BUF_SIZE 256

/* minor → owner write-fd (broker broadcast fd, returned by INPUT_REGISTER). */
static int device_table[MAX_EVDEV_DEVICES];

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

// Broadcast a batch of events to all clients via the broker owner write-fd.
// The kernel broker fans out to per-client kfifo and wakes blocked readers.
static void broadcast_events(const input_event *evs, int n) {
  if (n <= 0)
    return;
  int fd = device_table[0];
  if (fd < 0)
    return;
  ssize_t r = write(fd, evs, (size_t)n * sizeof(input_event));
  if (r < 0)
    fprintf(stderr, "evdev: broadcast write failed errno=%d\n", errno);
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
  // adding our pid into xhci kbd_openers[]). /dev/hidraw0 is the xHCI HID
  // node (Ring #1, refact_evdev.md §14); third-party hidraw tools read() it.
  for (int i = 0; i < MAX_EVDEV_DEVICES; i++)
    device_table[i] = -1;
  int hid_fd = open("/dev/hidraw0", O_RDWR);
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

  // 3. Open control node + register event0 via INPUT_REGISTER ioctl.
  //    Broker returns an owner write-fd; write() broadcasts to all clients.
  int control_fd = open("/dev/input/control", O_RDWR);
  if (control_fd < 0) {
    fprintf(stderr, "evdev: control open failed errno=%d\n", errno);
    return 1;
  }
  struct {
    char name[64];
    uint32_t minor;
  } reg;
  __builtin_memset(&reg, 0, sizeof(reg));
  strncpy(reg.name, "input/event0", sizeof(reg.name) - 1);
  reg.minor = 0;
  int owner_fd = ioctl(control_fd, INPUT_REGISTER, &reg);
  if (owner_fd < 0) {
    fprintf(stderr, "evdev: INPUT_REGISTER failed errno=%d\n", errno);
    return 1;
  }
  device_table[0] = owner_fd; /* minor 0 → owner write-fd */

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

  // 5. Split the two event sources onto separate pollable fds (evdev_refact.md
  //    §3.1/§4.4): HID hardware interrupts arrive on irqfd (an eventfd the
  //    xHCI ISR signals via eventfd_signal_isr), downstream IPC requests
  //    arrive on ipcfd (bound to our own recv queue).  epoll_wait joins them.
  int irqfd = eventfd(0, EFD_NONBLOCK); // HID interrupt fd (LT, default)
  if (irqfd < 0)
    fprintf(stderr, "evdev: irqfd create failed errno=%d\n", errno);
  int ipcfd = ipcfd_create(); // downstream-IPC fd (read = dequeue, §4.3)
  if (ipcfd < 0)
    fprintf(stderr, "evdev: ipcfd create failed errno=%d\n", errno);
  // Bind irqfd into the xHCI HID interrupt registry (§4.2).  hid_fd is the
  // /dev/hidraw0 fd opened at the top of main.
  int brc = ioctl(hid_fd, HID_BIND_IRQFD, &irqfd);
  if (brc < 0)
    fprintf(stderr, "evdev: HID_BIND_IRQFD failed errno=%d\n", errno);

  int epfd = epoll_create1(0);
  if (epfd < 0)
    fprintf(stderr, "evdev: epoll_create1 failed errno=%d\n", errno);
  struct epoll_event epev;
  epev.events = EPOLLIN; // level-triggered (default, no EPOLLET) — §3.2/§5.4
  epev.data.fd = irqfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, irqfd, &epev);
  epev.events = EPOLLIN;
  epev.data.fd = ipcfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, ipcfd, &epev);

  // 4. Main loop: epoll_wait(irqfd | ipcfd).  HID hardware interrupts land on
  //    irqfd (read clears the count, then drain the HID SHM sub-ring to empty);
  //    downstream IPC requests land on ipcfd (read = non-blocking dequeue,
  //    -EAGAIN when empty).  The two sources are physically separated — they
  //    share no wait queue (evdev_refact.md §3.1/§4.4).  HID drain, broadcast,
  //    EV_SYN, handle_req/handle_ioctl internals are unchanged from the old
  //    recv loop; only the outer block/dispatch structure is replaced.
  struct epoll_event evs[2];
  while (1) {
    int n = epoll_wait(epfd, evs, 2, -1);
    for (int i = 0; i < n; i++) {
      if (evs[i].data.fd == irqfd) {
        // HID interrupt arrived: clear the notification count, then drain the
        // HID SHM sub-ring to empty (LT + drain-to-empty = no loss, §3.2/§5.4).
        uint64_t c;
        read(irqfd, &c, 8);
        input_event batch[16];
        int nevents = 0;
        input_event ev;
        while (nevents < (int)(sizeof(batch) / sizeof(batch[0])) &&
               on_key_event(&ev)) {
          batch[nevents++] = ev;
        }
        // Send EV_SYN/SYN_REPORT after each batch — libinput/evdev requires it
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
          broadcast_events(batch, nevents);
          broadcast_events(&syn_ev, 1);
        }
      } else if (evs[i].data.fd == ipcfd) {
        // Downstream IPC request: non-blocking dequeue (ipcfd_read =
        // ipc_dequeue, §4.3).  Drain until -EAGAIN so a batch of requests is
        // fully served.  data_buf holds RECV_IOCTL variable-length arg data.
        struct recv_msg msg;
        uint8_t data_buf[256];
        while (1) {
          int rc = ipcfd_read(ipcfd, &msg, data_buf, sizeof(data_buf));
          if (rc < 0)
            break; // -EAGAIN: queue empty (errno set by ipcfd_do_read)
          if (msg.type == RECV_REQ) {
            // inline path: req_data = [cmd][arg<=48B][minor@52]
            uint32_t cmd = *(uint32_t *)msg.data;
            uint32_t minor = *(uint32_t *)(msg.data + 52);
            int32_t grab_val =
                (_IOC_DIR(cmd) & _IOC_WRITE) ? *(int32_t *)(msg.data + 4) : 0;
            handle_ioctl(cmd, minor, (pid_t)msg.src, grab_val);
          } else if (msg.type == RECV_IOCTL) {
            handle_ioctl(msg.ioctl.cmd, msg.ioctl.minor, (pid_t)msg.src, 0);
          }
        }
      }
    }
  }

  return 0;
}
