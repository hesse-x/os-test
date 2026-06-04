# cmake/do_link.cmake — Link script called by add_custom_command
# OBJ_LIST is passed as a string where groups from same target are semicolon-separated
# and different targets are space-separated. We need to normalize to a clean list.

# First, replace spaces with semicolons to merge all groups
string(REPLACE " " ";" OBJ_LIST "${OBJ_LIST}")

# Now OBJ_LIST is a proper CMake list (all semicolons)
execute_process(
    COMMAND ld -m elf_x86_64 -T ${LINKER_SCRIPT} ${OBJ_LIST} -o ${BIN_FILE}
    RESULT_VARIABLE ret
)

if(NOT ret EQUAL 0)
    message(FATAL_ERROR "Linking failed with code: ${ret}")
endif()
