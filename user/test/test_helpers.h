#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/process.h>

/* Spawn an ELF from filesystem (same pattern as init's spawn_service) */
static inline pid_t spawn_elf(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint64_t size = fd_file_size(fd);
    if (size == 0) { close(fd); return -1; }

    void *buf = malloc((size_t)size);
    if (!buf) { close(fd); return -1; }

    ssize_t nread = read(fd, buf, (size_t)size);
    close(fd);

    if ((uint64_t)nread != size) { free(buf); return -1; }

    pid_t pid = spawn(buf, (size_t)size);
    free(buf);
    return pid;
}

/* Allocate a shared mmap page for inter-process role marker */
static inline volatile int *alloc_shared_marker(void) {
    return (volatile int *)mmap(NULL, 4096,
        PROT_READ | PROT_WRITE, 0, -1, 0);
}

#endif /* TEST_HELPERS_H */
