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

/* libc.so now provides versionsort + strverscmp (musl dirent/string). Tell
 * libinput-versionsort.h (HAVE_VERSIONSORT) to drop its own static inline
 * copies and use the system ones — avoids a "static follows non-static" clash
 * with <dirent.h>'s versionsort declaration (libinput defines _GNU_SOURCE). */
#define HAVE_VERSIONSORT 1

#define LIBINPUT_QUIRKS_OVERRIDE_FILE "/usr/share/libinput/quirks"
#define LIBINPUT_PLUGIN_ETCDIR "/etc/libinput"
#define LIBINPUT_PLUGIN_LIBDIR "/usr/lib/libinput"

#define HTTP_DOC_LINK "https://wayland.freedesktop.org/libinput/doc/1.30.4"

/* HAVE_VERSIONSORT: this libc provides strverscmp/versionsort (musl
 * src/string/strverscmp.c, merged into libc.a). libinput's
 * libinput-versionsort.h #ifndef-guards a static-inline strverscmp fallback;
 * under _GNU_SOURCE musl's <string.h> also declares strverscmp (non-static),
 * so without this the static definition clashes ("static declaration follows
 * non-static declaration"). Defining HAVE_VERSIONSORT skips libinput's
 * fallback — mirroring libinput's own meson feature-probe (meson.build:116). */
#define HAVE_VERSIONSORT 1

#undef HAVE_LIBWACOM
#undef HAVE_LUA
#undef HAVE_MTDEV
#undef HAVE_GSTACK
#undef HAVE_LIBWACOM_BUTTON_DIAL_MODESWITCH
#undef HAVE_LIBWACOM_BUTTON_MODESWITCH_MODE
#undef HAVE_LOCALE_H
#undef HAVE_XLOCALE_H

#endif
