# Copyright (c) 2026 hesse
#
# SPDX-License-Identifier: MIT

# user/compositor/ — Wayland compositor, desktop shell and Vulkan terminal
#
# core/server.c contains the migrated wlroots control plane (scene API,
# layer-shell and SSD decorations); app/compositor_main.c is assembly only.
# The target links the Meson-built libwlroots
# staged at build/libwlroots-0.20.so, so it is EXCLUDE_FROM_ALL and built by
# build.sh after `meson install` (same pattern as test_pixman_smoke).
#
# desktop_shell composes independent wallpaper and Dock modules.

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
set(XDG_DECORATION_XML
    ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
set(LAYER_SHELL_OUTPUTS
    ${CMAKE_BINARY_DIR}/wlr-layer-shell-client-protocol.h
    ${CMAKE_BINARY_DIR}/wlr-layer-shell-protocol.c
    ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
    ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c
    ${CMAKE_BINARY_DIR}/xdg-decoration-client-protocol.h
    ${CMAKE_BINARY_DIR}/xdg-decoration-protocol.c)
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
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner client-header
                ${XDG_DECORATION_XML}
                ${CMAKE_BINARY_DIR}/xdg-decoration-client-protocol.h
    COMMAND ${CMAKE_BINARY_DIR}/wayland-scanner private-code
                ${XDG_DECORATION_XML}
                ${CMAKE_BINARY_DIR}/xdg-decoration-protocol.c
    DEPENDS wayland_scanner_host ${LAYER_SHELL_XML} ${XDG_SHELL_XML}
            ${XDG_DECORATION_XML}
    COMMENT "Generating desktop client protocol sources"
    VERBATIM)

# wayland-client headers: third_party/wayland/src (wayland-client.h) +
# ${CMAKE_BINARY_DIR} (generated wayland-client-protocol.h, pnglibconf.h and
# wlr-layer-shell-client-protocol.h).
add_user_dyn_elf(desktop_shell C
    SOURCES app/desktop_shell_main.c dock/desktop_shell.c
        dock/dock.c dock/dock_layout.c
        ${CMAKE_BINARY_DIR}/wlr-layer-shell-protocol.c
        ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c
    LINK_LIBS png z wayland-client c
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/wayland/src
        ${CMAKE_SOURCE_DIR}/third_party/libpng
        ${CMAKE_BINARY_DIR}
    GEN_HEADERS
        ${CMAKE_BINARY_DIR}/wlr-layer-shell-client-protocol.h
        ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
    IMAGE_PATH usr/bin/desktop-shell)

# FreeType is kept deliberately small: the terminal only loads a bundled
# TrueType font and only needs the sfnt driver and smooth renderer.
set(FREETYPE_DIR ${CMAKE_SOURCE_DIR}/third_party/freetype)
add_third_party_lib(freetype STATIC C
    SOURCES
        ${FREETYPE_DIR}/src/base/ftbase.c
        ${FREETYPE_DIR}/src/base/ftbbox.c
        ${FREETYPE_DIR}/src/base/ftbitmap.c
        ${FREETYPE_DIR}/src/base/ftglyph.c
        ${FREETYPE_DIR}/src/base/ftinit.c
        ${FREETYPE_DIR}/src/base/ftmm.c
        ${FREETYPE_DIR}/src/base/ftsynth.c
        ${FREETYPE_DIR}/src/base/ftsystem.c
        ${FREETYPE_DIR}/src/base/ftdebug.c
        ${FREETYPE_DIR}/src/gzip/ftgzip.c
        ${FREETYPE_DIR}/src/lzw/ftlzw.c
        ${FREETYPE_DIR}/src/sfnt/sfnt.c
        ${FREETYPE_DIR}/src/smooth/smooth.c
        ${FREETYPE_DIR}/src/truetype/truetype.c
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/freetype-config
        ${FREETYPE_DIR}/include
    INTERFACE_INCLUDE_DIRS ${FREETYPE_DIR}/include
    FLAGS "-DFT2_BUILD_LIBRARY")

# Compile C-only protocol sources into a static archive so the terminal itself
# can remain C++ without compiling generated C as C++.
add_third_party_lib(terminal_protocols STATIC C
    SOURCES
        ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c
        ${CMAKE_BINARY_DIR}/xdg-decoration-protocol.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/third_party/wayland/src
        ${CMAKE_BINARY_DIR}
    GEN_HEADERS
        ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
        ${CMAKE_BINARY_DIR}/xdg-decoration-client-protocol.h)
add_dependencies(terminal_protocols wayland_scanner_host)

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TERMINAL_SHADER_DIR ${CMAKE_SOURCE_DIR}/user/driver/terminal_shader)
set(TERMINAL_SHADER_HEADERS)
foreach(_shader terminal_rect.vert terminal_rect.frag
                terminal_text.vert terminal_text.frag)
    string(REPLACE "." "_" _symbol ${_shader})
    set(_spv ${CMAKE_BINARY_DIR}/${_shader}.spv)
    set(_header ${CMAKE_BINARY_DIR}/${_shader}.spv.h)
    add_custom_command(
        OUTPUT ${_spv} ${_header}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.0
                -o ${_spv} ${TERMINAL_SHADER_DIR}/${_shader}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/build_script/embed_spirv.py
                ${_spv} ${_header} ${_symbol}_spv
        DEPENDS ${TERMINAL_SHADER_DIR}/${_shader}
                ${CMAKE_SOURCE_DIR}/build_script/embed_spirv.py
        COMMENT "Compiling terminal Vulkan shader ${_shader}"
        VERBATIM)
    list(APPEND TERMINAL_SHADER_HEADERS ${_header})
endforeach()

add_user_dyn_elf(terminal
    SOURCES ${CMAKE_SOURCE_DIR}/user/driver/terminal.cc
            ${CMAKE_SOURCE_DIR}/user/driver/terminal_vulkan.cc
    LINK_LIBS vulkan wayland-client xkbcommon z m c
    STATIC_LIBS freetype terminal_protocols
    EXCLUDE_FROM_ALL
    FLAGS -Wno-unused-parameter -Wno-missing-field-initializers
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/third_party/wayland/src
        ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include
        ${CMAKE_SOURCE_DIR}/user/driver
        ${CMAKE_SOURCE_DIR}/third_party/libxkbcommon/include
        ${FREETYPE_DIR}/include
        ${CMAKE_BINARY_DIR}
    GEN_HEADERS
        ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
        ${CMAKE_BINARY_DIR}/xdg-decoration-client-protocol.h
        ${TERMINAL_SHADER_HEADERS}
    IMAGE_PATH usr/bin/terminal)

if(TEST)
    add_user_dyn_elf(test_terminal_sgr
        SOURCES ${CMAKE_SOURCE_DIR}/user/driver/terminal.cc
        LINK_LIBS c
        STATIC_LIBS unity
        EXCLUDE_FROM_ALL
        DEFS TERMINAL_SGR_TEST
        FLAGS -Wno-unused-parameter -Wno-missing-field-initializers
              -Wno-unused-function
        INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/third_party/wayland/src
            ${CMAKE_SOURCE_DIR}/third_party/libxkbcommon/include
            ${FREETYPE_DIR}/include
            ${CMAKE_BINARY_DIR}
        GEN_HEADERS
            ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h
            ${CMAKE_BINARY_DIR}/xdg-decoration-client-protocol.h
        IMAGE_PATH test/test_terminal_sgr.elf)
endif()

# tinywl links the Meson-built libwlroots (staged to ${CMAKE_BINARY_DIR} by
# build.sh after `meson install --destdir sysroot`), so it stays out of ALL.
# Headers resolve from the sysroot: wlroots (include/wlroots-0.20), wayland,
# xkbcommon and pixman are all installed there by prepare-sysroot.sh /
# `meson install`. The *-protocol.h headers wlroots public headers reference
# are NOT installed — they only exist in the Meson build tree (protocol/).
#
# cursor.c is the only TU including user/cursor/cursors.h (os_cursors[] is
# static there); core/server.c only sees cursor.h. libpng (vendored png_so, OUTPUT
#_NAME png) decodes the cursor PNGs — same lib the desktop_shell wallpaper
# client links — and needs its header dir + ${CMAKE_BINARY_DIR} (pnglibconf.h).
set(WLROOTS_SYSROOT_DIR "${CMAKE_BINARY_DIR}/sysroot" CACHE PATH
    "Target sysroot containing installed wlroots dependencies")
set(WLROOTS_MESON_BUILD_DIR "${CMAKE_BINARY_DIR}/wlroots/wlroots" CACHE PATH
    "Meson build directory for wlroots")
set(WLROOTS_SYSROOT_INCLUDE ${WLROOTS_SYSROOT_DIR}/usr/include)
add_user_dyn_elf(tinywl C
    SOURCES app/compositor_main.c core/server.c cursor.c
        core/frame.c core/lifetime.c frame/frame_plan.c frame/scene_adapter.c
        window/window_state.c window/style/theme.c
        window/animation/legacy_genie.c
    LINK_LIBS wlroots-0.20 wayland-server input xkbcommon pixman-1 png z m c
    EXCLUDE_FROM_ALL
    DEFS WLR_USE_UNSTABLE
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${WLROOTS_SYSROOT_INCLUDE}
        ${WLROOTS_SYSROOT_INCLUDE}/wlroots-0.20
        ${WLROOTS_SYSROOT_INCLUDE}/pixman-1
        ${WLROOTS_MESON_BUILD_DIR}/protocol
        ${CMAKE_SOURCE_DIR}/user/cursor
        ${CMAKE_SOURCE_DIR}/third_party/libpng
        ${CMAKE_BINARY_DIR}
    IMAGE_PATH usr/bin/tinywl)

if(TEST)
    add_user_dyn_elf(test_compositor_contracts C
        SOURCES tests/test_contracts.c
            window/window_state.c window/style/theme.c core/lifetime.c
            dock/dock_layout.c protocols/effect_state.c
            protocols/trusted_client.c
            frame/frame_plan.c frame/scene_adapter.c
            renderer/mesh/mesh.c sdk/osgui.c
        LINK_LIBS m c
        FLAGS -UNDEBUG
        IMAGE_PATH test/test_compositor_contracts.elf)
endif()

# Desktop assets are staged verbatim into the root filesystem.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/wallpaper.png
    ${CMAKE_BINARY_DIR}/wallpaper.png COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/NotoMono-Regular.ttf
    ${CMAKE_BINARY_DIR}/NotoMono-Regular.ttf COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/OFL-1.1.txt
    ${CMAKE_BINARY_DIR}/OFL-1.1.txt COPYONLY)
os_image_path(wallpaper_png wallpaper.png usr/share/wallpaper.png)
os_image_path(terminal_font NotoMono-Regular.ttf
    usr/share/fonts/NotoMono-Regular.ttf)
os_image_path(terminal_font_license OFL-1.1.txt
    usr/share/licenses/NotoMono/OFL-1.1.txt)

# Apple-style cursor assets (MIT original, user/cursor/). The 44 static 128px
# PNGs are staged into build/ (os_image_path's artifact_relpath is relative to
# ${CMAKE_BINARY_DIR}, same pattern as wallpaper.png above) and registered in
# the image manifest. Animation sequences (wait/, left_ptr_watch/) are excluded
# — playback is out of scope this phase; cursors.h maps those names to the
# default arrow. The on-disk filenames must match the paths in cursors.h.
file(GLOB OS_CURSOR_PNGS "${CMAKE_SOURCE_DIR}/user/cursor/*.png")
foreach(_png ${OS_CURSOR_PNGS})
    get_filename_component(_name ${_png} NAME)
    configure_file(${_png} ${CMAKE_BINARY_DIR}/${_name} COPYONLY)
    os_image_path(cursor_${_name} ${_name} usr/share/cursors/${_name})
endforeach()
