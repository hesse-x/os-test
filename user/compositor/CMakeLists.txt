# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/compositor/ — Wayland compositor (tinywl) + wallpaper client
#
# tinywl.c is migrated from the host-side reference compositor (wlroots scene
# API, layer-shell, SSD decorations). It links the Meson-built libwlroots
# staged at build/libwlroots-0.20.so, so it is EXCLUDE_FROM_ALL and built by
# build.sh after `meson install` (same pattern as test_pixman_smoke).
#
# wallpaper.c is a layer-shell background client (shm + libpng) that renders
# /usr/share/wallpaper.png fullscreen. It links only CMake-built libraries and
# builds in the normal pass.

# ===== libpng (third_party/libpng) — build_script/third_party/libpng/libpng.cmake =====
# Shared `png.so` rules. include() runs after zlib.cmake (parent scope provides
# `z_so`) and wayland.cmake.
include(${CMAKE_SOURCE_DIR}/build_script/third_party/libpng/libpng.cmake)

# Keep protocol data roots explicit: standard protocols come from the
# wayland-protocols package, while wlr-layer-shell is a wlroots extension.
set(WAYLAND_PROTOCOLS_DIR
    "${CMAKE_SOURCE_DIR}/third_party/wayland-protocols" CACHE PATH
    "Path to the wayland-protocols data package")
set(WLR_PROTOCOLS_DIR
    "${CMAKE_SOURCE_DIR}/third_party/wlroots/protocol" CACHE PATH
    "Path to wlroots-specific protocol XML files")

# ---- wlr-layer-shell client protocol (wallpaper client) ----
# Generated with the host wayland-scanner built by wayland.cmake
# (wayland_scanner_host), mirroring its wayland.xml generation rule.
# xdg-shell is needed too: the layer-shell private-code references
# xdg_popup_interface, whose definition lives in the xdg-shell protocol code.
set(LAYER_SHELL_XML
    ${WLR_PROTOCOLS_DIR}/wlr-layer-shell-unstable-v1.xml)
set(XDG_SHELL_XML
    ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(LAYER_SHELL_OUTPUTS
    ${CMAKE_BINARY_DIR}/wlr-layer-shell-client-protocol.h
    ${CMAKE_BINARY_DIR}/wlr-layer-shell-protocol.c
    ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
    ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c)
add_custom_command(
    OUTPUT ${LAYER_SHELL_OUTPUTS}
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header
                ${LAYER_SHELL_XML}
                ${CMAKE_BINARY_DIR}/wlr-layer-shell-client-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner private-code
                ${LAYER_SHELL_XML}
                ${CMAKE_BINARY_DIR}/wlr-layer-shell-protocol.c
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header
                ${XDG_SHELL_XML}
                ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner private-code
                ${XDG_SHELL_XML}
                ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c
    DEPENDS wayland_scanner_host ${LAYER_SHELL_XML} ${XDG_SHELL_XML}
    COMMENT "Generating wlr-layer-shell/xdg-shell client protocol sources"
    VERBATIM)

# wayland-client headers: third_party/wayland/src (wayland-client.h) +
# ${CMAKE_BINARY_DIR} (generated wayland-client-protocol.h, pnglibconf.h and
# wlr-layer-shell-client-protocol.h).
add_user_dyn_elf(wallpaper C
    SOURCES wallpaper.c
        ${CMAKE_BINARY_DIR}/wlr-layer-shell-protocol.c
        ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c
    LINK_LIBS png z wayland-client c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/third_party/wayland/src
        ${CMAKE_SOURCE_DIR}/third_party/libpng
        ${CMAKE_BINARY_DIR}
    GEN_HEADERS
        ${CMAKE_BINARY_DIR}/wlr-layer-shell-client-protocol.h
        ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
    IMAGE_PATH usr/bin/wallpaper)

# tinywl links the Meson-built libwlroots (staged to ${CMAKE_BINARY_DIR} by
# build.sh after `meson install --destdir sysroot`), so it stays out of ALL.
# Headers resolve from the sysroot: wlroots (include/wlroots-0.20), wayland,
# xkbcommon and pixman are all installed there by prepare-sysroot.sh /
# `meson install`. The *-protocol.h headers wlroots public headers reference
# are NOT installed — they only exist in the Meson build tree (protocol/).
set(WLROOTS_SYSROOT_DIR "${CMAKE_BINARY_DIR}/sysroot" CACHE PATH
    "Target sysroot containing installed wlroots dependencies")
set(WLROOTS_MESON_BUILD_DIR "${CMAKE_BINARY_DIR}/wlroots/wlroots" CACHE PATH
    "Meson build directory for wlroots")
set(WLROOTS_SYSROOT_INCLUDE ${WLROOTS_SYSROOT_DIR}/usr/include)
add_user_dyn_elf(tinywl C
    SOURCES tinywl.c
    LINK_LIBS wlroots-0.20 wayland-server xkbcommon pixman-1 c
    EXCLUDE_FROM_ALL
    DEFS WLR_USE_UNSTABLE
    INCLUDE_DIRS
        ${WLROOTS_SYSROOT_INCLUDE}
        ${WLROOTS_SYSROOT_INCLUDE}/wlroots-0.20
        ${WLROOTS_SYSROOT_INCLUDE}/pixman-1
        ${WLROOTS_MESON_BUILD_DIR}/protocol
    IMAGE_PATH usr/bin/tinywl)

# Wallpaper asset: staged verbatim into the root filesystem.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/wallpaper.png
    ${CMAKE_BINARY_DIR}/wallpaper.png COPYONLY)
os_image_path(wallpaper_png wallpaper.png usr/share/wallpaper.png)
