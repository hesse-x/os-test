/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so loads .so into userspace memory (read + anonymous mapping)
// ld.md §3.3.3 / plan_ld2b3 T15
//
// ld.so does not link libc.a; syscalls use inline asm (see dl_puts.c pattern)

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>
#include <xos/fcntl.h>
#include <xos/mman.h>
#include <xos/syscall_nums.h>

// bootstrap stage helpers (hidden)
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// minilibc.c
__attribute__((visibility("hidden"))) void *memcpy(void *dst, const void *src,
                                                   unsigned long n);
__attribute__((visibility("hidden"))) void *memset(void *dst, int c,
                                                   unsigned long n);

// inline syscall wrappers (ld.so does not link libc.a, brings its own)
// 6-arg syscall register mapping: rdi, rsi, rdx, r10, r8, r9 (x86-64 SysV)
static long dl_sys_open(const char *path, int flags) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_OPEN), "D"((int64_t)(uintptr_t)path),
                     "S"((int64_t)flags)
                   : "rcx", "r11", "memory");
  return ret;
}

static long dl_sys_read(int fd, void *buf, size_t len) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_READ), "D"((int64_t)fd),
                     "S"((int64_t)(uintptr_t)buf), "d"((int64_t)len)
                   : "rcx", "r11", "memory");
  return ret;
}

static long dl_sys_lseek(int fd, int64_t offset, int whence) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_LSEEK), "D"((int64_t)fd), "S"(offset),
                     "d"((int64_t)whence)
                   : "rcx", "r11", "memory");
  return ret;
}

static long dl_sys_close(int fd) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_CLOSE), "D"((int64_t)fd)
                   : "rcx", "r11", "memory");
  return ret;
}

// 6-arg mmap: rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset
// hidden export: malloc in minilibc.c reuses the same wrapper, avoiding
// duplicate inline asm
__attribute__((visibility("hidden"))) void *dl_sys_mmap(void *addr, size_t size,
                                                        int prot, int flags,
                                                        int fd,
                                                        uint64_t offset) {
  long ret;
  register int64_t r10 __asm__("r10") = (int64_t)flags;
  register int64_t r8 __asm__("r8") = (int64_t)fd;
  register int64_t r9 __asm__("r9") = (int64_t)offset;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(SYS_MMAP), "D"((int64_t)(uintptr_t)addr),
                     "S"((int64_t)size), "d"((int64_t)prot), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return (void *)(uintptr_t)ret;
}

// this OS has no mprotect syscall yet; anonymous RW mapping is implicitly
// executable, segment permissions not enforced for now
static long dl_sys_mprotect(void *addr, size_t len, int prot) {
  (void)addr;
  (void)len;
  (void)prot;
  return 0;
}

// page alignment helpers
#define PAGE_SIZE 4096
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & ~((size_t)(PAGE_SIZE - 1)))
#define PAGE_DOWN(x) ((x) & ~((size_t)(PAGE_SIZE - 1)))

// static PHDR buffer (ld.so does not use malloc; PHDR usually < 4KB, 64 entries
// suffice)
static Elf64_Phdr dl_phdrs[64];

// load .so into userspace memory
// returns load base; outputs .dynamic section pointer via out_dyn
void *load_so(const char *path, Elf64_Dyn **out_dyn) {
  long fd = dl_sys_open(path, O_RDONLY);
  if (fd < 0) {
    dl_puts("dl: FATAL: cannot open ");
    dl_puts(path);
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }

  // 1. read ELF header
  Elf64_Ehdr ehdr;
  if (dl_sys_read((int)fd, &ehdr, sizeof(ehdr)) != (long)sizeof(ehdr)) {
    dl_puts("dl: FATAL: read ehdr failed");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }
  // ELF magic check: 0x7f 'E' 'L' 'F'
  if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
      ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
    dl_puts("dl: FATAL: bad ELF magic");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }

  // 2. read all PHDRs into static buffer
  size_t phnum = ehdr.e_phnum;
  if (phnum > sizeof(dl_phdrs) / sizeof(dl_phdrs[0])) {
    dl_puts("dl: FATAL: too many PHDRs");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }
  dl_sys_lseek((int)fd, (int64_t)ehdr.e_phoff, SEEK_SET);
  if (dl_sys_read((int)fd, dl_phdrs, phnum * sizeof(Elf64_Phdr)) !=
      (long)(phnum * sizeof(Elf64_Phdr))) {
    dl_puts("dl: FATAL: read PHDR failed");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }

  // 3. compute total load size (min vaddr ~ max vaddr, page-aligned)
  uintptr_t min_vaddr = (uintptr_t)-1, max_vaddr = 0;
  int has_load = 0;
  for (size_t i = 0; i < phnum; i++) {
    if (dl_phdrs[i].p_type == PT_LOAD) {
      has_load = 1;
      if (dl_phdrs[i].p_vaddr < min_vaddr)
        min_vaddr = dl_phdrs[i].p_vaddr;
      uintptr_t end = dl_phdrs[i].p_vaddr + dl_phdrs[i].p_memsz;
      if (end > max_vaddr)
        max_vaddr = end;
    }
  }
  if (!has_load) {
    dl_puts("dl: FATAL: no PT_LOAD");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }
  size_t load_sz = PAGE_ALIGN(max_vaddr) - PAGE_DOWN(min_vaddr);

  // 4. anonymous mapping (MAP_PRIVATE | MAP_ANONYMOUS)
  //    this OS has no mprotect syscall yet, cannot split segment permissions by
  //    PT_LOAD p_flags; use uniform RWX (kernel sys_mmap sets NX bit on
  //    mappings without PROT_EXEC, which would cause an instruction-fetch #PF
  //    on the .text segment. The load_so comment that once said "RW implicitly
  //    executable" was wrong)
  void *base = dl_sys_mmap(NULL, load_sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    dl_puts("dl: FATAL: mmap failed");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }

  // 5. for each PT_LOAD segment: lseek + read into mapped region
  for (size_t i = 0; i < phnum; i++) {
    if (dl_phdrs[i].p_type != PT_LOAD)
      continue;
    void *seg_dst = (char *)base + (dl_phdrs[i].p_vaddr - min_vaddr);
    dl_sys_lseek((int)fd, (int64_t)dl_phdrs[i].p_offset, SEEK_SET);
    if (dl_phdrs[i].p_filesz > 0) {
      dl_sys_read((int)fd, seg_dst, dl_phdrs[i].p_filesz);
    }
    // BSS zeroing (memsz > filesz portion) - mmap already returns zero pages,
    // no need to zero again but if filesz end and memsz end cross a page
    // boundary, mmap already zeroed; also zero within the same page
    if (dl_phdrs[i].p_memsz > dl_phdrs[i].p_filesz) {
      memset((char *)seg_dst + dl_phdrs[i].p_filesz, 0,
             dl_phdrs[i].p_memsz - dl_phdrs[i].p_filesz);
    }
  }

  // 6. set segment permissions (this OS has no mprotect yet; anonymous RW+X
  // mapping is implicitly executable)
  //    will enable once mprotect syscall is added
  (void)dl_sys_mprotect;

  // 7. find PT_DYNAMIC and output .dynamic pointer
  if (out_dyn) {
    *out_dyn = NULL;
    for (size_t i = 0; i < phnum; i++) {
      if (dl_phdrs[i].p_type == PT_DYNAMIC) {
        *out_dyn =
            (Elf64_Dyn *)((char *)base + (dl_phdrs[i].p_vaddr - min_vaddr));
        break;
      }
    }
  }

  dl_sys_close((int)fd);
  return base;
}
