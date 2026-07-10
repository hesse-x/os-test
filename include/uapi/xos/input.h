/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_INPUT_H
#define COMMON_INPUT_H

#include <stdint.h>
#include <xos/types.h> // pid_t

// ===================== Input SHM protocol (evdev-style) =====================

#define INPUT_SHM_MAGIC 0x494E5055 // "INPU"
#define INPUT_SHM_VERSION 1

#define INPUT_DEV_KBD 1
#define INPUT_DEV_MOUSE 2
#define INPUT_DEV_TOUCHPAD 3
#define INPUT_DEV_GAMEPAD 4

// evdev-style event types (aligned with linux/input.h)
#define EV_KEY 0x01 // key, button
#define EV_REL 0x02 // relative motion (mouse move, wheel)
#define EV_ABS 0x03 // absolute coord (touchpad, stick)
#define EV_SYN 0x00 // sync separator

// evdev protocol version + capability bounds (aligned with linux/input.h)
#define EV_VERSION 0x010001
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)
#define KEY_MAX 0x2ff
#define KEY_CNT (KEY_MAX + 1)

// Bus types (subset of linux/input.h)
#define BUS_USB 0x03

// Ring capacity: 128 slots per consumer SHM page (1 page = 4KB)
#define INPUT_RING_CAPACITY_DEFAULT 128

typedef struct input_shm_header {
  uint32_t magic;         // INPUT_SHM_MAGIC
  uint32_t version;       // INPUT_SHM_VERSION
  uint32_t device_type;   // INPUT_DEV_KBD / MOUSE / TOUCHPAD / GAMEPAD
  uint32_t event_size;    // sizeof(input_event), consumer sanity check
  uint32_t ring_offset;   // ring start offset (after header)
  uint32_t ring_capacity; // ring slot count (default 128)
  uint32_t head;          // write position (driver writes)
  uint32_t tail;          // read position (consumer reads)
  uint8_t reserved[28];   // pad to 64 bytes (no consumer_sleeping — notify is
                          // unconditional broadcast)
} input_shm_header;

/* ringbuf_header — 替代 input_shm_header (design 3.5) */
#define RINGBUF_MAGIC 0x52424E52 /* "RBNR" */
#define RINGBUF_OPEN 1
#define RINGBUF_CLOSE 2

typedef struct ringbuf_header {
  uint32_t magic;       /* RINGBUF_MAGIC */
  uint32_t version;     /* 1 */
  uint32_t capacity;    /* ring slot 数 */
  uint32_t head;        /* 写入位置 (evdev 推进) */
  uint32_t data_offset; /* ring data 区在 SHM 内的偏移 */
  uint32_t elem_size;   /* sizeof(input_event) */
} ringbuf_header;

/* 生命周期通知消息 (内核 → evdev, via RECV_REQ) */
struct ringbuf_lifecycle_msg {
  uint32_t opcode; /* RINGBUF_OPEN / RINGBUF_CLOSE */
  pid_t pid;       /* 操作者 PID */
  char name[32];   /* 设备名 */
};

typedef struct input_event {
  uint64_t timestamp_ns; // monotonic timestamp
  uint16_t type;         // EV_KEY / EV_REL / EV_ABS / EV_SYN
  uint16_t code;         // KEY_A / BTN_LEFT / REL_X / ABS_X ...
  int32_t value;         // key: 1=press 0=release; rel: delta; abs: coordinate
} input_event;           // 16 bytes

// Device identity (EVIOCGID)
struct input_id {
  uint16_t bustype;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

// Absolute axis info (EVIOCGABS)
struct input_absinfo {
  int32_t value;
  int32_t minimum;
  int32_t maximum;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

// Bind ioctl arg (consumer → driver)
// Direction A: driver owns SHM (bound to /dev/<name> inode). Consumer just
// registers its pid for notify; shm_fd is unused (-1).
struct input_bind_arg {
  int shm_fd; // unused in Direction A (kept for ABI stability)
  int result; // output: 0 on success
};

#endif /* COMMON_INPUT_H */
