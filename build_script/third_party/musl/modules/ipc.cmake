# SysV shared-memory libc surface. The kernel currently returns ENOSYS for
# these syscall numbers; consumers such as Mesa use that result to fall back
# to non-shared allocations, but still require the public symbols at link time.
set(MUSL_IPC_SOURCES
    ${MUSL_DIR}/src/ipc/shmat.c
    ${MUSL_DIR}/src/ipc/shmctl.c
    ${MUSL_DIR}/src/ipc/shmdt.c
    ${MUSL_DIR}/src/ipc/shmget.c)

add_musl_lib(musl_ipc_objs SOURCES ${MUSL_IPC_SOURCES})
