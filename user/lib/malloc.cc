#include "stdlib.h"
#include "string.h"
#include "common/syscall.h"
#include <sys/mman.h>
#include "common/errno.h"
#include "common/macro.h"
#include "arch/x64/memlayout.h"
#include <unistd.h>

// ===================== Size class 定义 =====================
#define NUM_KMALLOC_CLASSES 9

static const size_t class_sizes[NUM_KMALLOC_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

static inline int size_to_class(size_t size) {
    if (size <= 8)   return 0;
    if (size <= 16)  return 1;
    if (size <= 32)  return 2;
    if (size <= 64)  return 3;
    if (size <= 128) return 4;
    if (size <= 256) return 5;
    if (size <= 512) return 6;
    if (size <= 1024) return 7;
    return 8;  // <= 2048
}

// ===================== 用户态 slab 页 header =====================
#define USER_SLAB_MAGIC  0x5B
#define BIG_ALLOC_MAGIC  0xA1

struct user_slab_header {
    uint8_t magic;             // = USER_SLAB_MAGIC
    uint8_t class_idx;         // 0-8，size class 索引
    uint16_t inuse;            // 已分配对象数
    uint16_t obj_count;        // 页内总对象数
    void *freelist;            // 空闲对象链表头（侵入式）
    user_slab_header *next;    // partial list
};

struct big_alloc_header {
    uint32_t magic;            // = BIG_ALLOC_MAGIC
    uint32_t npages;           // 占用页数
};

// ===================== 全局状态 =====================
static void *class_freelist[NUM_KMALLOC_CLASSES];
static user_slab_header *class_partial[NUM_KMALLOC_CLASSES];
// Phase 4: 全局锁保护单线程 slab 分配器（多线程安全）
static volatile int __malloc_spinlock = 0;
static inline void __malloc_lock(void) {
    while (!__atomic_test_and_set(&__malloc_spinlock, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}
static inline void __malloc_unlock(void) {
    __atomic_clear(&__malloc_spinlock, __ATOMIC_RELEASE);
}

// ===================== 辅助函数 =====================
static inline void *page_start(void *ptr) {
    return (void *)((uintptr_t)ptr & ~(uintptr_t)0xFFF);
}

// 初始化一个 slab 页
static void init_user_slab(void *page, int class_idx) {
    user_slab_header *hdr = (user_slab_header *)page;
    hdr->magic = USER_SLAB_MAGIC;
    hdr->class_idx = (uint8_t)class_idx;
    hdr->inuse = 0;
    hdr->obj_count = (uint16_t)((PAGE_SIZE - sizeof(user_slab_header)) / class_sizes[class_idx]);
    hdr->freelist = NULL;
    hdr->next = NULL;

    // 对象起始在 header 之后，按 obj_size 对齐
    char *base = (char *)page + sizeof(user_slab_header);
    // 对齐到 class_sizes[class_idx]
    uintptr_t base_addr = (uintptr_t)base;
    uintptr_t aligned = (base_addr + class_sizes[class_idx] - 1) & ~(class_sizes[class_idx] - 1);
    base = (char *)aligned;

    // 重新计算可用对象数
    size_t avail = (PAGE_SIZE - ((uintptr_t)base - (uintptr_t)page)) / class_sizes[class_idx];
    hdr->obj_count = (uint16_t)avail;

    // 建立侵入式空闲链表
    for (uint16_t i = 0; i < hdr->obj_count; i++) {
        void *obj = base + i * class_sizes[class_idx];
        *(void **)obj = hdr->freelist;
        hdr->freelist = obj;
    }
}

// 获取分配大小
static size_t get_alloc_size(void *ptr) {
    void *ps = page_start(ptr);
    user_slab_header *hdr = (user_slab_header *)ps;

    if (hdr->magic == USER_SLAB_MAGIC) {
        return class_sizes[hdr->class_idx];
    } else if (hdr->magic == BIG_ALLOC_MAGIC) {
        big_alloc_header *bhdr = (big_alloc_header *)ps;
        return bhdr->npages * PAGE_SIZE - sizeof(big_alloc_header);
    }
    return 0;
}

// ===================== malloc =====================
void *malloc(size_t size) {
    if (size == 0) size = 1;

    // 大分配：> 2048
    if (size > 2048) {
        size_t npages = (size + sizeof(big_alloc_header) + PAGE_SIZE - 1) / PAGE_SIZE;
        __malloc_lock();
        void *addr = sys_mmap(NULL, npages * PAGE_SIZE, PROT_READ | PROT_WRITE, 0, -1, 0);
        if (addr == MAP_FAILED) { __malloc_unlock(); return NULL; }

        big_alloc_header *hdr = (big_alloc_header *)addr;
        hdr->magic = BIG_ALLOC_MAGIC;
        hdr->npages = (uint32_t)npages;
        __malloc_unlock();
        return (char *)addr + sizeof(big_alloc_header);
    }

    // 小分配：slab
    int c = size_to_class(size);
    __malloc_lock();

    // 1. 从 class_freelist 取
    if (class_freelist[c]) {
        void *obj = class_freelist[c];
        class_freelist[c] = *(void **)obj;
        user_slab_header *hdr = (user_slab_header *)page_start(obj);
        hdr->inuse++;
        __malloc_unlock();
        return obj;
    }

    // 2. 从 class_partial 取
    if (class_partial[c]) {
        user_slab_header *hdr = class_partial[c];
        class_partial[c] = hdr->next;
        // 搬其 freelist 到 class_freelist
        class_freelist[c] = hdr->freelist;
        hdr->freelist = NULL;

        void *obj = class_freelist[c];
        class_freelist[c] = *(void **)obj;
        hdr->inuse++;
        __malloc_unlock();
        return obj;
    }

    // 3. 分配新 slab 页
    void *page = sys_mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, 0, -1, 0);
    if (page == MAP_FAILED) { __malloc_unlock(); return NULL; }

    init_user_slab(page, c);

    // pop 一个对象
    void *obj = ((user_slab_header *)page)->freelist;
    ((user_slab_header *)page)->freelist = *(void **)obj;
    ((user_slab_header *)page)->inuse++;

    // 剩余对象入 class_freelist
    class_freelist[c] = ((user_slab_header *)page)->freelist;
    ((user_slab_header *)page)->freelist = NULL;

    __malloc_unlock();
    return obj;
}

// ===================== free =====================
void free(void *ptr) {
    if (!ptr) return;

    void *ps = page_start(ptr);
    user_slab_header *hdr = (user_slab_header *)ps;

    if (hdr->magic == USER_SLAB_MAGIC) {
        // slab 释放
        int c = hdr->class_idx;
        __malloc_lock();
        *(void **)ptr = class_freelist[c];
        class_freelist[c] = ptr;
        hdr->inuse--;
        __malloc_unlock();
    } else if (hdr->magic == BIG_ALLOC_MAGIC) {
        // 大分配释放
        big_alloc_header *bhdr = (big_alloc_header *)ps;
        sys_munmap(ps, bhdr->npages * PAGE_SIZE);
    }
    // 其他：非法指针，忽略
}

// ===================== calloc =====================
void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size && nmemb > (size_t)-1 / size) return NULL;
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// ===================== realloc =====================
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    size_t old_size = get_alloc_size(ptr);
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    size_t copy = old_size < size ? old_size : size;
    memcpy(new_ptr, ptr, copy);
    free(ptr);
    return new_ptr;
}
