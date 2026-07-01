# user/ — 用户态 libc + 测试程序

# CMake add_library(STATIC c) 输出 libc.a
add_user_lib(c
    SOURCES
        lib/start.cc lib/stdio.cc lib/string.cc lib/malloc.cc
        lib/unistd.cc lib/sys_wait.cc lib/sys_mman.cc lib/sys_shm.cc
        lib/sys_irq.cc lib/sys_device.cc lib/sys_ipc.cc
lib/sys_process.cc lib/time.cc lib/file.cc
        lib/ctype.c lib/strtol.c lib/stdlib_misc.c lib/uname.c
        lib/sleep.c lib/assert.c lib/sys_pci.cc
        lib/signal.cc
        lib/input_client.cc
        ${CMAKE_SOURCE_DIR}/common/kvformat.c
)

# Unity test framework (static library)
add_user_lib(unity SOURCES ${CMAKE_SOURCE_DIR}/third_party/Unity/src/unity.c)
target_include_directories(unity PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/third_party/Unity/src
)
target_compile_definitions(unity PRIVATE UNITY_EXCLUDE_SETJMP_H)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c)
add_user_elf(test_fpu C SOURCES test/test_fpu.c LINK_LIBS c)
