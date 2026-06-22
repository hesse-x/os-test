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
#include <sys/process.h>
#include "common/dev.h"

static int spawn_service(const char *path) {
    printf("spawn: %s\n", path);

    int fd = open(path, O_RDONLY);
    printf("open fd=%d\n", fd);
    if (fd < 0) return -1;

    uint64_t size = fd_file_size(fd);
    printf("size: 0x%lx\n", size);
    void *buf = malloc(size);
    if (!buf) { close(fd); return -1; }

    ssize_t nread = read(fd, buf, size);
    printf("nread: 0x%lx after read\n", nread);
    close(fd);

    printf("calling spawn...\n");
    pid_t pid = spawn(buf, size);
    printf("spawn returned pid=%d\n", pid);
    free(buf);

    return (pid > 0) ? (int)pid : -1;
}

static void wait_dev_ready(const char *dev_path) {
    int fd;
    while ((fd = open(dev_path, O_RDWR)) < 0) {
        struct recv_msg m;
        recv(&m, NULL, 0, 10);  // wait 10ms, allows other processes to run
    }
    close(fd);
}

int main(void) {
    printf("init: started\n");
    // 1. Wait for fs_driver to be ready (device registered + initialized)
    printf("init: waiting for fs_driver\n");
    wait_dev_ready("/dev/fs");
    printf("init: fs_driver ready\n");

    // 2. Spawn kbd_driver, wait for DEV_KBD
    printf("init: spawning kbd_driver\n");
    spawn_service("/driver/kbd.dev");
    wait_dev_ready("/dev/kbd");
    printf("init: kbd_driver ready\n");

    // 3. Spawn kms_driver, wait for DEV_KMS
    printf("init: spawning kms_driver\n");
    spawn_service("/driver/kms.dev");
    wait_dev_ready("/dev/kms");
    printf("init: kms_driver ready\n");

    // 4. Spawn terminal
    printf("init: spawning terminal\n");
    spawn_service("/usr/bin/terminal");
    printf("init: terminal spawned\n");

#ifdef TEST
    // Test build: run automated test suite after all services are ready
    printf("init: spawning test_runner\n");
    spawn_service("/test/test_runner.elf");
    printf("init: test_runner spawned\n");
#endif

    // 5. Adopt orphans + reap children
    while (1) {
        int status;
        waitpid(-1, &status, 0);
    }

    return 0;
}
