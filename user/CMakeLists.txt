# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/ — 用户态 libc + 测试程序

# libc 源文件列表（同源双产物：libc.a + libc.so）
# start.cc 已移除（_start 移至 crt0.o，见 plan_ld2b3 T4/T21）
# 新增 start_main.cc（__libc_start_main，T11）、atexit.c（T8）、init_array.c（T10）
set(LIBC_SOURCES
    lib/stdio.cc lib/string.cc lib/malloc.cc
    lib/unistd.cc lib/sys_wait.cc lib/sys_mman.cc lib/sys_shm.cc
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
    ${CMAKE_SOURCE_DIR}/common/kvformat.c
)

# libc.a（静态，-fno-pie -DDYNAMIC=0）
add_user_lib(c
    SOURCES ${LIBC_SOURCES}
    C
    FLAGS "-fno-pie -DDYNAMIC=0"
)

# libc.so（动态，-fPIC -DDYNAMIC=1）
# OUTPUT_NAME c → 产物 libc.so（gcc -shared -Wl,-soname,libc.so）
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
# UNITY_EXCLUDE_SETJMP_H / UNITY_EXCLUDE_MATH_H：排除宿主机 <setjmp.h>/<math.h>。
# freestanding + -nostdlib 下 <sys/cdefs.h> 不引入，宿主机 math.h 的
# __BEGIN_DECLS 等宏未定义会解析失败；Unity 自带 UNITY_IS_NAN/IS_INF 兜底。
target_compile_definitions(unity PRIVATE UNITY_EXCLUDE_SETJMP_H UNITY_EXCLUDE_MATH_H)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c)
add_user_dyn_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c)

# ld.so（阶段 2b+3：完整加载链路）
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

# hello_dyn（阶段 2b+3：完整动态链路验证）
# LINK_LIBS c → -lc 解析到 libc.so（动态链接，记 DT_NEEDED libc.so）
add_user_dyn_elf(hello_dyn C SOURCES hello_dyn.c LINK_LIBS c)
# 确保 libc.so 先于 hello_dyn 构建
add_dependencies(hello_dyn_dyn_elf c_so)

# driver/ — 用户态驱动进程（kbd_driver 跨目录引用 lib/ 下的 usb_kbd/input_driver）
add_user_elf(kbd_driver   SOURCES driver/kbd_driver.cc lib/usb_kbd.cc lib/input_driver.cc   LINK_LIBS c)
add_user_elf(terminal     SOURCES driver/terminal.cc     LINK_LIBS c)

# shell/ — Shell 用户进程
if(TEST)
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c DEFS TEST)
else()
    add_user_elf(shell SOURCES shell/shell.cc LINK_LIBS c)
endif()

# ===== ld.so 多依赖单元测试（ld.md §8 / plan_ld 阶段 D）=====
# 完全脱钩 drm：用 stub .so 拓扑 + 动态主 ELF 覆盖装配层（递归加载/去重/闭环）。
# stub .so 不走 add_user_lib(SHARED)（该路径绑死 libc.map + libc 导出验证），
# 这里用独立自定义命令，与 ld.so/libc.so 同模式（gcc -shared -fPIC -Wl,-soname）。
#
# 闭环场景需 a↔b 互 NEEDED（plan_ld D7）。互引用存在链接期 bootstrap 问题
# （a 链 b 时 b 尚未建）。解法：
#   1. libb.so.seed：空 .so（SONAME=libb.so，仅 NEEDED libc.so）作 bootstrap 种子
#   2. liba.so：链接 ld_stub_a.c + 直接引用 libb.so.seed（--no-as-needed），
#      记 NEEDED libb.so（取 seed 的 SONAME）+ libc.so。liba 不调任何 b 符号，
#      无需符号解析，DT_NEEDED 纯记录。
#   3. libb.so：链接 ld_stub_b.c + 直接引用 liba.so（--no-as-needed）+ libc.so。
#      libb 调 lda_answer（来自 liba）—— --allow-shlib-undefined 容忍该未定义引用，
#      记 NEEDED liba.so + libc.so。
# 链式/菱形场景复用同一对 stub，去重保证正确性。

# 1. libb.so.seed：bootstrap 种子（空 .so，SONAME=libb.so，仅 NEEDED libc.so）
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/libb.so.seed
    COMMAND gcc -shared -fPIC -nostdlib -nodefaultlibs
            -Wl,--hash-style=gnu -Wl,-soname,libb.so
            -o ${CMAKE_BINARY_DIR}/libb.so.seed -x c /dev/null
            -L${CMAKE_BINARY_DIR} -Wl,--no-as-needed -lc
    DEPENDS ${CMAKE_BINARY_DIR}/libc.so
    COMMENT "Linking libb.so.seed (ld.so stub bootstrap)")
# 2. liba.so：NEEDED libb.so（seed SONAME）+ libc.so
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
# 3. libb.so：NEEDED liba.so + libc.so（lda_answer 引用由 --allow-shlib-undefined 容忍）
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

# 4 个动态主 ELF：每场景一个，挂进 test_runner.c 的 tests[] 表（D9）
# 场景 1 单依赖回归
add_user_dyn_elf(ld_test_single C SOURCES test/ld_test_single.c LINK_LIBS c)
# 场景 2 线性链（主→b→{a,libc}）
add_user_dyn_elf(ld_test_chain C SOURCES test/ld_test_chain.c LINK_LIBS b a c)
add_dependencies(ld_test_chain_dyn_elf ld_stub_b_so ld_stub_a_so c_so)
# 场景 3 菱形去重（主→{a,libc}, a→libc）
add_user_dyn_elf(ld_test_diamond C SOURCES test/ld_test_diamond.c LINK_LIBS a c)
add_dependencies(ld_test_diamond_dyn_elf ld_stub_a_so c_so)
# 场景 4 闭环检测（a↔b 互 NEEDED）
add_user_dyn_elf(ld_test_cycle C SOURCES test/ld_test_cycle.c LINK_LIBS a b c)
add_dependencies(ld_test_cycle_dyn_elf ld_stub_a_so ld_stub_b_so c_so)
