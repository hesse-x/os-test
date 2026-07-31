/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/cdefs.h>
#include <sys/time.h> // struct timeval (for ru_utime/ru_stime below)
#include <sys/types.h>

/* id_t for the getpriority/setpriority signature (musl: int (int, id_t, int)).
 * bits/alltypes.h gates the typedef on __NEED_id_t; define it so the include
 * above (sys/types.h → bits/alltypes.h) exposes id_t. Mesa's u_queue.c calls
 * setpriority(PRIO_PROCESS, ...) and needs both the decl and PRIO_PROCESS. */
#define __NEED_id_t
#include <bits/alltypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal <sys/resource.h> — only the types and constants needed to
 * compile programs that reference the header.  getrlimit / setrlimit
 * return ENOSYS (no resource limits enforced). */

/* Resource identifiers (from POSIX / Linux) */
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9
#define RLIMIT_LOCKS 10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE 13
#define RLIMIT_RTPRIO 14
#define RLIMIT_RTTIME 15
#define RLIM_NLIMITS 16

/* Resource usage (empty — no per-process statistics yet) */
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

/* Priority (nice) control. setpriority/getpriority are exported by libc
 * (musl src/misc/{set,get}priority.c → syscall(SYS_setpriority/getpriority)),
 * BUT the kernel does not yet implement those two syscalls (no dispatch case in
 * kernel/bsd/syscall.c) — so the wrappers return -ENOSYS. They are exported
 * only so callers that merely reference the symbol (Mesa's u_queue.c lowers
 * worker- thread priority; failure is benign) link and compile. Decl +
 * constants are the minimal <sys/resource.h> surface needed; full priority
 * support is a kernel TODO. */
#define PRIO_MIN (-20)
#define PRIO_MAX 20
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

typedef unsigned long rlim_t;

struct rlimit {
  rlim_t rlim_cur; /* soft limit */
  rlim_t rlim_max; /* hard limit */
};

struct rusage {
  struct timeval ru_utime; /* user time used */
  struct timeval ru_stime; /* system time used */
  /* remaining fields zeroed — not tracked */
  long ru_maxrss;
  long ru_ixrss;
  long ru_idrss;
  long ru_isrss;
  long ru_minflt;
  long ru_majflt;
  long ru_nswap;
  long ru_inblock;
  long ru_oublock;
  long ru_msgsnd;
  long ru_msgrcv;
  long ru_nsignals;
  long ru_nvcsw;
  long ru_nivcsw;
};

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RESOURCE_H */
