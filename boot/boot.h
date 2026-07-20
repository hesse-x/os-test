/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_BOOT_H
#define COMMON_BOOT_H

#include <stdint.h>

// VMA constants (shared between boot stub, kernel paging, and assembly)
#define VMA_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_VMA_BASE 0xFFFFFFFF80100000ULL
#define KERNEL_LOAD_ADDR                                                       \
  0x100000ULL // unified name (was KERNEL_LMA_BASE in paging.h, KERNEL_LOAD_ADDR
              // in stub.c)
#define BOOT_INFO_MAGIC 0x4F53424F544F4F42ULL // "BOOTBOOS"

// Boot info structure (passed from EFI stub to kernel)
typedef struct boot_info {
  uint64_t magic;
  uint64_t kernel_phys;
  uint64_t rsdp;
  uint64_t mmap_addr;
  uint64_t mmap_size;
  uint64_t mmap_desc_size;
  uint64_t mmap_desc_ver;

  // init.elf loaded by stub into physical memory (initrd-style):
  // kernel creates the init process directly from this buffer, avoiding
  // any early disk I/O. The stub reads init.elf from the ESP alongside
  // myos.elf.
  uint64_t init_elf_addr;
  uint64_t init_elf_size;
} boot_info;

// Byte length of boot_info, as consumed by the hand-written rep movsb in
// arch/x64/start.S (_entry64 copies exactly this many bytes from the UEFI
// boot_info buffer into g_boot_info). Kept as a literal macro so the
// assembler can use the matching `.set BOOT_INFO_SIZE` in start.S; the
// _Static_assert below pins it to the real struct size so any field
// addition that forgets to bump it (or the .set mirror) is a build break
// instead of a silent BSS overrun that clobbers the globals sitting right
// after g_boot_info (device_vma_base / early_bump_end / gdt).
#define BOOT_INFO_SIZE 72
_Static_assert(BOOT_INFO_SIZE == sizeof(boot_info),
               "BOOT_INFO_SIZE out of sync with sizeof(boot_info) — update "
               "arch/x64/start.S `BOOT_INFO_SIZE` .set to match");

#endif /* COMMON_BOOT_H */
