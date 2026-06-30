#ifndef KERNEL_ELF_LOADER_H
#define KERNEL_ELF_LOADER_H

#include <stdbool.h>
#include "common/elf.h"

typedef struct elf_load_result {
    uint64_t entry;
    bool     success;
} elf_load_result_t;

// Load ELF64 static binary into user address space
// new_pml4: caller-allocated PML4 (kernel entries already copied)
// Returns entry point and success status
elf_load_result_t elf_load(const uint8_t *data, uint64_t size,
                         uint64_t *new_pml4);

#endif // KERNEL_ELF_LOADER_H
