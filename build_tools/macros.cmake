function(align_binary)
  set(options "")
  set(oneValueArgs NAME SRC OUT)
  cmake_parse_arguments(ALIGN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  add_custom_command(
      OUTPUT ${ALIGN_OUT}
      COMMAND bash ${BUILD_TOOLS}/align.sh $<TARGET_FILE:${ALIGN_SRC}> ${ALIGN_OUT}
      DEPENDS ${ALIGN_SRC}
      COMMENT "Align load_kernel.bin"
  )
  add_custom_target(${ALIGN_NAME} ALL DEPENDS ${ALIGN_OUT})
endfunction()
