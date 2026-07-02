// ld.so 自带最小 libc（不链 libc.a）
// ld.md §7.1 偏离：ld.so 不链 libc.a，自带最小 libc

void *memcpy(void *dst, const void *src, unsigned long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    char *d = (char *)dst;
    while (n--) *d++ = (char)c;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}
