/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <syscall.h>
#include <xos/mman.h>
#include <stdint.h>
#include <sys/mman.h>

// POSIX-like mmap: (addr, length, prot, flags, fd, offset)
// For MAP_SHARED (SHM): fd is the SHM fd or FD_DEV fd
// For MAP_PHYSICAL: fd=-1, offset is phys addr
// For anonymous: fd=-1, offset ignored
//
// Kernel handles FD_DEV mmap internally (both kernel device ops->mmap
// and user-space driver auto shm_attach). Libc no longer needs
// __fd_dev_target_pid.
void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           uint64_t offset) {
  // MAP_SHARED: SHM or DEV fd mapping (kernel handles both)
  if (flags & MAP_SHARED) {
    void *r = sys_mmap(addr, length, prot, flags, fd, offset);
    if (r == MAP_FAILED)
      return MAP_FAILED;
    return r;
  }

  // MAP_PHYSICAL: pass fd=-1, offset=phys_addr
  // Anonymous: pass fd=-1, offset=0
  void *r = sys_mmap(addr, length, prot, flags, -1, offset);
  if (r == MAP_FAILED)
    return MAP_FAILED;
  return r;
}

int munmap(void *addr, size_t length) { return sys_munmap(addr, length); }

int memfd_create(const char *name, unsigned int flags) {
  int fd = sys_memfd_create(name, flags);
  if (fd < 0)
    return -1;
  return fd;
}
