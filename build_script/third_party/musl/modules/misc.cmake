# modules/misc.cmake -- selected musl misc interfaces with complete OS support.
# syslog.c owns the syslog API family. Keep this explicit rather than globbing
# src/misc, whose unrelated sources have their own migration prerequisites.
add_musl_lib(musl_misc_objs SOURCES
    ${MUSL_DIR}/src/misc/syslog.c)
