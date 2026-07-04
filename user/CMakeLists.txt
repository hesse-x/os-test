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
