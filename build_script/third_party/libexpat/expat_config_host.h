/* expat_config_host.h — host-side expat config for the wayland-scanner build
 * tool.
 *
 * The freestanding libexpat.so uses
 * build_script/third_party/libexpat/expat_config.h with XML_DEV_URANDOM
 * (entropy from /dev/urandom via random_dev_urandom.c). The host
 * wayland-scanner is a build-time tool that runs on the dev machine; it has no
 * need for /dev/urandom and the dev box may not wire it the same way. Instead
 * we enable HAVE_GETRANDOM (host glibc >=2.25 provides getrandom(2)) and
 * compile random_getrandom.c, dropping the XML_DEV_URANDOM path entirely.
 * Codegen needs no cryptographic entropy — this only seeds expat's hash
 * randomization.
 *
 * CMake copies this to build/wayland_host/expat_config.h and the host expat
 * objects put -I build/wayland_host ahead of -I build so this overrides the
 * shared build/expat_config.h for the "expat_config.h" include in xmlparse.c.
 *
 * Copyright (c) 2026 hesse
 * SPDX-License-Identifier: MIT
 */

#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

/* x86_64 is little-endian */
#define BYTEORDER 1234

/* Standard headers available in the host glibc */
#define HAVE_FCNTL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* Expat feature configuration */
#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1

/* Entropy source: host getrandom(2) (NOT /dev/urandom). */
#define HAVE_GETRANDOM 1

#endif /* EXPAT_CONFIG_H */
