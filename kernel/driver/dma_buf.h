/* SPDX-License-Identifier: MIT */
#ifndef KERNEL_DRIVER_DMA_BUF_H
#define KERNEL_DRIVER_DMA_BUF_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/driver/dma_resv.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/mutex.h"

struct dev_mmap_backing;
struct dev_mmap_request;
struct xtask;

struct dma_buf;

struct dma_buf_ops {
  void (*release)(struct dma_buf *dmabuf);
};

struct dma_buf_export_info {
  uint64_t size;
  struct page **pages;
  uint32_t page_count;
  const struct dma_buf_ops *ops;
  void *priv;
};

struct dma_buf {
  refcount_t refs;
  uint64_t id;
  uint64_t size;
  mutex lock;
  struct dma_resv resv;
  struct page **pages;
  uint32_t page_count;
  const struct dma_buf_ops *ops;
  void *priv;
  char name[32];
};

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *info);
void dma_buf_get(struct dma_buf *dmabuf);
void dma_buf_put(struct dma_buf *dmabuf);
int dma_buf_fd_install(struct xtask *task, struct dma_buf *dmabuf,
                       bool cloexec);
struct dma_buf *dma_buf_get_from_fd(struct xtask *task, int fd);
long dma_buf_ioctl(struct xtask *task, struct dma_buf *dmabuf, uint32_t cmd,
                   void *arg);
int dma_buf_mmap_prepare(struct dma_buf *dmabuf,
                         const struct dev_mmap_request *request,
                         struct dev_mmap_backing *backing);
uint64_t dma_buf_size(const struct dma_buf *dmabuf);
uint64_t dma_buf_id(const struct dma_buf *dmabuf);
struct page **dma_buf_pages(struct dma_buf *dmabuf, uint32_t *count);
bool dma_buf_poll_ready(struct dma_buf *dmabuf, bool write);

#endif
