/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_ELF_LOADER_H
#define KERNEL_ELF_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <xos/elf.h>

typedef struct elf_load_result {
  uint64_t entry;
  bool success;
  // TLS template info (PT_TLS); each field is 0 when there is no PT_TLS segment
  uint64_t tls_tdata_size;   // .tdata initial image size
  uint64_t tls_tbss_size;    // .tbss zeroed region size
  uint64_t tls_align;        // alignment
  uint64_t tls_template_off; // .tdata offset within the ELF file
  // PHDR info (passed to ld.so via auxv)
  uint64_t phdr_vaddr; // PHDR table user-space address (AT_PHDR)
  uint64_t phnum;      // number of PHDR entries (AT_PHNUM)
  uint64_t phent;      // PHDR entry size (AT_PHENT)
  // Load base address (used by elf_load_at; 0 for elf_load)
  uint64_t load_base; // actual load base address
} elf_load_result;

// Load ELF64 static binary into user address space
// new_pml4: caller-allocated PML4 (kernel entries already copied)
// Returns entry point and success status
elf_load_result elf_load(const uint8_t *data, uint64_t size,
                           uint64_t *new_pml4);

// Base-offset load (for -shared -fPIC images such as ld.so)
// PT_LOAD segments are mapped to base + p_vaddr
elf_load_result elf_load_at(const uint8_t *data, uint64_t size,
                              uint64_t *new_pml4, uint64_t base);

#endif // KERNEL_ELF_LOADER_H
