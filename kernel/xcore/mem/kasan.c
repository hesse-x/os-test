// KASAN runtime — kernel-address sanitizer for x86-64
// Shadow formula: shadow_addr = (addr >> 3) + KASAN_SHADOW_OFFSET
// Shadow byte: 0x00 = fully accessible, 1-7 = partial, >= 0xF9 = error

#ifdef SANITIZER

#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/slab.h"
#include "arch/x64/paging.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/log.h"
#include "arch/x64/memlayout.h"

// ===================== State =====================
static bool kasan_ready = false;

// ===================== kasan_global — GCC-generated metadata =====================
struct kasan_global {
    const void *beg;        // address of global variable
    size_t size;            // size of global variable
    size_t size_with_redzone; // size + redzone
    const void *name;
    const void *module_name;
    unsigned long has_dynamic_init;
};

extern const struct kasan_global __start_kasan_globals[];
extern const struct kasan_global __stop_kasan_globals[];

// ===================== Forward declarations =====================
static void kasan_report(const void *addr, size_t size, bool is_write);

// ===================== Shadow page table mapping =====================
// Map shadow memory into kernel PML4 (PML4 index 503 for our shadow range).
// This operates directly on kernel page tables — no user PML4 involved.

static uint64_t *kasan_ensure_pdpt(uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    if (pml4[pml4_idx] & PTE_PRESENT) {
        return (__force uint64_t *)phys_to_virt(
            (__force phys_addr_t)(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL));
    }
    Page *pg = bfc_alloc_page(1);
    if (!pg) return NULL;
    uint64_t phys = (__force uint64_t)page_to_phys(pg);
    uint64_t *pdpt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)phys);
    for (int i = 0; i < 512; i++) pdpt[i] = 0;
    pml4[pml4_idx] = phys | PTE_PRESENT | PTE_RW;
    return pdpt;
}

static uint64_t *kasan_ensure_pd(uint64_t *pdpt, uint64_t vaddr) {
    uint64_t idx = (vaddr >> 30) & 0x1FF;
    if (pdpt[idx] & PTE_PRESENT) {
        return (__force uint64_t *)phys_to_virt(
            (__force phys_addr_t)(pdpt[idx] & 0x000FFFFFFFFFF000ULL));
    }
    Page *pg = bfc_alloc_page(1);
    if (!pg) return NULL;
    uint64_t phys = (__force uint64_t)page_to_phys(pg);
    uint64_t *pd = (__force uint64_t *)phys_to_virt((__force phys_addr_t)phys);
    for (int i = 0; i < 512; i++) pd[i] = 0;
    pdpt[idx] = phys | PTE_PRESENT | PTE_RW;
    return pd;
}

static uint64_t *kasan_ensure_pt(uint64_t *pd, uint64_t vaddr) {
    uint64_t idx = (vaddr >> 21) & 0x1FF;
    if (pd[idx] & PTE_PRESENT) {
        return (__force uint64_t *)phys_to_virt(
            (__force phys_addr_t)(pd[idx] & 0x000FFFFFFFFFF000ULL));
    }
    Page *pg = bfc_alloc_page(1);
    if (!pg) return NULL;
    uint64_t phys = (__force uint64_t)page_to_phys(pg);
    uint64_t *pt = (__force uint64_t *)phys_to_virt((__force phys_addr_t)phys);
    for (int i = 0; i < 512; i++) pt[i] = 0;
    pd[idx] = phys | PTE_PRESENT | PTE_RW;
    return pt;
}

// Map one 4KB page at vaddr → phys with flags
static bool kasan_map_page(uint64_t vaddr, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = kasan_ensure_pdpt(vaddr);
    if (!pdpt) return false;
    uint64_t *pd = kasan_ensure_pd(pdpt, vaddr);
    if (!pd) return false;
    uint64_t *pt = kasan_ensure_pt(pd, vaddr);
    if (!pt) return false;
    uint64_t idx = (vaddr >> 12) & 0x1FF;
    pt[idx] = phys | flags;
    return true;
}

// ===================== kasan_init =====================
__attribute__((no_sanitize("kernel-address")))
void kasan_init(void) {
    printk(LOG_INFO, "kasan_init: mapping shadow memory...\n");

    // 1. Allocate physical pages and map them to the shadow range
    uint64_t shadow_start = (uint64_t)KASAN_SHADOW_START;
    uint64_t shadow_size  = (uint64_t)KASAN_SHADOW_SIZE;
    // Round up to page granularity
    size_t num_pages = (shadow_size + PAGE_SIZE - 1) / PAGE_SIZE;

    printk(LOG_INFO, "  shadow_start=0x%lx size=%luMB pages=%lu\n",
                  shadow_start, shadow_size / (1024 * 1024), num_pages);

    for (size_t i = 0; i < num_pages; i++) {
        Page *pg = bfc_alloc_page(1);
        if (!pg) {
            printk(LOG_ERROR, "kasan_init: OOM at shadow page %lu\n", i);
            halt();
        }
        uint64_t phys = (__force uint64_t)page_to_phys(pg);
        uint64_t vaddr = shadow_start + i * PAGE_SIZE;

        if (!kasan_map_page(vaddr, phys, PTE_PRESENT | PTE_RW)) {
            printk(LOG_ERROR, "kasan_init: map failed at 0x%lx\n", vaddr);
            halt();
        }
    }

    flush_tlb();

    // 2. Poison all shadow as inaccessible (0xFF)
    __memset((void *)shadow_start, 0xFF, num_pages * PAGE_SIZE);

    // 3. Unpoison kernel image: [0xFFFFFFFF80100000, kernel_end)
    kasan_unpoison_shadow((const void *)0xFFFFFFFF80100000ULL,
                          (size_t)((uintptr_t)kernel_end - 0xFFFFFFFF80100000ULL));

    // 4. Poison global variable redzones
    kasan_poison_globals();

    kasan_ready = true;

    printk(LOG_INFO, "kasan_init: done, shadow 0x%lx-0x%lx\n",
                  shadow_start, shadow_start + shadow_size);
}

// ===================== Shadow manipulation =====================
__attribute__((no_sanitize("kernel-address")))
void kasan_unpoison_shadow(const void *addr, size_t size) {
    if (size == 0) return;
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    size_t aligned = size >> KASAN_SHADOW_SCALE;
    size_t remainder = size & 7;

    // Full 8-byte blocks
    for (size_t i = 0; i < aligned; i++)
        shadow[i] = KASAN_SHADOW_VALID;

    // Partial last block
    if (remainder)
        shadow[aligned] = (uint8_t)remainder;
}

__attribute__((no_sanitize("kernel-address")))
void kasan_poison_shadow(const void *addr, size_t size, uint8_t value) {
    if (size == 0) return;
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    size_t count = (size + 7) >> KASAN_SHADOW_SCALE;
    for (size_t i = 0; i < count; i++)
        shadow[i] = value;
}

// ===================== Access checking =====================
__attribute__((no_sanitize("kernel-address")))
void kasan_check_read(const void *addr, size_t size) {
    if (!kasan_ready) return;
    // Only check kernel higher-half addresses (shadow is mapped for these)
    uint64_t a = (uint64_t)addr;
    if (a < 0xFFFFFFFF80000000ULL) return;
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    size_t i;

    for (i = 0; i < size; i++) {
        uint8_t s = shadow[i >> KASAN_SHADOW_SCALE];
        if (s == 0) continue;
        if (s >= 0xF9) { kasan_report(addr, size, false); return; }
        // s is 1-7: check if the access falls in the partial byte
        size_t offset_in_block = i & 7;
        if (offset_in_block >= s) { kasan_report(addr, size, false); return; }
    }
}

__attribute__((no_sanitize("kernel-address")))
void kasan_check_write(const void *addr, size_t size) {
    if (!kasan_ready) return;
    uint64_t a = (uint64_t)addr;
    if (a < 0xFFFFFFFF80000000ULL) return;
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    size_t i;

    for (i = 0; i < size; i++) {
        uint8_t s = shadow[i >> KASAN_SHADOW_SCALE];
        if (s == 0) continue;
        if (s >= 0xF9) { kasan_report(addr, size, true); return; }
        size_t offset_in_block = i & 7;
        if (offset_in_block >= s) { kasan_report(addr, size, true); return; }
    }
}

// ===================== Report =====================
__attribute__((no_sanitize("kernel-address")))
static void kasan_report(const void *addr, size_t size, bool is_write) {
    printk(LOG_ERROR, "\n=== KASAN ERROR ===\n");
    if (is_write)
        printk(LOG_ERROR, "  out-of-bounds WRITE");
    else
        printk(LOG_ERROR, "  out-of-bounds READ");

    uint8_t *shadow = KASAN_MEM_TO_SHADOW(addr);
    uint8_t sv = *shadow;
    if (sv == KASAN_SHADOW_FREED)
        printk(LOG_ERROR, " (use-after-free)");
    else if (sv == KASAN_SHADOW_REDZONE)
        printk(LOG_ERROR, " (global-redzone)");

    printk(LOG_ERROR, "\n  addr=0x%016X size=%lu shadow=0x%02X\n",
                  (uint64_t)addr, (unsigned long)size, sv);

    // Stack trace via RBP chain
    printk(LOG_ERROR, "  backtrace:\n");
    uint64_t *rbp;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
    for (int depth = 0; depth < 16 && (uint64_t)rbp > 0xFFFFFFFF80000000ULL; depth++) {
        uint64_t ret_addr = rbp[1];
        printk(LOG_ERROR, "    0x%016X\n", ret_addr);
        rbp = (uint64_t *)rbp[0];
        if (!rbp) break;
    }

    printk(LOG_ERROR, "===================\n");
    halt();
}

// ===================== kasan_shadow_exists =====================
__attribute__((no_sanitize("kernel-address")))
bool kasan_shadow_exists(void) {
    return kasan_ready;
}

// ===================== copy_from_user / copy_to_user =====================
// User addresses are not in the shadow range; we bypass KASAN entirely.
__attribute__((no_sanitize("kernel-address")))
size_t copy_from_user(void *dst, const void __user *src, size_t size) {
    __memcpy(dst, (const void __force *)src, size);
    return 0;
}

__attribute__((no_sanitize("kernel-address")))
size_t copy_to_user(void __user *dst, const void *src, size_t size) {
    __memcpy((void __force *)dst, src, size);
    return 0;
}

// strncpy_from_user — copy a string from user space byte-by-byte.
// Stops at '\0' or maxlen, never crosses page boundaries for short strings
// near page edges (unlike copy_from_user which reads a fixed block).
// Returns: length of string (excluding '\0'), or -EFAULT if src is NULL.
__attribute__((no_sanitize("kernel-address")))
long strncpy_from_user(char *dst, const char __user *src, long maxlen) {
    if (!src || maxlen <= 0) return -EFAULT;
    long len = 0;
    while (len < maxlen) {
        char c = *(volatile const char __force *)(src + len);
        dst[len] = c;
        if (c == '\0') return len;
        len++;
    }
    dst[maxlen - 1] = '\0';
    return len;
}

// ===================== Global variable poisoning =====================
__attribute__((no_sanitize("kernel-address")))
void kasan_poison_globals(void) {
    const struct kasan_global *g = __start_kasan_globals;
    const struct kasan_global *end = __stop_kasan_globals;

    for (; g < end; g++) {
        // Unpoison the actual global variable
        kasan_unpoison_shadow(g->beg, g->size);
        // Poison the redzone after it
        if (g->size_with_redzone > g->size) {
            kasan_poison_shadow((const void *)((uintptr_t)g->beg + g->size),
                                g->size_with_redzone - g->size,
                                KASAN_SHADOW_REDZONE);
        }
    }
}

// ===================== Slab/BFC hooks =====================
__attribute__((no_sanitize("kernel-address")))
void kasan_slab_alloc(const void *object, size_t size) {
    kasan_unpoison_shadow(object, size);
}

__attribute__((no_sanitize("kernel-address")))
void kasan_slab_free(const void *object, size_t size) {
    // Double-free detection
    uint8_t *shadow = KASAN_MEM_TO_SHADOW(object);
    if (*shadow == KASAN_SHADOW_FREED) {
        kasan_report(object, size, false);
    }
    kasan_poison_shadow(object, size, KASAN_SHADOW_FREED);
}

__attribute__((no_sanitize("kernel-address")))
void kasan_bfc_alloc(const void *addr, size_t size) {
    if (!kasan_ready) return;
    kasan_unpoison_shadow(addr, size);
}

__attribute__((no_sanitize("kernel-address")))
void kasan_bfc_free(const void *addr, size_t size) {
    kasan_poison_shadow(addr, size, KASAN_SHADOW_FREED);
}

// ===================== GCC-required __asan_* interface =====================
// The compiler injects calls to these when -fsanitize=kernel-address is active.

#define DEFINE_ASAN_LOADSTORE(n) \
    __attribute__((no_sanitize("kernel-address"))) \
    void __asan_load##n(unsigned long addr) { kasan_check_read((const void *)addr, n); } \
    __attribute__((no_sanitize("kernel-address"))) \
    void __asan_store##n(unsigned long addr) { kasan_check_write((const void *)addr, n); } \
    __attribute__((no_sanitize("kernel-address"))) \
    void __asan_load##n##_noabort(unsigned long addr) { kasan_check_read((const void *)addr, n); } \
    __attribute__((no_sanitize("kernel-address"))) \
    void __asan_store##n##_noabort(unsigned long addr) { kasan_check_write((const void *)addr, n); }

DEFINE_ASAN_LOADSTORE(1)
DEFINE_ASAN_LOADSTORE(2)
DEFINE_ASAN_LOADSTORE(4)
DEFINE_ASAN_LOADSTORE(8)
DEFINE_ASAN_LOADSTORE(16)

__attribute__((no_sanitize("kernel-address")))
void __asan_loadN(unsigned long addr, size_t size) {
    kasan_check_read((const void *)addr, size);
}

__attribute__((no_sanitize("kernel-address")))
void __asan_storeN(unsigned long addr, size_t size) {
    kasan_check_write((const void *)addr, size);
}

__attribute__((no_sanitize("kernel-address")))
void __asan_loadN_noabort(unsigned long addr, size_t size) {
    kasan_check_read((const void *)addr, size);
}

__attribute__((no_sanitize("kernel-address")))
void __asan_storeN_noabort(unsigned long addr, size_t size) {
    kasan_check_write((const void *)addr, size);
}

__attribute__((no_sanitize("kernel-address")))
void __asan_handle_no_return(void) {
    // No shadow stack cleanup needed for our minimal implementation
}

__attribute__((no_sanitize("kernel-address")))
void __asan_before_dynamic_init(const char *module) {
    (void)module;
}

__attribute__((no_sanitize("kernel-address")))
void __asan_after_dynamic_init(void) {
}

__attribute__((no_sanitize("kernel-address")))
void __asan_register_globals(const struct kasan_global *globals, size_t n) {
    for (size_t i = 0; i < n; i++) {
        kasan_unpoison_shadow(globals[i].beg, globals[i].size);
        if (globals[i].size_with_redzone > globals[i].size) {
            kasan_poison_shadow(
                (const void *)((uintptr_t)globals[i].beg + globals[i].size),
                globals[i].size_with_redzone - globals[i].size,
                KASAN_SHADOW_REDZONE);
        }
    }
}

__attribute__((no_sanitize("kernel-address")))
void __asan_unregister_globals(const struct kasan_global *globals, size_t n) {
    (void)globals;
    (void)n;
}

// ===================== __asan_memcpy / __asan_memset / __asan_memmove =====================
// Compiler replaces all memcpy/memset/memmove calls with these.

__attribute__((no_sanitize("kernel-address")))
void *__asan_memcpy(void *dst, const void *src, size_t n) {
    kasan_check_read(src, n);
    kasan_check_write(dst, n);
    return __memcpy(dst, src, n);
}

__attribute__((no_sanitize("kernel-address")))
void *__asan_memset(void *dst, int val, size_t n) {
    kasan_check_write(dst, n);
    return __memset(dst, val, n);
}

__attribute__((no_sanitize("kernel-address")))
void *__asan_memmove(void *dst, const void *src, size_t n) {
    kasan_check_read(src, n);
    kasan_check_write(dst, n);
    return __memmove(dst, src, n);
}

#endif /* SANITIZER */
