#ifndef KERNEL_MEM_BLOCK_H_
#define KERNEL_MEM_BLOCK_H_
#include <stddef.h>
#include <stdint.h>

struct block;

struct block {
  void *addr;
  size_t size;
  struct block *next;
};

extern struct block *free_list;

void init_block();
struct block *alloc_block();
void free_block(struct block *b);
struct block *find_last_block(struct block *b);
#endif // KERNEL_MEM_BLOCK_H_
