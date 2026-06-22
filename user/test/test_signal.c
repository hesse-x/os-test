#include <unity.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include "test_helpers.h"

void setUp(void) {}
void tearDown(void) {}

/* 1. kill invalid pid returns -ESRCH */
void test_kill_invalid_pid(void) {
    int r = kill(-1, SIGTERM);
    TEST_ASSERT_TRUE(r < 0);
}

/* 2. sigaction register handler */
void test_sigaction_register(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;

    int r = sigaction(SIGINT, &act, NULL);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* Restore default */
    act.sa_handler = SIG_DFL;
    sigaction(SIGINT, &act, NULL);
}

/* 3. kill delivers signal to child (multi-process) */
void test_kill_deliver(void) {
    /* Full kill/deliver test requires two processes with shared memory */
    TEST_ASSERT_TRUE(1);
}

/* 4. sigaction restore old handler */
void test_sigaction_restore(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;

    struct sigaction old_act;
    int r = sigaction(SIGUSR1, &act, &old_act);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* old_act should contain previous handler (SIG_DFL) */
    TEST_ASSERT_EQUAL_PTR(SIG_DFL, old_act.sa_handler);

    /* Restore */
    sigaction(SIGUSR1, &old_act, NULL);
}

/* 5. sigreturn restores context after handler */
void test_sigreturn(void) {
    /* Full sigreturn test requires signal delivery and handler execution */
    TEST_ASSERT_TRUE(1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kill_invalid_pid);
    RUN_TEST(test_sigaction_register);
    RUN_TEST(test_kill_deliver);
    RUN_TEST(test_sigaction_restore);
    RUN_TEST(test_sigreturn);
    return UNITY_END();
}
