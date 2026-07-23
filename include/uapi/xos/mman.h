/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_MMAN_H
#define COMMON_MMAN_H

// Memory protection flags (Linux x86-64 values)
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
// Linux flags with no OS-specific semantics: accepted and masked to no-op.
// PROT_SEM/PROT_GROWSDOWN apply to mprotect; PROT_GROWSUP is an mmap flag
// (mprotect rejects it like Linux). No stack-grow mechanism exists here.
#define PROT_SEM 0x10
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP 0x02000000

// Mapping types (Linux x86-64 values)
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_SHARED_VALIDATE 0x03 // Linux 4.15+; treated as MAP_SHARED here
#define MAP_FIXED 0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_ANONYMOUS 0x20
#define MAP_HUGETLB 0x40000
#define MAP_POPULATE 0x8000
#define MAP_STACK 0x20000
#define MAP_LOCKED 0x2000
#define MAP_NORESERVE 0x4000
#define MAP_GROWSDOWN 0x100 // OS has no stack-grow; accepted as no-op
#define MAP_GROWSUP 0x200   // OS has no stack-grow; accepted as no-op

// mmap return value on failure
#define MAP_FAILED ((void *)-1)

// memfd_create flags
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

#endif /* COMMON_MMAN_H */
