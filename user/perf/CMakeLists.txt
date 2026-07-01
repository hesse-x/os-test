# user/perf/ — perf采样包装器
# START → spawn test_runner → waitpid → STOP_DUMP
add_user_elf(perf C SOURCES perf.cc LINK_LIBS c)
