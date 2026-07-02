// ld.so 重定位：9 种类型 + eager binding + hard-fail
// ld.md §3.3.4, §3.3.6 / plan_ld2b3 T16

#include <stddef.h>
#include <stdint.h>
#include "elf.h"
#include "common/syscall_nums.h"
#include "sys/link_map.h"

// bootstrap 阶段辅助（hidden）
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// symtab.c
__attribute__((visibility("hidden"))) void *lookup_symbol_in_link_map(const char *name, struct link_map *lmap);

// 取符号名：symtab[sym_idx].st_name + strtab
static const char *sym_name(struct link_map *l, uint32_t sym_idx) {
    Elf64_Sym *symtab = (Elf64_Sym *)l->symtab;
    const char *strtab = l->strtab;
    return strtab + symtab[sym_idx].st_name;
}

// 应用单条重定位
void apply_relocation(Elf64_Rela *r, void *base, struct link_map *lmap) {
    uint32_t type = ELF64_R_TYPE(r->r_info);
    uint32_t sym_idx = ELF64_R_SYM(r->r_info);
    uintptr_t *addr = (uintptr_t *)((char *)base + r->r_offset);
    int64_t addend = r->r_addend;

    switch (type) {
    case R_X86_64_RELATIVE:
        *addr = (uintptr_t)base + addend;
        break;

    case R_X86_64_64: {
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        *addr = (uintptr_t)sym + addend;
        break;
    }

    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend - (uintptr_t)addr);
        break;
    }

    case R_X86_64_GOTPCREL: {
        // plan_ld2b3 决策 6：两步但**不改指令 disp**
        // 指令形式 mov foo@GOTPCREL(%rip), %reg
        // disp 字段在指令末尾前 4 字节，r_offset 指向 disp 字段位置
        // instr_end = r_offset + 4（即 disp 之后的地址 = 下条指令地址）
        // got_entry = instr_end + disp（RIP-relative 计算）
        // 填 *got_entry = sym，disp 不改（保持 RIP-relative 寻址正确）
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        // 从指令读静态 disp（disp 字段紧跟 r_offset 位置，4 字节）
        int32_t disp = *(int32_t *)addr;
        uintptr_t instr_end = (uintptr_t)addr + 4;
        uintptr_t got_entry = instr_end + disp;
        *(uintptr_t *)got_entry = (uintptr_t)sym;
        // disp 不改
        break;
    }

    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: {
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        *addr = (uintptr_t)sym;
        break;
    }

    case R_X86_64_32: {
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        *(uint32_t *)addr = (uint32_t)((uintptr_t)sym + addend);
        break;
    }

    case R_X86_64_32S: {
        const char *name = sym_name(lmap, sym_idx);
        void *sym = lookup_symbol_in_link_map(name, lmap);
        if (!sym) goto unresolved;
        *(int32_t *)addr = (int32_t)((uintptr_t)sym + addend);
        break;
    }

    default:
        dl_puts("dl: FATAL: unknown reloc type ");
        dl_put_hex(type);
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }
    return;

unresolved:
    dl_puts("dl: FATAL: unresolved symbol ");
    dl_puts(sym_name(lmap, sym_idx));
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
    while (1) ;
}

// eager binding：遍历 .rela.plt，立即解析每个 JUMP_SLOT
void eager_bind(struct link_map *l) {
    Elf64_Rela *plt_rela = (Elf64_Rela *)l->rela_plt;
    size_t plt_sz = l->rela_plt_sz;
    if (!plt_rela || plt_sz == 0) return;

    for (size_t i = 0; i < plt_sz / sizeof(Elf64_Rela); i++) {
        Elf64_Rela *r = &plt_rela[i];
        if (ELF64_R_TYPE(r->r_info) != R_X86_64_JUMP_SLOT) continue;

        uint32_t sym_idx = ELF64_R_SYM(r->r_info);
        const char *name = sym_name(l, sym_idx);
        // 从前驱链表查（主 ELF 优先，再 libc.so，再 ld.so）
        void *sym = lookup_symbol_in_link_map(name, _dl_link_map);
        if (!sym) {
            dl_puts("dl: FATAL: unresolved PLT symbol ");
            dl_puts(name);
            long ret;
            __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
            while (1) ;
        }

        uintptr_t *got_entry = (uintptr_t *)((char *)l->base + r->r_offset);
        *got_entry = (uintptr_t)sym;
    }
}

// _dl_link_map 声明（link_map.c 定义）
extern struct link_map *_dl_link_map;
