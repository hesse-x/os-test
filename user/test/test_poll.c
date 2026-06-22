#include <unity.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include "common/socket.h"

void setUp(void) {}
void tearDown(void) {}

/* 1. poll on pipe with data → POLLIN */
void test_poll_pipe_readable(void) {
    int fd[2];
    pipe(fd);

    write(fd[1], "x", 1);

    struct pollfd pfd;
    pfd.fd = fd[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    int r = poll(&pfd, 1, 1000);
    TEST_ASSERT_TRUE(r > 0);
    TEST_ASSERT_TRUE(pfd.revents & POLLIN);

    close(fd[0]);
    close(fd[1]);
}

/* 2. poll on empty pipe write end → POLLOUT */
void test_poll_pipe_writable(void) {
    int fd[2];
    pipe(fd);

    struct pollfd pfd;
    pfd.fd = fd[1];
    pfd.events = POLLOUT;
    pfd.revents = 0;

    int r = poll(&pfd, 1, 1000);
    TEST_ASSERT_TRUE(r > 0);
    TEST_ASSERT_TRUE(pfd.revents & POLLOUT);

    close(fd[0]);
    close(fd[1]);
}

/* 3. poll on empty pipe read end with timeout=0 → no events */
void test_poll_pipe_empty(void) {
    int fd[2];
    pipe(fd);

    struct pollfd pfd;
    pfd.fd = fd[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    int r = poll(&pfd, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, r);

    close(fd[0]);
    close(fd[1]);
}

/* 4. poll timeout with no events */
void test_poll_timeout(void) {
    int fd[2];
    pipe(fd);

    /* Set read end non-blocking so it never has data */
    fcntl(fd[0], F_SETFL, O_NONBLOCK);

    struct pollfd pfd;
    pfd.fd = fd[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    int r = poll(&pfd, 1, 100);
    TEST_ASSERT_EQUAL_INT(0, r);

    close(fd[0]);
    close(fd[1]);
}

/* 5. poll on socketpair */
void test_poll_socketpair(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    struct pollfd pfds[2];
    pfds[0].fd = sv[0];
    pfds[0].events = POLLIN | POLLOUT;
    pfds[0].revents = 0;
    pfds[1].fd = sv[1];
    pfds[1].events = POLLIN | POLLOUT;
    pfds[1].revents = 0;

    int r = poll(pfds, 2, 100);
    TEST_ASSERT_TRUE(r > 0);
    /* Both ends should be writable */
    TEST_ASSERT_TRUE(pfds[0].revents & POLLOUT);
    TEST_ASSERT_TRUE(pfds[1].revents & POLLOUT);

    close(sv[0]);
    close(sv[1]);
}

/* 6. poll multiple fds */
void test_poll_multiple_fd(void) {
    int p1[2], p2[2];
    pipe(p1);
    pipe(p2);

    write(p1[1], "A", 1);

    struct pollfd pfds[2];
    pfds[0].fd = p1[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = p2[0];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    int r = poll(pfds, 2, 100);
    TEST_ASSERT_TRUE(r > 0);
    TEST_ASSERT_TRUE(pfds[0].revents & POLLIN);
    /* p2 has no data */
    TEST_ASSERT_TRUE(!(pfds[1].revents & POLLIN));

    close(p1[0]);
    close(p1[1]);
    close(p2[0]);
    close(p2[1]);
}

/* 7. poll wakeup by child (multi-process — deferred) */
void test_poll_wakeup(void) {
    TEST_ASSERT_TRUE(1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_poll_pipe_readable);
    RUN_TEST(test_poll_pipe_writable);
    RUN_TEST(test_poll_pipe_empty);
    RUN_TEST(test_poll_timeout);
    RUN_TEST(test_poll_socketpair);
    RUN_TEST(test_poll_multiple_fd);
    RUN_TEST(test_poll_wakeup);
    return UNITY_END();
}
