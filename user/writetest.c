// writetest.c — test file write: create file, open O_WRONLY, write, close, then read back
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <stdlib.h>
#include "common/errno.h"

// Direct fs_driver message protocol (must match fs_driver)
#define FILE_CMD_OPEN      1
#define FILE_CMD_READ      2
#define FILE_CMD_WRITE     3
#define FILE_CMD_CLOSE     4
#define FILE_CMD_CREATE    6

struct file_req {
    uint32_t cmd;
    char     path[256];
    uint32_t flags;
    uint32_t fs_fd;
    uint64_t offset;
    uint32_t count;
};

struct file_resp {
    int32_t  status;
    uint32_t fd;
    uint64_t file_size;
    uint32_t count;
    uint32_t total;
    uint8_t  data[];
};

static int fs_fd = -1;

static int fs_send(struct file_req *freq, struct file_resp *fresp, size_t resp_len) {
    return msg_fd(fs_fd, freq, sizeof(*freq), fresp, resp_len);
}

static int fs_write_send(uint32_t fs_fd_param, const void *data, size_t len,
                         struct file_resp *fresp) {
    size_t msg_len = sizeof(struct file_req) + len;
    uint8_t *msg = (uint8_t *)malloc(msg_len);
    if (!msg) { printf("writetest: malloc(%u) failed\n", (unsigned)msg_len); return -ENOMEM; }
    struct file_req *freq = (struct file_req *)msg;
    memset(freq, 0, sizeof(*freq));
    freq->cmd = FILE_CMD_WRITE;
    freq->fs_fd = fs_fd_param;
    freq->count = (uint32_t)len;
    memcpy(msg + sizeof(struct file_req), data, len);
    int r = msg_fd(fs_fd, msg, msg_len, fresp, sizeof(struct file_resp));
    free(msg);
    if (r < 0) { printf("writetest: msg_fd returned %d\n", r); return r; }
    return (int)fresp->status;
}

int main() {
    // Open fs_driver device node
    fs_fd = open("/dev/fs", O_RDWR);
    if (fs_fd < 0) {
        printf("writetest: fs_driver not found\n");
        return 1;
    }
    printf("writetest: fs_fd=%d\n", (int)fs_fd);

    const char *path = "/local/writetest.txt";
    const char *msg = "Hello from writetest!";
    int len = strlen(msg);

    // Step 1: Create the file (touch)
    struct file_req freq;
    struct file_resp fresp;
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CREATE;
    strncpy(freq.path, path, 255);
    int rc = fs_send(&freq, &fresp, sizeof(fresp));
    if (rc != 0 && rc != -EEXIST) {
        printf("writetest: create failed (rc=%d)\n", rc);
        return 1;
    }
    printf("writetest: file created\n");

    // Step 2: Open for write
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_OPEN;
    freq.flags = O_WRONLY;
    strncpy(freq.path, path, 255);

    rc = fs_send(&freq, &fresp, sizeof(fresp));
    if (rc != 0) {
        printf("writetest: open for write failed (rc=%d)\n", rc);
        return 1;
    }
    uint32_t fd = fresp.fd;
    printf("writetest: opened fd=%d file_size=%lu\n", fd, (unsigned long)fresp.file_size);

    // Step 3: Write
    rc = fs_write_send(fd, msg, len, &fresp);
    if (rc != 0) {
        printf("writetest: write failed (rc=%d)\n", rc);
        // close anyway
        memset(&freq, 0, sizeof(freq));
        freq.cmd = FILE_CMD_CLOSE;
        freq.fs_fd = fd;
        fs_send(&freq, &fresp, sizeof(fresp));
        return 1;
    }
    printf("writetest: wrote %u bytes\n", fresp.count);

    // Step 4: Close
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CLOSE;
    freq.fs_fd = fd;
    fs_send(&freq, &fresp, sizeof(fresp));
    printf("writetest: closed\n");

    // Step 5: Open for read and verify
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_OPEN;
    freq.flags = O_RDONLY;
    strncpy(freq.path, path, 255);

    rc = fs_send(&freq, &fresp, sizeof(fresp));
    if (rc != 0) {
        printf("writetest: open for read failed (rc=%d)\n", rc);
        return 1;
    }
    fd = fresp.fd;
    uint64_t file_size = fresp.file_size;
    printf("writetest: read-opened fd=%d file_size=%lu\n", fd, (unsigned long)file_size);

    // Step 6: Read back
    uint8_t readbuf[256];
    memset(readbuf, 0, sizeof(readbuf));
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_READ;
    freq.fs_fd = fd;
    freq.count = sizeof(readbuf) - 1;

    uint8_t reply_buf[sizeof(struct file_resp) + 256];
    rc = msg_fd(fs_fd, &freq, sizeof(freq), reply_buf, sizeof(reply_buf));
    if (rc < 0) {
        printf("writetest: read failed (rc=%d)\n", rc);
    } else {
        struct file_resp *rp = (struct file_resp *)reply_buf;
        if (rp->status != 0) {
            printf("writetest: read error status=%d\n", rp->status);
        } else {
            uint32_t nread = rp->count;
            memcpy(readbuf, rp->data, nread);
            readbuf[nread] = '\0';
            printf("writetest: read back %u bytes: \"%s\"\n", nread, (char*)readbuf);
            if (nread == (uint32_t)len && strcmp((char*)readbuf, msg) == 0) {
                printf("writetest: PASS\n");
            } else {
                printf("writetest: FAIL (expected %d bytes \"%s\")\n", len, msg);
            }
        }
    }

    // Close
    memset(&freq, 0, sizeof(freq));
    freq.cmd = FILE_CMD_CLOSE;
    freq.fs_fd = fd;
    fs_send(&freq, &fresp, sizeof(fresp));

    return 0;
}
