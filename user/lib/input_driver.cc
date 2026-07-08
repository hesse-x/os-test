/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// libinput_driver: driver-side main loop + single SHM ring (bound to
// /dev/<name> inode)
// + unconditional notify to all bound consumers.
//
// Direction A (decided 2026-06-30): SHM owned by driver, bound to device inode
// via device_register_shm. Consumers access via open("/dev/<name>") +
// mmap(MAP_SHARED, fd). BIND request only registers the consumer pid for notify
// (no cross-process fd passing).
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>
#include <xos/errno.h>
#include <xos/input.h>
#include <xos/input_key.h>
#include <xos/shm.h>

#define MAX_CONSUMERS 8

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

// Driver-side SHM ring (single, shared via inode).
static volatile input_shm_header *g_ring_hdr;
static uint32_t g_ring_cap;
static uint32_t g_ring_off;

// Write one event to the shared ring + unconditional notify to all consumers.
static void broadcast_event(const input_event *ev) {
  volatile input_shm_header *hdr = g_ring_hdr;
  if (!hdr)
    return;

  uint32_t head = __atomic_load_n(&hdr->head, __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n(&hdr->tail, __ATOMIC_ACQUIRE);
  uint32_t next = (head + 1) % g_ring_cap;
  if (next == tail)
    return; // ring full, drop

  volatile input_event *slot =
      (volatile input_event *)((volatile uint8_t *)hdr + g_ring_off +
                               head * sizeof(input_event));
  slot->timestamp_ns = ev->timestamp_ns;
  slot->type = ev->type;
  slot->code = ev->code;
  slot->value = ev->value;
  __atomic_store_n(&hdr->head, next, __ATOMIC_RELEASE);

  // Unconditional notify. If pid gone/exited, notify is no-op.
  for (int i = 0; i < MAX_CONSUMERS; i++) {
    pid_t p = consumers[i];
    if (p > 0)
      notify(p);
  }
}

static void handle_req(struct recv_msg *msg) {
  if (msg->type != RECV_REQ)
    return;
  uint32_t opcode = *(uint32_t *)msg->data;

  int32_t result = 0;
  if (opcode == (uint32_t)INPUT_BIND) {
    // Direction A: consumer just registers itself. SHM is accessed via
    // open("/dev/<name>") + mmap(MAP_SHARED, fd) — no fd passing.
    consumer_add((pid_t)msg->src);
    result = 0;
  } else if (opcode == (uint32_t)INPUT_UNBIND) {
    consumer_remove_pid((pid_t)msg->src);
    result = 0;
  } else {
    result = -EINVAL;
  }
  // INPUT_BIND/UNBIND carry no _IOC_READ data; pass NULL/0 and result only
  sys_resp(NULL, 0, result);
}

void input_driver_run(uint32_t device_type, const char *dev_name,
                      const char *hid_dev_path,
                      int (*on_event)(input_event *ev),
                      void (*hid_init)(void *hid_shm)) {
  // 1. Open HID node + mmap kernel HID SHM (this triggers usb_hid_kbd_open
  // callback,
  //    adding our pid into xhci kbd_openers[])
  int hid_fd = open(hid_dev_path, O_RDWR);
  void *hid_shm =
      mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, hid_fd, 0);
  if (hid_init)
    hid_init(hid_shm);

  // 2. Create output SHM ring (driver-owned) + bind to /dev/<dev_name> inode.
  //    Consumers access via open("/dev/<dev_name>") + mmap(MAP_SHARED, fd).
  int shm_fd = memfd_create("input_ring", 0);
  ftruncate(shm_fd, 4096);
  void *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

  // Zero-init + fill header
  for (int i = 0; i < 4096; i++)
    ((volatile uint8_t *)shm)[i] = 0;
  volatile input_shm_header *hdr = (volatile input_shm_header *)shm;
  hdr->magic = INPUT_SHM_MAGIC;
  hdr->version = INPUT_SHM_VERSION;
  hdr->device_type = device_type;
  hdr->event_size = sizeof(input_event);
  hdr->ring_offset = sizeof(input_shm_header);
  hdr->ring_capacity = INPUT_RING_CAPACITY_DEFAULT;
  hdr->head = 0;
  hdr->tail = 0;

  g_ring_hdr = hdr;
  g_ring_off = hdr->ring_offset;
  g_ring_cap = hdr->ring_capacity;

  device_register_shm(dev_name, shm_fd, 0);

  // 3. Main loop: recv() → EINTR: drain HID via on_event, write ring + notify;
  //    REQ: handle bind/unbind.
  while (1) {
    struct recv_msg msg;
    int rc = recv(&msg, NULL, 0, 0);

    if (rc < 0) {
      if (errno != EINTR)
        continue;
      // ISR woke us — drain HID reports
      input_event ev;
      while (on_event(&ev)) {
        broadcast_event(&ev);
      }
      continue;
    }

    if (msg.type == RECV_REQ) {
      handle_req(&msg);
      continue;
    }
    // RECV_NOTIFY etc. ignored
  }
}
