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
    serial_write("spawn: ", 7);
    serial_write(path, strlen(path));
    serial_write("\n", 1);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint64_t size = fd_file_size(fd);
    { char hx[]="0123456789ABCDEF"; char hb[24]; int p=0; hb[p++]='s'; hb[p++]=':'; for(int i=15;i>=0;i--){hb[p++]=hx[(size>>(i*4))&0xF];} hb[p++]='\n'; serial_write(hb,p); }
    void *buf = malloc(size);
    if (!buf) { close(fd); return -1; }

    ssize_t nread = read(fd, buf, size);
    { char hx[]="0123456789ABCDEF"; char hb[24]; int p=0; hb[p++]='n'; hb[p++]=':'; uint64_t v=nread; for(int i=15;i>=0;i--){hb[p++]=hx[(v>>(i*4))&0xF];} hb[p++]='\n'; serial_write(hb,p); }
    close(fd);

    serial_write("spawn_call\n", 11);
    int64_t pid = sys_spawn(buf, size);
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
    serial_write("init: started\n", 14);
    // 1. Wait for fs_driver to be ready (device registered + initialized)
    serial_write("init: waiting for fs_driver\n", 28);
    wait_dev_ready("/dev/fs");
    serial_write("init: fs_driver ready\n", 22);

    // 2. Spawn kbd_driver, wait for DEV_KBD
    serial_write("init: spawning kbd_driver\n", 26);
    spawn_service("/driver/kbd.dev");
    wait_dev_ready("/dev/kbd");
    serial_write("init: kbd_driver ready\n", 23);

    // 3. Spawn kms_driver, wait for DEV_KMS
    serial_write("init: spawning kms_driver\n", 26);
    spawn_service("/driver/kms.dev");
    wait_dev_ready("/dev/kms");
    serial_write("init: kms_driver ready\n", 23);

    // 4. Spawn terminal
    serial_write("init: spawning terminal\n", 24);
    spawn_service("/usr/bin/terminal");
    serial_write("init: terminal spawned\n", 23);

    // 5. Adopt orphans + reap children
    while (1) {
        int status;
        waitpid(-1, &status, 0);
    }

    return 0;
}
