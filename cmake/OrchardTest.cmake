include(CMakeParseArguments)

function(orchard_add_cpp_test target_name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "orchard_add_cpp_test(${target_name}) requires SOURCES.")
  endif()

  add_executable("${target_name}" ${ARG_SOURCES})
  orchard_configure_target("${target_name}")

  target_include_directories(
    "${target_name}"
    PRIVATE
      "${PROJECT_SOURCE_DIR}/tests/support/include"
  )

  if(ARG_LIBRARIES)
    target_link_libraries("${target_name}" PRIVATE ${ARG_LIBRARIES})
  endif()

  add_test(NAME "${target_name}" COMMAND "${target_name}")
endfunction()

