# libdrm build rules — extracted from user/CMakeLists.txt (was lines 1296-1344 + 1357-1372).
# Static `drm` (add_drm_lib) + shared `drm_so` (add_third_party_lib) from the
# third_party/drm submodule. The TEST-gated drm_test_link ELF sits BETWEEN these
# two segments in user/CMakeLists.txt and stays there; this file is include()d
# before it, so `drm`/`drm_so` exist when drm_test_link links them.

# ===== libdrm (third_party/drm, core only) — plan_drm2 =====
# libdrm 公共编译参数 (libdrm.md §3.3)
#
# config.h / libdrm.map live in the MAIN repo (build_script/libdrm/), not in
# the third_party/drm submodule — we must not pollute the upstream submodule
# working tree. CMake copies them into the build tree (configure_file) so the
# -include path resolves to build/libdrm_config.h and the (phase-4) version
# script to build/libdrm.map. generated_static_table_fourcc.h is produced by
# upstream's gen_table_fourcc.py (add_custom_command) so the build is fully
# reproducible with no manual one-shot steps.
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/config.h
               ${CMAKE_BINARY_DIR}/libdrm_config.h COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/build_script/libdrm/libdrm.map
               ${CMAKE_BINARY_DIR}/libdrm.map COPYONLY)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    COMMAND python3
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
            ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm/drm_fourcc.h
            ${CMAKE_SOURCE_DIR}/third_party/drm/gen_table_fourcc.py
    COMMENT "Generating libdrm static fourcc table")
add_custom_target(libdrm_fourcc_table
    DEPENDS ${CMAKE_BINARY_DIR}/generated_static_table_fourcc.h)

set(LIBDRM_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drm.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmMode.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmHash.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmRandom.c
    ${CMAKE_SOURCE_DIR}/third_party/drm/xf86drmSL.c)

# libdrm.a (static, -fno-pie, relaxed warnings — upstream submodule code)
# INCLUDE_DIRS: private paths for compiling libdrm sources (upstream <drm.h>,
# xf86drm internal headers, build dir for config.h/fourcc table).
# INTERFACE_INCLUDE_DIRS: propagated to any target that links drm — provides
# xf86drm.h / xf86drmMode.h location + upstream <drm.h> resolution path,
# so consumers don't need explicit INCLUDE_DIRS.
add_drm_lib(drm C SOURCES ${LIBDRM_SOURCES}
            INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                         ${CMAKE_SOURCE_DIR}/third_party/drm
                         ${CMAKE_BINARY_DIR}
            INTERFACE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                                   ${CMAKE_SOURCE_DIR}/third_party/drm
            FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h -Wno-unknown-warning-option -Wno-deprecated-non-prototype -Wno-macro-redefined")
# ensure generated_static_table_fourcc.h exists before libdrm sources compile
add_dependencies(drm libdrm_fourcc_table)

# ===== libdrm.so (shared library, third_party SHARED-only) — Phase A =====
# add_third_party_lib（分支 B：custom-command .so）关 warning(-w)、用 os_base_options
# 等价基础项，不携带我方 WARN_FLAGS(-Werror)，实现第三方 warning 自治。SO_LINK_LIBS c
# 记 DT_NEEDED libc.so。LINK_DEPS 挂 fourcc 表生成 target。-fvisibility=hidden + drm_public
# (via HAVE_VISIBILITY) gate exports（无 VERSION_MAP：verify_libc_exports.sh 不处理 drm* glob）。
add_third_party_lib(drm_so
    SOURCES ${LIBDRM_SOURCES}
    C
    OUTPUT_NAME drm
    SO_LINK_LIBS c
    LINK_DEPS libdrm_fourcc_table
    FLAGS "-Wno-unused-parameter -Wno-format -D__linux__ -include ${CMAKE_BINARY_DIR}/libdrm_config.h -Wno-macro-redefined"
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/drm/include/drm
                 ${CMAKE_SOURCE_DIR}/third_party/drm
                 ${CMAKE_BINARY_DIR}
)
