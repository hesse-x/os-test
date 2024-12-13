#include "os-test/kernel/mem/block.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/utils/os_utils.h"

// This is free mem block list.
struct block *free_list = NULL;

// This is free block list.
struct block *free_block_list = NULL;

static size_t resevered_page_blocks = 0;
static size_t total_mem_size = 0;
static size_t total_avail_mem_size = 0;

struct mem_part {
  size_t page_count;
  size_t avail_size;
  void *rest_mem0;
  size_t rest_size0;
  void *rest_mem1;
  size_t rest_size1;
};

static void analysis_mem(struct mem_part *parts, void *ptr, size_t size);
static void append_block_mem(void *mem, size_t size);
static void append_mem(void *mem, size_t size);

static void analysis_mem(struct mem_part *parts, void *ptr, size_t size) {
  uintptr_t start_addr = (uintptr_t)ptr;
  size_t offset = start_addr % PAGE_SIZE;
  parts->rest_mem0 = parts;
  parts>rest_size0 = offset;
  if (offset != 0) {
    start_addr += (PAGE_SIZE - offset);
    size -= (PAGE_SIZE - offset);
  }
  if (size <= 0) {
    parts->page_count = 0;
  } else {
    parts->page_count = size / PAGE_SIZE;
  }
  parts->aval_size = parts->page_count * PAGE_SIZE;
  parts->rest_mem1 = (void*)(start_add + parts->aval_size);
  parts->rest_size1 = size % PAGE_SIZE;
}

static void append_block_mem(void *mem, size_t size) {
  if (size < sizeof(block))
    return;
  size_t new_block_num = size / sizeof(block);
  resevered_page_blocks += new_block_num;
  struct block *new_block = (struct block*)mem;
  struct block *block_end = new_block + new_block_num;
  if (free_block_list == NULL) {
    new_block->next = NULL;
    free_block_list = new_block;
    new_block += 1;
  }
  struct block *last = find_last_block(free_block_list);
  for(; new_block < block_end; new_block++) {
    new_block->next = NULL;
    last->next = new_block;
    last = new_block;
  }
}

struct block *find_last_block(struct block *b) {
  while(b->next != NULL) {
    b = b->next;
  }
  return b;
}

static void append_mem(void *mem, size_t size) {
  total_mem_size += size;
  struct mem_part parts;
  analysis_mem(&parts, mem, size);
  append_block_mem(parts.rest_mem0, parts.rest_size0);
  append_block_mem(parts.rest_mem1, parts.rest_size1);
  uintptr_t addr_start = (uintptr_t)mem + parts->rest_size0;

  size_t need_block = total_avail_mem_size / PAGE_SIZE;
  if (resevered_page_blocks < need_block) {
    append_block_mem(addr_start, PAGE_SIZE);
    addr_start += PAGE_SIZE;
    parts->aval_size -= PAGE_SIZE;
  }
  total_avail_mem_size += parts->aval_size;

  struct block *new_block = alloc_block();
  new_block->next = NULL;
  new_block->addr = addr_start;
  new_block->size = parts->aval_size;

  if (free_list == NULL) {
    free_list = new_block;
  } else {
    struct block *last = find_last_block(free_list);
    last->next = new_block;
  }
}

void init_block(struct e820map *memmap) {
  for (int i = 0; i < memmap->nr_map; i++) {
    uint32_t type = memmap->map[i].type;
    uint64_t begin = memmap->map[i].addr;
    uint64_t size = memmap->map[i].size;
    uint64_t end = begin + size;
    if (type == E820_ARM) {
      if (begin > KERNEL_STACK_TOP) {
        if (begin <= KERNEL_ENTRY_ADDR & end >= KERNEL_ENTRY_ADDR) {
          append_mem(kern_end, end - (uint64_t)kern_end);
        } else {
          append_mem((void*)begin, size);
        }
      }
    }
  }
}

struct block *alloc_block() {
  if (free_block_list != NULL) {
    struct block *ret = free_block_list;
    free_block_list = free_block_list->next;
    return ret;
  }
  return NULL;
}

void free_block(struct block *b) {
  b->next = NULL;
  if (free_block_list == NULL) {
    free_block_list = b;
  } else {
    struct block *last = find_last_block(free_block_list);
    last->next = b;
  }
}
