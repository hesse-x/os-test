# user/ — 用户态 libc + 测试程序

# CMake add_library(STATIC c) 输出 libc.a
add_user_lib(c
    SOURCES lib/start.cc lib/stdio.cc lib/string.cc lib/malloc.cc lib/sys.cc
)

add_user_elf(hello  C SOURCES hello.c  LINK_LIBS c)
add_user_elf(malloc C SOURCES malloctest.c LINK_LIBS c)
