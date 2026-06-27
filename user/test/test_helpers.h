#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/process.h>

/* Spawn an ELF from filesystem (delegates to spawn() which does fork+execve internally) */
static inline pid_t spawn_elf(const char *path) {
    return spawn(path);
}

/* Allocate a shared mmap page for inter-process role marker */
static inline volatile int *alloc_shared_marker(void) {
    return (volatile int *)mmap(NULL, 4096,
        PROT_READ | PROT_WRITE, 0, -1, 0);
}

#endif /* TEST_HELPERS_H */
