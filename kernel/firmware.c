/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/firmware.h"

#include <xos/errno.h>

#include "arch/x64/utils.h"
#include "kernel/bsd/inode.h"
#include "kernel/bsd/vfs.h"
#include "kernel/xcore/atomic.h"
#include "kernel/xcore/kthread.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/slab.h"
#include "kernel/xcore/mutex.h"

#define FIRMWARE_PATH_PREFIX "/lib/firmware/"
#define FIRMWARE_PATH_MAX (sizeof(FIRMWARE_PATH_PREFIX) + FIRMWARE_NAME_MAX)

struct firmware_source_ops {
  void *(*open)(void *ctx, const char *path, int *error);
  int (*type)(void *source);
  int (*size)(void *source, uint64_t *size);
  int (*read)(void *source, void *data, size_t size);
  void (*close)(void *source);
};

static atomic_t firmware_live_blobs;

static int firmware_make_path(const char *name, char *path, size_t path_size) {
  if (!name || !path)
    return -EINVAL;

  size_t length = 0;
  while (length <= FIRMWARE_NAME_MAX && name[length])
    length++;
  if (length == 0)
    return -EINVAL;
  if (length > FIRMWARE_NAME_MAX)
    return -ENAMETOOLONG;
  if (name[0] == '/' || name[length - 1] == '/')
    return -EINVAL;

  size_t component_start = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i != length &&
        ((unsigned char)name[i] < 0x21U || (unsigned char)name[i] == 0x7fU))
      return -EINVAL;
    if (i != length && name[i] != '/')
      continue;
    size_t component_length = i - component_start;
    if (component_length == 0 ||
        (component_length == 1 && name[component_start] == '.') ||
        (component_length == 2 && name[component_start] == '.' &&
         name[component_start + 1] == '.'))
      return -EINVAL;
    component_start = i + 1;
  }

  const size_t prefix_length = sizeof(FIRMWARE_PATH_PREFIX) - 1;
  if (prefix_length + length + 1 > path_size)
    return -ENAMETOOLONG;
  __memcpy(path, FIRMWARE_PATH_PREFIX, prefix_length);
  __memcpy(path + prefix_length, name, length + 1);
  return 0;
}

static int firmware_request_from_source(const struct firmware **out,
                                        const char *name,
                                        const struct device *dev,
                                        const struct firmware_source_ops *ops,
                                        void *ctx) {
  if (!out || !ops || !ops->open || !ops->type || !ops->size || !ops->read ||
      !ops->close)
    return -EINVAL;
  *out = NULL;

  char path[FIRMWARE_PATH_MAX];
  int rc = firmware_make_path(name, path, sizeof(path));
  if (rc)
    return rc;

  int open_error = -ENOENT;
  void *source = ops->open(ctx, path, &open_error);
  if (!source) {
    rc = open_error < 0 ? open_error : -EIO;
    printk(LOG_WARN, "firmware: dev=%p name=%s open failed rc=%d\n", dev, name,
           rc);
    return rc;
  }

  int type = ops->type(source);
  if (type != INODE_REGULAR) {
    rc = type == INODE_DIR ? -EISDIR : -EINVAL;
    goto out_close;
  }

  uint64_t size_before = 0;
  rc = ops->size(source, &size_before);
  if (rc)
    goto out_close;
  if (size_before > FIRMWARE_MAX_SIZE || size_before > (uint64_t)SIZE_MAX) {
    rc = -EFBIG;
    goto out_close;
  }

  struct firmware *fw = kmalloc(sizeof(*fw));
  if (!fw) {
    rc = -ENOMEM;
    goto out_close;
  }
  uint8_t *data = NULL;
  if (size_before != 0) {
    data = kmalloc((size_t)size_before);
    if (!data) {
      kfree(fw);
      rc = -ENOMEM;
      goto out_close;
    }
  }

  int bytes_read =
      size_before == 0 ? 0 : ops->read(source, data, (size_t)size_before);
  uint64_t size_after = 0;
  int size_rc = ops->size(source, &size_after);
  if (size_rc) {
    rc = size_rc;
    goto out_blob;
  }
  if (size_after != size_before) {
    rc = -EAGAIN;
    goto out_blob;
  }
  if (bytes_read < 0) {
    rc = bytes_read;
    goto out_blob;
  }
  if ((uint64_t)bytes_read != size_before) {
    rc = -EIO;
    goto out_blob;
  }

  fw->size = (size_t)size_before;
  fw->data = data;
  *out = fw;
  atomic_inc(&firmware_live_blobs);
  ops->close(source);
  printk(LOG_INFO, "firmware: dev=%p name=%s size=%zu loaded\n", dev, name,
         fw->size);
  return 0;

out_blob:
  kfree(data);
  kfree(fw);
out_close:
  ops->close(source);
  printk(LOG_WARN, "firmware: dev=%p name=%s load failed rc=%d\n", dev, name,
         rc);
  return rc;
}

static void *firmware_vfs_open(void *ctx, const char *path, int *error) {
  (void)ctx;
  struct inode *inode = vfs_open_kern(path);
  if (!inode)
    *error = -ENOENT;
  return inode;
}

static int firmware_vfs_type(void *source) {
  return ((struct inode *)source)->type;
}

static int firmware_vfs_size(void *source, uint64_t *size) {
  struct inode *inode = source;
  mutex_lock(&inode->i_lock);
  *size = inode->size;
  mutex_unlock(&inode->i_lock);
  return 0;
}

static int firmware_vfs_read(void *source, void *data, size_t size) {
  return vfs_read_kernel((struct inode *)source, 0, data, size);
}

static void firmware_vfs_close(void *source) {
  inode_put((struct inode *)source);
}

static const struct firmware_source_ops firmware_vfs_ops = {
    .open = firmware_vfs_open,
    .type = firmware_vfs_type,
    .size = firmware_vfs_size,
    .read = firmware_vfs_read,
    .close = firmware_vfs_close,
};

int request_firmware(const struct firmware **out, const char *name,
                     const struct device *dev) {
  return firmware_request_from_source(out, name, dev, &firmware_vfs_ops, NULL);
}

void release_firmware(const struct firmware *fw) {
  if (!fw)
    return;
  kfree(fw->data);
  kfree(fw);
  BUG_ON(atomic_dec_return(&firmware_live_blobs) < 0);
}

#ifdef TEST
struct firmware_fake_source {
  uint64_t size_before;
  uint64_t size_after;
  int type;
  int read_result;
  int open_error;
  atomic_t open_handles;
};

struct firmware_fake_handle {
  struct firmware_fake_source *source;
  int size_calls;
};

static void *firmware_fake_open(void *ctx, const char *path, int *error) {
  (void)path;
  struct firmware_fake_source *source = ctx;
  if (source->open_error) {
    *error = source->open_error;
    return NULL;
  }
  struct firmware_fake_handle *handle = kmalloc(sizeof(*handle));
  if (!handle) {
    *error = -ENOMEM;
    return NULL;
  }
  handle->source = source;
  handle->size_calls = 0;
  atomic_inc(&source->open_handles);
  return handle;
}

static int firmware_fake_type(void *opaque) {
  struct firmware_fake_handle *handle = opaque;
  return handle->source->type;
}

static int firmware_fake_size(void *opaque, uint64_t *size) {
  struct firmware_fake_handle *handle = opaque;
  *size = handle->size_calls++ == 0 ? handle->source->size_before
                                    : handle->source->size_after;
  return 0;
}

static int firmware_fake_read(void *opaque, void *data, size_t size) {
  struct firmware_fake_handle *handle = opaque;
  for (size_t i = 0; i < size; i++)
    ((uint8_t *)data)[i] = (uint8_t)(i ^ 0xa5U);
  return handle->source->read_result == -1 ? (int)size
                                           : handle->source->read_result;
}

static void firmware_fake_close(void *opaque) {
  struct firmware_fake_handle *handle = opaque;
  atomic_dec(&handle->source->open_handles);
  kfree(handle);
}

static const struct firmware_source_ops firmware_fake_ops = {
    .open = firmware_fake_open,
    .type = firmware_fake_type,
    .size = firmware_fake_size,
    .read = firmware_fake_read,
    .close = firmware_fake_close,
};

#define FIRMWARE_TEST_CHECK(condition) BUG_ON(!(condition))

static int firmware_concurrent_test(void *arg) {
  struct firmware_fake_source *source = arg;
  const struct firmware *fw = NULL;
  int rc = firmware_request_from_source(&fw, "i915/test.bin", NULL,
                                        &firmware_fake_ops, source);
  if (!rc)
    release_firmware(fw);
  return rc;
}

static int firmware_selftest(void *arg) {
  (void)arg;
  const struct firmware *fw = (const struct firmware *)1;
  struct firmware_fake_source source = {
      .size_before = 4,
      .size_after = 4,
      .type = INODE_REGULAR,
      .read_result = -1,
  };
  atomic_set(&source.open_handles, 0);
  int baseline = atomic_read(&firmware_live_blobs);

  const char *bad_names[] = {"",     "/absolute", "../escape", "a/../b",
                             "a//b", "a/./b",     "a/",        ".",
                             "..",   "bad name",  "bad\nname"};
  for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
    fw = (const struct firmware *)1;
    FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, bad_names[i], NULL,
                                                     &firmware_fake_ops,
                                                     &source) == -EINVAL);
    FIRMWARE_TEST_CHECK(fw == NULL);
  }

  char long_name[FIRMWARE_NAME_MAX + 2];
  for (size_t i = 0; i < sizeof(long_name) - 1; i++)
    long_name[i] = 'a';
  long_name[sizeof(long_name) - 1] = '\0';
  fw = (const struct firmware *)1;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, long_name, NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -ENAMETOOLONG);
  FIRMWARE_TEST_CHECK(fw == NULL);

  source.type = INODE_DIR;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "directory", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -EISDIR);
  source.type = INODE_REGULAR;
  source.size_before = FIRMWARE_MAX_SIZE + 1ULL;
  source.size_after = source.size_before;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "too-large.bin", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -EFBIG);
  source.size_before = 4;
  source.size_after = 5;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "changing.bin", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -EAGAIN);
  source.size_after = 4;
  source.read_result = 3;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "short.bin", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -EIO);
  source.read_result = -EACCES;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "unreadable.bin", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == -EACCES);

  source.size_before = 0;
  source.size_after = 0;
  source.read_result = -1;
  FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "empty.bin", NULL,
                                                   &firmware_fake_ops,
                                                   &source) == 0);
  FIRMWARE_TEST_CHECK(fw && fw->size == 0 && fw->data == NULL);
  release_firmware(fw);

  source.size_before = 4;
  source.size_after = 4;
  source.read_result = -1;
  for (int i = 0; i < 8; i++) {
    FIRMWARE_TEST_CHECK(firmware_request_from_source(&fw, "i915/test.bin", NULL,
                                                     &firmware_fake_ops,
                                                     &source) == 0);
    FIRMWARE_TEST_CHECK(fw && fw->size == 4 && fw->data[0] == 0xa5U);
    release_firmware(fw);
  }

  struct kthread *first =
      kthread_create(firmware_concurrent_test, &source, "firmware-test-1");
  struct kthread *second =
      kthread_create(firmware_concurrent_test, &source, "firmware-test-2");
  FIRMWARE_TEST_CHECK(first && second);
  FIRMWARE_TEST_CHECK(kthread_stop(first) == 0);
  FIRMWARE_TEST_CHECK(kthread_stop(second) == 0);

  fw = (const struct firmware *)1;
  FIRMWARE_TEST_CHECK(request_firmware(&fw, "m1-selftest-missing.bin", NULL) ==
                      -ENOENT);
  FIRMWARE_TEST_CHECK(fw == NULL);
  FIRMWARE_TEST_CHECK(atomic_read(&source.open_handles) == 0);
  FIRMWARE_TEST_CHECK(atomic_read(&firmware_live_blobs) == baseline);
  printk(LOG_INFO, "firmware selftest: PASS\n");
  return 0;
}
#endif

void firmware_selftest_start(void) {
#ifdef TEST
  BUG_ON(kthread_run_detached(firmware_selftest, NULL, "firmware-selftest") !=
         0);
#endif
}
