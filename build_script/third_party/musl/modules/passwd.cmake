# modules/passwd.cmake — musl src/passwd integration (passwd_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib (user_rules.cmake).
# ===================== musl src/passwd integration =====================
# Build the upstream musl src/passwd/*.c into libc via a single OBJECT library
# (musl_passwd_objs, -fPIC via add_musl_lib — one compile serves both libc.a
# and libc.so, wired in user/CMakeLists.txt via $<TARGET_OBJECTS:musl_passwd_objs>).
#
# Glob the whole directory (mirrors stdio/regex/multibyte — no exclude). The
# passwd database APIs (getpwnam/getpwuid/and _r, getgrnam/getgrgid/and _r,
# getpwent/getgrent + set/end, getspnam/getspent + set/end, fget*/put*,
# lckpwdf/ulckpwdf, getgrouplist) read /etc/passwd, /etc/group, /etc/shadow
# via stdio fopen/getline and fall back to nscd over an AF_UNIX socket when
# the file lookup misses — they issue NO syscalls of their own.
#
# Header deps all already migrated (no shim needed — same as sched.h/fnmatch.h):
#   pwd.h / grp.h / shadow.h  — third_party/musl/include, on every user ELF's
#                               -I path via MUSL_INCLUDE_FLAGS (user_rules.cmake)
#   pwf.h / nscd.h            — musl-internal, resolved by add_musl_lib's
#                               src/internal + src/include on the include path
#
# Link-time deps satisfied by already-compiled modules:
#   get*_a.c / getgrouplist.c   fopen/getline/fread/fdopen (stdio module)
#   get*_a.c / getgr_r.c        realloc/calloc/free (malloc/stdlib modules)
#   all                         pthread_setcancelstate (pthread module)
#   nscd_query.c                socket/connect/sendmsg PF_UNIX (socket module),
#                               bswap_32 (string module)
#   getspnam_r.c                open/fstat (unistd/fcntl modules), __parsespent
#                               (sibling getspnam_r.c, same glob)
#
# NOT in this directory (so NOT pulled by this glob — recorded for clarity):
#   initgroups.c   lives in src/misc; depends on getgrouplist (gained here) +
#                  setgroups (src/linux, routes to SYS_setgroups which the
#                  kernel does NOT implement → ENOSYS leak). Stays excluded by
#                  misc.cmake's own rule; revisit once SYS_setgroups lands.
#   setgroups.c    lives in src/linux (linux module territory).
#
# Runtime note: /etc/passwd, /etc/group, /etc/shadow are NOT pre-installed on
# disk.img. Until they exist, every lookup fopen()s ENOENT, the nscd fallback
# connect()s ENOENT, and the APIs return NULL + errno=ENOENT — correct
# "not found" semantics, not a crash. Tests that exercise real parsing write
# the files themselves (see user/test/test_passwd.c), mirroring the
# tmp-dir-not-precreated pattern.
file(GLOB MUSL_PASSWD_SOURCES CONFIGURE_DEPENDS ${MUSL_DIR}/src/passwd/*.c)

add_musl_lib(musl_passwd_objs SOURCES ${MUSL_PASSWD_SOURCES})
