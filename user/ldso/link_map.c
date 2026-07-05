// ld.so link_map 构造：3 个 map（主 ELF → libc.so → ld.so）
// ld.md §3.3.7 / plan_ld2b3 T18

#include <stddef.h>
#include <stdint.h>
#include <xos/elf.h>
#include <sys/link_map.h>

// 全局 link_map 链表头（libc.so 的 collect_tls_from_link_map 通过 extern 读取）
// visibility("default")：ld.so 全局 -fvisibility=hidden，仅此符号需导出给 libc.so
__attribute__((visibility("default")))
struct link_map *_dl_link_map = NULL;

// 静态分配 3 个 link_map（ld.so 早期不用 malloc）
static struct link_map main_map;
static struct link_map libc_map;
static struct link_map ld_map;

// 解析 .dynamic 填 link_map 的符号查找 + TLS 字段
static void fill_link_map(struct link_map *l, uintptr_t base, Elf64_Dyn *dyn) {
    l->base = base;
    l->dynamic = dyn;

    // 默认清零
    l->symtab = NULL;
    l->strtab = NULL;
    l->gnu_hash = NULL;
    l->rela_dyn = NULL;
    l->rela_dyn_sz = 0;
    l->rela_plt = NULL;
    l->rela_plt_sz = 0;
    l->tls_template = NULL;
    l->tls_tdata_size = 0;
    l->tls_tbss_size = 0;
    l->tls_align = 0;

    if (!dyn) return;

    // 第一遍：找 DT_STRTAB/DT_SYMTAB/DT_GNU_HASH（地址需加 base，.so 是 PIC）
    // 注：主 ELF 非 PIE，d_ptr 已是绝对地址；libc.so/ld.so 是 PIC，d_ptr 是相对 vaddr 需加 base
    // 简化：对所有对象都尝试 base + d_ptr（主 ELF base=0 退化为绝对地址）
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:
            l->strtab = (const char *)(base + d->d_un.d_ptr);
            break;
        case DT_SYMTAB:
            l->symtab = (void *)(base + d->d_un.d_ptr);
            break;
        case DT_GNU_HASH:
            l->gnu_hash = (void *)(base + d->d_un.d_ptr);
            break;
        }
    }

    // 第二遍：找 DT_RELA/DT_RELASZ/DT_JMPREL/DT_PLTRELSZ + PT_TLS
    // PT_TLS 信息不通过 .dynamic 传递，需遍历 PHDR — 但 ld.so 此时已无主 ELF PHDR
    // 简化：TLS 信息由 fill 时单独遍历 PHDR 填充（caller 传 phdr）
    // 这里只填 .dynamic 能提供的字段
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:
            l->rela_dyn = (void *)(base + d->d_un.d_ptr);
            break;
        case DT_RELASZ:
            l->rela_dyn_sz = d->d_un.d_val;
            break;
        case DT_JMPREL:
            l->rela_plt = (void *)(base + d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            l->rela_plt_sz = d->d_un.d_val;
            break;
        }
    }
}

// 从 PHDR 找 PT_TLS，填 link_map 的 tls_* 字段
// base 是加载基址（PIC .so 加偏移；主 ELF base=0）
__attribute__((visibility("hidden")))
void fill_tls_from_phdr(struct link_map *l, uintptr_t base, uintptr_t phdr,
                        size_t phent, size_t phnum) {
    for (size_t i = 0; i < phnum; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
        if (p->p_type == PT_TLS) {
            l->tls_template = (void *)(base + p->p_vaddr);
            l->tls_tdata_size = p->p_filesz;
            l->tls_tbss_size = p->p_memsz - p->p_filesz;
            l->tls_align = p->p_align;
            return;
        }
    }
}
// 构造 link_map 链表：主 ELF → libc.so → ld.so
void build_link_map(uintptr_t main_base, Elf64_Dyn *main_dyn,
                    uintptr_t libc_base, Elf64_Dyn *libc_dyn,
                    uintptr_t ld_base, Elf64_Dyn *ld_dyn) {
    fill_link_map(&main_map, main_base, main_dyn);
    fill_link_map(&libc_map, libc_base, libc_dyn);
    fill_link_map(&ld_map,   ld_base,   ld_dyn);

    // 链表连接：main → libc → ld
    main_map.l_prev = NULL;
    main_map.l_next = &libc_map;
    libc_map.l_prev = &main_map;
    libc_map.l_next = &ld_map;
    ld_map.l_prev = &libc_map;
    ld_map.l_next = NULL;

    _dl_link_map = &main_map;
}
