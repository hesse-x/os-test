// ld.so bootstrap 入口
// ld.md §3.2.3 / plan_ld2b3 T13

#include <stddef.h>
#include <stdint.h>
#include "elf.h"
#include "xos/syscall_nums.h"

// 链接器提供的 .dynamic 起始符号
// 用 asm 取 RIP-relative 地址，避免走 GOT（bootstrap 前 GOT 未填）
extern Elf64_Dyn _DYNAMIC[];
// bootstrap 阶段辅助函数声明（hidden：跨文件可见但不走 PLT，bootstrap 前 GOT 未填）
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// post-bootstrap 主流程（dls_init.c）
__attribute__((visibility("hidden"))) void __dls_init(uintptr_t *sp, uintptr_t ld_base);

// 取 _DYNAMIC 运行时地址（RIP-relative，不依赖 GOT）
static Elf64_Dyn *get_dynamic(void) {
    Elf64_Dyn *p;
    __asm__("leaq _DYNAMIC(%%rip), %0" : "=r"(p));
    return p;
}

// 全局指针（触发 R_X86_64_RELATIVE 重定位，验证自重定位后指向 ld_base + offset）
// 不被使用，仅用于让 .rela.dyn 非空，使 bootstrap 路径实际执行重定位
static void *self_ptr = (void *)dl_puts;

// 从 auxv 取指定类型值
static uintptr_t find_auxv(uintptr_t *sp, uint64_t type) {
    // sp 指向 argc
    int argc = (int)sp[0];
    uintptr_t *argv = sp + 1;
    uintptr_t *envp = argv + argc + 1;  // 跳过 argv + NULL
    // 遍历 envp 找 NULL
    uintptr_t *p = envp;
    while (*p) p++;
    p++;  // 跳过 envp 的 NULL 终止符
    // p 现在指向 auxv
    while (*p != AT_NULL) {
        if (p[0] == type) return p[1];
        p += 2;
    }
    return 0;
}

// bootstrap 入口
void __dls3(uintptr_t *sp) {
    // 1. 从 auxv 取 AT_BASE（自己的基址）
    uintptr_t ld_base = find_auxv(sp, AT_BASE);

    // 2. 定位 .dynamic（RIP-relative 直接取运行时地址，不走 GOT）
    Elf64_Dyn *dyn = get_dynamic();

    // 3. 先遍历 .dynamic raw 找 DT_RELA/DT_RELASZ/DT_SYMTAB
    //    d_ptr 是 link-time 相对 vaddr，用时 + ld_base
    Elf64_Rela *rela = NULL;
    size_t rela_sz = 0;
    Elf64_Sym *symtab = NULL;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:    rela = (Elf64_Rela *)(ld_base + d->d_un.d_ptr); break;
        case DT_RELASZ:  rela_sz = d->d_un.d_val; break;
        case DT_SYMTAB:  symtab = (Elf64_Sym *)(ld_base + d->d_un.d_ptr); break;
        }
    }

    // 4. 遍历 .rela.dyn 应用 R_X86_64_RELATIVE 与 R_X86_64_GLOB_DAT
    //    RELATIVE:   *addr = ld_base + addend
    //    GLOB_DAT:   *addr = ld_base + symtab[sym].st_value + addend
    //    （ld.so 自身定义的符号，st_value 是 link-time vaddr，加 base 得运行时地址）
    //    bootstrap 前 GOT 未填，必须在此处理 GLOB_DAT，否则 build_link_map 读
    //    _dl_link_map 的 GOT entry 得 0，解引用即 #PF
    if (rela && rela_sz) {
        for (size_t i = 0; i < rela_sz / sizeof(Elf64_Rela); i++) {
            Elf64_Rela *r = &rela[i];
            uintptr_t *addr = (uintptr_t *)(ld_base + r->r_offset);
            uint32_t type = ELF64_R_TYPE(r->r_info);
            if (type == R_X86_64_RELATIVE) {
                *addr = ld_base + r->r_addend;
            } else if (type == R_X86_64_GLOB_DAT) {
                uint32_t sym_idx = ELF64_R_SYM(r->r_info);
                if (symtab && sym_idx) {
                    *addr = ld_base + symtab[sym_idx].st_value + r->r_addend;
                }
            } else {
                // 未处理类型打 WARN：bootstrap 只做 RELATIVE/GLOB_DAT，
                // 其他类型（如 JUMP_SLOT/PC32）意味着 ld.so 内部有跨模块调用，
                // bootstrap 前 GOT 未填会崩溃。正常情况下 ld.so 全局
                // -fvisibility=hidden 后 .rela.dyn 只剩 RELATIVE/GLOB_DAT
                dl_puts("dl: WARN: skip reloc type ");
                dl_put_hex(type);
                dl_puts(" at offset ");
                dl_put_hex(r->r_offset);
                dl_puts("\n");
            }
        }
    }

    // 5. 自重定位完成
    dl_puts("dl: self-relocate done");

    // 6. 进入 post-bootstrap 主流程（可用全局变量/GOT）
    __dls_init(sp, ld_base);
    // __dls_init 不返回（跳主 ELF entry）
    while (1) ;
}

