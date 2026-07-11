/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_H
#define CONFIG_H

#define _GNU_SOURCE

#define PACKAGE_VERSION "1.30.4"
#define PACKAGE_STRING "libinput 1.30.4"
#define PACKAGE_BUGREPORT "https://gitlab.freedesktop.org/libinput/libinput"

#define LIBINPUT_VERSION_MAJOR 1
#define LIBINPUT_VERSION_MINOR 30
#define LIBINPUT_VERSION_MICRO 4
#define LIBINPUT_VERSION "1.30.4"

#define MESON_BUILD_ROOT ""

#define HAVE_QUIRKS 1
#define LIBINPUT_QUIRKS_DIR "/usr/share/libinput"

#define LIBINPUT_QUIRKS_OVERRIDE_FILE "/usr/share/libinput/quirks"
#define LIBINPUT_PLUGIN_ETCDIR "/etc/libinput"
#define LIBINPUT_PLUGIN_LIBDIR "/usr/lib/libinput"

#define HTTP_DOC_LINK "https://wayland.freedesktop.org/libinput/doc/1.30.4"

#undef HAVE_LIBWACOM
#undef HAVE_LUA
#undef HAVE_MTDEV
#undef HAVE_GSTACK
#undef HAVE_LIBWACOM_BUTTON_DIAL_MODESWITCH
#undef HAVE_LIBWACOM_BUTTON_MODESWITCH_MODE
#undef HAVE_LOCALE_H
#undef HAVE_XLOCALE_H

#endif
