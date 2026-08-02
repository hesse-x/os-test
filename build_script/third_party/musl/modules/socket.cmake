# modules/socket.cmake — musl socket integration (wayland §1.5).
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).
# Expects: MUSL_DIR, raw add_library, USER_FREESTANDING_FLAGS.
# ===================== musl socket integration (wayland §1.5) =====================
# Build the upstream musl src/network socket-syscall wrappers into libc, mirroring
# musl_fcntl_objs (same musl-internal include order, same dual -fno-pie/-fPIC build).
# These are NOT a network stack — each is a 6-30 line syscall passthrough. On x86_64
# (SYSCALL_USE_SOCKETCALL undefined) musl expands socketcall_cp(recvmsg,...) →
# syscall_cp(SYS_recvmsg, ...) → direct __NR_recvmsg, NOT the i386 socketcall(2)
# multiplex. The actual socket semantics live in the kernel's AF_UNIX SOCK_STREAM +
# SCM_RIGHTS impl (socket.c); wayland only uses AF_UNIX domain sockets, never TCP/IP,
# so no network stack is required. syscall_cp (cancellation point) is provided by
# musl_pthread.
#
# NOT globbed on purpose: src/network/ also holds the DNS resolver (getaddrinfo/
# resolv/res_msend/dns/lookup_*) which needs /etc/resolv.conf + a real network
# stack and would drag in undefined symbols. Only the pure socket-syscall wrappers
# are listed explicitly — same discipline as musl_fcntl_objs.
#
# Excluded from this list (supplied elsewhere):
#   recv.c was previously excluded for a name collision with the microkernel IPC
#   `recv` primitive, but that IPC symbol has been renamed to `ipc_recv` (see
#   user/include/xos/ipc.h), freeing the POSIX `recv` name — musl recv.c is now
#   included.
set(MUSL_SOCKET_SOURCES
    ${MUSL_DIR}/src/network/socket.c
    ${MUSL_DIR}/src/network/socketpair.c
    ${MUSL_DIR}/src/network/accept.c
    ${MUSL_DIR}/src/network/accept4.c
    ${MUSL_DIR}/src/network/connect.c
    ${MUSL_DIR}/src/network/bind.c
    ${MUSL_DIR}/src/network/listen.c
    ${MUSL_DIR}/src/network/shutdown.c
    ${MUSL_DIR}/src/network/getsockname.c
    ${MUSL_DIR}/src/network/getpeername.c
    ${MUSL_DIR}/src/network/getsockopt.c
    ${MUSL_DIR}/src/network/setsockopt.c
    ${MUSL_DIR}/src/network/send.c
    ${MUSL_DIR}/src/network/recv.c
    ${MUSL_DIR}/src/network/sendto.c
    ${MUSL_DIR}/src/network/recvfrom.c
    ${MUSL_DIR}/src/network/sendmsg.c
    ${MUSL_DIR}/src/network/recvmsg.c)

add_library(musl_socket_objs OBJECT ${MUSL_SOCKET_SOURCES})
target_include_directories(musl_socket_objs PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_socket_objs PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fno-pie -Wno-all)

# libc.so needs PIC objects (mirror the libc.a(-fno-pie)/libc.so(-fPIC) dual build).
add_library(musl_socket_objs_so OBJECT ${MUSL_SOCKET_SOURCES})
target_include_directories(musl_socket_objs_so PRIVATE
    ${MUSL_GEN_INCLUDE_DIR}
    ${MUSL_DIR}/src/include
    ${MUSL_DIR}/src/internal
    ${MUSL_DIR}/include
    ${MUSL_DIR}/arch/x86_64
    ${MUSL_DIR}/arch/generic
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi)
target_compile_options(musl_socket_objs_so PRIVATE -m64 ${USER_FREESTANDING_FLAGS} -D_XOPEN_SOURCE=700 -fPIC -Wno-all)
add_dependencies(musl_socket_objs musl_headers)
add_dependencies(musl_socket_objs_so musl_headers)
