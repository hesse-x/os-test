# wayland build rules — extracted from user/CMakeLists.txt (was lines 1502-1778).
# Host wayland-scanner + 3 freestanding .so (server/client/cursor) from the
# third_party/wayland submodule. Depends on LIBEXPAT_DIR (set by
# libexpat.cmake) for the host-scanner's vendored expat recompile, and on the
# `ffi` target (set by libffi.cmake) via SO_LINK_LIBS — include this AFTER both.

# ===================== wayland (third_party/wayland, 1.25.91) =====================
#
# Build libwayland-server.so / libwayland-client.so via add_third_party_lib(SHARED),
# no meson. See wayland.md. Two build-time roles:
#   - wayland-scanner: a HOST tool (runs on the dev machine during ./build.sh) that
#     parses protocol/wayland.xml → wayland-protocol.c + 4 *-protocol.h. Built with
#     the host cc + host glibc + vendored expat (dev box has no system expat header).
#   - libwayland-*.so: freestanding target libs, built by add_third_party_lib.
#
# §1.5 of wayland_worklist.md is obsolete: libc.so already exports
# readv/writev/sendmsg/recvmsg/socketpair/posix_fallocate/memfd_create/accept4/flock/
# mremap/strndup/gettid/prctl and the kernel implements sys_flock/sys_mremap, so no
# libc wrappers or syscall gaps remain — only the build adapter (this section).

# ---- 1. config.h + wayland-version.h (consumed by the freestanding .so) ----
# config.h template lives alongside this .cmake (build_script/third_party/wayland/),
# NOT in the wayland submodule working tree — mirrors libdrm (config.h in
# build_script/third_party/libdrm). Copied to build/wayland_config.h and
# force-injected via -include (see FLAGS below).
#
# Three sources have EXPLICIT config includes that -include cannot satisfy (the
# preprocessor still tries to resolve the literal #include):
#   wayland-os.c:28   #include "../config.h"   (src/ → third_party/wayland/config.h)
#   connection.c:29   #include "../config.h"   (same)
#   wayland-shm.c:33  #include "config.h"      (via -I third_party/wayland)
# ../config.h resolves relative to src/ into the submodule tree, which we refuse to
# pollute (third_party/wayland is a submodule; meson writes config.h there but we
# don't). Instead we copy those 3 sources to build/wayland_src/ at build time and
# rewrite the include to "wayland_config.h", resolved via -I build. The other .so
# sources compile straight from the submodule untouched. The -include remains as a
# belt-and-suspenders injection for any TU that lacks an explicit include.
configure_file(${CMAKE_CURRENT_LIST_DIR}/config.h
               ${CMAKE_BINARY_DIR}/wayland_config.h COPYONLY)

# Rewrite the 3 explicit config includes → "wayland_config.h" in build/ copies.
# Both "../config.h" and "config.h" spellings are swapped; only the 3 affected
# files are listed so the rest of the submodule compiles verbatim. We use python3
# (already a build dep via musl_rules) rather than sed: under CMake VERBATIM the
# '#' in "#include" gets mis-parsed by the ninja command line, and sed's quoting
# is fragile. A tiny read/replace/write is unambiguous.
set(WAYLAND_SRC_DIR ${CMAKE_SOURCE_DIR}/third_party/wayland/src)
set(WAYLAND_REWRITE_OUTPUTS
    ${CMAKE_BINARY_DIR}/wayland_src/connection.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
    ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
    ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c)
set(WAYLAND_CURSOR_DIR ${CMAKE_SOURCE_DIR}/third_party/wayland/cursor)
set(WAYLAND_REWRITE_SCRIPT ${CMAKE_BINARY_DIR}/wayland_rewrite_includes.py)
file(WRITE ${WAYLAND_REWRITE_SCRIPT}
"import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src).read()
data = data.replace('#include \"../config.h\"', '#include \"wayland_config.h\"')
data = data.replace('#include \"config.h\"', '#include \"wayland_config.h\"')
open(dst, 'w').write(data)
")
add_custom_command(
    OUTPUT ${WAYLAND_REWRITE_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/wayland_src
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/connection.c
                ${CMAKE_BINARY_DIR}/wayland_src/connection.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/wayland-os.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_SRC_DIR}/wayland-shm.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_CURSOR_DIR}/wayland-cursor.c
                ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
    COMMAND python3 ${WAYLAND_REWRITE_SCRIPT}
                ${WAYLAND_CURSOR_DIR}/os-compatibility.c
                ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c
    DEPENDS ${WAYLAND_SRC_DIR}/connection.c
            ${WAYLAND_SRC_DIR}/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-shm.c
            ${WAYLAND_CURSOR_DIR}/wayland-cursor.c
            ${WAYLAND_CURSOR_DIR}/os-compatibility.c
            ${CMAKE_BINARY_DIR}/wayland_config.h
    COMMENT "Rewriting wayland config.h includes"
    VERBATIM)
add_custom_target(wayland_rewrite_src DEPENDS ${WAYLAND_REWRITE_OUTPUTS})

# wayland-version.h: @-substituted from src/wayland-version.h.in. Included by
# scanner.c:28 (host) and wayland-server-core.h:33 / wayland-client-core.h:31
# (freestanding .so), so it must resolve from both — generate into build/ and -I it
# from both sides. Wrapped in a function so the short VERSION name stays local
# (same discipline as _libffi_generate_ffi_header above).
function(_wayland_generate_version_header)
    set(WAYLAND_VERSION "1.25.91")
    set(WAYLAND_VERSION_MAJOR 1)
    set(WAYLAND_VERSION_MINOR 25)
    set(WAYLAND_VERSION_MICRO 91)
    configure_file(${CMAKE_SOURCE_DIR}/third_party/wayland/src/wayland-version.h.in
                   ${CMAKE_BINARY_DIR}/wayland-version.h)
endfunction()
_wayland_generate_version_header()

# ---- 2. host wayland-scanner (build-time tool) ----
# Built with the bare host cc (NO freestanding flags / NO _tp_base_compile_flags) so
# it links against the host glibc and runs on the dev machine. scanner.c needs only
# wayland-version.h (WAYLAND_VERSION) + expat for XML parsing. We deliberately do NOT
# define HAVE_LIBXML (skips DTD validation + the #include "wayland.dtd.h" at
# scanner.c:41-46, so embed.py/wayland.dtd are unused) and do NOT define HAVE_STRNDUP
# (host glibc has strndup anyway; the #ifndef fallback at scanner.c:1031 is inert).
# expat: the dev box has no system expat.h, so recompile the vendored
# third_party/libexpat sources for the host and link them in directly. -w silences
# expat's ~6k-line warnings (host build, not gated by -Werror).
# Entropy: the shared build/expat_config.h defines XML_DEV_URANDOM, which makes
# xmlparse.c call writeRandomBytes_dev_urandom (supplied by random_dev_urandom.c).
# For the host scanner we use a SEPARATE host expat config
# (build_script/third_party/libexpat/expat_config_host.h →
# build/wayland_host/expat_config.h) that defines
# HAVE_GETRANDOM instead, and compile random_getrandom.c (not random_dev_urandom.c)
# — host glibc >=2.25 provides getrandom(2). Codegen needs no cryptographic
# entropy; this only seeds expat's hash randomization. The host expat objects put
# -I build/wayland_host ahead of -I build so "expat_config.h" resolves to the host
# copy, overriding the shared freestanding one.
configure_file(${CMAKE_SOURCE_DIR}/build_script/third_party/libexpat/expat_config_host.h
               ${CMAKE_BINARY_DIR}/wayland_host/expat_config.h COPYONLY)
set(WAYLAND_SCANNER_HOST_FLAGS
    -O2 -w
    -I${WAYLAND_SRC_DIR}
    -I${CMAKE_SOURCE_DIR}/third_party/wayland
    -I${CMAKE_BINARY_DIR}
    -I${LIBEXPAT_DIR}/lib)
# expat objects get the host config dir first so "expat_config.h" hits the host copy.
set(WAYLAND_SCANNER_HOST_EXPAT_FLAGS
    -O2 -w
    -I${WAYLAND_SRC_DIR}
    -I${CMAKE_SOURCE_DIR}/third_party/wayland
    -I${CMAKE_BINARY_DIR}/wayland_host
    -I${CMAKE_BINARY_DIR}
    -I${LIBEXPAT_DIR}/lib)
set(WAYLAND_SCANNER_HOST_OBJS
    ${CMAKE_BINARY_DIR}/wayland_scanner_scanner.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_util.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmlparse.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmlrole.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_xmltok.o
    ${CMAKE_BINARY_DIR}/wayland_scanner_getrandom.o)
add_custom_command(
    OUTPUT ${WAYLAND_SCANNER_HOST_OBJS}
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_FLAGS} -c
                ${WAYLAND_SRC_DIR}/scanner.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_scanner.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_FLAGS} -c
                ${WAYLAND_SRC_DIR}/wayland-util.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_util.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmlparse.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmlparse.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmlrole.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmlrole.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/xmltok.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_xmltok.o
    COMMAND ${CMAKE_C_COMPILER} ${WAYLAND_SCANNER_HOST_EXPAT_FLAGS} -c
                ${LIBEXPAT_DIR}/lib/random_getrandom.c -o ${CMAKE_BINARY_DIR}/wayland_scanner_getrandom.o
    DEPENDS ${WAYLAND_SRC_DIR}/scanner.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${LIBEXPAT_DIR}/lib/xmlparse.c
            ${LIBEXPAT_DIR}/lib/xmlrole.c
            ${LIBEXPAT_DIR}/lib/xmltok.c
            ${LIBEXPAT_DIR}/lib/random_getrandom.c
            ${CMAKE_BINARY_DIR}/wayland-version.h
            ${CMAKE_BINARY_DIR}/wayland_host/expat_config.h
    COMMENT "Compiling host wayland-scanner objects"
    VERBATIM)
# wayland-util.c uses round() (wl_fixed_from_double) → -lm on the host link.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/wayland-scanner
    COMMAND ${CMAKE_C_COMPILER} -o ${CMAKE_BINARY_DIR}/wayland-scanner
                ${WAYLAND_SCANNER_HOST_OBJS} -lm
    DEPENDS ${WAYLAND_SCANNER_HOST_OBJS}
    COMMENT "Linking host wayland-scanner")
add_custom_target(wayland_scanner_host DEPENDS ${CMAKE_BINARY_DIR}/wayland-scanner)

# ---- 3. generate the 5 protocol files from wayland.xml ----
set(WL_XML ${CMAKE_SOURCE_DIR}/third_party/wayland/protocol/wayland.xml)
set(WL_PROTOCOL_OUTPUTS
    ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-protocol.c)
add_custom_command(
    OUTPUT ${WL_PROTOCOL_OUTPUTS}
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner server-header
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner server-header -c
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header -c
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner public-code
                ${WL_XML} ${CMAKE_BINARY_DIR}/wayland-protocol.c
    DEPENDS wayland_scanner_host ${WL_XML}
    COMMENT "Generating wayland protocol sources"
    VERBATIM)
add_custom_target(wayland_protocol ALL DEPENDS ${WL_PROTOCOL_OUTPUTS})

# ---- 4. freestanding .so ----
# Generated headers all land in build/ so the .so sources resolve them via -I build
# (wayland-server.h:109 #include "wayland-server-protocol.h", wayland-client.h:40 likewise).
# -include wayland_config.h forces config.h into every TU (see step 1). Visibility:
# wayland-util.h:45 defines WL_EXPORT as visibility("default"); add_third_party_lib's
# SHARED default -fvisibility=hidden exports only WL_EXPORT symbols — matches upstream.
# SO_LINK_LIBS ffi c → DT_NEEDED libffi.so (ffi closures, event-loop.c) + libc.so;
# resolves to the ffi_so / c_so targets (third_party_rules.cmake SO_LINK_LIBS loop).
set(WL_GEN_HEADERS
    ${CMAKE_BINARY_DIR}/wayland-server-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-server-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol.h
    ${CMAKE_BINARY_DIR}/wayland-client-protocol-core.h
    ${CMAKE_BINARY_DIR}/wayland-version.h
    ${CMAKE_BINARY_DIR}/wayland_config.h)

# connection.c / wayland-os.c / wayland-shm.c come from build/wayland_src/ (the
# sed-rewritten copies with config includes pointed at wayland_config.h); the rest
# compile verbatim from the submodule. add_dependencies(wayland_rewrite_src) below
# guarantees the rewritten copies exist before the .so objects compile.
add_third_party_lib(wayland_server_so
    SOURCES ${WAYLAND_SRC_DIR}/wayland-server.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-shm.c
            ${WAYLAND_SRC_DIR}/event-loop.c
            ${CMAKE_BINARY_DIR}/wayland_src/connection.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${CMAKE_BINARY_DIR}/wayland-protocol.c
    C
    OUTPUT_NAME wayland-server
    SO_LINK_LIBS ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_server_so wayland_protocol wayland_rewrite_src)

add_third_party_lib(wayland_client_so
    SOURCES ${WAYLAND_SRC_DIR}/wayland-client.c
            ${CMAKE_BINARY_DIR}/wayland_src/connection.c
            ${CMAKE_BINARY_DIR}/wayland_src/wayland-os.c
            ${WAYLAND_SRC_DIR}/wayland-util.c
            ${CMAKE_BINARY_DIR}/wayland-protocol.c
    C
    OUTPUT_NAME wayland-client
    SO_LINK_LIBS ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_client_so wayland_protocol wayland_rewrite_src)

# ---- 5. libwayland-cursor.so (第三个 wayland target) ----
# cursor/ 下 wayland-cursor.c / os-compatibility.c 也 #include "config.h"（相对当前
# 目录解析），而 third_party/wayland/config.h 不存在（项目用 build/wayland_config.h
# + -include 注入替代），故二者同样走 rewrite（WAYLAND_REWRITE_OUTPUTS 已含之）；
# xcursor.c 不 include config.h，verbatim 编译。cursor 调用 wl_shm_* / wl_proxy_*
# 等客户端协议符号，故 SO_LINK_LIBS 拉入 wayland-client（文件级 DEPENDS 自动牵
# build/libwayland-client.so，add_dependencies 再补 target 保险）。INCLUDE_DIRS 多
# 加 cursor 目录以解析 "xcursor.h" / "os-compatibility.h"。运行时基本光标走内置
# load_fallback_theme（cursor-data.h，无需磁盘主题）；完整主题需向根分区放 cursor
# theme 数据并传 ICONDIR / XCURSOR_PATH（见 doc/design/todo.md）。
add_third_party_lib(wayland_cursor_so
    SOURCES ${CMAKE_BINARY_DIR}/wayland_src/wayland-cursor.c
            ${CMAKE_BINARY_DIR}/wayland_src/os-compatibility.c
            ${WAYLAND_CURSOR_DIR}/xcursor.c
    C
    OUTPUT_NAME wayland-cursor
    SO_LINK_LIBS wayland-client ffi c
    INCLUDE_DIRS ${WAYLAND_SRC_DIR}
                 ${CMAKE_SOURCE_DIR}/third_party/wayland
                 ${WAYLAND_CURSOR_DIR}
                 ${CMAKE_BINARY_DIR}
    FLAGS "-include ${CMAKE_BINARY_DIR}/wayland_config.h"
    GEN_HEADERS ${WL_GEN_HEADERS})
add_dependencies(wayland_cursor_so wayland_protocol wayland_rewrite_src wayland_client_so)
