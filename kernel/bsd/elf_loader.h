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
  // TLS 模板信息（PT_TLS）；无 PT_TLS 段时各字段为 0
  uint64_t tls_tdata_size;   // .tdata 初始镜像大小
  uint64_t tls_tbss_size;    // .tbss 清零区大小
  uint64_t tls_align;        // 对齐
  uint64_t tls_template_off; // ELF 文件内 .tdata 偏移
  // PHDR 信息（auxv 传递给 ld.so）
  uint64_t phdr_vaddr; // PHDR 表用户态地址（AT_PHDR）
  uint64_t phnum;      // PHDR 条目数（AT_PHNUM）
  uint64_t phent;      // PHDR 条目大小（AT_PHENT）
  // 加载基址（elf_load_at 使用，elf_load 为 0）
  uint64_t load_base; // 实际加载基址
} elf_load_result_t;

// Load ELF64 static binary into user address space
// new_pml4: caller-allocated PML4 (kernel entries already copied)
// Returns entry point and success status
elf_load_result_t elf_load(const uint8_t *data, uint64_t size,
                           uint64_t *new_pml4);

// base-offset 加载（ld.so 等 -shared -fPIC 镜像用）
// PT_LOAD 段映射到 base + p_vaddr
elf_load_result_t elf_load_at(const uint8_t *data, uint64_t size,
                              uint64_t *new_pml4, uint64_t base);

#endif // KERNEL_ELF_LOADER_H
