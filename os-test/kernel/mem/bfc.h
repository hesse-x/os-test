#ifndef KERNEL_MEM_BFC_H_
#define KERNEL_MEM_BFC_H_
#include <stddef.h>
#include <stdint.h>

struct block {
    unsigned char is_free:1;        // 是否空闲
    void *addr;
    size_t size;        // 块的大小
    struct block *next;
};

static inline void *get_ptr(struct block *block) __attribute__((always_inline));

static inline void *get_ptr(struct block *block) {
  return (void*)(block + 1);
}

void bfc_append_memory(void *mem, size_t size);
void *bfc_alloc(size_t size);
void bfc_free(void *ptr);
#endif // KERNEL_MEM_BFC_H_
