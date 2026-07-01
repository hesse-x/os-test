#include <stdio.h>

int main(void) {
    double a = 3.14, b = 2.71, c;
    // Explicit inline asm: guarantee movsd + mulsd execute, regardless of
    // compiler optimization level (Release may keep vars in SSE regs but
    // avoid movsd; Debug uses stack). This precisely exercises the user-mode
    // SSE instruction path that depends on CR4.OSFXSR.
    __asm__ volatile("movsd %1, %%xmm0\n"
                     "mulsd %2, %%xmm0\n"
                     "movsd %%xmm0, %0\n"
                     : "=m"(c) : "m"(a), "m"(b) : "xmm0");
    if (c > 8.5 && c < 8.6) {
        printf("sse_smoke: PASS\n");
        return 0;
    }
    printf("sse_smoke: FAIL c=%f\n", c);
    return 1;
}
