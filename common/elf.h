#ifndef COMMON_ELF_H
#define COMMON_ELF_H

#include <stdint.h>

// ELF64 fixed header
#define EI_NIDENT 16
#define PT_LOAD 1
#define PT_DYNAMIC 2   // Dynamic linking information
#define PT_INTERP 3    // Program interpreter path name
#define PT_TLS 7

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct Elf64_Ehdr {
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
} Elf64_Ehdr;

typedef struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

// 重定位类型
#define R_X86_64_RELATIVE  8

// .dynamic 标签
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8

// ELF64 重定位宏
#define ELF64_R_TYPE(info)  ((info) & 0xffffffff)
#define ELF64_R_SYM(info)   ((info) >> 32)

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

typedef union {
    uint64_t d_val;
    uint64_t d_ptr;
} Elf64_Dyn_v;

typedef struct {
    int64_t  d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} Elf64_Dyn;

// Auxiliary vector types (AT_*)
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_RANDOM   25
#define AT_EXECFN   31

#endif // COMMON_ELF_H
