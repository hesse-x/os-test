# libinput build rules — extracted from user/CMakeLists.txt (was lines 1115-1193).
# Static `input` (add_user_lib) + shared `libinput_so` (add_third_party_lib) from
# the third_party/libinput submodule + repo compat/shim sources. libudev_so stays
# in user/CMakeLists.txt (its sources live in user/lib/udev-shim, not a submodule).

# ===== libinput (static library, keyboard subset) =====
# Provides libinput event processing pipeline. Wraps libevdev/udev shims.
set(LIBINPUT_SOURCES
    # libinput core (keyboard-only subset)
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-fallback.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-middle-button.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/evdev-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-button-debounce.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/libinput-plugin-mouse-wheel.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/path-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/udev-seat.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/timer.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-libinput.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/quirks.c
    # filters
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/filter.c
    # utilities
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-files.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-list.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-ratelimit.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-strings.c
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src/util-prop-parsers.c
    # os-test compat layer (scandir etc.)
    lib/libinput-compat/compat.c
    lib/libinput-compat/stubs.c
    lib/fnmatch.c
    # evdev/udev shims (needed by libinput internal)
    lib/evdev-shim/evdev.c
    # udev.c 不在此:libinput.so 经 SO_LINK_LIBS udev 链 libudev.so 取 udev_*
    # (完整 Linux 模式:libudev 独立 .so,见下方 libudev_so)。
)

# Shared warning-suppression list for the libinput third-party sources, used by
# both the static `input` library (below) and the shared `libinput.so` (further
# below). libinput upstream relies on implicit function declarations and other
# patterns that clang treats as hard errors in C11+ (not downgraded by -w), so
# these must be suppressed explicitly per-target. -Wno-unknown-warning-option
# lets the same list pass under gcc (which doesn't know the clang-only entries).
set(LIBINPUT_WARN_FLAGS
    "-Wno-unused-parameter -Wno-duplicate-decl-specifier -Wno-implicit-function-declaration -Wno-return-type -Wno-int-conversion -Wno-format -Wno-implicit-fallthrough -Wno-sign-compare -Wno-error=cpp -Wno-maybe-uninitialized -Wno-sometimes-uninitialized -Wno-unknown-warning-option -Wno-deprecated-non-prototype")

add_user_lib(input
    SOURCES ${LIBINPUT_SOURCES}
    C
    FLAGS "-O2 -DLIBINPUT_QUIRKS_DIR=\\\"/usr/share/libinput\\\" -DHAVE_QUIRKS ${LIBINPUT_WARN_FLAGS}"
    OUTPUT_NAME input
)

set(LIBINPUT_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include/uapi
    ${CMAKE_SOURCE_DIR}/user/include
    ${CMAKE_SOURCE_DIR}/include/uapi/compat
    ${CMAKE_SOURCE_DIR}/third_party/libinput/include
    ${CMAKE_SOURCE_DIR}/third_party/libinput
    ${CMAKE_SOURCE_DIR}/third_party/libinput/src
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-config
    ${CMAKE_SOURCE_DIR}/user/lib/evdev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/udev-shim
    ${CMAKE_SOURCE_DIR}/user/lib/libinput-compat
)

target_include_directories(input PRIVATE ${LIBINPUT_INCLUDE_DIRS})

# libinput.so (shared library, third_party SHARED-only, dynamically linked into terminal.elf)
# add_third_party_lib（分支 B）关 warning(-w)、用 os_base_options 等价基础项。-fvisibility=hidden
# + libinput 的 LIBINPUT_EXPORT(visibility("default")) 标记公共 API，仅导出公共符号。
# SO_LINK_LIBS c udev 记 DT_NEEDED libc.so + libudev.so。
add_third_party_lib(libinput_so
    SOURCES ${LIBINPUT_SOURCES}
    C
    OUTPUT_NAME input
    SO_LINK_LIBS c udev
    FLAGS "-O2 -DHAVE_QUIRKS ${LIBINPUT_WARN_FLAGS}"
    INCLUDE_DIRS ${LIBINPUT_INCLUDE_DIRS}
)
