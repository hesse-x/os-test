#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void exit(int status) {
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}

static unsigned long next_rand = 1;

int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (int)((next_rand / 65536) % 32768);
}

void srand(unsigned seed) {
    next_rand = seed;
}

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (nmemb <= 1) return;

    // Simple insertion sort for small arrays
    char *arr = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 && cmp(&arr[j * size], &arr[(j-1) * size]) < 0; j--) {
            // Swap arr[j] and arr[j-1]
            for (size_t k = 0; k < size; k++) {
                char tmp = arr[j * size + k];
                arr[j * size + k] = arr[(j-1) * size + k];
                arr[(j-1) * size + k] = tmp;
            }
        }
    }
}
