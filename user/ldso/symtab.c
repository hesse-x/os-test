// ld.so 符号查找：GNU hash + 链表遍历
// ld.md §3.3.5 / plan_ld2b3 T17

#include <stddef.h>
#include <stdint.h>
#include "elf.h"
#include "common/syscall_nums.h"
#include "sys/link_map.h"

// minilibc.c
__attribute__((visibility("hidden"))) int strcmp(const char *a, const char *b);

// GNU hash 函数
static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = (h << 5) + h + (uint8_t)(*s++);
    return h;
}

// GNU hash 查找：返回符号 st_value（相对基址），未找到返回 NULL
// gnu_hash_table 指向 .gnu.hash 段起始（uint32_t* 视角）
static void *gnu_hash_lookup(const char *name,
                              Elf64_Sym *symtab,
                              const char *strtab,
                              uint32_t *gnu_hash_table) {
    if (!gnu_hash_table) return NULL;

    uint32_t nbuckets    = gnu_hash_table[0];
    uint32_t symoffset   = gnu_hash_table[1];
    uint32_t bloom_size  = gnu_hash_table[2];
    uint32_t bloom_shift = gnu_hash_table[3];

    if (nbuckets == 0 || bloom_size == 0) return NULL;

    uint32_t h = gnu_hash(name);

    // bloom filter 快速排除
    uint64_t *bloom = (uint64_t *)&gnu_hash_table[4];
    uint64_t word = bloom[(h / 64) % bloom_size];
    uint64_t mask = (1ULL << (h % 64))
                  | (1ULL << ((h >> bloom_shift) % 64));
    if ((word & mask) != mask) return NULL;

    // bucket 找链头
    uint32_t *buckets = (uint32_t *)((char *)bloom + bloom_size * 8);
    uint32_t symidx = buckets[h % nbuckets];
    if (symidx == 0) return NULL;  // 空桶

    // chain 遍历
    uint32_t *chain = buckets + nbuckets;
    uint32_t chain_h;
    do {
        chain_h = chain[symidx - symoffset];
        if ((h | 1) == (chain_h | 1)) {  // 忽略最低位（结束标记）
            Elf64_Sym *sym = &symtab[symidx];
            if (strcmp(strtab + sym->st_name, name) == 0) {
                return (void *)(uintptr_t)sym->st_value;  // 调用方加 base
            }
        }
        symidx++;
    } while ((chain_h & 1) == 0);  // 最低位 1 表示链结束

    return NULL;
}

// 遍历 link_map 查找符号（链表顺序：主 ELF → libc.so → ld.so）
// 返回绝对地址（已加 base），未找到返回 NULL
void *lookup_symbol_in_link_map(const char *name, struct link_map *lmap) {
    for (struct link_map *l = lmap; l; l = l->l_next) {
        void *sym = gnu_hash_lookup(name, (Elf64_Sym *)l->symtab, l->strtab,
                                    (uint32_t *)l->gnu_hash);
        if (sym) return (char *)l->base + (uintptr_t)sym;
    }
    return NULL;
}

// _dl_link_map 声明（link_map.c 定义）
extern struct link_map *_dl_link_map;
