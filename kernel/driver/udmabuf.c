/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/xcore/xtask.h"

#include "kernel/driver/udmabuf.h"

#include <linux/udmabuf.h>
#include <xos/errno.h>
#include <xos/page.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/driver/dma_buf.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/trap.h"

#define UDMABUF_LIST_LIMIT 1024u
#define UDMABUF_MAX_SIZE (256ULL * 1024 * 1024)

struct udmabuf_segment {
  struct shm *shm;
  struct page **pinned_pages;
  uint32_t page_count;
};

struct udmabuf_backing {
  struct udmabuf_segment *segments;
  uint32_t segment_count;
  struct page **pages;
};

struct file *bsd_shm_fd_get(struct xtask *proc, int fd);

static void udmabuf_release(struct dma_buf *dmabuf) {
  struct udmabuf_backing *backing = dmabuf->priv;
  if (!backing)
    return;
  for (uint32_t i = 0; i < backing->segment_count; i++) {
    struct udmabuf_segment *segment = &backing->segments[i];
    if (segment->shm) {
      shm_unpin_range(segment->shm, segment->pinned_pages, segment->page_count);
      shm_put(segment->shm);
    }
  }
  kfree(backing->pages);
  kfree(backing->segments);
  kfree(backing);
}

static const struct dma_buf_ops udmabuf_ops = {.release = udmabuf_release};

static int udmabuf_create(xtask *proc, uint32_t flags,
                          const struct udmabuf_create_item *items,
                          uint32_t count) {
  if (!proc || !items || !count || count > UDMABUF_LIST_LIMIT ||
      (flags & ~UDMABUF_FLAGS_CLOEXEC))
    return -EINVAL;
  struct udmabuf_backing *backing = kmalloc(sizeof(*backing));
  if (!backing)
    return -ENOMEM;
  __memset(backing, 0, sizeof(*backing));
  backing->segments = kmalloc((size_t)count * sizeof(*backing->segments));
  if (!backing->segments) {
    kfree(backing);
    return -ENOMEM;
  }
  __memset(backing->segments, 0, (size_t)count * sizeof(*backing->segments));
  backing->segment_count = count;

  uint64_t total_size = 0;
  uint32_t total_pages = 0;
  int rc = 0;
  for (uint32_t i = 0; i < count; i++) {
    const struct udmabuf_create_item *item = &items[i];
    if (item->__pad || !item->size || (item->offset & (PAGE_SIZE - 1)) ||
        (item->size & (PAGE_SIZE - 1)) || item->offset > INT64_MAX ||
        item->size > INT64_MAX || item->offset > UINT64_MAX - item->size ||
        total_size > UDMABUF_MAX_SIZE - item->size) {
      rc = -EINVAL;
      goto fail;
    }
    struct file *file = bsd_shm_fd_get(proc, (int)item->memfd);
    if (!file) {
      rc = -EBADF;
      goto fail;
    }
    struct shm *shm = shm_get(file->shm);
    file_put(file);
    struct page **pages = NULL;
    uint32_t page_count = 0;
    rc = shm_pin_range(shm, item->offset, item->size, &pages, &page_count);
    if (rc) {
      shm_put(shm);
      goto fail;
    }
    if (page_count > UINT32_MAX - total_pages) {
      shm_unpin_range(shm, pages, page_count);
      shm_put(shm);
      rc = -EINVAL;
      goto fail;
    }
    backing->segments[i].shm = shm;
    backing->segments[i].pinned_pages = pages;
    backing->segments[i].page_count = page_count;
    total_pages += page_count;
    total_size += item->size;
  }

  backing->pages = kmalloc((size_t)total_pages * sizeof(*backing->pages));
  if (!backing->pages) {
    rc = -ENOMEM;
    goto fail;
  }
  uint32_t page = 0;
  for (uint32_t i = 0; i < count; i++) {
    struct udmabuf_segment *segment = &backing->segments[i];
    __memcpy(&backing->pages[page], segment->pinned_pages,
             (size_t)segment->page_count * sizeof(*backing->pages));
    page += segment->page_count;
  }

  const struct dma_buf_export_info info = {
      .size = total_size,
      .pages = backing->pages,
      .page_count = total_pages,
      .ops = &udmabuf_ops,
      .priv = backing,
  };
  struct dma_buf *dmabuf = dma_buf_export(&info);
  if (!dmabuf) {
    rc = -ENOMEM;
    goto fail;
  }
  int fd =
      dma_buf_fd_install(proc, dmabuf, (flags & UDMABUF_FLAGS_CLOEXEC) != 0);
  if (fd < 0) {
    dma_buf_put(dmabuf);
    return fd;
  }
  printk(LOG_INFO, "UDBUF: create id=%llu segments=%u size=%llu\n",
         (unsigned long long)dma_buf_id(dmabuf), count,
         (unsigned long long)total_size);
  return fd;

fail:
  for (uint32_t i = 0; i < count; i++) {
    struct udmabuf_segment *segment = &backing->segments[i];
    if (!segment->shm)
      continue;
    shm_unpin_range(segment->shm, segment->pinned_pages, segment->page_count);
    shm_put(segment->shm);
  }
  kfree(backing->pages);
  kfree(backing->segments);
  kfree(backing);
  return rc;
}

static long udmabuf_ioctl_file(xtask *proc, struct file *file, uint32_t cmd,
                               void *arg) {
  (void)file;
  if (cmd == UDMABUF_CREATE) {
    const struct udmabuf_create *create = arg;
    if (!create)
      return -EFAULT;
    const struct udmabuf_create_item item = {
        .memfd = create->memfd,
        .__pad = 0,
        .offset = create->offset,
        .size = create->size,
    };
    return udmabuf_create(proc, create->flags, &item, 1);
  }
  if (cmd == UDMABUF_CREATE_LIST) {
    const struct udmabuf_create_list *list = arg;
    if (!list)
      return -EFAULT;
    return udmabuf_create(proc, list->flags, list->list, list->count);
  }
  return -ENOTTY;
}

static struct dev_ops udmabuf_dev_ops = {
    .driver_pid = 0,
    .is_block = false,
    .storage = DEV_OPS_STATIC,
    .subsystem = "misc",
    .devtype = "udmabuf",
    .ioctl_file = udmabuf_ioctl_file,
};

void udmabuf_init(void) {
  int rc = devtmpfs_create("udmabuf", &udmabuf_dev_ops, NULL);
  if (rc)
    printk(LOG_ERROR, "UDBUF: failed to register /dev/udmabuf: %d\n", rc);
}
