# modules/regex.cmake - musl POSIX regex and fnmatch integration.
# Included by musl_rules.cmake after musl_generate_headers() + set(MUSL_DIR).

# Derive the module from upstream so new regex support files are picked up on
# musl updates. glob.c is intentionally excluded: GLOB_TILDE depends on the
# not-yet-migrated passwd database APIs getpwnam_r/getpwuid_r. The remaining
# files provide regcomp/regexec/regerror/regfree and fnmatch, with dependencies
# already supplied by the malloc, stdio, locale, multibyte, and wchar modules.
file(GLOB MUSL_REGEX_SOURCES ${MUSL_DIR}/src/regex/*.c)
list(REMOVE_ITEM MUSL_REGEX_SOURCES ${MUSL_DIR}/src/regex/glob.c)

add_musl_lib(musl_regex_objs SOURCES ${MUSL_REGEX_SOURCES})

