#include "stdlib.h"
#include "common/syscall.h"
#include "common/errno.h"
#include "arch/x64/utils.h"

// ===================== Serial debug =====================
// TODO: 统一用 NDEBUG 宏控制，NDEBUG 下为空实现
static void serial_puts(const char *s) {
    while (*s) sys_putc(*s++);
}

static void serial_put_hex64(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4)
        sys_putc(hex[(v >> i) & 0xF]);
}

// ===================== Lock placeholders =====================
// TODO: 支持多线程时替换为实际锁
#define MALLOC_LOCK()
#define MALLOC_UNLOCK()

// ===================== Constants =====================
#define ALIGN       16          // 16 字节对齐
#define HEADER_SIZE 8           // header 大小（8 字节：size|alloc|prev_size）
#define MIN_BLOCK   32          // 最小块 = header(8) + prev(8) + next(8) + footer(8)
#define SBRK_INIT   4096       // 初始 sbrk 增量
#define SBRK_MAX    65536      // sbrk 增量上限

// ===================== Block header encoding =====================
// header (8 字节):
//   [63:1]  block_size（含 header，16 字节对齐，低 4 位为 0）
//   [0]     alloc 标志（1=已分配，0=空闲）
//
// 空闲块布局:
//   [header(8)] [prev_ptr(8)] [next_ptr(8)] [free_space...] [footer(8)]
// 已分配块布局:
//   [header(8)] [payload...]
//
// prev_size: 紧邻当前块 header 之前的 8 字节存前一块大小（用于向前合并）
// footer: 空闲块末尾 8 字节 = header 值（用于向后合并时快速定位前块）

// ===================== Inline helpers =====================
static inline uint64_t block_size(uint64_t header) { return header & ~(uint64_t)1; }
static inline int      is_alloc(uint64_t header)   { return header & 1; }
static inline uint64_t make_header(uint64_t size, int alloc) { return size | (alloc & 1); }

static inline uint64_t *header_ptr(void *block)    { return (uint64_t *)block; }
static inline uint64_t *prev_ptr(void *block)       { return (uint64_t *)((char *)block + HEADER_SIZE); }
static inline uint64_t *next_ptr(void *block)       { return (uint64_t *)((char *)block + HEADER_SIZE + 8); }
static inline uint64_t *footer_ptr(void *block)     { return (uint64_t *)((char *)block + block_size(*(header_ptr(block))) - 8); }
static inline uint64_t *prev_size_ptr(void *block)  { return (uint64_t *)((char *)block - HEADER_SIZE); }

static inline void *payload(void *block)     { return (char *)block + HEADER_SIZE; }
static inline void *block_from_ptr(void *ptr) { return (char *)ptr - HEADER_SIZE; }

// ===================== Global state =====================
static void *free_list = nullptr;    // 空闲链表头
static uint64_t heap_start = 0;      // 堆起始地址
static uint64_t heap_end = 0;        // 堆结束地址（= brk）
static size_t sbrk_increment = SBRK_INIT;  // 当前 sbrk 增量
static int malloc_initialized = 0;

// ===================== 空闲链表操作 =====================

static void fl_insert(void *block) {
    uint64_t *prev = prev_ptr(block);
    uint64_t *next = next_ptr(block);
    *prev = 0;
    *next = (uint64_t)free_list;
    if (free_list) {
        uint64_t *fl_prev = prev_ptr(free_list);
        *fl_prev = (uint64_t)block;
    }
    free_list = block;
}

static void fl_remove(void *block) {
    uint64_t *prev = prev_ptr(block);
    uint64_t *next = next_ptr(block);
    uint64_t prev_val = *prev;
    uint64_t next_val = *next;

    if (prev_val) {
        uint64_t *p_next = next_ptr((void *)prev_val);
        *p_next = next_val;
    } else {
        free_list = (void *)next_val;
    }
    if (next_val) {
        uint64_t *n_prev = prev_ptr((void *)next_val);
        *n_prev = prev_val;
    }
}

// ===================== 扩展堆 =====================

static void *extend_heap(size_t need) {
    // 计算本次 sbrk 大小：至少 need，按 sbrk_increment 向上取整
    size_t alloc = need;
    if (alloc < sbrk_increment)
        alloc = sbrk_increment;
    // 按 ALIGN 对齐
    alloc = (alloc + ALIGN - 1) & ~(ALIGN - 1);

    int64_t result = sys_sbrk((int64_t)alloc);
    if (result < 0) {
        serial_puts("malloc: sbrk failed, err=");
        serial_put_hex64((uint64_t)(-result));
        sys_putc('\n');
        return nullptr;
    }

    // 翻倍 sbrk 增量（上限 SBRK_MAX）
    if (sbrk_increment < SBRK_MAX)
        sbrk_increment <<= 1;

    void *block = (void *)result;
    uint64_t old_end = heap_end;
    heap_end = (uint64_t)result + alloc;

    // 设置新块 header + footer
    uint64_t size = alloc;
    *header_ptr(block) = make_header(size, 0);
    *footer_ptr(block) = make_header(size, 0);

    // 设置下一个块的 prev_size
    uint64_t *next_prev_size = (uint64_t *)((char *)block + size - HEADER_SIZE);
    // 注意：footer 和 prev_size 重叠在空闲块的末尾，这是 ok 的
    // 当这个块被分配后，下一个块的 prev_size 应该指向这里
    (void)next_prev_size;

    // 尝试与前一个块合并（如果堆顶块也是空闲的）
    if (old_end > heap_start && (char *)block == (char *)old_end) {
        // block 紧接在旧堆顶之后，检查旧堆顶块是否空闲
        // 需要找到旧堆顶块：通过 heap_end 反推不现实，直接从 free_list 检查
        // 简化：遍历 free_list 找地址紧接的块
        void *prev_block = nullptr;
        void *it = free_list;
        while (it) {
            if ((char *)it + block_size(*(header_ptr(it))) == (char *)block) {
                prev_block = it;
                break;
            }
            it = (void *)*next_ptr(it);
        }
        if (prev_block) {
            fl_remove(prev_block);
            uint64_t prev_size = block_size(*(header_ptr(prev_block)));
            uint64_t new_size = prev_size + size;
            *header_ptr(prev_block) = make_header(new_size, 0);
            *footer_ptr(block) = make_header(new_size, 0);
            block = prev_block;
            size = new_size;
        }
    }

    fl_insert(block);
    return block;
}

// ===================== malloc =====================

void *malloc(size_t size) {
    MALLOC_LOCK();

    if (!malloc_initialized) {
        heap_start = (uint64_t)sys_sbrk(0);
        heap_end = heap_start;
        malloc_initialized = 1;
        serial_puts("malloc: heap_start=");
        serial_put_hex64(heap_start);
        sys_putc('\n');
    }

    // malloc(0) 返回唯一可 free 的指针（POSIX）
    if (size == 0)
        size = 1;

    // 对齐到 ALIGN，加上 header
    size_t total = (size + HEADER_SIZE + ALIGN - 1) & ~(ALIGN - 1);
    if (total < MIN_BLOCK)
        total = MIN_BLOCK;

    // First-fit 搜索空闲链表
    void *best = nullptr;
    void *it = free_list;
    while (it) {
        uint64_t bsize = block_size(*(header_ptr(it)));
        if (bsize >= total) {
            best = it;
            break;  // first-fit
        }
        it = (void *)*next_ptr(it);
    }

    if (!best) {
        // 没有合适的块，扩展堆
        best = extend_heap(total);
        if (!best) {
            MALLOC_UNLOCK();
            return nullptr;
        }
    }

    uint64_t bsize = block_size(*(header_ptr(best)));

    // Split if remainder >= MIN_BLOCK
    if (bsize - total >= MIN_BLOCK) {
        void *remainder = (char *)best + total;
        uint64_t rem_size = bsize - total;

        *header_ptr(best) = make_header(total, 0);
        *header_ptr(remainder) = make_header(rem_size, 0);
        *footer_ptr(remainder) = make_header(rem_size, 0);

        // 设置 remainder 下一块的 prev_size
        // (下一块可能是堆顶之外，不需要设置)

        fl_remove(best);
        fl_insert(remainder);
    } else {
        fl_remove(best);
        // 使用整个块（不 split）
        total = bsize;
    }

    // 标记已分配
    *header_ptr(best) = make_header(total, 1);

    // 设置下一个块的 prev_size
    void *next_block = (char *)best + total;
    if ((uint64_t)next_block < heap_end) {
        *prev_size_ptr(next_block) = total;
    }

    MALLOC_UNLOCK();
    return payload(best);
}

// ===================== free =====================

void free(void *ptr) {
    if (!ptr) return;  // free(NULL) is no-op (POSIX)

    MALLOC_LOCK();

    void *block = block_from_ptr(ptr);
    uint64_t size = block_size(*(header_ptr(block)));

    serial_puts("free: block=");
    serial_put_hex64((uint64_t)block);
    serial_puts(" size=");
    serial_put_hex64(size);
    sys_putc('\n');

    // 标记为空闲，写入 footer
    *header_ptr(block) = make_header(size, 0);
    *footer_ptr(block) = make_header(size, 0);

    // 向后合并：检查后一块是否空闲
    void *next_block = (char *)block + size;
    if ((uint64_t)next_block < heap_end) {
        uint64_t next_hdr = *(header_ptr(next_block));
        if (!is_alloc(next_hdr)) {
            uint64_t next_size = block_size(next_hdr);
            fl_remove(next_block);
            size += next_size;
            *header_ptr(block) = make_header(size, 0);
            *footer_ptr(block) = make_header(size, 0);
        }
    }

    // 向前合并：检查前一块是否空闲（通过 prev_size）
    if ((char *)block > (char *)heap_start + MIN_BLOCK) {
        uint64_t prev_size = *prev_size_ptr(block);
        if (prev_size >= MIN_BLOCK) {
            void *prev_block = (char *)block - prev_size;
            uint64_t prev_hdr = *(header_ptr(prev_block));
            if (!is_alloc(prev_hdr) && block_size(prev_hdr) == prev_size) {
                fl_remove(prev_block);
                size += prev_size;
                *header_ptr(prev_block) = make_header(size, 0);
                *footer_ptr(block) = make_header(size, 0);
                block = prev_block;
            }
        }
    }

    // 更新下一个块的 prev_size
    void *adj_next = (char *)block + size;
    if ((uint64_t)adj_next < heap_end) {
        *prev_size_ptr(adj_next) = size;
    }

    fl_insert(block);

    MALLOC_UNLOCK();
}

// ===================== calloc =====================

void *calloc(size_t nmemb, size_t size) {
    // 溢出检查
    if (nmemb && size && nmemb > (size_t)-1 / size) {
        return nullptr;
    }

    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        // memset to zero
        char *dst = (char *)p;
        for (size_t i = 0; i < total; i++)
            dst[i] = 0;
    }
    return p;
}

// ===================== realloc =====================

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return nullptr;
    }

    void *block = block_from_ptr(ptr);
    uint64_t old_size = block_size(*(header_ptr(block)));
    size_t payload_size = old_size - HEADER_SIZE;

    void *new_ptr = malloc(size);
    if (!new_ptr) return nullptr;

    // 拷贝旧数据（取较小值）
    size_t copy = payload_size < size ? payload_size : size;
    char *src = (char *)ptr;
    char *dst = (char *)new_ptr;
    for (size_t i = 0; i < copy; i++)
        dst[i] = src[i];

    free(ptr);
    return new_ptr;
}
