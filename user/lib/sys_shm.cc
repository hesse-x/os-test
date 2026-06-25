#include <sys/shm.h>
#include <sys/mman.h>
#include <errno.h>
#include "common/syscall.h"

/* shm_create, shm_attach, shm_attach_kernel removed.
 * Use sys_shm_create / sys_shm_attach + mmap directly:
 *   int fd = sys_shm_create(size);
 *   void *addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
 */
