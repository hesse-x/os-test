# modules/select.cmake -- musl poll/ppoll syscall wrappers.
#
# Both wrappers only use SYS_poll/SYS_ppoll, which kernel/bsd/socket.c
# implements. syscall_cp also makes them musl cancellation points.
add_musl_lib(musl_select_objs SOURCES
    ${MUSL_DIR}/src/select/poll.c
    ${MUSL_DIR}/src/select/ppoll.c)
