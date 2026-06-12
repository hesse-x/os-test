#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("=== malloc/free test ===\n");

    // 1. 小分配测试（slab path）
    void *p1 = malloc(32);
    printf("malloc(32) = %p\n", p1);
    if (p1) {
        for (int i = 0; i < 32; i++) ((char *)p1)[i] = 'A' + i;
        ((char *)p1)[31] = '\0';
        printf("  content: %s\n", (char *)p1);
    }

    // 2. 多个不同 size class
    void *p2 = malloc(8);
    void *p3 = malloc(128);
    void *p4 = malloc(1024);
    printf("malloc(8)=%p malloc(128)=%p malloc(1024)=%p\n", p2, p3, p4);

    // 3. 释放后重新分配
    free(p2);
    printf("free(%p)\n", p2);
    void *p5 = malloc(8);
    printf("malloc(8) = %p (should reuse)\n", p5);

    // 4. calloc 测试
    int *arr = (int *)calloc(10, sizeof(int));
    if (arr) {
        arr[0] = 42;
        arr[9] = 99;
        printf("calloc: arr[0]=%d arr[9]=%d\n", arr[0], arr[9]);
    }
    free(arr);

    // 5. 大分配测试（mmap path）
    void *big = malloc(4096);
    printf("malloc(4096) = %p\n", big);
    if (big) {
        for (int i = 0; i < 4096; i++) ((char *)big)[i] = (char)(i & 0xFF);
        printf("  big[0]=%d big[4095]=%d\n", ((char *)big)[0], ((char *)big)[4095]);
    }
    free(big);
    printf("free(big) ok\n");

    // 6. realloc 测试
    void *r = malloc(64);
    if (r) {
        for (int i = 0; i < 64; i++) ((char *)r)[i] = 'X';
        r = realloc(r, 128);
        printf("realloc(64->128) = %p r[0]=%c r[63]=%c\n", r,
               r ? ((char *)r)[0] : '?', r ? ((char *)r)[63] : '?');
    }
    free(r);

    // 7. 释放剩余
    free(p1);
    free(p3);
    free(p4);
    free(p5);
    printf("all freed\n");

    printf("=== test done ===\n");
    return 0;
}
