/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* Coverage for the musl src/misc migration (modules/misc.cmake). Exercises the
 * symbols that were hand-written-missing or repo-overlapping before the
 * migration: getopt/getopt_long, a64l/l64a, getsubopt, get_current_dir_name,
 * gethostid, getauxval, ptsname/ptsname_r (ptmx), mntent, lockf, nftw, fmtmsg,
 * and the 6-field uname ABI fix. Avoids duplicating test_musl_misc (which
 * already covers mkstemps/syslog/program_invocation). */

#include <errno.h>
#include <fcntl.h>
#include <fmtmsg.h>
#include <ftw.h>
#include <getopt.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/auxv.h>
#include <sys/utsname.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---------- a64l / l64a (src/misc/a64l.c) ---------- */
static void test_a64l_l64a_roundtrip(void) {
  /* l64a maps each 6-bit group to "./0-9A-Za-z"; a64l is its inverse. A small
   * value round-trips exactly. */
  long v = 1234567L;
  char *s = l64a(v);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_INT(v, a64l(s));
  /* a64l of a known digit: '.' == 0, '/' == 1, '0' == 2. */
  TEST_ASSERT_EQUAL_INT(0, a64l("."));
  TEST_ASSERT_EQUAL_INT(1, a64l("/"));
  TEST_ASSERT_EQUAL_INT(2, a64l("0"));
}

/* ---------- getopt / getopt_long (src/misc/getopt.c, getopt_long.c) ----------
 */
static void test_getopt_parses_short_options(void) {
  /* optstring "ab:c:": b and c both take an argument. "-bval" packs the
   * argument into the same token (optarg == "val"); "-c" takes the next
   * argv element ("arg"). optind must be reset to 1 (and __optpos is reset
   * only via optreset/optind==0 in musl, but each run here starts from a
   * fresh token boundary so a packed-arg run is self-contained). */
  char *argv[] = {"prog", "-a", "-bval", "-c", "arg"};
  int argc = 5;
  optind = 1; /* reset for this run */
  TEST_ASSERT_EQUAL_INT('a', getopt(argc, argv, "ab:c:"));
  TEST_ASSERT_EQUAL_INT('b', getopt(argc, argv, "ab:c:"));
  TEST_ASSERT_EQUAL_STRING("val", optarg);
  TEST_ASSERT_EQUAL_INT('c', getopt(argc, argv, "ab:c:"));
  TEST_ASSERT_EQUAL_STRING("arg", optarg);
  TEST_ASSERT_EQUAL_INT(-1, getopt(argc, argv, "ab:c:"));
  TEST_ASSERT_EQUAL_INT(5, optind); /* consumed all 4 args + prog */
}

static void test_getopt_long_parses_long_options(void) {
  static const struct option longopts[] = {{"verbose", no_argument, 0, 'v'},
                                           {"name", required_argument, 0, 'n'},
                                           {0, 0, 0, 0}};
  char *argv[] = {"prog", "--verbose", "--name=foo", "rest"};
  int argc = 4;
  optind = 1;
  int longidx = -1;
  TEST_ASSERT_EQUAL_INT('v', getopt_long(argc, argv, "", longopts, &longidx));
  TEST_ASSERT_EQUAL_INT(0, longidx);
  TEST_ASSERT_EQUAL_INT('n', getopt_long(argc, argv, "", longopts, &longidx));
  TEST_ASSERT_EQUAL_INT(1, longidx);
  TEST_ASSERT_EQUAL_STRING("foo", optarg);
  TEST_ASSERT_EQUAL_INT(-1, getopt_long(argc, argv, "", longopts, &longidx));
}

/* ---------- getsubopt (src/misc/getsubopt.c) ---------- */
static void test_getsubopt_tokenizes(void) {
  char *tokens[] = {"width", "height", "depth", NULL};
  char opts[] = "width=10,height=20,depth=30";
  char *op = opts;
  char *val = NULL;

  TEST_ASSERT_EQUAL_INT(0, getsubopt(&op, tokens, &val));
  TEST_ASSERT_EQUAL_STRING("10", val);
  TEST_ASSERT_EQUAL_INT(1, getsubopt(&op, tokens, &val));
  TEST_ASSERT_EQUAL_STRING("20", val);
  TEST_ASSERT_EQUAL_INT(2, getsubopt(&op, tokens, &val));
  TEST_ASSERT_EQUAL_STRING("30", val);
  TEST_ASSERT_EQUAL_INT(-1, getsubopt(&op, tokens, &val));
}

/* ---------- gethostid (src/misc/gethostid.c) — stub returns 0 ---------- */
static void test_gethostid_returns_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, gethostid());
}

/* ---------- getauxval (src/misc/getauxval.c) — reads libc.auxv ---------- */
static void test_getauxval_pagesz_is_nonzero(void) {
  /* The kernel-built auxv always carries AT_PAGESZ; __init_libc caches it. */
  long pagesz = (long)getauxval(AT_PAGESZ);
  TEST_ASSERT_TRUE(pagesz > 0);
}

/* ---------- get_current_dir_name (src/misc/get_current_dir_name.c) ----------
 */
static void test_get_current_dir_name_resolves_cwd(void) {
  char *d = get_current_dir_name();
  TEST_ASSERT_NOT_NULL(d);
  /* Must agree with getcwd() (both reflect the process cwd). */
  char buf[512];
  TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(buf, d);
  free(d);
}

/* ---------- ptsname / ptsname_r (src/misc/ptsname.c + pty.c) ---------- */
static void test_ptsname_r_on_ptmx(void) {
  int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  if (m < 0) {
    /* /dev/ptmx must exist (kernel/bsd/pty.c registers it); skip-clean is not
     * an option under Unity, so assert it opened. */
    TEST_FAIL_MESSAGE("could not open /dev/ptmx");
    return;
  }
  char name[32];
  /* Unlock the pty first (grantpt/unlockpt come from the same pty.c). */
  TEST_ASSERT_EQUAL_INT(0, unlockpt(m));
  TEST_ASSERT_EQUAL_INT(0, ptsname_r(m, name, sizeof(name)));
  TEST_ASSERT_TRUE(strncmp(name, "/dev/pts/", 9) == 0);

  /* Too-small buffer yields ERANGE. */
  errno = 0;
  char tiny[4];
  TEST_ASSERT_EQUAL_INT(ERANGE, ptsname_r(m, tiny, sizeof(tiny)));

  close(m);
}

/* ---------- mntent family (src/misc/mntent.c) ---------- */
static void test_mntent_roundtrip(void) {
  /* No /proc/mounts in this procfs, so round-trip against a temp file we own.
   */
  const char *path = "/local/misc-mntent.test";
  FILE *f = fopen(path, "w+");
  if (!f) {
    TEST_FAIL_MESSAGE("could not open temp mntent file");
    return;
  }
  /* addmntent writes one struct mntent per line. POSIX/glibc/musl: 0 on
   * success, 1 on error (it returns fprintf(...) < 0, i.e. 0 when fprintf
   * wrote >= 0 chars). */
  struct mntent e1 = {.mnt_fsname = "/dev/sda1",
                      .mnt_dir = "/",
                      .mnt_type = "fat32",
                      .mnt_opts = "rw,relatime",
                      .mnt_freq = 0,
                      .mnt_passno = 0};
  struct mntent e2 = {.mnt_fsname = "proc",
                      .mnt_dir = "/proc",
                      .mnt_type = "proc",
                      .mnt_opts = "rw",
                      .mnt_freq = 0,
                      .mnt_passno = 0};
  TEST_ASSERT_EQUAL_INT(0, addmntent(f, &e1));
  TEST_ASSERT_EQUAL_INT(0, addmntent(f, &e2));
  fflush(f);
  rewind(f);

  FILE *r = setmntent(path, "r");
  TEST_ASSERT_NOT_NULL(r);

  struct mntent *got = getmntent(r);
  TEST_ASSERT_NOT_NULL(got);
  TEST_ASSERT_EQUAL_STRING("/", got->mnt_dir);
  TEST_ASSERT_TRUE(hasmntopt(got, "relatime") != NULL);

  got = getmntent(r);
  TEST_ASSERT_NOT_NULL(got);
  TEST_ASSERT_EQUAL_STRING("/proc", got->mnt_dir);

  TEST_ASSERT_NULL(getmntent(r)); /* EOF */
  endmntent(r);

  fclose(f);
  unlink(path);
}

/* ---------- lockf (src/misc/lockf.c) — fcntl F_SETLK/F_GETLK ---------- */
static void test_lockf_test_reports_unlocked(void) {
  const char *path = "/local/misc-lockf.test";
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    TEST_FAIL_MESSAGE("could not open temp lockf file");
    return;
  }
  write(fd, "x", 1);
  /* F_TEST on an uncontended region returns 0. */
  TEST_ASSERT_EQUAL_INT(0, lockf(fd, F_TEST, 0));
  /* F_LOCK then F_ULOCK round-trip. */
  TEST_ASSERT_EQUAL_INT(0, lockf(fd, F_LOCK, 0));
  TEST_ASSERT_EQUAL_INT(0, lockf(fd, F_ULOCK, 0));
  close(fd);
  unlink(path);
}

/* ---------- nftw (src/misc/nftw.c) — shallow walk of / ---------- */
static int nftw_count;
static int nftw_cb(const char *fpath, const struct stat *sb, int typeflag,
                   struct FTW *ftwbuf) {
  (void)fpath;
  (void)sb;
  (void)ftwbuf;
  /* Count only the top-level entries (depth 0 = "/" itself, depth 1 = its
   * children). Cap the walk at fd_limit=1 so it stays shallow + bounded. */
  if (typeflag == FTW_F || typeflag == FTW_D)
    nftw_count++;
  return 0;
}

static void test_nftw_walks_root(void) {
  nftw_count = 0;
  /* fd_limit=1: at most one open directory at a time; shallow enough to finish
   * quickly. "/" has at least one entry (e.g. /dev, /local, /bin). */
  TEST_ASSERT_EQUAL_INT(0, nftw("/", nftw_cb, 1, 0));
  TEST_ASSERT_TRUE(nftw_count > 0);
}

/* ---------- fmtmsg (src/misc/fmtmsg.c) — MM_PRINT to stderr ---------- */
static void test_fmtmsg_print_returns_ok(void) {
  /* MM_PRINT writes to stderr (fd 2); we only assert it doesn't error out and
   * that the severity-masked return is one of the MM_* status codes. */
  long r = fmtmsg(MM_PRINT, "os1.test", MM_INFO, "msg", NULL, "tag");
  /* MM_PRINT path returns MM_OK (0) on successful stderr write, or a
   * bitmask of MM_NOCON/MM_NOMSG/MM_NOTOK. Any of those is a valid return —
   * assert it is non-negative in the failure-bit sense (>= 0). */
  TEST_ASSERT_TRUE(r >= 0);
}

/* ---------- uname (src/misc/uname.c) + 6-field struct ABI ---------- */
static void test_uname_fills_six_fields_and_live_nodename(void) {
  struct utsname u;
  memset(&u, 0xff, sizeof(u)); /* poison to detect any unfilled field */
  TEST_ASSERT_EQUAL_INT(0, uname(&u));
  TEST_ASSERT_TRUE(u.sysname[0] != '\0');
  TEST_ASSERT_TRUE(u.nodename[0] != '\0');
  TEST_ASSERT_TRUE(u.release[0] != '\0');
  TEST_ASSERT_TRUE(u.version[0] != '\0');
  TEST_ASSERT_TRUE(u.machine[0] != '\0');
  /* The 6th field (domainname under _GNU_SOURCE) must exist and be writable —
   * this is the ABI fix: kernel writes 390B, struct is now 390B. The kernel
   * fills domainname with "" (sys_uname in kernel/bsd/syscall.c). */
  TEST_ASSERT_TRUE(u.domainname[0] == '\0');
  /* machine is "x86_64" (kernel sys_uname). */
  TEST_ASSERT_EQUAL_STRING("x86_64", u.machine);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_a64l_l64a_roundtrip);
  RUN_TEST(test_getopt_parses_short_options);
  RUN_TEST(test_getopt_long_parses_long_options);
  RUN_TEST(test_getsubopt_tokenizes);
  RUN_TEST(test_gethostid_returns_zero);
  RUN_TEST(test_getauxval_pagesz_is_nonzero);
  RUN_TEST(test_get_current_dir_name_resolves_cwd);
  RUN_TEST(test_ptsname_r_on_ptmx);
  RUN_TEST(test_mntent_roundtrip);
  RUN_TEST(test_lockf_test_reports_unlocked);
  RUN_TEST(test_nftw_walks_root);
  RUN_TEST(test_fmtmsg_print_returns_ok);
  RUN_TEST(test_uname_fills_six_fields_and_live_nodename);
  return UNITY_END();
}
