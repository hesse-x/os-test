#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

void __assert_fail(const char *expr, const char *file, int line) {
    // Print assertion failure message to stderr
    fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", expr, file, line);
    _exit(1);
}
