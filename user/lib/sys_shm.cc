#include <sys/shm.h>
#include <sys/mman.h>
#include <errno.h>
#include "syscall.h"

/* shm_create, shm_attach, shm_attach_kernel removed.
 * Use memfd_create + ftruncate + mmap:
 *   int fd = memfd_create("name", 0);
 *   ftruncate(fd, size);
 *   void *addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
 */
