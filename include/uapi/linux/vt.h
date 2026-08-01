/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _UAPI_LINUX_VT_H
#define _UAPI_LINUX_VT_H

struct vt_mode {
  char mode;
  char waitv;
  short relsig;
  short acqsig;
  short frsig;
};

struct vt_stat {
  unsigned short v_active;
  unsigned short v_signal;
  unsigned short v_state;
};

#define VT_SETMODE 0x5602
#define VT_AUTO 0x00
#define VT_PROCESS 0x01
#define VT_ACKACQ 0x02
#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606

#endif
