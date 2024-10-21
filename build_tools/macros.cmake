function(align_binary)
  set(options "")
  set(oneValueArgs NAME SRC OUT PATH)
  cmake_parse_arguments(ALIGN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  add_custom_target(${ALIGN_NAME}
      bash ${BUILD_TOOLS}/align.sh ${ALIGN_PATH}/${ALIGN_SRC} ${ALIGN_OUT}
      DEPENDS ${ALIGN_SRC}
      COMMENT "Align load_kernel.bin"
  )
endfunction()
