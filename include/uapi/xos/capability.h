/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Capability identifiers (aligned with Linux uapi/linux/capability.h numbers).
 * Capability identifiers and the bounded XOS service-profile representation.
 */
#ifndef _XOS_CAPABILITY_H
#define _XOS_CAPABILITY_H

#define CAP_CHOWN 0        // chown/fchown
#define CAP_DAC_OVERRIDE 1 // inode_permission W/X override
#define CAP_DAC_READ_SEARCH                                                    \
  2 // inode_permission R override (not split this round, left as todo)
#define CAP_FOWNER 3 // chmod: non-owner but euid==0
#define CAP_FSETID 4 // chmod/chown: setuid/setgid bit-clear exemption
#define CAP_KILL 5   // kill_permitted
#define CAP_SETGID 6 // setgid ladder (ladder untouched this round, reserved)
#define CAP_SETUID 7 // setuid ladder (same as above)
#define CAP_SYS_ADMIN 21 // sys_mount
#define CAP_SYS_TIME 25  // sys_clock_settime

#define XOS_CAP_LAST CAP_SYS_TIME
#define XOS_CAP_VALID_MASK ((1ULL << (XOS_CAP_LAST + 1)) - 1ULL)
#define XOS_CAP_BIT(cap) (1ULL << (cap))

struct xos_cap_state {
  unsigned long long permitted;
  unsigned long long effective;
  unsigned long long inheritable;
};

#endif /* _XOS_CAPABILITY_H */
