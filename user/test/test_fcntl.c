#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* 1. open(O_WRONLY|O_CREAT) → close → open(O_RDONLY) → read back → strcmp */
void test_open_create_read(void) {
  const char *path = "/local/fcntl_test.txt";
  const char *msg = "hello fcntl";

  int fd = open(path, O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  ssize_t w = write(fd, msg, strlen(msg));
  TEST_ASSERT_EQUAL_INT((int)strlen(msg), (int)w);
  close(fd);

  fd = open(path, O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  char buf[64] = {0};
  ssize_t r = read(fd, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_INT((int)strlen(msg), (int)r);
  TEST_ASSERT_EQUAL_STRING(msg, buf);
  close(fd);
}

/* 2. open nonexistent file returns -ENOENT */
void test_open_nonexist(void) {
  int fd = open("/local/no_such_file_12345", O_RDONLY);
  TEST_ASSERT_TRUE(fd < 0);
  TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}

/* 3. close same fd twice, second returns -EBADF */
void test_close_twice(void) {
  int fd[2];
  pipe(fd);

  int r1 = close(fd[0]);
  TEST_ASSERT_EQUAL_INT(0, r1);

  int r2 = close(fd[0]);
  TEST_ASSERT_TRUE(r2 < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);

  close(fd[1]);
}

/* 4. dup2(old, new) copies fd */
void test_dup2_basic(void) {
  int fd[2];
  pipe(fd);

  int new_fd = dup2(fd[0], 10);
  TEST_ASSERT_EQUAL_INT(10, new_fd);

  /* Write to fd[1], read from new_fd */
  write(fd[1], "x", 1);
  char buf[2] = {0};
  ssize_t r = read(new_fd, buf, 1);
  TEST_ASSERT_EQUAL_INT(1, (int)r);
  TEST_ASSERT_EQUAL_STRING("x", buf);

  close(fd[1]);
  close(new_fd);
  /* fd[0] was also closed by the dup2/share, but close it again */
}

/* 5. dup2 with bad fd returns -EBADF */
void test_dup2_bad_fd(void) {
  int r = dup2(-1, 10);
  TEST_ASSERT_TRUE(r < 0);
  TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* 6. fcntl F_SETFL / F_GETFL O_NONBLOCK */
void test_fcntl_setfl(void) {
  int fd[2];
  pipe(fd);

  int r = fcntl(fd[0], F_SETFL, O_NONBLOCK);
  TEST_ASSERT_EQUAL_INT(0, r);

  int flags = fcntl(fd[0], F_GETFL);
  TEST_ASSERT_TRUE(flags & O_NONBLOCK);

  /* Clear NONBLOCK */
  fcntl(fd[0], F_SETFL, 0);
  flags = fcntl(fd[0], F_GETFL);
  TEST_ASSERT_TRUE(!(flags & O_NONBLOCK));

  close(fd[0]);
  close(fd[1]);
}

/* 7. lseek SEEK_SET */
void test_lseek_set(void) {
  int fd = open("/local/fcntl_seek.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  const char *data = "ABCDEFGHIJ";
  write(fd, data, 10);
  close(fd);

  fd = open("/local/fcntl_seek.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  lseek(fd, 5, SEEK_SET);
  char buf[6] = {0};
  read(fd, buf, 5);
  TEST_ASSERT_EQUAL_STRING("FGHIJ", buf);

  close(fd);
}

/* 8. lseek SEEK_CUR */
void test_lseek_cur(void) {
  int fd = open("/local/fcntl_seek2.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  const char *data = "ABCDEFGHIJ";
  write(fd, data, 10);
  close(fd);

  fd = open("/local/fcntl_seek2.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  /* Read 3 bytes first */
  char tmp[4];
  read(fd, tmp, 3);

  /* SEEK_CUR +2 → skip DE, land on F */
  lseek(fd, 2, SEEK_CUR);
  char buf[4] = {0};
  read(fd, buf, 3);
  TEST_ASSERT_EQUAL_STRING("FGH", buf);

  close(fd);
}

/* 9. Write 20 bytes → lseek to 10 → read 10 → verify */
void test_write_read_lseek(void) {
  int fd = open("/local/fcntl_wr.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);

  char data[20];
  for (int i = 0; i < 20; i++)
    data[i] = 'A' + i;
  write(fd, data, 20);
  close(fd);

  fd = open("/local/fcntl_wr.txt", O_RDONLY);
  lseek(fd, 10, SEEK_SET);

  char buf[11] = {0};
  read(fd, buf, 10);
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL_INT('K' + i, buf[i]);
  }

  close(fd);
}

/* 10. fstat on regular file → S_ISREG + size */
void test_fstat_regular(void) {
  int fd = open("/local/fcntl_fstat.txt", O_WRONLY | O_CREAT);
  TEST_ASSERT_TRUE(fd >= 0);
  const char *data = "fstat_test_data";
  write(fd, data, strlen(data));
  close(fd);

  fd = open("/local/fcntl_fstat.txt", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);

  struct stat st;
  int r = fstat(fd, &st);
  TEST_ASSERT_EQUAL_INT(0, r);
  TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
  TEST_ASSERT_EQUAL_INT((int)strlen(data), (int)st.st_size);

  close(fd);
}

/* 11. isatty on pipe → 0 */
void test_isatty_pipe(void) {
  int fd[2];
  pipe(fd);

  int r = isatty(fd[0]);
  TEST_ASSERT_EQUAL_INT(0, r);

  close(fd[0]);
  close(fd[1]);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  RUN_TEST(test_open_create_read);
  RUN_TEST(test_open_nonexist);
  RUN_TEST(test_close_twice);
  RUN_TEST(test_dup2_basic);
  RUN_TEST(test_dup2_bad_fd);
  RUN_TEST(test_fcntl_setfl);
  RUN_TEST(test_lseek_set);
  RUN_TEST(test_lseek_cur);
  RUN_TEST(test_write_read_lseek);
  RUN_TEST(test_fstat_regular);
  RUN_TEST(test_isatty_pipe);
  return UNITY_END();
}
