#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>

// ELF64 fixed header
#define EI_NIDENT 16
#define PT_LOAD 1

struct Elf64_Ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf_load_result {
    uint64_t entry;
    bool     success;
};

// Load ELF64 static binary into user address space
// new_pml4: caller-allocated PML4 (kernel entries already copied)
// Returns entry point and success status
elf_load_result elf_load(const uint8_t *data, uint64_t size,
                         uint64_t *new_pml4);

#endif // KERNEL_ELF_H
