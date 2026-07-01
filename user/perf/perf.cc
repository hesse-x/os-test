// perf.cc — perf采样包装器
// START → spawn test_runner → waitpid → STOP_DUMP
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/process.h>
#include "common/syscall.h"
#include "common/syscall_nums.h"

int main(void) {
    printf("perf: starting test_runner under timer sampling\n");
    fflush(stdout);

    // 1. 清空 buffer + 开启采样
    if (sys_perf_ctl(PERF_CTL_START) < 0) {
        printf("perf: sys_perf_ctl(START) failed\n");
        return 1;
    }

    // 2. spawn test_runner
    pid_t pid = spawn("/test/test_runner.elf");
    if (pid <= 0) {
        printf("perf: spawn test_runner failed\n");
        sys_perf_ctl(PERF_CTL_STOP);
        return 1;
    }
    printf("perf: spawned test_runner pid=%d\n", (int)pid);
    fflush(stdout);

    // 3. 等待 test_runner 完成
    int status;
    pid_t reaped = waitpid(pid, &status, 0);
    printf("perf: test_runner reaped pid=%d status=%d\n", (int)reaped, status);

    // 4. 停止采样 + 串口 dump 全部样本
    sys_perf_ctl(PERF_CTL_STOP_DUMP);
    printf("perf: dump complete\n");
    fflush(stdout);

    return 0;
}
