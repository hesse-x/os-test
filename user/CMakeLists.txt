# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — userspace libc + test programs

# libc source list (dual artifacts from same source: libc.a + libc.so)
# start.cc removed (_start moved to crt0.o, see plan_ld2b3 T4/T21)
# start_main.cc removed (stdlib.md): musl src/env/__libc_start_main.c now
# provides __libc_start_main + __init_libc; atexit/__libc_run_atexit, the env
# set (environ/getenv/setenv/putenv/unsetenv/clearenv), and .init/.fini-array
# handling all come from musl upstream (src/env + src/exit). strtol.c removed
# (stdlib.md): musl src/stdlib/strtol.c + strtod.c + atoi/atol/atoll.c cover
# the whole strto* / ato* family. stdio.cc + stdio_exit.c removed (stdio.md):
# musl src/stdio/*.c (musl_stdio_objs) now provides the full stdio family with
# musl's struct _IO_FILE; musl's own __stdio_exit.c (globbed in) walks the ofl
# list + __stdin_used/__stdout_used/__stderr_used at exit, so the repo's
# stdout/stderr fflush override (stdio_exit.c) is obsolete. utils/kvformat.c
# removed from here: the only userspace caller was stdio.cc (gone); the kernel
# has its own -D__KERNEL__ -mno-sse compile of the same file (utils/CMakeLists
# .txt deliberately dual-compiles), so it is unaffected and utils/kvformat.{c,h}
# are NOT deleted.
# malloc.cc removed (malloc.md): musl src/malloc/mallocng/*.c + src/malloc/*.c
# (musl_malloc_objs) now provide the full malloc/free/calloc/realloc/aligned_alloc
# family plus the __libc_malloc/__libc_free/__malloc_replaced surface musl 1.2.x's
# loader requires. mallocng runs in mmap-only mode (this kernel's sys_brk is a
# fail stub; see kernel/bsd/syscall.c).
set(LIBC_SOURCES
    lib/unistd.cc lib/time.cc lib/file.cc
    lib/stdlib_misc.c lib/uname.c
    lib/assert.c lib/signal.cc lib/getrandom.c
    lib/musl_glue.c
    lib/io_multiplex.cc
    lib/sys_process.cc
    lib/sys_device.cc
    lib/sys_ipc.cc
    lib/musl_loader_shim.c
)

# ===================== musl single-libc philosophy =====================
# Real math (musl src/math + src/fenv) is merged INTO libc (c + c_so) via
# EXTRA_OBJS below — every static ELF gets math from libc.a, every dynamic
# ELF/.so gets it from libc.so, with zero -lm bookkeeping. libm.so is kept
# only as an EMPTY stub .so (see m_so below) so legacy `-lm` links succeed
# (the build-gcc / Mesa milestones pass -lm and expect libm.so to exist); it
# contributes no symbols, math resolves from libc.so at runtime. Same pattern
# glibc uses after merging libm/libpthread/libdl into libc. libpthread.so stub
# can be added the same way if a `-lpthread` consumer appears (pthread is
# already in libc via musl_pthread).

# libc.a (static, -fno-pie -DDYNAMIC=0)
# IMAGE_PATH usr/lib/libc.a: ship the static libc.a into the root FS (STATIC has
# no auto-default). IMAGE_ARTIFACT libc.a overrides the default lib<c>.a build name.
# EXTRA_OBJS: musl unistd + fcntl upstream objects (unistd_worklist M0.2/M0.3,
# fcntl_worklist §3d) merged into the archive, replacing the repo's hand-written
# wrappers, AND musl_pthread so every static-linked ELF automatically gets
# __init_tls, __errno_location, pthread_*, sigemptyset/..., __restore_rt, struct
# __libc — required because errno lives at %fs:0 (musl TCB) and EVERY ELF (even
# single-threaded hello) must run __init_tls at startup or the first errno fetch
# crashes. musl_pthread is built -fPIC by add_musl_lib (one compile serves both
# the static -no-pie ELF and the shared libc.so links).
add_user_lib(c
    SOURCES ${LIBC_SOURCES}
    EXTRA_OBJS $<TARGET_OBJECTS:musl_unistd_objs> $<TARGET_OBJECTS:musl_fcntl_objs>
               $<TARGET_OBJECTS:musl_socket_objs>
               $<TARGET_OBJECTS:musl_dl_objs> $<TARGET_OBJECTS:musl_linux_objs>
               $<TARGET_OBJECTS:musl_dirent_objs>
               $<TARGET_OBJECTS:musl_resource_objs>
               $<TARGET_OBJECTS:musl_process_objs>
               $<TARGET_OBJECTS:musl_pthread>
               $<TARGET_OBJECTS:musl_signal_objs>
               $<TARGET_OBJECTS:musl_string_objs>
               $<TARGET_OBJECTS:musl_ctype_objs>
               $<TARGET_OBJECTS:musl_setjmp_objs>
               $<TARGET_OBJECTS:musl_math_objs>
               $<TARGET_OBJECTS:musl_stdlib_objs>
               $<TARGET_OBJECTS:musl_malloc_objs>
               $<TARGET_OBJECTS:musl_mman_objs>
               $<TARGET_OBJECTS:musl_time_objs>
               $<TARGET_OBJECTS:musl_locale_objs>
               $<TARGET_OBJECTS:musl_stdio_objs>
               $<TARGET_OBJECTS:musl_multibyte_objs>
               $<TARGET_OBJECTS:musl_wchar_objs>
               $<TARGET_OBJECTS:musl_regex_objs>
               $<TARGET_OBJECTS:musl_misc_objs>
               $<TARGET_OBJECTS:compiler_rt_int128>
    C
    FLAGS "-fno-pie -DDYNAMIC=0"
    IMAGE_PATH usr/lib/libc.a
    IMAGE_ARTIFACT libc.a
)

# libc.so (dynamic, -fPIC -DDYNAMIC=1) — FUSED musl loader + hand-written libc
# (ldso.md). The musl loader objects (dynlink.lo/dlstart.lo, PIC) are spliced in
# via EXTRA_OBJS; -e _dlstart makes the loader entry the .so entry point. The
# loader's musl-internal symbol dependencies (__libc/__init_tp/__copy_tls/
# __block_all_sigs/... ) are satisfied by the musl_pthread objects below — the
# loader runs musl's real TLS setup during __dls3 (FS_BASE → musl struct
# pthread), and __libc_start_main does NOT override fs_base. Only the
# loader-only, non-pthread symbols (__tlsdesc_static/__tlsdesc_dynamic) still
# come from lib/musl_loader_shim.c (in LIBC_SOURCES). After bootstrap the
# loader jumps to the main ELF _start; the hand-written __libc_start_main then
# runs the loader's __libc_start_init() (do_init_fni) over every DSO's
# .init_array (loader dormant thereafter — ldso.md §2).
# Merges musl_pthread's objects too so libc.so carries __init_tls/__errno_location/
# pthread_*/sigset helpers — the dynamic loader and every dyn ELF depend on
# libc.so for these.
add_user_lib(c_so
    SOURCES ${LIBC_SOURCES}
    EXTRA_OBJS $<TARGET_OBJECTS:musl_unistd_objs_so> $<TARGET_OBJECTS:musl_fcntl_objs_so>
               $<TARGET_OBJECTS:musl_socket_objs_so>
               $<TARGET_OBJECTS:musl_dl_objs_so> $<TARGET_OBJECTS:musl_linux_objs_so>
               $<TARGET_OBJECTS:musl_dirent_objs_so>
               $<TARGET_OBJECTS:musl_resource_objs>
               $<TARGET_OBJECTS:musl_process_objs>
               $<TARGET_OBJECTS:musl_loader_objs> $<TARGET_OBJECTS:musl_pthread>
               $<TARGET_OBJECTS:musl_signal_objs>
               $<TARGET_OBJECTS:musl_string_objs>
               $<TARGET_OBJECTS:musl_ctype_objs>
               $<TARGET_OBJECTS:musl_setjmp_objs>
               $<TARGET_OBJECTS:musl_math_objs>
               $<TARGET_OBJECTS:musl_stdlib_objs>
               $<TARGET_OBJECTS:musl_malloc_objs_so>
               $<TARGET_OBJECTS:musl_mman_objs_so>
               $<TARGET_OBJECTS:musl_time_objs>
               $<TARGET_OBJECTS:musl_locale_objs>
               $<TARGET_OBJECTS:musl_stdio_objs>
               $<TARGET_OBJECTS:musl_multibyte_objs>
               $<TARGET_OBJECTS:musl_wchar_objs>
               $<TARGET_OBJECTS:musl_regex_objs>
               $<TARGET_OBJECTS:musl_misc_objs>
               $<TARGET_OBJECTS:compiler_rt_int128>
    C
    FLAGS "-fPIC -DDYNAMIC=1"
    SHARED
    OUTPUT_NAME c
    ENTRY _dlstart
    VERSION_MAP user/libc.map
    # NOTE: no SO_LINK_LIBS — libc.so is the base library, does not record DT_NEEDED on itself
)
add_dependencies(c_so musl_libc)
# Publish the fused libc.so under BOTH names (ldso.md §3 Phase 2, mirroring
# musl Makefile:214's hardlink): the SHARED auto-default already ships it to
# /lib/libc.so (DT_NEEDED soname target); this extra entry ships the SAME
# artifact to /lib/ld-musl-x86_64.so.1 (PT_INTERP path the kernel opens).
os_image_path(c_so libc.so lib/ld-musl-x86_64.so.1 PARTITION 2)

# compiler-rt's __int128 arithmetic helpers are an implementation runtime, not
# libc ABI. libc++ links this DSO explicitly when it needs them.
add_user_lib(clang_rt
    SOURCES
    EXTRA_OBJS $<TARGET_OBJECTS:compiler_rt_int128>
    C
    FLAGS "-fPIC"
    SHARED
    OUTPUT_NAME clang_rt
)

# libm.so — empty stub ELF. No SOURCES/EXTRA_OBJS → the link emits a .so with
# only a SONAME and no dynsym exports. Shipped to lib/libm.so so -lm resolves
# at link time; runtime math comes from libc.so. (If a future -lm-static
# consumer appears, add an empty static `m` stub analogously; none exists now.)
add_user_lib(m_so
    SOURCES lib/libm_stub.c
    C
    FLAGS "-fPIC -fvisibility=default"
    SHARED
    OUTPUT_NAME m
)


# Unity test framework (third_party 例外：编译期符号依赖，STATIC 链入 test ELF)
# add_third_party_lib(unity STATIC ...)：关 warning(-w)，仅 link os_base_options 取共性基础项，
# 不被我方 -Werror 门禁污染。test ELF 链 unity 替代内联 unity.c（消除 28 处内联 + 重复 DEFS）。
# UNITY_EXCLUDE_MATH_H：排除 host <math.h>。freestanding + -nostdlib 下 <sys/cdefs.h>
# 不被拉入，host math.h 的 __BEGIN_DECLS 等宏无法解析；Unity 自带 UNITY_IS_NAN/IS_INF fallback。
# 注意不要再加 UNITY_EXCLUDE_SETJMP_H：那会把 TEST_ABORT() 退化为 return，断言失败后
# 测试继续执行（NULL 级联解引用杀掉整个测试二进制，bug.md Bug 1）。user/include/setjmp.h
# + lib/setjmp.S 提供可用的 setjmp/longjmp，断言失败经 longjmp 正确中止单个测试。
# 定义挂在 unity target，经 INTERFACE 传给链入它的 test ELF。
add_third_party_lib(unity STATIC
    SOURCES ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/user/include
        ${CMAKE_SOURCE_DIR}/third_party/Unity/src
    INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/Unity/src)
target_compile_definitions(unity PUBLIC UNITY_EXCLUDE_MATH_H UNITY_INCLUDE_DOUBLE)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c IMAGE_PATH local/hello.elf)
# mathsink — musl libm end-to-end smoke (sqrt/sin/exp/copysignl/fmodl/sincos).
# Temporary diagnostic; ships to /usr/bin so the shell can run it.
add_user_elf(mathsink C SOURCES test/mathsink.c LINK_LIBS c IMAGE_PATH usr/bin/mathsink)
# test_fpu / ld_test_* / drm_test_link are diagnostic ELFs shipped only under /test/.
# In non-TEST builds they still compile (consumed nowhere at runtime — test_runner
# itself is TEST-gated in user/test/), but are NOT shipped to the image, preserving
# the pre-manifest mkdisk behavior (those ELFs were never in the non-TEST image).
# The TEST flag selects whether to register the manifest entry.
if(TEST)
    add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c)
else()
    add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c NO_IMAGE)
endif()

# hello_dyn (phase 2b+3: full dynamic chain verification)
# LINK_LIBS c -> -lc resolves to libc.so (dynamic link, records DT_NEEDED libc.so)
# add_user_dyn_elf 内部已自动解析 LINK_LIBS→<lib>_so/lib<lib>_so target 并 add_dependencies
# （见 user_rules.cmake），无需手写 add_dependencies(hello_dyn_dyn_elf c_so)。
# IMAGE_PATH local/hello_dyn.elf: ship to /local/ (override dyn-elf default test/).
add_user_dyn_elf(hello_dyn C SOURCES hello_dyn.c LINK_LIBS c IMAGE_PATH local/hello_dyn.elf)

# ===================== libc++ smoke ELF (opt-in, -DLIBCXX=1) =====================
# libc++ 端到端回归（refact_cmake.md §3.6）：覆盖 vector/string、throw/catch（libc++abi
# TLS __cxa_thread_atexit）、filesystem（int128 compiler runtime）、messages facet（catgets）
# 四条历史上反复断链的路径。test_runner 报 [PASS]/[FAIL]。
#
# 不走 add_user_dyn_elf：那条路径硬编码 -nostdlib -nodefaultlibs 链接，无法注入
# -stdlib=libc++ + libc++/libc++abi/libunwind 运行时（terminal 能用它只因零 STL/异常）。
# 这里照 ld_test_* stub 模式用手写 add_custom_command：clang++ 驱动 + -stdlib=libc++
# + sysroot 的 c++/v1 头与 libc++*.so 库（build_libcxx.sh 装入 build/sysroot/usr）。
#
# 时序：libc++ 由 build_libcxx.sh 在主 ninja 之后装 sysroot（--cxx），故本 target
# EXCLUDE_FROM_ALL——默认 ./build.sh 不构建（sysroot 可能无 libc++，强编会链接失败）；
# 仅 -DLIBCXX=1（./build.sh --cxx 传）时纳入构建。os_image_path 仍注册到 test/，但
# EXCLUDE_FROM_ALL target 不进 ninja 默认目标，manifest 里的依赖检查由 mkdisk 在
# 实际构建后满足。
if(LIBCXX)
    set(LIBCXX_SYSROOT ${CMAKE_BINARY_DIR}/sysroot)
    set(LIBCXX_LIB_DIR ${LIBCXX_SYSROOT}/usr/lib)
    set(LIBCXX_SMOKE_ELF ${CMAKE_BINARY_DIR}/libcxx_smoke.elf)
    set(LIBCXX_SMOKE_OBJ ${CMAKE_BINARY_DIR}/libcxx_smoke.o)

    # 编译：--sysroot + -stdlib=libc++ 让 clang 自然解析 sysroot 的 c++/v1 头（C++ 标准库）
    # 与 sysroot 的 musl C 头（<cstdio>→<stdio.h>）。不用 USER_FREESTANDING_FLAGS：其 -nostdinc
    # 会同时禁掉 sysroot 的 c++/v1 搜索路径，导致 <cstdio> 找不到；其 -ffreestanding 会让
    # clang 把 main 当普通函数 mangling（freestanding 下 main 不特殊）→ Scrt1.o 的 _start_c
    # 找不到 C 链接的 main。本程序是 hosted 动态 ELF（有 PT_INTERP、libc、main），故去掉
    # -ffreestanding。只用 C++ 标准库头，无 OS 专有头依赖，故无需项目 -I 路径（user/include
    # shim 反会抢先匹配 <stdio.h>）。--sysroot 已把搜索限制在 sysroot 内（无 host /usr/include
    # 泄漏，已验证）。
    add_custom_command(OUTPUT ${LIBCXX_SMOKE_OBJ}
        COMMAND ${CMAKE_CXX_COMPILER}
                -m64 -fno-builtin -fno-stack-protector -mno-red-zone
                -fno-pie ${USER_BUILD_FLAGS}
                --sysroot=${LIBCXX_SYSROOT} -stdlib=libc++
                -c ${CMAKE_CURRENT_SOURCE_DIR}/test/libcxx_smoke.cpp
                -o ${LIBCXX_SMOKE_OBJ}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/libcxx_smoke.cpp
        IMPLICIT_DEPENDS CXX ${CMAKE_CURRENT_SOURCE_DIR}/test/libcxx_smoke.cpp
        COMMENT "Compiling libcxx_smoke.o (libc++, opt-in)")

    # 链接：clang++ 驱动（理解 -stdlib=libc++ 的隐式运行时）+ Scrt1/crti/crtn + libc.so
    # + sysroot 的 libc++/libc++abi/libunwind。--no-as-needed 保留 libc++ DT_NEEDED；
    # --allow-shlib-undefined 容忍 libc++ → libc++abi → libunwind 的传递依赖。
    # libc++*.so 来自 sysroot（build_libcxx.sh 装入），不是 build/ 的 CMake target，
    # 故无 CMake target 依赖——EXCLUDE_FROM_ALL 下由 --cxx 全流程保证其在场。
    add_custom_command(OUTPUT ${LIBCXX_SMOKE_ELF}
        COMMAND ${CMAKE_CXX_COMPILER} -fno-pie -no-pie
                -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1
                -Wl,--hash-style=gnu -Wl,--no-as-needed -Wl,--allow-shlib-undefined
                --sysroot=${LIBCXX_SYSROOT} -stdlib=libc++ -nostdlib -nodefaultlibs
                -o ${LIBCXX_SMOKE_ELF}
                ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o
                ${LIBCXX_SMOKE_OBJ}
                -L${LIBCXX_LIB_DIR} -lc++ -lc++abi -lunwind -lclang_rt
                ${CMAKE_BINARY_DIR}/libc.so ${CMAKE_BINARY_DIR}/libclang_rt.so
                ${MUSL_LIB_DIR}/crtn.o
        DEPENDS ${LIBCXX_SMOKE_OBJ}
                ${MUSL_LIB_DIR}/Scrt1.o ${MUSL_LIB_DIR}/crti.o ${MUSL_LIB_DIR}/crtn.o
                ${CMAKE_BINARY_DIR}/libc.so ${CMAKE_BINARY_DIR}/libclang_rt.so
        COMMENT "Linking libcxx_smoke.elf (libc++, opt-in)")
    # No ALL keyword → not in the default build (the main `ninja` skips it); built on
    # demand by `ninja libcxx_smoke_elf` in build.sh after build_libcxx.sh installs libc++.
    add_custom_target(libcxx_smoke_elf DEPENDS ${LIBCXX_SMOKE_ELF})
    add_dependencies(libcxx_smoke_elf musl_libc c_so clang_rt)

    # Ship to /test/ for test_runner (registered even though EXCLUDE_FROM_ALL; built only
    # when -DLIBCXX=1 drives the target, which always coincides with --cxx having installed
    # libc++ into the sysroot and mkdisk probe-shipped the runtime .so into /lib).
    os_image_path(libcxx_smoke_elf libcxx_smoke.elf test/libcxx_smoke.elf PARTITION 2)
endif()

# terminal — dynamic ELF (libc.so + libinput.so), PT_INTERP → ld.so
# IMAGE_PATH usr/bin/terminal: override dyn-elf default test/.
add_user_dyn_elf(terminal
    SOURCES driver/terminal_libinput.cc
    LINK_LIBS input drm m c udev
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libinput/src;${CMAKE_SOURCE_DIR}/third_party/libinput/include;${CMAKE_SOURCE_DIR}/user/lib/udev-shim;${CMAKE_SOURCE_DIR}/user/lib/evdev-shim;${CMAKE_SOURCE_DIR}/third_party/drm;${CMAKE_SOURCE_DIR}/third_party/drm/include/drm"
    IMAGE_PATH usr/bin/terminal
)

# evdev: real keyboard event source (xHCI HID → SHM ring → terminal) +
# EVIOCG* ioctl query. Replaces the kbd driver for terminal input.
# IMAGE_PATH driver/evdev.dev: rename evdev.elf → driver/evdev.dev in the image.
add_user_elf(evdev  SOURCES driver/evdev.cc lib/usb_kbd.cc  LINK_LIBS c IMAGE_PATH driver/evdev.dev)

# ===== libinput (static library, keyboard subset) — see build_script/third_party/libinput/libinput.cmake =====
# Static `input` + shared `libinput.so` build rules extracted to keep this file
# focused on libc + user ELFs. include() runs in this scope, so the targets and
# LIBINPUT_* vars behave exactly as when inlined.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libinput/libinput.cmake)

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
    add_user_elf(shell SOURCES shell/shell.cc shell/shell_command.cc LINK_LIBS c DEFS TEST IMAGE_PATH usr/bin/shell)
else()
    add_user_elf(shell SOURCES shell/shell.cc shell/shell_command.cc LINK_LIBS c IMAGE_PATH usr/bin/shell)
endif()
os_image_path(shell shell.elf bin/sh)

# udev/ — device event daemon (subscribes to netlink uevent group)
add_user_elf(udevd C SOURCES udev/udevd.c LINK_LIBS c IMAGE_PATH usr/bin/udevd)
add_user_elf(syslogd C SOURCES syslogd.c LINK_LIBS c IMAGE_PATH usr/bin/syslogd)

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
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -o ${CMAKE_BINARY_DIR}/libb.so.seed -x c /dev/null
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so.seed (ld.so stub bootstrap)")
# 2. liba.so: NEEDED libb.so (seed SONAME) + libc.so
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/liba.so
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,liba.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c
            ${CMAKE_BINARY_DIR}/libb.so.seed
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_a.c ${CMAKE_BINARY_DIR}/libb.so.seed ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking liba.so (ld.so stub, NEEDED libb.so+libc.so)")
add_custom_target(ld_stub_a_so ALL DEPENDS ${CMAKE_BINARY_DIR}/liba.so)
add_dependencies(ld_stub_a_so c_so)
# Ship liba.so to /test/lib/liba.so (ld.so load fallback path — separate from
# production /lib/; standalone custom targets aren't covered by helper defaults).
os_image_path(ld_stub_a_so liba.so test/lib/liba.so)
# 3. libb.so: NEEDED liba.so + libc.so (lda_answer reference tolerated via --allow-shlib-undefined)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -Wl,--no-as-needed -Wl,--allow-shlib-undefined
            -o ${CMAKE_BINARY_DIR}/libb.so ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c
            ${CMAKE_BINARY_DIR}/liba.so
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_SOURCE_DIR}/user/test/ld_stub_b.c ${CMAKE_BINARY_DIR}/liba.so ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so (ld.so stub, NEEDED liba.so+libc.so)")
add_custom_target(ld_stub_b_so ALL DEPENDS ${CMAKE_BINARY_DIR}/libb.so)
add_dependencies(ld_stub_b_so ld_stub_a_so c_so)
os_image_path(ld_stub_b_so libb.so test/lib/libb.so)

# 4 dynamic main ELFs: one per scenario, hooked into test_runner.c's tests[] table (D9)
# Diagnostic-only (test_runner is TEST-gated): ship to /test/ only in TEST builds.
if(TEST)
    set(_LD_TEST_NO_IMAGE "")
else()
    set(_LD_TEST_NO_IMAGE NO_IMAGE)
endif()
# Scenario 1 single-dependency regression
add_user_dyn_elf(ld_test_single C SOURCES test/ld_test_single.c LINK_LIBS c ${_LD_TEST_NO_IMAGE})
# Scenario 2 linear chain (main -> b -> {a, libc})
add_user_dyn_elf(ld_test_chain C SOURCES test/ld_test_chain.c LINK_LIBS b a c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_chain_dyn_elf ld_stub_b_so ld_stub_a_so c_so)
# Scenario 3 diamond dedup (main -> {a, libc}, a -> libc)
add_user_dyn_elf(ld_test_diamond C SOURCES test/ld_test_diamond.c LINK_LIBS a c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_diamond_dyn_elf ld_stub_a_so c_so)
# Scenario 4 cycle detection (a<->b mutual NEEDED)
add_user_dyn_elf(ld_test_cycle C SOURCES test/ld_test_cycle.c LINK_LIBS a b c ${_LD_TEST_NO_IMAGE})
add_dependencies(ld_test_cycle_dyn_elf ld_stub_a_so ld_stub_b_so c_so)

# ===== libdrm (third_party/drm, core only) — see build_script/third_party/libdrm/libdrm.cmake =====
# Static `drm` + shared `drm_so` build rules extracted. include() runs in this
# scope and BEFORE the drm_test_link ELF below, so `drm`/`drm_so` are defined
# when it links them.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libdrm/libdrm.cmake)

# libdrm.a 验证小程序 (libdrm.md §3.4: drmOpen + drmModeGetResources)
# xf86drm.h / xf86drmMode.h and upstream <drm.h> resolution paths
# propagate automatically from the drm target's INTERFACE_INCLUDE_DIRS.
# IMAGE_PATH test/drm_test_link.elf: ship to /test/ in TEST builds only (diagnostic;
# test_runner is TEST-gated). In non-TEST builds, NO_IMAGE keeps it out of the image
# (matching the pre-manifest mkdisk, which never shipped it outside the TEST loop).
if(TEST)
    add_user_elf(drm_test_link C SOURCES test/drm_test_link.c LINK_LIBS drm c IMAGE_PATH test/drm_test_link.elf)
else()
    add_user_elf(drm_test_link C SOURCES test/drm_test_link.c LINK_LIBS drm c NO_IMAGE)
endif()

# ===== libffi (third_party/libffi, full-feature incl. closures) — see build_script/third_party/libffi/libffi.cmake =====
# Shared `ffi.so` build rules extracted. include() runs in this scope and before
# wayland (which SO_LINK_LIBS ffi) / libexpat.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libffi/libffi.cmake)

# ===================== libexpat.so — see build_script/third_party/libexpat/libexpat.cmake =====================
# Shared `expat.so` build rules extracted. Sets LIBEXPAT_DIR (consumed by
# wayland's host scanner) — include() runs in this scope, BEFORE wayland.cmake.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libexpat/libexpat.cmake)

# ===================== wayland (third_party/wayland, 1.25.91) — see build_script/third_party/wayland/wayland.cmake =====================
# Host wayland-scanner + 3 freestanding .so (server/client/cursor) build rules
# extracted. include() runs in this scope, AFTER libffi.cmake (provides `ffi`)
# and libexpat.cmake (provides LIBEXPAT_DIR for the host scanner).
include(${CMAKE_SOURCE_DIR}/build_script/third_party/wayland/wayland.cmake)
