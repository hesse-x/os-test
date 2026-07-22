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

// Mapping types (Linux x86-64 values)
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_ANONYMOUS 0x20
#define MAP_HUGETLB 0x40000
#define MAP_POPULATE 0x8000
#define MAP_STACK 0x20000
#define MAP_LOCKED 0x100
#define MAP_NORESERVE 0x4000

// mmap return value on failure
#define MAP_FAILED ((void *)-1)

// memfd_create flags
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

#endif /* COMMON_MMAN_H */
