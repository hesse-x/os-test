# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — userspace libc + test programs

# libc source list (dual artifacts from same source: libc.a + libc.so)
# start.cc removed (_start moved to crt0.o, see plan_ld2b3 T4/T21)
# Added start_main.cc (__libc_start_main, T11), atexit.c (T8), init_array.c (T10)
set(LIBC_SOURCES
    lib/stdio.cc lib/string.cc lib/malloc.cc
    lib/unistd.cc lib/sys_wait.cc lib/sys_mman.cc
    lib/sys_irq.cc lib/sys_device.cc lib/sys_ipc.cc
    lib/sys_process.cc lib/time.cc lib/file.cc
    lib/ctype.c lib/strtol.c lib/stdlib_misc.c lib/uname.c
    lib/sleep.c lib/assert.c lib/sys_pci.cc
    lib/signal.cc
    lib/input_client.cc
    lib/tls.cc
    lib/pthread.cc
    lib/start_main.cc
    lib/atexit.c
    lib/init_array.c
    lib/environ.c
    lib/errno.cc
    lib/select.c
    lib/setjmp.S
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
)

# Unity test framework (static library)
add_user_lib(unity SOURCES ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c)
target_include_directories(unity PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/third_party/Unity/src
)
# UNITY_EXCLUDE_SETJMP_H / UNITY_EXCLUDE_MATH_H: exclude host <setjmp.h>/<math.h>.
# Under freestanding + -nostdlib, <sys/cdefs.h> is not pulled in; host math.h's
# __BEGIN_DECLS and similar macros would fail to resolve. Unity ships its own
# UNITY_IS_NAN/IS_INF fallback.
target_compile_definitions(unity PRIVATE UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

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
add_user_dyn_elf(hello_dyn C SOURCES hello_dyn.c LINK_LIBS c)
# Ensure libc.so is built before hello_dyn
add_dependencies(hello_dyn_dyn_elf c_so)

# driver/ — userspace driver processes (kbd_driver references lib/usb_kbd and
# lib/input_driver across directories)
add_user_elf(kbd_driver   SOURCES driver/kbd_driver.cc lib/usb_kbd.cc lib/input_driver.cc   LINK_LIBS c)
add_user_elf(terminal     SOURCES driver/terminal.cc     LINK_LIBS c)

# shell/ — Shell user process
if(TEST)
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c DEFS TEST)
else()
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c)
endif()

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
