// init process — PID 3
// Waits for fs_driver, then spawns kbd_driver, kms_driver, terminal
// Adopts orphan children and reaps them via waitpid(-1)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/device.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include "common/syscall.h"
#include "common/dev.h"

static int spawn_service(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint64_t size = fd_file_size(fd);
    void *buf = malloc(size);
    if (!buf) { close(fd); return -1; }

    read(fd, buf, size);
    close(fd);

    int64_t pid = sys_spawn(buf, size);
    free(buf);

    return (pid > 0) ? (int)pid : -1;
}

static void wait_dev_ready(int dev) {
    while (device_lookup(dev) <= 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 10);  // wait 10ms, allows other processes to run
    }
}

int main(void) {
    // 1. Wait for fs_driver to be ready (device registered + initialized)
    int32_t fs_pid;
    while ((fs_pid = device_lookup(DEV_FS)) <= 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 10);
    }

    // 2. Spawn kbd_driver, wait for DEV_KBD
    spawn_service("/driver/kbd.dev");
    wait_dev_ready(DEV_KBD);

    // 3. Spawn kms_driver, wait for DEV_KMS
    spawn_service("/driver/kms.dev");
    wait_dev_ready(DEV_KMS);

    // 4. Spawn terminal
    spawn_service("/usr/bin/terminal");

    // 5. Adopt orphans + reap children
    while (1) {
        int status;
        waitpid(-1, &status, 0);
    }

    return 0;
}
