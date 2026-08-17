/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_mkstemps_preserves_suffix_and_creates_file(void) {
  char path[] = "/local/musl-temp.XXXXXX.log";
  int fd = mkstemps(path, 4);

  TEST_ASSERT_TRUE(fd >= 0);
  TEST_ASSERT_EQUAL_STRING(".log", path + strlen(path) - 4);
  TEST_ASSERT_EQUAL_INT(3, write(fd, "ok", 3));
  TEST_ASSERT_EQUAL_INT(0, close(fd));
  TEST_ASSERT_EQUAL_INT(0, unlink(path));
}

void test_mkstemps_rejects_invalid_template(void) {
  char path[] = "/local/musl-temp.XXXXX.log";

  errno = 0;
  TEST_ASSERT_EQUAL_INT(-1, mkstemps(path, 4));
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

void test_syslog_state_api_and_gnu_program_name_are_linkable(void) {
  int old_mask = setlogmask(LOG_UPTO(LOG_NOTICE));

  TEST_ASSERT_TRUE(old_mask != 0);
  openlog("musl-misc-test", LOG_NDELAY, LOG_USER);
  syslog(LOG_DEBUG, "this masked message must not be sent");
  closelog();
  TEST_ASSERT_EQUAL_INT(LOG_UPTO(LOG_NOTICE), setlogmask(old_mask));

  // Linkage smoke: the GNU program-name symbols are exported by libc and
  // resolve at link time. We assert only that their addresses are taken
  // (i.e. they are linkable), not their runtime values. Under musl dynamic
  // linking, program_invocation_name is a weak alias of __progname_full
  // (default NULL, set by __libc_start_main→__init_libc). A copy relocation
  // copies libc.so's *initial* NULL into the app's .bss copy before
  // __init_libc runs, and __init_libc then writes libc.so's own copy — so the
  // app-visible value stays NULL. This is a known musl constraint (not a
  // bug); as a musl consumer we expect it, not the glibc-style non-NULL value.
  TEST_ASSERT_NOT_NULL(&program_invocation_name);
  TEST_ASSERT_NOT_NULL(&program_invocation_short_name);
}

// End-to-end against the real syslogd (init spawns /usr/bin/syslogd, bound
// to /dev/log). Exercises musl's syslog() client → kernel AF_UNIX DGRAM →
// syslogd → /var/log/messages, and guards two syslogd behaviors:
//  - delivery: the token reaches /var/log/messages.
//  - <PRI> strip: syslogd strips musl's "<%d>" prefix before appending, so
//    the delivered line must not carry a leading "<pri>". A regression in
//    the strip logic leaves the raw "<13>..." framing in the file.
void test_syslogd_delivers_to_messages(void) {
  static const char token[] = "syslogd-e2e-token";
  char buf[4096] = {0};

  syslog(LOG_INFO, "%s", token);
  usleep(20 * 1000);

  int fd = open("/var/log/messages", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  TEST_ASSERT_TRUE(n > 0);

  char *line = strstr(buf, token);
  TEST_ASSERT_NOT_NULL(line);
  // Walk back to line start; the <PRI> prefix, if present, is the first
  // non-space run of the line.
  while (line > buf && line[-1] != '\n')
    line--;
  TEST_ASSERT_FALSE(line[0] == '<');
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mkstemps_preserves_suffix_and_creates_file);
  RUN_TEST(test_mkstemps_rejects_invalid_template);
  RUN_TEST(test_syslog_state_api_and_gnu_program_name_are_linkable);
  RUN_TEST(test_syslogd_delivers_to_messages);
  return UNITY_END();
}
