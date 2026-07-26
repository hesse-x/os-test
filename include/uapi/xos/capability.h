/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 *
 * Capability identifiers (对齐 Linux uapi/linux/capability.h 编号)。
 * 本 OS 今天无 capability bitmap：capable() 仅判 euid==0，所有 cap 等价 root。
 * 编号对齐 Linux 以便未来接 libcap/capget 零摩擦，bitmap 实化时按 cap 分流。
 * 详见 doc/design/todo.md。
 */
#ifndef _XOS_CAPABILITY_H
#define _XOS_CAPABILITY_H

#define CAP_CHOWN 0        /* chown/fchown */
#define CAP_DAC_OVERRIDE 1 /* inode_permission W/X override */
#define CAP_DAC_READ_SEARCH                                                    \
  2 /* inode_permission R override（本轮未拆，留 todo） */
#define CAP_FOWNER 3     /* chmod: 非 owner 但 euid==0 */
#define CAP_FSETID 4     /* chmod/chown: setuid/setgid 位清除豁免 */
#define CAP_KILL 5       /* kill_permitted */
#define CAP_SETGID 6     /* setgid 阶梯（本轮不动阶梯，仅预留） */
#define CAP_SETUID 7     /* setuid 阶梯（同上） */
#define CAP_SYS_ADMIN 21 /* sys_mount */
#define CAP_SYS_TIME 25  /* sys_clock_settime */

#endif /* _XOS_CAPABILITY_H */
