# modules/misc.cmake — musl src/misc integration (misc_worklist).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib (user_rules.cmake).
# ===================== musl src/misc integration =====================
# Build the upstream musl src/misc/*.c into libc via a single OBJECT library
# (musl_misc_objs, -fPIC via add_musl_lib — one compile serves both libc.a
# and libc.so, wired in user/CMakeLists.txt via $<TARGET_OBJECTS:musl_misc_objs>).
#
# Glob + exclude (mirrors resource.cmake / unistd.cmake): every src/misc/*.c is
# pulled in EXCEPT those already compiled by another module (multi-define guard)
# and those whose routed syscalls this kernel does NOT implement (which would
# leak an ENOSYS-returning symbol into libc or fail to link).
#
# Already-compiled elsewhere (multi-define guard, EXCLUDED):
#   basename.c / dirname.c / ffs.c / ffsl.c / ffsll.c — string module (libgen.h
#       + <strings.h>); see modules/string.cmake.
#   getpriority.c / setpriority.c — unistd module (pulled there so nice.c, also
#       in src/unistd, resolves setpriority at link); see modules/unistd.cmake.
#   syscall.c   — unistd module (public indirect-syscall primitive syscall()).
#   getrlimit.c / setrlimit.c / getrusage.c — resource module (prlimit64 +
#       __synccall fallback); see modules/resource.cmake.
#   getentropy.c — repo retains its own in user/lib/getrandom.c (deliberately,
#       same getrandom() wrapper); exclude to avoid a multi-define.
#
# Routed syscall NOT implemented by the kernel (EXCLUDED — would set errno=ENOSYS
# at runtime, or fail link via an uncompiled helper):
#   getresuid.c    SYS_getresuid (#118, no case in bsd/syscall.c → stub)
#   getresgid.c    SYS_getresgid (#120, stub)
#   setdomainname.c SYS_setdomainname (#171, stub)
#   initgroups.c   needs getgrouplist (now compiled by the passwd module) +
#                 setgroups (src/linux/, routes to SYS_setgroups which the kernel
#                 does NOT implement → ENOSYS leak). Still excluded; revisit once
#                 SYS_setgroups lands.
#
# INCLUDED (24): a64l fmtmsg forkpty getauxval get_current_dir_name getdomainname
#   gethostid getopt getopt_long getsubopt ioctl issetugid lockf login_tty mntent
#   nftw openpty ptsname pty realpath syslog uname wordexp.
#
# Link-time deps satisfied by already-compiled modules:
#   getopt.c       __lctrans_cur (src/locale/__lctrans.c, locale module)
#   wordexp.c      __block_all_sigs / __restore_sigs (src/signal/block.c, signal
#                  module); execl("/bin/sh") fails at runtime (no shell on disk)
#                  but links clean and returns a sane error — acceptable.
#   realpath.c     __strchrnul (string module); pure lexical (readlink/getcwd/
#                  strdup) — no /proc or O_PATH dep. Replaces repo's
#                  stdlib_misc.c:realpath (which assumed no symlinks exist).
#   ptsname.c      __ptsname_r (sibling pty.c, same glob)
#   ioctl.c / uname.c  __syscall / __syscall_ret (pthread/internal)
#   ioctl.c        SIOCGSTAMP / SIOCGSTAMPNS + time64 compat map resolve against
#                  musl's own arch/generic/bits/ioctl.h (on this target's -I list
#                  ahead of user/include). Replaces repo file.cc:ioctl — the
#                  kernel already does _IOC_SIZE/_IOC_DIR copy_in/copy_out itself
#                  (kernel/bsd/syscall.c sys_ioctl), so the repo's libc-side
#                  second copy layer was redundant; musl's thin
#                  __syscall(SYS_ioctl,fd,req,arg) shim is correct against the
#                  kernel's IPC-based dispatch (libc only forwards the args).
#   uname.c        SYS_uname (bsd/syscall.c, implemented). Replaces repo
#                  user/lib/uname.c, which hard-coded strings and never called
#                  the syscall (masking a latent ABI bug: the kernel writes a
#                  6-field new_utsname = 390B, but the old user struct utsname
#                  was 5 fields = 325B → 65B user-buffer overrun). The 6-field
#                  struct is now adopted in user/include/sys/utsname.h.
file(GLOB MUSL_MISC_SOURCES CONFIGURE_DEPENDS ${MUSL_DIR}/src/misc/*.c)
# ftw() lives in src/legacy (not src/misc): a one-line wrapper over nftw() with
# FTW_PHYS, same <ftw.h> API + no extra deps/syscalls. Pulled in here so the
# <ftw.h> family (ftw + nftw) is complete and the libc.map `ftw` export resolves.
list(APPEND MUSL_MISC_SOURCES ${MUSL_DIR}/src/legacy/ftw.c)
list(REMOVE_ITEM MUSL_MISC_SOURCES
    # Already compiled by other modules (multi-define guard):
    ${MUSL_DIR}/src/misc/basename.c       # string module (libgen.h)
    ${MUSL_DIR}/src/misc/dirname.c        # string module (libgen.h)
    ${MUSL_DIR}/src/misc/ffs.c            # string module (<strings.h>)
    ${MUSL_DIR}/src/misc/ffsl.c           # string module (<strings.h>)
    ${MUSL_DIR}/src/misc/ffsll.c          # string module (<strings.h>)
    ${MUSL_DIR}/src/misc/getpriority.c    # unistd module (nice.c dep)
    ${MUSL_DIR}/src/misc/setpriority.c    # unistd module
    ${MUSL_DIR}/src/misc/syscall.c        # unistd module (public syscall())
    ${MUSL_DIR}/src/misc/getrlimit.c      # resource module (prlimit64)
    ${MUSL_DIR}/src/misc/setrlimit.c      # resource module (__synccall fallback)
    ${MUSL_DIR}/src/misc/getrusage.c      # resource module
    ${MUSL_DIR}/src/misc/getentropy.c     # repo retains own in getrandom.c
    # Routed syscalls NOT implemented by the kernel (ENOSYS leak / link fail):
    ${MUSL_DIR}/src/misc/getresuid.c      # SYS_getresuid #118 stub
    ${MUSL_DIR}/src/misc/getresgid.c      # SYS_getresgid #120 stub
    ${MUSL_DIR}/src/misc/setdomainname.c  # SYS_setdomainname #171 stub
    ${MUSL_DIR}/src/misc/initgroups.c     # needs getgrouplist+setgroups (uncompiled)
)

add_musl_lib(musl_misc_objs SOURCES ${MUSL_MISC_SOURCES})
