/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

#include "kernel/xcore/mm_types.h"
#include "linux/types.h"
#include "xos/ioctl.h"

#include "kernel/driver/dma_buf.h"

#include <linux/dma-buf.h>
#include <xos/errno.h>
#include <xos/mman.h>
#include <xos/page.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/bsd_types.h"
#include "kernel/driver/drm/drm_fence.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/mem/slab.h"
#include "utils/macro.h"

int bsd_dma_buf_fd_install(struct xtask *proc, struct dma_buf *dmabuf,
                           bool cloexec);
struct dma_buf *bsd_dma_buf_get_from_fd(struct xtask *proc, int fd);
struct file *bsd_sync_file_fd_get(struct xtask *proc, int fd);
int bsd_close_installed_fd(struct xtask *proc, int fd);

static atomic_t dma_buf_next_id = {.counter = 0};

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *info) {
  if (!info || !info->size || !info->pages || !info->page_count ||
      info->page_count != ALIGN_UP(info->size, PAGE_SIZE) / PAGE_SIZE)
    return NULL;
  struct dma_buf *dmabuf = kmalloc(sizeof(*dmabuf));
  if (!dmabuf)
    return NULL;
  __memset(dmabuf, 0, sizeof(*dmabuf));
  refcount_set(&dmabuf->refs, 1);
  dmabuf->id = (uint64_t)atomic_inc_return(&dma_buf_next_id);
  dmabuf->size = info->size;
  dmabuf->pages = info->pages;
  dmabuf->page_count = info->page_count;
  dmabuf->ops = info->ops;
  dmabuf->priv = info->priv;
  mutex_init(&dmabuf->lock);
  dma_resv_init(&dmabuf->resv);
  return dmabuf;
}

void dma_buf_get(struct dma_buf *dmabuf) {
  if (dmabuf)
    refcount_inc(&dmabuf->refs);
}

void dma_buf_put(struct dma_buf *dmabuf) {
  if (!dmabuf || !refcount_dec_and_test(&dmabuf->refs))
    return;
  dma_resv_fini(&dmabuf->resv);
  if (dmabuf->ops && dmabuf->ops->release)
    dmabuf->ops->release(dmabuf);
  kfree(dmabuf);
}

int dma_buf_fd_install(struct xtask *task, struct dma_buf *dmabuf,
                       bool cloexec) {
  return bsd_dma_buf_fd_install(task, dmabuf, cloexec);
}

struct dma_buf *dma_buf_get_from_fd(struct xtask *task, int fd) {
  return bsd_dma_buf_get_from_fd(task, fd);
}

static void dma_buf_vma_get(void *owner) { dma_buf_get(owner); }
static void dma_buf_vma_put(void *owner) { dma_buf_put(owner); }

static const struct vma_owner_ops dma_buf_vma_owner_ops = {
    .get = dma_buf_vma_get,
    .put = dma_buf_vma_put,
};

int dma_buf_mmap_prepare(struct dma_buf *dmabuf,
                         const struct dev_mmap_request *request,
                         struct dev_mmap_backing *backing) {
  if (!dmabuf || !request || !backing || !request->requested_length ||
      !(request->flags & MAP_SHARED) || (request->flags & MAP_PRIVATE) ||
      (request->flags & MAP_ANONYMOUS) || (request->prot & PROT_EXEC) ||
      (request->offset & (PAGE_SIZE - 1)) || request->offset > dmabuf->size ||
      request->requested_length > dmabuf->size - request->offset)
    return -EINVAL;
  uint64_t first = request->offset / PAGE_SIZE;
  uint64_t count = request->length / PAGE_SIZE;
  if (first + count > dmabuf->page_count)
    return -EINVAL;
  dma_buf_get(dmabuf);
  backing->owner = dmabuf;
  backing->owner_ops = &dma_buf_vma_owner_ops;
  backing->pages = &dmabuf->pages[first];
  backing->page_count = (uint32_t)count;
  backing->cache_flags = 0;
  return 0;
}

long dma_buf_ioctl(struct xtask *task, struct dma_buf *dmabuf, uint32_t cmd,
                   void *arg) {
  if (!task || !dmabuf)
    return -EBADF;
  if (cmd == DMA_BUF_IOCTL_SYNC) {
    struct dma_buf_sync sync;
    if (copy_from_user(&sync, arg, sizeof(sync)))
      return -EFAULT;
    if ((sync.flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK) ||
        !(sync.flags & DMA_BUF_SYNC_RW))
      return -EINVAL;
    if (!(sync.flags & DMA_BUF_SYNC_END))
      return dma_resv_wait(&dmabuf->resv,
                           (sync.flags & DMA_BUF_SYNC_WRITE) != 0, UINT64_MAX);
    return 0;
  }
  if (_IOC_TYPE(cmd) == DMA_BUF_BASE && _IOC_NR(cmd) == 1 &&
      (_IOC_SIZE(cmd) == sizeof(__u32) || _IOC_SIZE(cmd) == sizeof(__u64))) {
    char name[DMA_BUF_NAME_LEN];
    long copied = strncpy_from_user(name, arg, DMA_BUF_NAME_LEN - 1);
    if (copied < 0)
      return -EFAULT;
    name[DMA_BUF_NAME_LEN - 1] = '\0';
    mutex_lock(&dmabuf->lock);
    __memcpy(dmabuf->name, name, DMA_BUF_NAME_LEN);
    mutex_unlock(&dmabuf->lock);
    return 0;
  }
  if (cmd == DMA_BUF_IOCTL_EXPORT_SYNC_FILE) {
    struct dma_buf_export_sync_file request;
    if (copy_from_user(&request, arg, sizeof(request)))
      return -EFAULT;
    if (!(request.flags & DMA_BUF_SYNC_RW) ||
        (request.flags & ~DMA_BUF_SYNC_RW))
      return -EINVAL;
    struct drm_fence *fence = dma_resv_export_fence(
        &dmabuf->resv, (request.flags & DMA_BUF_SYNC_WRITE) != 0);
    if (!fence)
      return -ENOMEM;
    request.fd = drm_fence_install_sync_file(fence, task);
    drm_fence_put(fence);
    if (request.fd < 0)
      return request.fd;
    if (copy_to_user(arg, &request, sizeof(request))) {
      bsd_close_installed_fd(task, request.fd);
      return -EFAULT;
    }
    return 0;
  }
  if (cmd == DMA_BUF_IOCTL_IMPORT_SYNC_FILE) {
    struct dma_buf_import_sync_file request;
    if (copy_from_user(&request, arg, sizeof(request)))
      return -EFAULT;
    if (!(request.flags & DMA_BUF_SYNC_RW) ||
        (request.flags & ~DMA_BUF_SYNC_RW))
      return -EINVAL;
    struct file *file = bsd_sync_file_fd_get(task, request.fd);
    if (!file)
      return -EBADF;
    struct drm_fence *fence = file->sync_file_fence;
    int rc = dma_resv_add_fence(&dmabuf->resv, fence,
                                (request.flags & DMA_BUF_SYNC_WRITE) != 0);
    file_put(file);
    return rc;
  }
  return -ENOTTY;
}

uint64_t dma_buf_size(const struct dma_buf *dmabuf) {
  return dmabuf ? dmabuf->size : 0;
}

uint64_t dma_buf_id(const struct dma_buf *dmabuf) {
  return dmabuf ? dmabuf->id : 0;
}

struct page **dma_buf_pages(struct dma_buf *dmabuf, uint32_t *count) {
  if (!dmabuf || !count)
    return NULL;
  *count = dmabuf->page_count;
  return dmabuf->pages;
}

bool dma_buf_poll_ready(struct dma_buf *dmabuf, bool write) {
  return dmabuf && dma_resv_ready(&dmabuf->resv, write);
}
