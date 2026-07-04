// ld.so 主流程（post-bootstrap）
// ld.md §3.3 / plan_ld2b3 T14

#include <stddef.h>
#include <stdint.h>
#include "elf.h"
#include "xos/syscall_nums.h"
#include "sys/link_map.h"

// bootstrap 阶段辅助（hidden：跨文件可见但不走 PLT）
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// 外部函数声明（load_so.c / relocate.c / symtab.c / link_map.c）
__attribute__((visibility("hidden"))) void *load_so(const char *path, Elf64_Dyn **out_dyn);
__attribute__((visibility("hidden"))) void apply_relocation(Elf64_Rela *r, void *base, struct link_map *lmap);
__attribute__((visibility("hidden"))) void *lookup_symbol_in_link_map(const char *name, struct link_map *lmap);
__attribute__((visibility("hidden"))) void eager_bind(struct link_map *l);
__attribute__((visibility("hidden"))) void build_link_map(uintptr_t main_base, Elf64_Dyn *main_dyn,
                           uintptr_t libc_base, Elf64_Dyn *libc_dyn,
                           uintptr_t ld_base, Elf64_Dyn *ld_dyn);

// 全局 link_map 链表（link_map.c 定义）
extern struct link_map *_dl_link_map;

// 链接器提供的 .dynamic 起始符号（ld.so 自身）
extern Elf64_Dyn _DYNAMIC[];

static uintptr_t find_auxv_val(uintptr_t *sp, uint64_t type) {
    int argc = (int)sp[0];
    uintptr_t *argv = sp + 1;
    uintptr_t *envp = argv + argc + 1;
    uintptr_t *p = envp;
    while (*p) p++;
    p++;
    while (*p != AT_NULL) {
        if (p[0] == type) return p[1];
        p += 2;
    }
    return 0;
}

// 重定位单个对象：遍历 .rela.dyn 应用所有重定位 + eager bind .rela.plt
static void relocate_object(struct link_map *l) {
    Elf64_Rela *rela = (Elf64_Rela *)l->rela_dyn;
    size_t rela_sz = l->rela_dyn_sz;
    if (rela && rela_sz) {
        for (size_t i = 0; i < rela_sz / sizeof(Elf64_Rela); i++) {
            apply_relocation(&rela[i], (void *)l->base, l);
        }
    }
    eager_bind(l);
}

void __dls_init(uintptr_t *sp, uintptr_t ld_base) {
    // 1. 从 auxv 取主 ELF 信息
    uintptr_t phdr = find_auxv_val(sp, AT_PHDR);
    size_t phent = find_auxv_val(sp, AT_PHENT);
    size_t phnum = find_auxv_val(sp, AT_PHNUM);
    uintptr_t entry = find_auxv_val(sp, AT_ENTRY);

    // 2. 遍历 PHDR 找 PT_DYNAMIC
    Elf64_Dyn *main_dyn = NULL;
    for (size_t i = 0; i < phnum; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
        if (p->p_type == PT_DYNAMIC) {
            main_dyn = (Elf64_Dyn *)p->p_vaddr;
            break;
        }
    }
    if (!main_dyn) {
        dl_puts("dl: FATAL: main ELF has no PT_DYNAMIC");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 3. 找 DT_NEEDED（libc.so）— 先找 DT_STRTAB
    const char *main_dynstr = NULL;
    for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB) { main_dynstr = (const char *)d->d_un.d_ptr; break; }
    }
    const char *needed_path = NULL;
    if (main_dynstr) {
        for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED) {
                needed_path = main_dynstr + d->d_un.d_val;
                break;
            }
        }
    }

    // 4. 加载 libc.so
    void *libc_base = NULL;
    Elf64_Dyn *libc_dyn = NULL;
    if (needed_path) {
        // DT_NEEDED 是 soname（如 libc.so），拼成 /lib/<soname>
        // 简单拼接（needed_path 长度有限，/lib/ 前缀 + soname < 256）
        static char lib_path[256];
        const char *prefix = "/lib/";
        char *dp = lib_path;
        while (*prefix) *dp++ = *prefix++;
        const char *np = needed_path;
        while (*np) *dp++ = *np++;
        *dp = '\0';

        dl_puts("dl: loading ");
        dl_puts(lib_path);
        libc_base = load_so(lib_path, &libc_dyn);
        dl_puts("dl: loaded libc.so @ ");
        dl_put_hex((uint64_t)libc_base);
    } else {
        dl_puts("dl: FATAL: no DT_NEEDED libc.so");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 5. 构造 link_map（主 ELF → libc.so → ld.so）
    //    主 ELF 非 PIE，base=0（p_vaddr 是绝对地址）
    build_link_map(0, main_dyn, (uintptr_t)libc_base, libc_dyn,
                   ld_base, _DYNAMIC);

    // 5.1 填主 ELF 的 PT_TLS 信息（libc.so/ld.so 通常无 TLS，留 0）
    //     libc.so 的 TLS 需从其 PHDR 填，本阶段 libc.so 无 thread_local，暂略
    if (_dl_link_map) {
        fill_tls_from_phdr(_dl_link_map, 0, phdr, phent, phnum);
    }

    // 6. 重定位：先 libc.so，再主 ELF（决策 5）
    //    ld.so 自身已在 bootstrap 完成 RELATIVE 重定位，无需再动
    //    链表顺序：main_map → libc_map → ld_map
    //    先重定位 libc_map（符号查找能查到 ld.so 已就绪的全局）
    struct link_map *main_map = _dl_link_map;
    struct link_map *libc_map = main_map ? main_map->l_next : NULL;
    if (libc_map) {
        dl_puts("dl: relocating libc.so");
        relocate_object(libc_map);
        dl_puts("dl: libc.so relocated");
    }

    // 再重定位主 ELF（其 JUMP_SLOT 此时能解析 libc.so 符号）
    if (main_map) {
        dl_puts("dl: relocating main ELF");
        relocate_object(main_map);
        dl_puts("dl: main ELF relocated");
    }

    // 7. 跳主 ELF entry
    dl_puts("dl: jump to entry");
    dl_put_hex(entry);

    // 栈仍指向 argc（ld.so 没动栈）
    __asm__ volatile("mov %0, %%rsp\n"
                     "jmp *%1\n"
                     :
                     : "r"(sp), "r"(entry));
    // 不返回
    while (1) ;
}
