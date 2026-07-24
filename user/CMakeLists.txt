# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — userspace libc + test programs

# libc source list (dual artifacts from same source: libc.a + libc.so)
# start.cc removed (_start moved to crt0.o, see plan_ld2b3 T4/T21)
# Added start_main.cc (__libc_start_main, T11), atexit.c (T8), init_array.c (T10)
set(LIBC_SOURCES
    lib/stdio.cc lib/string.cc lib/malloc.cc
    lib/unistd.cc lib/time.cc lib/file.cc
    lib/ctype.c lib/strtol.c lib/stdlib_misc.c lib/uname.c
    lib/assert.c lib/errno.cc lib/signal.cc lib/getrandom.c
    lib/tls.cc lib/pthread.cc
    lib/setjmp.S
    lib/start_main.cc
    lib/io_multiplex.cc
    lib/sys_process.cc
    lib/sys_device.cc
    lib/sys_ipc.cc
    ${CMAKE_SOURCE_DIR}/utils/kvformat.c
)

# libc.a (static, -fno-pie -DDYNAMIC=0)
add_user_lib(c
    SOURCES ${LIBC_SOURCES}
    C
    FLAGS "-fno-pie -DDYNAMIC=0"
)

# libc.so (dynamic, -fPIC -DDYNAMIC=1)
# OUTPUT_NAME c -> artifact libc.so (gcc -shared -Wl,-soname,libc.so)
add_user_lib(c_so
    SOURCES ${LIBC_SOURCES}
    C
    FLAGS "-fPIC -DDYNAMIC=1"
    SHARED
    OUTPUT_NAME c
    VERSION_MAP user/libc.map
    # NOTE: no SO_LINK_LIBS — libc.so is the base library, does not record DT_NEEDED on itself
)

# libm source list
set(LIBM_SOURCES
    lib/math/math_basic.c
    lib/math/math_float.c
    lib/math/sincos.c
)

# libm.a (static)
add_user_lib(m
    SOURCES ${LIBM_SOURCES}
    C
    FLAGS "-fno-pie -D__LIBM_BUILD__"
)

# libm.so (dynamic)
add_user_lib(m_so
    SOURCES ${LIBM_SOURCES}
    C
    FLAGS "-fPIC -DDYNAMIC=1 -D__LIBM_BUILD__ -fvisibility=default"
    SHARED
    OUTPUT_NAME m
    VERSION_MAP user/libm.map
)


# Unity test framework (third_party 例外：编译期符号依赖，STATIC 链入 test ELF)
# add_third_party_lib(unity STATIC ...)：关 warning(-w)，仅 link os_base_options 取共性基础项，
# 不被我方 -Werror 门禁污染。test ELF 链 unity 替代内联 unity.c（消除 28 处内联 + 重复 DEFS）。
# UNITY_EXCLUDE_SETJMP_H / UNITY_EXCLUDE_MATH_H：排除 host <setjmp.h>/<math.h>。freestanding +
# -nostdlib 下 <sys/cdefs.h> 不被拉入，host math.h 的 __BEGIN_DECLS 等宏无法解析；Unity 自带
# UNITY_IS_NAN/IS_INF fallback。定义挂在 unity target，经 INTERFACE 传给链入它的 test ELF。
add_third_party_lib(unity STATIC
    SOURCES ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/user/include
        ${CMAKE_SOURCE_DIR}/third_party/Unity/src
    INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/Unity/src)
target_compile_definitions(unity PUBLIC UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H UNITY_INCLUDE_DOUBLE)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c)
add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c)

# ld.so (phase 2b+3: full load chain)
add_user_ldso(ldso SOURCES
    ldso/start.S
    ldso/dls3.c
    ldso/dls_init.c
    ldso/load_so.c
    ldso/relocate.c
    ldso/symtab.c
    ldso/link_map.c
    ldso/dl_puts.c
    ldso/minilibc.c
)

# hello_dyn (phase 2b+3: full dynamic chain verification)
# LINK_LIBS c -> -lc resolves to libc.so (dynamic link, records DT_NEEDED libc.so)
# add_user_dyn_elf 内部已自动解析 LINK_LIBS→<lib>_so/lib<lib>_so target 并 add_dependencies
# （见 user_rules.cmake），无需手写 add_dependencies(hello_dyn_dyn_elf c_so)。
add_user_dyn_elf(hello_dyn C SOURCES hello_dyn.c LINK_LIBS c)

# terminal — dynamic ELF (libc.so + libinput.so), PT_INTERP → ld.so
add_user_dyn_elf(terminal
    SOURCES driver/terminal_libinput.cc
    LINK_LIBS input drm m c udev
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libinput/src;${CMAKE_SOURCE_DIR}/third_party/libinput/include;${CMAKE_SOURCE_DIR}/user/lib/udev-shim;${CMAKE_SOURCE_DIR}/user/lib/evdev-shim;${CMAKE_SOURCE_DIR}/third_party/drm;${CMAKE_SOURCE_DIR}/third_party/drm/include/drm"
)

# evdev: real keyboard event source (xHCI HID → SHM ring → terminal) +
# EVIOCG* ioctl query. Replaces the kbd driver for terminal input.
add_user_elf(evdev  SOURCES driver/evdev.cc lib/usb_kbd.cc  LINK_LIBS c)

# ===== libinput (static library, keyboard subset) =====
# Provides libinput event processing pipeline. Wraps libevdev/udev shims.
set(LIBINPUT_SOURCES
    # libinput core (keyboard-only subset)
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-fallback.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-middle-button.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-button-debounce.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-mouse-wheel.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/path-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/udev-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/timer.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/quirks.c
    # filters
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/filter.c
    # utilities
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-files.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-list.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-ratelimit.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-strings.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-prop-parsers.c
    # os-test compat layer (scandir etc.)
    lib/libinput-compat/compat.c
    lib/libinput-compat/stubs.c
    lib/fnmatch.c
    # evdev/udev shims (needed by libinput internal)
    lib/evdev-shim/evdev.c
    # udev.c 不在此:libinput.so 经 SO_LINK_LIBS udev 链 libudev.so 取 udev_*
    # (完整 Linux 模式:libudev 独立 .so,见下方 libudev_so)。
)

add_user_lib(input
    SOURCES ${LIBINPUT_SOURCES}
    C
    FLAGS "-O2 -DLIBINPUT_QUIRKS_DIR=\\\"/usr/share/libinput\\\" -DHAVE_QUIRKS -Wno-unused-parameter -Wno-duplicate-decl-specifier -Wno-implicit-function-declaration -Wno-return-type -Wno-int-conversion -Wno-format -Wno-implicit-fallthrough -Wno-sign-compare -Wno-error=cpp -Wno-maybe-uninitialized"
    OUTPUT_NAME input
)

set(LIBINPUT_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include/uapi
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi/compat
    ${CMAKE_SOURCE_DIR}/third_party/libinput/include
    ${CMAKE_SOURCE_DIR}/third_party/libinput
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-config
    ${CMAKE_SOURCE_DIR}/user/lib/evdev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/udev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-compat
)

target_include_directories(input PRIVATE ${LIBINPUT_INCLUDE_DIRS})

# libinput.so (shared library, third_party SHARED-only, dynamically linked into terminal.elf)
# add_third_party_lib（分支 B）关 warning(-w)、用 os_base_options 等价基础项。-fvisibility=hidden
# + libinput 的 LIBINPUT_EXPORT(visibility("default")) 标记公共 API，仅导出公共符号。
# SO_LINK_LIBS c udev 记 DT_NEEDED libc.so + libudev.so。
add_third_party_lib(libinput_so
    SOURCES ${LIBINPUT_SOURCES}
    C
    OUTPUT_NAME input
    SO_LINK_LIBS c udev
    FLAGS "-O2 -DHAVE_QUIRKS"
    INCLUDE_DIRS ${LIBINPUT_INCLUDE_DIRS}
)

# libudev.so (独立共享库,完整 Linux 模式,third_party SHARED-only):udev-shim 的
# udev_* API 导出供终端 main(libinput_udev_create_context 前的 udev_new())与 libinput.so
# 内部(udev-seat.c 调 udev_*)共享。add_third_party_lib（分支 B）关 warning(-w)、用
# os_base_options 等价基础项。FLAGS 追加 -fvisibility=default 覆盖默认 hidden——udev.c 内
# 非 static 函数全为公共 udev_* API(无内部泄漏),故默认可见即全导出,无需逐个 UDEV_EXPORT 标记。
# 对齐 Linux:libudev 与 libinput 分离;静态消费者(test_udevd/test_libudev/test_udevd_db
# 各自 SOURCES 编 udev.c)不受影响。
add_third_party_lib(libudev_so
    SOURCES lib/udev-shim/udev.c
    C
    OUTPUT_NAME udev
    SO_LINK_LIBS c
    FLAGS "-fvisibility=default -O2"
)

if(TEST)
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c DEFS TEST)
else()
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c)
endif()

# udev/ — device event daemon (subscribes to netlink uevent group)
add_user_elf(udevd C SOURCES udev/udevd.c LINK_LIBS c)

# ===== ld.so multi-dependency unit tests (ld.md §8 / plan_ld phase D) =====
# Fully decoupled from drm: use stub .so topologies + a dynamic main ELF to
# cover the loading layer (recursive load / dedup / cycle detection).
# stub .so does not go through add_user_lib(SHARED) (that path is tied to
# libc.map + libc export validation); here we use standalone custom commands,
# matching the ld.so/libc.so pattern (gcc -shared -fPIC -Wl,-soname).
#
# Cycle scenario needs a<->b mutual NEEDED (plan_ld D7). Mutual references have
# a link-time bootstrap problem (when linking a, b is not yet built). Solution:
#   1. libb.so.seed: empty .so (SONAME=libb.so, only NEEDED libc.so) as
#      bootstrap seed.
#   2. liba.so: link ld_stub_a.c + directly reference libb.so.seed
#      (--no-as-needed), records NEEDED libb.so (takes the seed's SONAME) +
#      libc.so. liba does not call any b symbol, no symbol resolution needed,
#      DT_NEEDED is purely a record.
#   3. libb.so: link ld_stub_b.c + directly reference liba.so (--no-as-needed)
#      + libc.so. libb calls lda_answer (from liba) — --allow-shlib-undefined
#      tolerates this undefined reference, records NEEDED liba.so + libc.so.
# Chain/diamond scenarios reuse the same pair of stubs; dedup guarantees
# correctness.

# 1. libb.so.seed: bootstrap seed (empty .so, SONAME=libb.so, only NEEDED libc.so)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so.seed
    COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -o ${CMAKE_BINARY_DIR}/libb.so.seed -x c /dev/null
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so.seed (ld.so stub bootstrap)")
# 2. liba.so: NEEDED libb.so (seed SONAME) + libc.so
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/liba.so
    COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,liba.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c
            ${CMAKE_BINARY_DIR}/libb.so.seed
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c ${CMAKE_BINARY_DIR}/libb.so.seed ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking liba.so (ld.so stub, NEEDED libb.so+libc.so)")
add_custom_target(ld_stub_a_so ALL DEPENDS ${CMAKE_BINARY_DIR}/liba.so)
add_dependencies(ld_stub_a_so c_so)
# 3. libb.so: NEEDED liba.so + libc.so (lda_answer reference tolerated via --allow-shlib-undefined)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so
    COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/libb.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c
            ${CMAKE_BINARY_DIR}/liba.so
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so (ld.so stub, NEEDED liba.so+libc.so)")
add_custom_target(ld_stub_b_so ALL DEPENDS ${CMAKE_BINARY_DIR}/libb.so)
add_dependencies(ld_stub_b_so ld_stub_a_so c_so)

# 4 dynamic main ELFs: one per scenario, hooked into test_runner.c's tests[] table (D9)
# Scenario 1 single-dependency regression
add_user_dyn_elf(ld_test_single C SOURCES test/ld_test_single.c LINK_LIBS c)
# Scenario 2 linear chain (main -> b -> {a, libc})
add_user_dyn_elf(ld_test_chain C SOURCES test/ld_test_chain.c LINK_LIBS b a c)
add_dependencies(ld_test_chain_dyn_elf ld_stub_b_so ld_stub_a_so c_so)
# Scenario 3 diamond dedup (main -> {a, libc}, a -> libc)
add_user_dyn_elf(ld_test_diamond C SOURCES test/ld_test_diamond.c LINK_LIBS a c)
add_dependencies(ld_test_diamond_dyn_elf ld_stub_a_so c_so)
# Scenario 4 cycle detection (a<->b mutual NEEDED)
add_user_dyn_elf(ld_test_cycle C SOURCES test/ld_test_cycle.c LINK_LIBS a b c)
add_dependencies(ld_test_cycle_dyn_elf ld_stub_a_so ld_stub_b_so c_so)

# ===== libdrm (third_party/drm, core only) — plan_drm2 =====
# libdrm 公共编译参数 (libdrm.md §3.3)
#
# config.h / libdrm.map live in the MAIN repo (build_script/libdrm/), not in
# the third_party/drm submodule — we must not pollute the upstream submodule
# working tree. CMake copies them into the build tree (configure_file) so the
# -include path resolves to build/libdrm_config.h and the (phase-4) version
# script to build/libdrm.map. generated_static_table_fourcc.h is produced by
# upstream's gen_table_fourcc.py (add_custom_command) so the build is fully
# reproducible with no manual one-shot steps.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/config.h
               ${CMAKE_BINARY_DIR}/libdrm_config.h COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/libdrm.map
               ${CMAKE_BINARY_DIR}/libdrm.map COPYONLY)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    COMMAND python3
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
            ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
    COMMENT "Generating libdrm static fourcc table")
add_custom_target(libdrm_fourcc_table
    DEPENDS ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h)

set(LIBDRM_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drm.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmMode.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmHash.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmRandom.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmSL.c)

# libdrm.a (static, -fno-pie, relaxed warnings — upstream submodule code)
# INCLUDE_DIRS: private paths for compiling libdrm sources (upstream <drm.h>,
# xf86drm internal headers, build dir for config.h/fourcc table).
# INTERFACE_INCLUDE_DIRS: propagated to any target that links drm — provides
# xf86drm.h / xf86drmMode.h location + upstream <drm.h> resolution path,
# so consumers don't need explicit INCLUDE_DIRS.
add_drm_lib(drm C SOURCES ${LIBDRM_SOURCES}
            INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                         ${CMAKE_SOURCE_DIR}/third_party/drm
                         ${CMAKE_BINARY_DIR}
            INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                                   ${CMAKE_SOURCE_DIR}/third_party/drm
            FLAGS "-Wno-unused-parameter -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h")
# ensure generated_static_table_fourcc.h exists before libdrm sources compile
add_dependencies(drm libdrm_fourcc_table)

# libdrm.a 验证小程序 (libdrm.md §3.4: drmOpen + drmModeGetResources)
# xf86drm.h / xf86drmMode.h and upstream <drm.h> resolution paths
# propagate automatically from the drm target's INTERFACE_INCLUDE_DIRS.
add_user_elf(drm_test_link C SOURCES test/drm_test_link.c LINK_LIBS drm c)

# ===== libdrm.so (shared library, third_party SHARED-only) — Phase A =====
# add_third_party_lib（分支 B：custom-command .so）关 warning(-w)、用 os_base_options
# 等价基础项，不携带我方 WARN_FLAGS(-Werror)，实现第三方 warning 自治。SO_LINK_LIBS c
# 记 DT_NEEDED libc.so。LINK_DEPS 挂 fourcc 表生成 target。-fvisibility=hidden + drm_public
# (via HAVE_VISIBILITY) gate exports（无 VERSION_MAP：verify_libc_exports.sh 不处理 drm* glob）。
add_third_party_lib(drm_so
    SOURCES ${LIBDRM_SOURCES}
    C
    OUTPUT_NAME drm
    SO_LINK_LIBS c
    LINK_DEPS libdrm_fourcc_table
    FLAGS "-Wno-unused-parameter -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h"
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                 ${CMAKE_SOURCE_DIR}/third_party/drm
                 ${CMAKE_BINARY_DIR}
)

# ===== libffi (third_party/libffi, full-feature incl. closures) — ffi_worklist =====
# fficonfig.h: hand-written fixed template (COPYONLY copy into build tree).
# Pre-defines FFI_MMAP_EXEC_SELINUX/PAX=0 to skip /proc/mounts/statfs probing,
# HAVE_MEMFD_CREATE=1 so closures.c open_temp_exec_file picks memfd first, and
# HAVE_MREMAP=0 (kernel has no mremap). See libffi.md §3.2 / ffi_worklist §1.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libffi/fficonfig.h
               ${CMAKE_BINARY_DIR}/fficonfig.h COPYONLY)

# ffitarget.h: upstream public header (include/Makefile.am:9 nodist_include_HEADERS
# = ffi.h ffitarget.h). ffi.h:86 does `#include <ffitarget.h>`, so every consumer
# of <ffi.h> transitively needs it. The per-arch copies live under src/<arch>/
# (noinst_HEADERS, Makefile.am:53); configure picks the current arch's copy into
# the public include dir. Mirror that: COPYONLY the x86-64 ffitarget.h into the
# build tree next to ffi.h/fficonfig.h, so consumers (test_ffi) and ffi_so itself
# both resolve <ffitarget.h> from the single public dir ${CMAKE_BINARY_DIR} —
# nobody reaches into src/x86/.
configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/src/x86/ffitarget.h
               ${CMAKE_BINARY_DIR}/ffitarget.h COPYONLY)

# ffi.h: generated from ffi.h.in by @-substitution. The 6 placeholders in
# ffi.h.in are @TARGET@/@HAVE_LONG_DOUBLE@/@FFI_EXEC_TRAMPOLINE_TABLE@/
# @VERSION@/@FFI_VERSION_STRING@/@FFI_VERSION_NUMBER@ (NO LIBFFI_ prefix —
# verified). configure_file substitutes @VAR@ by exact CMake variable name, so
# the variables MUST share the placeholder names. Wrap in a function() so the
# short names TARGET/VERSION do not leak into the parent scope.
function(_libffi_generate_ffi_header)
    set(TARGET X86_64)                       # ffitarget.h:57
    set(VERSION 3.7.1)                       # configure.ac:5 AC_INIT
    set(HAVE_LONG_DOUBLE 1)                  # x86-64 long double = 80-bit x87
    set(FFI_VERSION_STRING "3.7.1")
    set(FFI_VERSION_NUMBER 30701)            # 3*10000 + 7*100 + 1
    set(FFI_EXEC_TRAMPOLINE_TABLE 0)         # no tramp.c → dynamic trampoline
    configure_file(${CMAKE_SOURCE_DIR}/third_party/libffi/include/ffi.h.in
                   ${CMAKE_BINARY_DIR}/ffi.h)
endfunction()
_libffi_generate_ffi_header()

set(LIBFFI_DIR ${CMAKE_SOURCE_DIR}/third_party/libffi)
set(LIBFFI_SOURCES
    ${LIBFFI_DIR}/src/prep_cif.c
    ${LIBFFI_DIR}/src/types.c
    ${LIBFFI_DIR}/src/raw_api.c
    ${LIBFFI_DIR}/src/java_raw_api.c
    ${LIBFFI_DIR}/src/debug.c
    ${LIBFFI_DIR}/src/x86/ffi64.c
    ${LIBFFI_DIR}/src/x86/unix64.S
    ${LIBFFI_DIR}/src/x86/ffiw64.c
    ${LIBFFI_DIR}/src/x86/win64.S
    ${LIBFFI_DIR}/src/closures.c
    ${LIBFFI_DIR}/src/tramp.c
)
set(LIBFFI_INCLUDE_DIRS
    ${CMAKE_BINARY_DIR}              # <ffi.h> <fficonfig.h> <ffitarget.h>(生成/拷贝)
    ${LIBFFI_DIR}/include            # <tramp.h> <ffi_common.h>
    ${LIBFFI_DIR}/src                # dlmalloc.c(closures.c #include)
    ${LIBFFI_DIR}                    # 兜底
)

# libffi.so (shared, full-feature incl. ffi_closure). ffi.h.in's FFI_API is
# an empty define on non-Windows (ffi.h.in:152), so libffi does NOT mark
# exports with visibility("default") — mirror libudev.so: FLAGS adds
# -fvisibility=default to override add_user_lib(SHARED)'s default hidden
# (user_rules.cmake:109), exporting all non-static symbols. -Wno-error
# suppresses WARN_FLAGS' -Werror (dlmalloc is 6443 lines; per-warning
# -Wno-* is impractical) while keeping -Wall -Wextra visible.
# dlmalloc.c is NOT in SOURCES — closures.c:570 #includes it textually.
# tramp.c IS compiled but FFI_EXEC_STATIC_TRAMP is undefined, so only its
# #else stub branch builds (ffi_tramp_is_supported()→0, ffi_tramp_alloc/free
# no-ops). closures.c:901/1003/1029/1050 gate calls on ffi_tramp_is_supported(),
# so the stubs are never hit at runtime — but the link-time symbol refs must
# resolve, else libffi.so's PLT leaves ffi_tramp_free unresolved and the
# dynamic loader FATALs on reloc (was the test_ffi failure).
# ffiw64.c + win64.S provide the FFI_EFI64/FFI_GNUW64 (Windows x64) ABI path.
# ffitarget.h:134 unconditionally #defines FFI_GO_CLOSURES, so ffi64.c's
# ffi_call()/ffi_call_go() emit extern refs to ffi_call_efi64 /
# ffi_call_go_efi64 (plus ffi_prep_cif_machdep_efi64, _closure_loc_efi64,
# _prep_go_closure_efi64) — all defined in ffiw64.c via the EFI64() macro,
# which on non-X86_WIN64 expands to FFI_HIDDEN name##_efi64 (win64.S gives
# the asm backend ffi_call_win64/ffi_closure_win64/ffi_go_closure_win64).
# FFI_DEFAULT_ABI is FFI_UNIX64 here (ffitarget.h:103), so the runtime
# `if (abi == FFI_EFI64 || FFI_GNUW64)` branches are dead — but the link-time
# symbol refs must still resolve, same story as tramp.c. Symbols are
# FFI_HIDDEN so they don't pollute the exported ABI.
add_third_party_lib(ffi_so
    SOURCES ${LIBFFI_SOURCES}
    C
    OUTPUT_NAME ffi
    SO_LINK_LIBS c
    FLAGS "-fvisibility=default -O2"
    INCLUDE_DIRS ${LIBFFI_INCLUDE_DIRS}
    GEN_HEADERS ${CMAKE_BINARY_DIR}/fficonfig.h
                ${CMAKE_BINARY_DIR}/ffi.h
                ${CMAKE_BINARY_DIR}/ffitarget.h
)
