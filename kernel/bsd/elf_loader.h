#ifndef KERNEL_ELF_LOADER_H
#define KERNEL_ELF_LOADER_H

#include <stdbool.h>
#include "common/elf.h"

typedef struct elf_load_result {
    uint64_t entry;
    bool     success;
    // TLS 模板信息（PT_TLS）；无 PT_TLS 段时各字段为 0
    uint64_t tls_tdata_size;   // .tdata 初始镜像大小
    uint64_t tls_tbss_size;    // .tbss 清零区大小
    uint64_t tls_align;        // 对齐
    uint64_t tls_template_off; // ELF 文件内 .tdata 偏移
} elf_load_result_t;

// Load ELF64 static binary into user address space
// new_pml4: caller-allocated PML4 (kernel entries already copied)
// Returns entry point and success status
elf_load_result_t elf_load(const uint8_t *data, uint64_t size,
                         uint64_t *new_pml4);

#endif // KERNEL_ELF_LOADER_H
