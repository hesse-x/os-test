configure_file(perf_config.h.in perf_config.h @ONLY)

add_user_dyn_elf(perf_collector C SOURCES perf.c LINK_LIBS c
    INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}
    GEN_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/perf_config.h
    IMAGE_PATH usr/bin/perf)
