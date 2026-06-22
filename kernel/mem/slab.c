#include "kernel/mem/slab.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "kernel/serial.h"
#include "common/macro.h"

// 全局 kmalloc cache 数组
kmem_cache_t kmalloc_caches[NUM_KMALLOC_CLASSES];

// Size class 对应的对象大小
static const size_t class_sizes[NUM_KMALLOC_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

// ===================== Slab 页初始化 =====================
static void slab_page_init(Page *page, kmem_cache_t *cache, int cpu_id) {
    page->status = PAGE_SLAB;
    page->slab.cache = cache;
    page->slab.inuse = 0;
    page->slab.obj_count = PAGE_SIZE / cache->obj_size;
    page->slab.cpu_id = (int8_t)cpu_id;
    page->slab.freelist = NULL;
    page->slab.partial_next = NULL;
    page->slab.partial_prev = NULL;

    // 侵入式空闲链表：每个空闲对象首 8 字节存 next
    char *base = (__force char *)phys_to_virt((__force phys_addr_t)page_to_phys(page));
    for (uint32_t i = 0; i < page->slab.obj_count; i++) {
        void *obj = base + i * cache->obj_size;
        *(void **)obj = page->slab.freelist;
        page->slab.freelist = obj;
    }
}

// ===================== partial list 操作 =====================
static void partial_add(kmem_cache_t *cache, Page *page) {
    page->slab.partial_next = cache->partial;
    page->slab.partial_prev = NULL;
    if (cache->partial) {
        cache->partial->slab.partial_prev = page;
    }
    cache->partial = page;
}

static void partial_remove(kmem_cache_t *cache, Page *page) {
    if (page->slab.partial_prev) {
        page->slab.partial_prev->slab.partial_next = page->slab.partial_next;
    } else {
        cache->partial = page->slab.partial_next;
    }
    if (page->slab.partial_next) {
        page->slab.partial_next->slab.partial_prev = page->slab.partial_prev;
    }
    page->slab.partial_next = NULL;
    page->slab.partial_prev = NULL;
}

// ===================== slab_init =====================
void slab_init() {
    for (int i = 0; i < NUM_KMALLOC_CLASSES; i++) {
        kmalloc_caches[i].obj_size = class_sizes[i];
        kmalloc_caches[i].redzone_size = 0;
        kmalloc_caches[i].lock.locked = 0;
        kmalloc_caches[i].partial = NULL;
    }
    serial_printf("slab_init: ok\n");
}

// ===================== kmalloc =====================
void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // 大分配：走 BFC
    if (size > 2048) {
        size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        Page *page = bfc_alloc_page(npages);
        if (!page) return NULL;
        return (__force void *)phys_to_virt((__force phys_addr_t)page_to_phys(page));
    }

    int c = size_to_class(size);
    kmem_cache_t *cache = &kmalloc_caches[c];
    cpu_local_t *cpu = get_cpu_local();
    Page *active = cpu->active_slab[c];

    // Fast path：active slab 有空闲对象
    if (active && active->slab.freelist) {
        void *obj = active->slab.freelist;
        active->slab.freelist = *(void **)obj;
        active->slab.inuse++;
        return obj;
    }

    // Slow path：需要获取新 slab 页
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);

    // 如果 active slab 刚变满（freelist 空），移到 partial 或标记 full
    // 先检查 partial list
    if (cache->partial) {
        Page *page = cache->partial;
        partial_remove(cache, page);
        page->slab.cpu_id = (int8_t)cpu->cpu_id;
        cpu->active_slab[c] = page;

        void *obj = page->slab.freelist;
        page->slab.freelist = *(void **)obj;
        page->slab.inuse++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return obj;
    }

    // partial 也空：从 BFC 分配新页
    Page *new_page = bfc_alloc_page(1);
    if (!new_page) {
        spin_unlock_irqrestore(&cache->lock, flags);
        return NULL;
    }

    slab_page_init(new_page, cache, cpu->cpu_id);
    cpu->active_slab[c] = new_page;

    void *obj = new_page->slab.freelist;
    new_page->slab.freelist = *(void **)obj;
    new_page->slab.inuse++;
    spin_unlock_irqrestore(&cache->lock, flags);
    return obj;
}

// ===================== kfree =====================
void kfree(const void *ptr) {
    if (!ptr) return;

    uint64_t addr = (uint64_t)ptr;
    uint64_t phys = (__force uint64_t)PHY_ADDR(addr);
    Page *page = &bfc_frames[PHY_TO_PAGE(phys)];

    if (page->status == PAGE_USED) {
        // BFC 大分配释放
        bfc_free_page(page, page->bfc.cont_page_num);
        return;
    }

    if (page->status != PAGE_SLAB) {
        serial_puts("kfree: bad page status\n");
        return;
    }

    kmem_cache_t *cache = page->slab.cache;
    int my_cpu = get_cpu_local()->cpu_id;

    if (page->slab.cpu_id == my_cpu) {
        // 同 CPU：无锁释放
        *(void **)ptr = page->slab.freelist;
        page->slab.freelist = (void *)ptr;
        page->slab.inuse--;

        // 如果从 full 变为 partial，加入 partial list
        // full = inuse == obj_count - 1 (释放前)，释放后 inuse == obj_count - 1
        // 实际上：freelist 刚变非空意味着这个页从 full 变为 partial
        // 判断：如果释放前 freelist 为 NULL（即 inuse == obj_count）
        if (page->slab.inuse == page->slab.obj_count - 1) {
            // 从 full 变为 partial，加入 partial list
            uint64_t flags;
            spin_lock_irqsave(&cache->lock, &flags);
            partial_add(cache, page);
            spin_unlock_irqrestore(&cache->lock, flags);
        }
        // inuse == 0 且是 active_slab → 保留（下次分配直接用）
        // inuse == 0 且在 partial list → 暂不回收
    } else {
        // 跨 CPU 释放：持锁
        uint64_t flags;
        spin_lock_irqsave(&cache->lock, &flags);
        *(void **)ptr = page->slab.freelist;
        page->slab.freelist = (void *)ptr;
        page->slab.inuse--;

        if (page->slab.inuse == page->slab.obj_count - 1) {
            partial_add(cache, page);
        }
        spin_unlock_irqrestore(&cache->lock, flags);
    }
}

// ===================== kcalloc =====================
void *kcalloc(size_t n, size_t size) {
    if (n && size && n > (size_t)-1 / size) return NULL;
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) {
        char *dst = (char *)p;
        for (size_t i = 0; i < total; i++)
            dst[i] = 0;
    }
    return p;
}

// ===================== krealloc =====================
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    // 获取旧大小
    uint64_t addr = (uint64_t)ptr;
    uint64_t phys = (__force uint64_t)PHY_ADDR(addr);
    Page *page = &bfc_frames[PHY_TO_PAGE(phys)];

    size_t old_size;
    if (page->status == PAGE_SLAB) {
        old_size = page->slab.cache->obj_size;
    } else {
        old_size = page->bfc.cont_page_num * PAGE_SIZE;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy = old_size < new_size ? old_size : new_size;
    char *src = (char *)ptr;
    char *dst = (char *)new_ptr;
    for (size_t i = 0; i < copy; i++)
        dst[i] = src[i];

    kfree(ptr);
    return new_ptr;
}
