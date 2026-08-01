# modules/stdio.cmake — musl stdio integration (musl_worklist stdio).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, add_musl_lib.
# ===================== musl stdio integration (musl_worklist stdio) =====================
# Build the upstream musl src/stdio/*.c into libc via add_musl_lib (single
# -fPIC OBJECT lib, same musl-internal include order). This replaces the
# hand-written user/lib/stdio.cc + user/lib/stdio_exit.c (deleted this batch):
# the repo's custom struct _FILE → musl's struct _IO_FILE (stdio_impl.h), with
# the full printf/scanf/getline/getdelim/open_memstream/fmemopen/... family now
# from upstream. vfprintf.c carries the built-in fmt_fp (long double) float
# printer — %f/%g/%e work (was a TODO in the hand-written version); it links
# libm (already merged). vfscanf.c uses __intscan/__floatscan (already in
# musl_stdlib_objs). vfprintf's narrow path references wctomb; vfscanf's
# references mbrtowc + mbsinit — supplied by the musl_multibyte_objs block
# (narrow-path 5-file subset). The 23 wide-char stdio files (vfwprintf/fgetwc/
# ...) are EXCLUDED — see the REMOVE_ITEM list; they drag isw*/wcsnlen/btowc/
# __c_locale which this OS doesn't compile, causing ld-musl runtime reloc
# failures, and no consumer + no <wchar.h> publication justifies them.
# No UAPI sync step (no xos/stdio.h).
#
# glob + exclude: src/stdio holds 117 files; a handful clash or drag heavy deps
# and are removed. The exclude rationale:
#   ofl.c, __lockfile.c — ALREADY compiled into musl_pthread (ofl.c PROVIDES
#     __ofl_lock/__ofl_unlock; __lockfile.c PROVIDES __lockfile/__unlockfile,
#     whose deps __pthread_self/a_cas/__wait/a_store/__wake are all in
#     musl_pthread). Recompiling here would multi-define. After this migration
#     the ofl list IS real (musl __fdopen/fopen call __ofl_add) and FLOCK IS
#     real (musl stdio paths call __lockfile) — see the updated musl_pthread
#     comment below.
#   rename.c — already in musl_unistd_objs (+ _so); recompiling multi-defines.
#   popen.c and pclose.c are included; musl_process_objs supplies spawn.
#   tmpfile.c, tmpnam.c, tempnam.c — NOT excluded. They need __randname →
#     __clock_gettime; the time module is now migrated (musl clock_gettime.c
#     provides __clock_gettime), and __randname ships in musl_stdlib_objs —
#     same blocker that stdlib mkstemp had (todo.md:344), already resolved.
#     Added back this batch (todo.md:379/391).
#   dprintf.c, vdprintf.c — NOT excluded. The musl dynamic loader (fused into
#     libc.so via musl_loader_objs) resolves its dprintf/vdprintf to these musl
#     native impls (which route through vfprintf). Every loader call site runs
#     AFTER reloc_all(&ldso) (dynlink.c:1432), so the loader's own PLT is
#     already relocated when vfprintf's PLT is reached — no boot-safe shim
#     needed. dprintf/vdprintf are NOT public libc symbols (hidden under
#     -fvisibility=hidden in the c_so SHARED path; libc.map does not list them).
# ofl_add.c is NOT excluded — it is currently compiled nowhere, and musl
# __fdopen/fopen call __ofl_add to register each FILE on the global open-FILE
# list (which __stdio_exit walks at exit). The glob brings it in.
file(GLOB MUSL_STDIO_SOURCES ${MUSL_DIR}/src/stdio/*.c)
list(REMOVE_ITEM MUSL_STDIO_SOURCES
    ${MUSL_DIR}/src/stdio/ofl.c          # in musl_pthread (PROVIDE __ofl_lock/__ofl_unlock)
    ${MUSL_DIR}/src/stdio/__lockfile.c   # in musl_pthread (PROVIDE __lockfile/__unlockfile)
    ${MUSL_DIR}/src/stdio/rename.c       # in musl_unistd_objs (+ _so)
    # NOTE: dprintf.c/vdprintf.c are NOT excluded — the loader resolves its
    # dprintf/vdprintf to musl's native stdio impl (routes through vfprintf).
    # Every loader call site runs AFTER reloc_all(&ldso) (dynlink.c:1432), so
    # the loader's own PLT is already relocated when vfprintf's PLT is reached.
    # musl_loader_shim.c no longer provides a boot-safe pair.
    # --- wide-char stdio (23 w* files) — INCLUDED. vfwprintf/vfwscanf/fgetwc/
    # fputwc/fputws/fgetws/ungetwc/fwide/fwprintf/fwscanf/wprintf/wscanf/swprintf/
    # swscanf/vwprintf/vwscanf/vswprintf/vswscanf/getwc/getwchar/putwc/putwchar/
    # open_wmemstream. These were once excluded because they referenced isw*/
    # wcsnlen/btowc/wctob + __c_locale/__c_dot_utf8_locale, none of which were
    # compiled — the dynamic loader failed with "iswspace: symbol not found".
    # All those deps are now satisfied: musl_wchar_objs provides isw*/wcsnlen/
    # btowc/wctob, and __c_locale/__c_dot_utf8_locale ship in musl_time_objs
    # (src/locale/c_locale.c). <wchar.h> (where the w* decls live) is now
    # published, so including these makes the wide-stdio family genuinely
    # usable, not declare-only. libc.map exports them under <wchar.h>. Their
    # engine helpers (__fgetwc_unlocked/__fputwc_unlocked/__fwritex/...) come
    # from these same files or musl_stdio_objs — no new hidden deps.
    )
add_musl_lib(musl_stdio_objs SOURCES ${MUSL_STDIO_SOURCES})
