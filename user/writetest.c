// writetest.c — test file write via POSIX interface:
// create file, open O_WRONLY|O_CREAT, write, close, then read back and verify
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main() {
    const char *path = "/local/writetest.txt";
    const char *msg = "Hello from writetest!";
    int len = strlen(msg);

    // Create and open for write
    int fd = open(path, O_WRONLY | O_CREAT);
    if (fd < 0) {
        printf("writetest: open(O_WRONLY|O_CREAT) failed, fd=%d, errno=%d\n", fd, errno);
        return 1;
    }
    printf("writetest: created fd=%d\n", fd);

    // Write
    int n = write(fd, msg, len);
    if (n != len) {
        printf("writetest: write failed (wrote %d of %d)\n", n, len);
        close(fd);
        return 1;
    }
    printf("writetest: wrote %d bytes\n", n);

    // Close
    close(fd);

    // Open for read and verify
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("writetest: open for read failed, errno=%d\n", errno);
        return 1;
    }

    char buf[256];
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("writetest: read failed\n");
        close(fd);
        return 1;
    }
    buf[n] = '\0';
    printf("writetest: read back %d bytes: \"%s\"\n", n, buf);

    close(fd);

    // Verify
    if (n == len && strcmp(buf, msg) == 0) {
        printf("writetest: PASS\n");
    } else {
        printf("writetest: FAIL (expected %d bytes \"%s\")\n", len, msg);
    }

    return 0;
}
