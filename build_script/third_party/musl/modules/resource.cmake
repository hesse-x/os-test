# Complete <sys/resource.h> implementation. musl groups these wrappers by
# syscall family under misc, with prlimit kept in the Linux extension tree.
file(GLOB MUSL_RESOURCE_SOURCES CONFIGURE_DEPENDS
    ${MUSL_DIR}/src/misc/*rlimit.c
    ${MUSL_DIR}/src/misc/getrusage.c
    ${MUSL_DIR}/src/linux/prlimit.c
    ${MUSL_DIR}/src/thread/synccall.c)

add_musl_lib(musl_resource_objs SOURCES ${MUSL_RESOURCE_SOURCES})
