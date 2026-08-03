# modules/legacy.cmake — musl legacy/ selected interfaces.
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl legacy integration =====================
# euidaccess.c provides euidaccess() + eaccess() (weak_alias). It calls
# faccessat(AT_FDCWD, filename, amode, AT_EACCESS), which resolves at link time
# to the REPO's faccessat (user/lib/file.cc) — musl's own src/unistd/faccessat.c
# is EXCLUDED in unistd.cmake (its AT_EACCESS clone path needs
# __block_all_sigs/__restore_sigs/__clone shims). So no clone shim is needed
# here. The kernel's do_faccessat honors AT_EACCESS by selecting the effective
# uid (vfs.c inode_permission check_uid/check_gid).
# DO NOT glob src/legacy: cuserid/err/ftw/getpass/utmpx/... have unmet deps
# (stdio stderr layout, etc.). Add legacy sources here one-by-one as their deps
# land.
# getpagesize.c — trivial `return PAGE_SIZE;` (no syscall deps). The repo's
# stdlib_misc.c getpagesize is deleted; this replaces it. <unistd.h> declares
# getpagesize only under _GNU_SOURCE/_BSD_SOURCE, so the LIBC_EXPORT
# re-declaration in <xos/unistd_ext.h> still carries the visible C-linkage decl
# the definition attaches to.
add_musl_lib(musl_legacy_objs SOURCES
    ${MUSL_DIR}/src/legacy/euidaccess.c
    ${MUSL_DIR}/src/legacy/getpagesize.c)
