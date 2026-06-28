# shell/ — Shell 用户进程

if(TEST)
    add_user_elf(shell SOURCES shell.cc LINK_LIBS c DEFS TEST)
else()
    add_user_elf(shell SOURCES shell.cc LINK_LIBS c)
endif()
