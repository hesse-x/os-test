#include <stddef.h>
#include <stdlib.h>

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

int atoi(const char *s) {
    return (int)strtol(s, (char **)NULL, 10);
}

long atol(const char *s) {
    return strtol(s, (char **)NULL, 10);
}

long strtol(const char *s, char **endptr, int base) {
    const char *p = s;
    long result = 0;
    int sign = 1;

    // Skip whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    // Handle sign
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    // Handle base detection
    if (base == 0) {
        if (*p == '0') {
            p++;
            if (*p == 'x' || *p == 'X') { base = 16; p++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    }

    // Parse digits
    while (*p) {
        int v = digit_val(*p);
        if (v < 0 || v >= base) break;
        result = result * base + v;
        p++;
    }

    if (endptr) *endptr = (char *)p;
    return sign * result;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    const char *p = s;
    unsigned long result = 0;

    // Skip whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    // Handle sign
    if (*p == '+') p++;

    // Handle base
    if (base == 0) {
        if (*p == '0') {
            p++;
            if (*p == 'x' || *p == 'X') { base = 16; p++; }
            else { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) p += 2;
    }

    while (*p) {
        int v = digit_val(*p);
        if (v < 0 || v >= base) break;
        result = result * base + v;
        p++;
    }

    if (endptr) *endptr = (char *)p;
    return result;
}
